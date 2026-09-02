// One-off hardware identification tool, not part of the firmware.
//
// AGENTS.md §6.1 warns that the 0.42" OLED's I2C pins differ across the C3, C6
// and S3 variants of this board family, and that the widely-quoted C3 numbers
// (SDA GPIO5 / SCL GPIO6) must not be carried over. board.h carried them over
// anyway, marked UNVERIFIED, and on the first real S3 the panel stayed frozen
// on whatever the vendor demo had left in its GDDRAM — the firmware never
// reached it. This finds the real pair by trying them.
//
// Two stages, because an ACK is not a display:
//
//   1. Sweep every ordered (SDA, SCL) pair of candidate pins and probe the
//      SSD1306's address. Report each pair that answers.
//   2. For each pair that answered, blink the panel: force "entire display
//      on" against normal mode, three times, announced on serial.
//
// Stage 2 is the part that actually proves something. A probe ACK says some
// device acknowledged an address; only the panel visibly lighting up says we
// are talking to *the panel*. It forces "entire display on" (0xA5) against
// normal RAM-driven mode (0xA4) after enabling the charge pump, rather than
// relying on whatever was already on the glass — GDDRAM is volatile and does
// not survive the power cycle this board needs to leave download mode, so by
// the time this runs the panel is back at its power-on default with nothing
// to rely on.
//
// Pins carrying flash, PSRAM, USB or the boot straps are excluded by name below
// rather than by a range check, so it is obvious what was skipped and why:
// driving those would end the session mid-scan.

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2c_scan";

// The SSD1306 on this board family is strapped to 0x3C. 0x3D is the only other
// address the part can take, and it is scanned too so that a mis-strapped panel
// reads as "found at 0x3D" rather than as "no panel".
static const uint8_t k_addrs[] = {0x3C, 0x3D};

// ESP32-S3 exclusions, by name — all four of this chip's strapping pins
// (GPIO0/3/45/46), not just the three that can land it in download mode:
//   GPIO0        BOOT strap — driving it risks landing back in download mode.
//   GPIO3        JTAG-source strap. Lower stakes than 0/45/46 (it only
//                selects the JTAG signal source at boot, not boot/download
//                mode or flash voltage), but it is a strap, so it is excluded
//                on the same basis as the other three rather than case-by-case.
//   GPIO19/20    USB D-/D+ — this is the console we are reporting over.
//   GPIO26-32    SPI flash (CS/Q/WP/HD/CLK/D).
//   GPIO33-37    reserved for octal flash/PSRAM; this part reports 2MB embedded
//                PSRAM, so they are not free to drive.
//   GPIO45/46    VDD_SPI and boot straps.
//   GPIO22-25    do not exist on the S3.
// GPIO43/44 are UART0 TX/RX and *are* included: the console is on USB-Serial/
// JTAG here, so the UART pins are free.
static const int k_pins[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                             13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41,
                             42, 43, 44, 47, 48};

#define PIN_COUNT (sizeof(k_pins) / sizeof(k_pins[0]))
#define MAX_HITS 8

struct hit {
  int sda;
  int scl;
  uint8_t addr;
};

static struct hit s_hits[MAX_HITS];
static int s_hit_count;

// Opens a bus on the given pair, runs `fn`, and always tears the bus down.
// Returns false if the bus could not be created at all (an invalid pin, say),
// which is distinct from a bus that came up and found nothing.
//
// Called PIN_COUNT*(PIN_COUNT-1) times across the sweep — 702 with the
// current exclusion list — each doing a fresh create/destroy of the I2C
// driver. Observed clean on real S3 hardware (2026-08-31, before this file's
// exclusion list dropped GPIO3, so at the prior count of 756) — no leak, no
// wedge — but that is one board on one run, not an audit of the driver's
// create/destroy path under repeated rapid cycling.
static bool with_bus(int sda, int scl,
                     void (*fn)(i2c_master_bus_handle_t, void *), void *ctx) {
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = sda,
      .scl_io_num = scl,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      // The board is expected to have external pull-ups on the panel's lines;
      // the internal ones are enabled so that a pair with no external pull-ups
      // still idles high rather than reading as a permanently busy bus.
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus;
  if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
    return false;
  }
  if (fn) {
    fn(bus, ctx);
  }
  i2c_del_master_bus(bus);
  return true;
}

struct probe_ctx {
  int sda;
  int scl;
};

static void probe_all_addrs(i2c_master_bus_handle_t bus, void *raw) {
  struct probe_ctx *ctx = raw;
  for (size_t a = 0; a < sizeof(k_addrs) / sizeof(k_addrs[0]); a++) {
    if (i2c_master_probe(bus, k_addrs[a], 50) != ESP_OK) {
      continue;
    }
    ESP_LOGI(TAG, "  ACK  SDA=%d SCL=%d addr=0x%02X", ctx->sda, ctx->scl,
             k_addrs[a]);
    if (s_hit_count < MAX_HITS) {
      s_hits[s_hit_count].sda = ctx->sda;
      s_hits[s_hit_count].scl = ctx->scl;
      s_hits[s_hit_count].addr = k_addrs[a];
      s_hit_count++;
    } else {
      ESP_LOGW(TAG,
               "  ACK above — MAX_HITS (%d) already reached, this pair will "
               "not be blinked in stage 2",
               MAX_HITS);
    }
  }
}

static void blink_panel(i2c_master_bus_handle_t bus, void *raw) {
  struct hit *h = raw;
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = h->addr,
      .scl_speed_hz = 400000,
  };
  i2c_master_dev_handle_t dev;
  if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
    ESP_LOGW(TAG, "  could not address 0x%02X on SDA=%d SCL=%d", h->addr,
             h->sda, h->scl);
    return;
  }
  // 0x00 is the SSD1306's "the bytes that follow are commands" control byte.
  //
  // GDDRAM is volatile SRAM: it does not survive a power cycle, and getting
  // this board out of download mode requires one (AGENTS.md §6.1), so by the
  // time this tool runs the panel is back at its power-on default — display
  // off, RAM contents undefined. Toggling only 0xAE/0xAF against that state
  // can be invisible even on the correct pins: the OLED's drive voltage comes
  // from an internal charge pump that 0xAF alone does not enable, so "on"
  // does not guarantee anything lights. Enable the charge pump once, then
  // alternate "entire display on" (0xA5, lights every pixel regardless of
  // RAM) against normal RAM-driven mode (0xA4) — that is visible with no
  // dependency on GDDRAM contents or prior display state.
  const uint8_t charge_pump[] = {0x00, 0x8D, 0x14};
  const uint8_t display_on[] = {0x00, 0xAF};
  const uint8_t all_on[] = {0x00, 0xA5};
  const uint8_t ram_mode[] = {0x00, 0xA4};
  if (i2c_master_transmit(dev, charge_pump, sizeof(charge_pump), 100) != ESP_OK ||
      i2c_master_transmit(dev, display_on, sizeof(display_on), 100) != ESP_OK) {
    ESP_LOGW(TAG, "  write failed on SDA=%d SCL=%d — not the panel", h->sda,
             h->scl);
    i2c_master_bus_rm_device(dev);
    return;
  }
  for (int i = 0; i < 3; i++) {
    if (i2c_master_transmit(dev, all_on, sizeof(all_on), 100) != ESP_OK) {
      ESP_LOGW(TAG, "  write failed on SDA=%d SCL=%d — not the panel", h->sda,
               h->scl);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(400));
    i2c_master_transmit(dev, ram_mode, sizeof(ram_mode), 100);
    vTaskDelay(pdMS_TO_TICKS(400));
  }
  i2c_master_bus_rm_device(dev);
}

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(2000));  // let the serial monitor attach

  ESP_LOGI(TAG, "sweeping %d pins => %d ordered pairs", (int)PIN_COUNT,
           (int)(PIN_COUNT * (PIN_COUNT - 1)));

  for (size_t i = 0; i < PIN_COUNT; i++) {
    ESP_LOGI(TAG, "SDA=%d ...", k_pins[i]);
    for (size_t j = 0; j < PIN_COUNT; j++) {
      if (i == j) {
        continue;
      }
      struct probe_ctx ctx = {k_pins[i], k_pins[j]};
      with_bus(k_pins[i], k_pins[j], probe_all_addrs, &ctx);
    }
  }

  if (s_hit_count == 0) {
    ESP_LOGW(TAG,
             "no device answered on any pair. The panel is not on these pins, "
             "or not on I2C at all — do not conclude the pins from silence.");
    return;
  }

  ESP_LOGI(TAG, "%d pair(s) answered; blinking each one now — watch the glass",
           s_hit_count);
  for (int i = 0; i < s_hit_count; i++) {
    ESP_LOGI(TAG, "blinking SDA=%d SCL=%d addr=0x%02X", s_hits[i].sda,
             s_hits[i].scl, s_hits[i].addr);
    with_bus(s_hits[i].sda, s_hits[i].scl, blink_panel, &s_hits[i]);
    vTaskDelay(pdMS_TO_TICKS(1200));
  }
  ESP_LOGI(TAG, "done. The pair whose blink you saw is the panel's.");
}
