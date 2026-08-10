#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "indicator.h"
#include "status.h"

static const char *TAG = "ui";

// The LED breathes, so it needs to be updated far more often than the screen
// needs redrawing. One task, two rates.
#define TICK_MS 30
#define DISPLAY_EVERY_TICKS (250 / TICK_MS)
#define RSSI_EVERY_TICKS (2000 / TICK_MS)

// Long enough to reject contact bounce, short enough that a deliberate press
// always registers.
#define DEBOUNCE_MS 40

// Signal bars from RSSI. Thresholds are the conventional WiFi ones; the point
// is relative ("is this the reason it feels slow"), not absolute accuracy.
static int rssi_bars(int8_t rssi) {
  if (rssi == 0) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -66) return 3;
  if (rssi >= -77) return 2;
  return 1;
}

static void draw(const struct status_snapshot *s) {
  display_clear();

  // Row 0: identity and who holds the keyboard. The client state is the thing
  // you check when the console feels unresponsive, so it goes top-right.
  display_text(0, 0, "AIRKVM");
  display_text(11, 0, s->client_connected ? "BOUND" : "IDLE");

  // Row 1: the IP, the whole reason this screen exists — you read it off the
  // board and type it into a browser.
  display_text(0, 1, s->wifi_connected ? s->ip : "NO WIFI");

  // Row 2: SSID, truncated by the clip in display_text, with signal bars right.
  if (s->wifi_connected) {
    display_text(0, 2, s->ssid);
    display_bar(14, 2, 4, rssi_bars(s->rssi));
  }

  // Row 3: whether a target is actually listening. On the bench board this
  // always reads NO HID, which is the honest answer for a board that cannot be
  // a keyboard rather than a fault to hide.
  display_text(0, 3, s->usb_mounted ? "USB: TARGET UP" : "USB: NO HID");

  // Row 4: event counter — proof that input is arriving even when the target
  // appears to do nothing, which is exactly the "is it me or the machine?"
  // question this device gets used to answer.
  char line[DISPLAY_COLS + 1];
  snprintf(line, sizeof(line), "EVENTS %lu", (unsigned long)s->events);
  display_text(0, 4, line);

  // Row 5: debug. Off by default and only reachable by physically pressing the
  // button on the device (R-006) — this row shows what is being typed, so it
  // must never be enable-able from the network.
  if (s->debug) {
    display_text(0, 5, s->last_event[0] ? s->last_event : "DEBUG ON");
  }

  display_flush();
}

static void poll_button(void) {
#if defined(BOARD_BUTTON)
  static bool was_pressed;
  static int64_t last_change_us;

  const int level = gpio_get_level(BOARD_BUTTON);
#if BOARD_BUTTON_ACTIVE_LOW
  const bool pressed = level == 0;
#else
  const bool pressed = level != 0;
#endif

  const int64_t now = esp_timer_get_time();
  if (pressed == was_pressed) {
    return;
  }
  if ((now - last_change_us) / 1000 < DEBOUNCE_MS) {
    return;
  }
  last_change_us = now;
  was_pressed = pressed;

  // Act on release, not press: it makes an accidental brush less likely to
  // toggle a mode that puts keystrokes on a screen.
  if (!pressed) {
    const bool on = status_toggle_debug();
    ESP_LOGW(TAG, "debug display %s (physical button)", on ? "ON" : "OFF");
  }
#endif
}

static void ui_task(void *arg) {
  (void)arg;
  int tick = 0;
  struct status_snapshot snapshot;

  for (;;) {
    poll_button();
    indicator_tick();

    if (tick % RSSI_EVERY_TICKS == 0) {
      wifi_ap_record_t ap;
      if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        status_set_rssi((int8_t)ap.rssi);
      }
    }

    if (tick % DISPLAY_EVERY_TICKS == 0 && display_present()) {
      status_get(&snapshot);
      draw(&snapshot);
    }

    tick++;
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
  }
}

void ui_start(void) {
#if defined(BOARD_BUTTON)
  const gpio_config_t btn = {
      .pin_bit_mask = 1ULL << BOARD_BUTTON,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en =
          BOARD_BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&btn);
#endif

  indicator_init();
  display_init();  // failure is logged and non-fatal; the device runs headless

  if (xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL) != pdPASS) {
    ESP_LOGW(TAG, "could not start UI task; running without front panel");
  }
}
