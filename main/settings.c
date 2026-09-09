#include "settings.h"

#include <string.h>

#include "board.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "indicator.h"
#include "nvs.h"
#include "settings_schema.h"
#include "status.h"
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
// serving /settings would need one. s_range[SCREEN] is the exception — read
// by ui_task every tick — and an aligned int store is atomic on every target
// here, so a reader sees the old value or the new one, never a mix.
#if BOARD_HAS_PALETTE
static const char *s_theme_names[DISPLAY_THEME_COUNT];
#endif
static struct settings_catalog s_cat;
static int s_theme;
static int s_range[SETTINGS_RANGE_COUNT];
static int s_choice[SETTINGS_CHOICE_COUNT];          // stored
static int s_choice_running[SETTINGS_CHOICE_COUNT];  // in effect since boot
static int64_t s_last_write_us;
static esp_timer_handle_t s_restart_timer;

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
  // Screen brightness exists wherever there is a panel: a backlight on the
  // LCD, the contrast register on the OLED (display.h). LED brightness needs
  // a LED.
  settings_ranges_for_board(s_cat.ranges, BOARD_HAS_DISPLAY, BOARD_HAS_LED);
#if CONFIG_AIRKVM_HID_BACKEND_USB
  settings_choices_for_board(s_cat.choices, true);
#else
  settings_choices_for_board(s_cat.choices, false);
#endif
}

static void apply_theme(int index) {
#if BOARD_HAS_PALETTE
  display_set_theme(&display_themes[index]);
#else
  (void)index;
#endif
}

// Pushes one range setting to its consumer. Screen brightness has no push:
// ui_task pulls it (settings_screen_brightness), because the setup-mode
// override lives there.
static void apply_range(enum settings_range_id id, int value) {
  switch (id) {
    case SETTINGS_RANGE_LED:
      indicator_set_brightness(value);
      break;
    case SETTINGS_RANGE_DEBUG_TIMEOUT:
      status_set_debug_timeout_ms((uint32_t)value * 1000u);
      break;
    case SETTINGS_RANGE_SCREEN:
    default:
      break;
  }
}

// Reads the stored theme name into `out`; false when there is none, or it is
// unreadable, or the namespace has never been written. Every one of those is
// "use the default", so they are not distinguished beyond the log.
static bool read_theme(nvs_handle_t h, char *out, size_t cap) {
  size_t len = cap;
  const esp_err_t err = nvs_get_str(h, KEY_THEME, out, &len);
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

// A stored choice value, or false when absent/unreadable (both "default").
static bool read_choice(nvs_handle_t h, const char *key, char *out,
                        size_t cap) {
  size_t len = cap;
  const esp_err_t err = nvs_get_str(h, key, out, &len);
  if (err == ESP_OK) {
    return true;
  }
  if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "stored %s unreadable (%s) — using default", key,
             esp_err_to_name(err));
  }
  return false;
}

static bool read_u16(nvs_handle_t h, const char *key, int *out) {
  uint16_t v;
  const esp_err_t err = nvs_get_u16(h, key, &v);
  if (err == ESP_OK) {
    *out = v;
    return true;
  }
  if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "stored %s unreadable (%s) — using default", key,
             esp_err_to_name(err));
  }
  return false;
}

esp_err_t settings_init(void) {
  // Idempotent: wifi_start() and the setup AP both call this too, and
  // nvs_flash_init() on an initialised partition is ESP_OK.
  ESP_RETURN_ON_ERROR(airkvm_nvs_init(), TAG, "nvs init failed");
  catalog_build();

  nvs_handle_t h = 0;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  const bool opened = err == ESP_OK;
  if (!opened && err != ESP_ERR_NVS_NOT_FOUND) {
    // NOT_FOUND is a fresh flash and says nothing; anything else is worth a
    // line, and either way every setting resolves to its default below.
    ESP_LOGW(TAG, "nvs_open: %s — using defaults", esp_err_to_name(err));
  }

  char stored[SETTINGS_THEME_NAME_MAX + 1];
  const bool have_theme = opened && read_theme(h, stored, sizeof(stored));
  s_theme = settings_resolve_theme(&s_cat, have_theme ? stored : NULL);
  if (s_cat.theme_count > 0) {
    if (have_theme && settings_theme_index(&s_cat, stored) < 0) {
      // Left in place deliberately: a later image may know it, and rewriting
      // it here would be a flash write to destroy information.
      ESP_LOGW(TAG, "stored theme \"%s\" unknown to this image — using %s",
               stored, s_cat.themes[s_theme]);
    } else {
      ESP_LOGI(TAG, "theme: %s (%s)", s_cat.themes[s_theme],
               have_theme ? "stored" : "default");
    }
    apply_theme(s_theme);
  }

  for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
    const struct settings_range *r = &s_cat.ranges[i];
    int v = 0;
    const bool have = opened && r->present && read_u16(h, r->key, &v);
    s_range[i] =
        settings_resolve_range(&s_cat, (enum settings_range_id)i, v, have);
    if (!r->present) {
      continue;
    }
    if (have && s_range[i] != v) {
      ESP_LOGW(TAG, "stored %s=%d outside %d..%d — using %d (left in place)",
               r->key, v, r->min, r->max, s_range[i]);
    } else {
      ESP_LOGI(TAG, "%s: %d%s (%s)", r->key, s_range[i], r->unit,
               have ? "stored" : "default");
    }
    apply_range((enum settings_range_id)i, s_range[i]);
  }
  for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
    const struct settings_choice *ch = &s_cat.choices[i];
    char name[SETTINGS_THEME_NAME_MAX + 1];
    const bool have =
        opened && ch->present && read_choice(h, ch->key, name, sizeof(name));
    s_choice[i] = settings_resolve_choice(&s_cat, (enum settings_choice_id)i,
                                          have ? name : NULL);
    // What runs is what was stored at boot, resolved. The stored value may
    // move later; this one does not until the next boot.
    s_choice_running[i] = s_choice[i];
    if (!ch->present) {
      continue;
    }
    if (have &&
        settings_choice_index(&s_cat, (enum settings_choice_id)i, name) < 0) {
      ESP_LOGW(TAG, "stored %s=\"%s\" unknown to this image — using %s",
               ch->key, name, ch->values[s_choice[i]]);
    } else {
      ESP_LOGI(TAG, "%s: %s (%s)", ch->key, ch->values[s_choice[i]],
               have ? "stored" : "default");
    }
  }
  if (opened) {
    nvs_close(h);
  }
  return ESP_OK;
}

bool settings_usb_disk_enabled(void) {
  return s_choice_running[SETTINGS_CHOICE_USB] == SETTINGS_USB_WITH_DISK;
}

static void restart_now(void *arg) {
  (void)arg;
  ESP_LOGW(TAG, "restarting to apply the stored USB composition");
  esp_restart();
}

// A restart is scheduled rather than done: the reply carrying the document
// has to reach the browser first, and this runs on the HTTP task that would
// send it. 750 ms is the same "one more beat so the reply is on the wire"
// that setup_mode.c uses before it restarts.
static esp_err_t schedule_restart(void) {
  if (s_restart_timer == NULL) {
    const esp_timer_create_args_t args = {.callback = restart_now,
                                          .name = "settings_restart"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_restart_timer), TAG,
                        "restart timer");
  }
  return esp_timer_start_once(s_restart_timer, 750 * 1000);
}

static bool restart_pending(void) {
  for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
    if (s_cat.choices[i].present && s_cat.choices[i].apply != NULL &&
        s_choice[i] != s_choice_running[i]) {
      return true;
    }
  }
  return false;
}

int settings_screen_brightness(void) {
  return s_range[SETTINGS_RANGE_SCREEN];
}

size_t settings_document(char *buf, size_t cap) {
  uint32_t flash_bytes = 0;
  if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
    flash_bytes = 0;  // reported as 0 rather than guessed
  }
  struct settings_view v = {
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
              .usb_disk = settings_usb_disk_enabled(),
#else
              .usb_device = false,
              .usb_disk = false,
#endif
          },
      .cat = &s_cat,
      .theme = s_theme,
  };
  memcpy(v.range_value, s_range, sizeof(v.range_value));
  memcpy(v.choice_value, s_choice, sizeof(v.choice_value));
  memcpy(v.choice_running, s_choice_running, sizeof(v.choice_running));
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

  const bool theme_changed = u.theme >= 0 && u.theme != s_theme;
  bool range_changed[SETTINGS_RANGE_COUNT];
  bool choice_changed[SETTINGS_CHOICE_COUNT];
  bool any = theme_changed;
  for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
    range_changed[i] = u.range[i] >= 0 && u.range[i] != s_range[i];
    any = any || range_changed[i];
  }
  for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
    choice_changed[i] = u.choice[i] >= 0 && u.choice[i] != s_choice[i];
    any = any || choice_changed[i];
  }
  if (!any) {
    if (u.restart) {
      // A bare restart: honoured only when a reboot-scoped change is waiting,
      // so it cannot be used to knock the keyboard off the target for sport.
      // (Setting the composition first and then restarting is still open to
      // the same caller — R-001 — and is at least a state change.)
      if (!restart_pending()) {
        return SETTINGS_BAD_REQUEST;
      }
      return schedule_restart() == ESP_OK ? SETTINGS_RESTARTING
                                          : SETTINGS_STORE_FAILED;
    }
    return SETTINGS_UNCHANGED;
  }
  const int64_t now = esp_timer_get_time();
  if (s_last_write_us != 0 && now - s_last_write_us < MIN_WRITE_INTERVAL_US) {
    return SETTINGS_TOO_SOON;
  }

  // RAM and consumers first, flash second: the visible effect never waits on
  // a flash erase, and a failed write is reported rather than hidden.
  if (theme_changed) {
    s_theme = u.theme;
    apply_theme(s_theme);
  }
  for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
    if (range_changed[i]) {
      s_range[i] = u.range[i];
      apply_range((enum settings_range_id)i, s_range[i]);
    }
  }
  for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
    if (choice_changed[i]) {
      s_choice[i] = u.choice[i];  // reboot-scoped: nothing to apply now
    }
  }
  s_last_write_us = now;

  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err == ESP_OK) {
    if (theme_changed) {
      err = nvs_set_str(h, KEY_THEME, s_cat.themes[s_theme]);
    }
    for (int i = 0; err == ESP_OK && i < SETTINGS_RANGE_COUNT; i++) {
      if (range_changed[i]) {
        err = nvs_set_u16(h, s_cat.ranges[i].key, (uint16_t)s_range[i]);
      }
    }
    for (int i = 0; err == ESP_OK && i < SETTINGS_CHOICE_COUNT; i++) {
      if (choice_changed[i]) {
        err = nvs_set_str(h, s_cat.choices[i].key,
                          s_cat.choices[i].values[s_choice[i]]);
      }
    }
    if (err == ESP_OK) {
      err = nvs_commit(h);
    }
    nvs_close(h);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "storing settings failed: %s", esp_err_to_name(err));
    return SETTINGS_STORE_FAILED;
  }
  if (theme_changed) {
    ESP_LOGI(TAG, "theme: %s (stored)", s_cat.themes[s_theme]);
  }
  for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
    if (range_changed[i]) {
      ESP_LOGI(TAG, "%s: %d%s (stored)", s_cat.ranges[i].key, s_range[i],
               s_cat.ranges[i].unit);
    }
  }
  for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
    if (choice_changed[i]) {
      ESP_LOGI(TAG, "%s: %s (stored; takes effect at the next boot)",
               s_cat.choices[i].key, s_cat.choices[i].values[s_choice[i]]);
    }
  }
  if (u.restart && restart_pending()) {
    return schedule_restart() == ESP_OK ? SETTINGS_RESTARTING
                                        : SETTINGS_STORE_FAILED;
  }
  return SETTINGS_APPLIED;
}
