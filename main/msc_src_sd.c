// See msc_src_sd.h. UNVERIFIED on hardware: the pin map is transcribed from
// the Waveshare schematic (board.h), the pull-ups likewise, and 4-bit SDMMC is
// the first thing to try; SDSPI on the same pins is the fallback if the init
// log never shows a card.

#include "msc_src_sd.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "msc_out.h"
#include "sdmmc_cmd.h"
#include "status.h"

#if !BOARD_HAS_SDCARD
#error "msc_src_sd.c is compiled for a board with no SD slot (board.h)"
#endif

static const char *TAG = "msc_sd";

// A no-card probe is a CMD8 with no response: the controller's own response
// timeout fires in under a millisecond, and the 1 s software timeout is a
// backstop, so the slot is cheap to poll. The driver still logs an error per
// attempt, which quiet_card_init() handles.
#define PROBE_ABSENT_MS 1000
// With a card present, CMD13 (SEND_STATUS) is microseconds; this is how
// quickly a pulled card is noticed.
#define PROBE_PRESENT_MS 500

static SemaphoreHandle_t s_lock;  // card init/deinit vs data transfers
static sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();
static sdmmc_card_t s_card;

// What the LUN asks about from TinyUSB's task, kept apart from the transfer
// lock on purpose: is_present is called under msc_out's lock from TEST UNIT
// READY and READ CAPACITY, and if it had to wait for a read in progress the
// USB task — the keyboard's — would stall for the read's duration. A
// spinlock-guarded snapshot the probe task writes answers in nanoseconds.
static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_present;             // under s_snap_lock
static struct msc_geometry s_geo;  // under s_snap_lock
static esp_err_t s_last_init_err;  // probe task only

// --- msc_source ----------------------------------------------------------------

static bool sd_present(void *ctx, struct msc_geometry *out) {
  (void)ctx;
  portENTER_CRITICAL(&s_snap_lock);
  const bool present = s_present;
  if (present) {
    *out = s_geo;
  }
  portEXIT_CRITICAL(&s_snap_lock);
  return present;
}

// Probe task only. Publishes the card's facts for sd_present.
static void set_present(bool present) {
  portENTER_CRITICAL(&s_snap_lock);
  s_present = present;
  if (present) {
    s_geo.block_count = (uint32_t)s_card.csd.capacity;
    s_geo.block_size = (uint16_t)s_card.csd.sector_size;
  }
  portEXIT_CRITICAL(&s_snap_lock);
}

static bool sd_writable(void *ctx) {
  (void)ctx;
  return false;  // read-only until issue #34
}

static int sd_read(void *ctx, uint32_t lba, uint32_t offset, void *buf,
                   uint32_t len) {
  (void)ctx;
  // TinyUSB hands out whole 512-byte multiples at 512-aligned offsets (its
  // buffer is 4 KB); anything else is a contract break, refused not rounded.
  if (offset % 512 != 0 || len % 512 != 0 || len == 0) {
    return -1;
  }
  portENTER_CRITICAL(&s_snap_lock);
  const bool present = s_present;
  portEXIT_CRITICAL(&s_snap_lock);
  if (!present) {
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const esp_err_t err =
      sdmmc_read_sectors(&s_card, buf, lba + offset / 512, len / 512);
  xSemaphoreGive(s_lock);
  return err == ESP_OK ? 0 : -1;
}

static const struct msc_source s_source = {
    .name = "microSD",
    .is_present = sd_present,
    .is_writable = sd_writable,
    .read = sd_read,
    .write = NULL,
    .flush = NULL,
    .ctx = NULL,
};

// --- probe task ------------------------------------------------------------------

static void card_arrived(void) {
  char name[STATUS_MEDIA_LEN];
  const uint64_t bytes = (uint64_t)s_card.csd.capacity * s_card.csd.sector_size;
  snprintf(name, sizeof(name), "microSD %.1fG", (double)bytes / 1e9);
  ESP_LOGI(TAG, "card: %s, %lu sectors of %d, %s-bit", s_card.cid.name,
           (unsigned long)s_card.csd.capacity, s_card.csd.sector_size,
           (s_host.flags & SDMMC_HOST_FLAG_4BIT) ? "4" : "1");
  // Only if nothing else holds the LUN: a browser image already attached
  // wins, and the card waits for it to be ejected. Atomic, so a race with
  // the media socket cannot end with the loser replacing the winner.
  if (msc_out_bind_if_free(&s_source)) {
    status_set_media(true, name);
  } else {
    ESP_LOGI(TAG, "LUN busy with another medium; card not exposed");
  }
}

static void card_gone(void) {
  ESP_LOGI(TAG, "card removed");
  if (msc_out_unbind_if(&s_source)) {
    status_set_media(false, NULL);
  }
}

// The driver logs an ERROR on every failed init, and with no card that is
// once a second forever. Silenced around the attempt — but a *changed* error
// is the signal that decides the SDSPI fallback (a card that is there and
// fails 4-bit init reports CRC or INVALID_RESPONSE, not the steady TIMEOUT of
// an empty slot), so that is logged whenever it differs from the last one.
static esp_err_t quiet_card_init(void) {
  static const char *tags[] = {"sdmmc_cmd", "sdmmc_req", "sdmmc_sd",
                               "sdmmc_common"};
  esp_log_level_t saved[4];
  for (int i = 0; i < 4; i++) {
    saved[i] = esp_log_level_get(tags[i]);
    esp_log_level_set(tags[i], ESP_LOG_NONE);
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const esp_err_t err = sdmmc_card_init(&s_host, &s_card);
  xSemaphoreGive(s_lock);
  for (int i = 0; i < 4; i++) {
    esp_log_level_set(tags[i], saved[i]);
  }
  if (err != s_last_init_err) {
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "card init ok");
    } else {
      ESP_LOGW(TAG,
               "card init: %s (an empty slot is a steady TIMEOUT; anything "
               "else with a card inserted is a wiring or bus-width problem)",
               esp_err_to_name(err));
    }
    s_last_init_err = err;
  }
  return err;
}

static void probe_task(void *arg) {
  (void)arg;
  s_last_init_err = ESP_OK;  // so the first failure is logged
  for (;;) {
    portENTER_CRITICAL(&s_snap_lock);
    const bool present = s_present;
    portEXIT_CRITICAL(&s_snap_lock);

    if (!present) {
      if (quiet_card_init() == ESP_OK) {
        set_present(true);
        card_arrived();
        continue;
      }
      vTaskDelay(pdMS_TO_TICKS(PROBE_ABSENT_MS));
      continue;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t st = sdmmc_get_status(&s_card);
    xSemaphoreGive(s_lock);
    if (st != ESP_OK) {
      set_present(false);
      card_gone();
      continue;
    }
    // If the browser medium was ejected meanwhile, the card takes over.
    if (msc_out_current() == NULL) {
      card_arrived();
    }
    vTaskDelay(pdMS_TO_TICKS(PROBE_PRESENT_MS));
  }
}

esp_err_t msc_src_sd_start(void) {
  s_lock = xSemaphoreCreateMutex();
  if (s_lock == NULL) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = sdmmc_host_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdmmc_host_init: %s", esp_err_to_name(err));
    return err;
  }
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = BOARD_SD_CLK;
  slot.cmd = BOARD_SD_CMD;
  slot.d0 = BOARD_SD_D0;
  slot.d1 = BOARD_SD_D1;
  slot.d2 = BOARD_SD_D2;
  slot.d3 = BOARD_SD_D3;
  slot.cd = SDMMC_SLOT_NO_CD;
  slot.wp = SDMMC_SLOT_NO_WP;
  slot.width = BOARD_SD_BUS_WIDTH;
  // The board has 10 K external pull-ups on all six lines (schematic); the
  // chip's internal ones are documented as debug-grade, so they stay off.
  slot.flags = 0;
  s_host.slot = SDMMC_HOST_SLOT_1;
  err = sdmmc_host_init_slot(s_host.slot, &slot);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdmmc_host_init_slot: %s", esp_err_to_name(err));
    return err;
  }
  // Below the msc_io worker (4): probing must never delay a transfer.
  if (xTaskCreatePinnedToCore(probe_task, "sd_probe", 4096, NULL, 3, NULL, 0) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "microSD slot ready (CLK %d CMD %d D0 %d, %d-bit)",
           BOARD_SD_CLK, BOARD_SD_CMD, BOARD_SD_D0, BOARD_SD_BUS_WIDTH);
  return ESP_OK;
}
