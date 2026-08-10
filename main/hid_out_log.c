// Bench backend: decodes and tracks HID state, but emits nothing to any target.
//
// This exists so the network, console, and protocol paths can be exercised on
// hardware that physically cannot be a USB keyboard — an ESP32-C3 on the desk,
// rather than waiting for an S3 in the post. It validates roughly everything
// except the transport: WiFi, mDNS, the HTTP server, the WebSocket framing, the
// decoder, the keymap, and the state machine all run for real.
//
// It is a bench instrument. It never drives a target, and selecting it is not a
// way to make a non-OTG board into a keyboard (AGENTS.md §4) — the whole point
// is that the last inch is missing and the log says so.

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_out.h"
#include "hid_state.h"
#include "keymap.h"

static const char *TAG = "hid_log";

static struct hid_state s_state;
static QueueHandle_t s_queue;
static volatile bool s_release_pending;

// Never mounted: there is no USB host, and claiming otherwise would put a lie
// on the console's status line.
bool hid_out_is_mounted(void) {
  return false;
}

void hid_out_release_all(void) {
  s_release_pending = true;
}

bool hid_out_submit(const struct airhid_intent *intent) {
  if (s_queue == NULL) {
    return false;
  }
  return xQueueSend(s_queue, intent, 0) == pdTRUE;
}

// Renders the keyboard report the USB backend would have sent, so the log shows
// the bytes a target would actually receive rather than a prettified summary.
static void log_keyboard_report(const char *why) {
  uint8_t report[AIRHID_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s_state, report);
  ESP_LOGI(TAG, "%-10s kbd [%02x %02x %02x %02x %02x %02x %02x %02x]", why,
           report[0], report[1], report[2], report[3], report[4], report[5],
           report[6], report[7]);
}

static void apply(const struct airhid_intent *in) {
  switch (in->type) {
    case AIRHID_FRAME_KEY_DOWN: {
      const uint8_t usage = keymap_lookup(in->code);
      if (usage == 0) {
        // Worth shouting about: this is how an unmapped key looks from the bench,
        // and it is the single most likely reason a keystroke "does nothing".
        ESP_LOGW(TAG, "down       %s -> UNMAPPED", in->code);
        return;
      }
      hid_state_key_down(&s_state, usage);
      ESP_LOGI(TAG, "down       %s -> usage 0x%02x", in->code, usage);
      log_keyboard_report("state");
      break;
    }
    case AIRHID_FRAME_KEY_UP: {
      const uint8_t usage = keymap_lookup(in->code);
      if (usage == 0) {
        return;
      }
      hid_state_key_up(&s_state, usage);
      ESP_LOGI(TAG, "up         %s -> usage 0x%02x", in->code, usage);
      log_keyboard_report("state");
      break;
    }
    case AIRHID_FRAME_MOUSE_MOVE:
      ESP_LOGI(TAG, "move       dx=%d dy=%d", in->move.dx, in->move.dy);
      break;
    case AIRHID_FRAME_MOUSE_BUTTON:
      hid_state_set_buttons(&s_state, in->button_mask);
      ESP_LOGI(TAG, "buttons    0x%02x", in->button_mask);
      break;
    case AIRHID_FRAME_MOUSE_SCROLL:
      ESP_LOGI(TAG, "scroll     v=%d h=%d", in->scroll.vertical,
               in->scroll.horizontal);
      break;
    case AIRHID_FRAME_RELEASE_ALL:
      s_release_pending = true;
      break;
    case AIRHID_FRAME_PING:
      ESP_LOGD(TAG, "ping");
      break;
  }
}

static void report_task(void *arg) {
  (void)arg;
  struct airhid_intent intent;
  for (;;) {
    if (s_release_pending) {
      s_release_pending = false;
      xQueueReset(s_queue);
      hid_state_release_all(&s_state);
      log_keyboard_report("RELEASE");
      continue;
    }
    if (xQueueReceive(s_queue, &intent, pdMS_TO_TICKS(20)) == pdTRUE) {
      apply(&intent);
    }
  }
}

esp_err_t hid_out_start(void) {
  s_queue = xQueueCreate(64, sizeof(struct airhid_intent));
  if (s_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }
  hid_state_reset(&s_state);
  if (xTaskCreate(report_task, "hid_log", 3072, NULL, 5, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGW(TAG,
           "bench backend: no USB HID. Intents are logged, not sent anywhere.");
  return ESP_OK;
}
