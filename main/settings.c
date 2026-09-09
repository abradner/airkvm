#include "settings.h"

#include <string.h>

#include "board.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "settings_schema.h"
#include "wifi.h"  // airkvm_nvs_init
#if BOARD_HAS_PALETTE
#include "theme.h"
#endif

static const char *TAG = "settings";

#define NVS_NAMESPACE "airkvm"
#define KEY_THEME "theme"

// One commit per second at most. The value is not tuned — it is the smallest
// round number that turns "an attacker can loop writes until the partition
// wears out" into "an attacker can wear it out in roughly three years of
// continuous effort at one write per second, on a partition NVS levels across
// 24KB". A change is applied to RAM before the write, so the operator never
// waits on flash; only the persistence is throttled.
#define MIN_WRITE_INTERVAL_US (1000 * 1000)

// All state below is written on the main task (settings_init) before the
// HTTP server exists, and thereafter only on the single esp_http_server task.
// No lock, and that single-task assumption is load-bearing: a second server
// serving /settings would need one.
#if BOARD_HAS_PALETTE
static const char *s_theme_names[DISPLAY_THEME_COUNT];
#endif
static struct settings_catalog s_cat;
static int s_theme;
static int64_t s_last_write_us;

static void catalog_build(void) {
#if BOARD_HAS_PALETTE
  // Not a const initialiser: C will not read a member of a const array at
  // compile time, and a parallel list of names would be a second copy of
  // theme.h's table that could disagree with it.
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    s_theme_names[i] = display_themes[i].name;
  }
  s_cat.themes = s_theme_names;
  s_cat.theme_count = DISPLAY_THEME_COUNT;
  s_cat.theme_default = DISPLAY_THEME_DEFAULT;
#else
  s_cat.themes = NULL;
  s_cat.theme_count = 0;
  s_cat.theme_default = 0;
#endif
}

static void apply_theme(int index) {
#if BOARD_HAS_PALETTE
  display_set_theme(&display_themes[index]);
#else
  (void)index;
#endif
}

// Reads the stored theme name into `out`; false when there is none, or it is
// unreadable, or the namespace has never been written. Every one of those is
// "use the default", so they are not distinguished beyond the log.
static bool read_theme(char *out, size_t cap) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;  // fresh flash: nothing was ever written
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs_open: %s — using defaults", esp_err_to_name(err));
    return false;
  }
  size_t len = cap;
  err = nvs_get_str(h, KEY_THEME, out, &len);
  nvs_close(h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  if (err != ESP_OK) {
    // Includes ESP_ERR_NVS_INVALID_LENGTH: a stored value longer than any
    // theme name is by definition not one, and is left where it is.
    ESP_LOGW(TAG, "stored %s unreadable (%s) — using default", KEY_THEME,
             esp_err_to_name(err));
    return false;
  }
  return true;
}

static esp_err_t write_theme(const char *name) {
  nvs_handle_t h;
  ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG,
                      "nvs_open");
  esp_err_t err = nvs_set_str(h, KEY_THEME, name);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err;
}

esp_err_t settings_init(void) {
  // Idempotent: wifi_start() and the setup AP both call this too, and
  // nvs_flash_init() on an initialised partition is ESP_OK.
  ESP_RETURN_ON_ERROR(airkvm_nvs_init(), TAG, "nvs init failed");
  catalog_build();

  char stored[SETTINGS_THEME_NAME_MAX + 1];
  const bool have = read_theme(stored, sizeof(stored));
  s_theme = settings_resolve_theme(&s_cat, have ? stored : NULL);
  if (s_cat.theme_count > 0) {
    if (have && settings_theme_index(&s_cat, stored) < 0) {
      // Left in place deliberately: a later image may know it, and rewriting
      // it here would be a flash write to destroy information.
      ESP_LOGW(TAG, "stored theme \"%s\" unknown to this image — using %s",
               stored, s_cat.themes[s_theme]);
    } else {
      ESP_LOGI(TAG, "theme: %s (%s)", s_cat.themes[s_theme],
               have ? "stored" : "default");
    }
    apply_theme(s_theme);
  }
  return ESP_OK;
}

size_t settings_document(char *buf, size_t cap) {
  uint32_t flash_bytes = 0;
  if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
    flash_bytes = 0;  // reported as 0 rather than guessed
  }
  const struct settings_view v = {
      .id =
          {
              .board = BOARD_NAME,
              .chip = CONFIG_IDF_TARGET,
              .flash_mb = flash_bytes / (1024u * 1024u),
              .fw = esp_app_get_description()->version,
              .idf = IDF_VER,
          },
      .feat =
          {
              .panel = BOARD_HAS_DISPLAY ? BOARD_PANEL_KIND : NULL,
              .panel_w = BOARD_PANEL_WIDTH,
              .panel_h = BOARD_PANEL_HEIGHT,
              .palette = BOARD_HAS_PALETTE,
              .backlight = BOARD_HAS_BACKLIGHT,
              .led = BOARD_LED_KIND,
              .sdcard = BOARD_HAS_SDCARD,
#if CONFIG_AIRKVM_HID_BACKEND_USB
              .usb_device = true,
#else
              .usb_device = false,
#endif
          },
      .cat = &s_cat,
      .theme = s_theme,
  };
  return settings_json(&v, buf, cap);
}

enum settings_apply_result settings_apply_form(const char *body, size_t len) {
  struct settings_update u;
  switch (settings_parse_form(&s_cat, body, len, &u)) {
    case SETTINGS_PARSE_OK:
      break;
    case SETTINGS_PARSE_NOTHING:
    case SETTINGS_PARSE_BAD:
    default:
      return SETTINGS_BAD_REQUEST;
  }
  if (u.theme == s_theme) {
    return SETTINGS_UNCHANGED;
  }
  const int64_t now = esp_timer_get_time();
  if (s_last_write_us != 0 && now - s_last_write_us < MIN_WRITE_INTERVAL_US) {
    return SETTINGS_TOO_SOON;
  }
  // RAM and panel first, flash second: the visible effect never waits on a
  // flash erase, and a failed write is reported rather than hidden.
  s_theme = u.theme;
  apply_theme(s_theme);
  s_last_write_us = now;
  const esp_err_t err = write_theme(s_cat.themes[s_theme]);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "storing theme failed: %s", esp_err_to_name(err));
    return SETTINGS_STORE_FAILED;
  }
  ESP_LOGI(TAG, "theme: %s (stored)", s_cat.themes[s_theme]);
  return SETTINGS_APPLIED;
}
