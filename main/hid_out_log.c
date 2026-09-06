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
#include "status.h"

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

// There is no bus to wake. Answering NO_HOST — the same answer the USB backend
// gives before enumeration — lets the bench prove the whole request/reply path
// short of the last inch (AGENTS.md §9, entry 8).
enum airkvm_wake_result hid_out_wake_target(void) {
  ESP_LOGI(TAG, "wake       bench backend has no USB host");
  return AIRKVM_WAKE_NO_HOST;
}

bool hid_out_submit(const struct airkvm_intent *intent) {
  if (s_queue == NULL) {
    return false;
  }
  return xQueueSend(s_queue, intent, 0) == pdTRUE;
}

// Renders the keyboard report the USB backend would have sent, so the log shows
// the bytes a target would actually receive rather than a prettified summary.
static void log_keyboard_report(const char *why) {
  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s_state, report);
  ESP_LOGI(TAG, "%-10s kbd [%02x %02x %02x %02x %02x %02x %02x %02x]", why,
           report[0], report[1], report[2], report[3], report[4], report[5],
           report[6], report[7]);
}

static void apply(const struct airkvm_intent *in) {
  switch (in->type) {
    case AIRKVM_FRAME_KEY_DOWN: {
      const uint8_t usage = keymap_lookup(in->code);
      if (usage == 0) {
        // Worth shouting about: this is how an unmapped key looks from the bench,
        // and it is the single most likely reason a keystroke "does nothing".
        ESP_LOGW(TAG, "down       %s -> UNMAPPED", in->code);
        return;
      }
      hid_state_key_down(&s_state, usage);
      status_note_event(in->code);
      ESP_LOGI(TAG, "down       %s -> usage 0x%02x", in->code, usage);
      log_keyboard_report("state");
      break;
    }
    case AIRKVM_FRAME_KEY_UP: {
      const uint8_t usage = keymap_lookup(in->code);
      if (usage == 0) {
        return;
      }
      hid_state_key_up(&s_state, usage);
      status_note_event(in->code);
      ESP_LOGI(TAG, "up         %s -> usage 0x%02x", in->code, usage);
      log_keyboard_report("state");
      break;
    }
    case AIRKVM_FRAME_MOUSE_MOVE:
      status_note_event("MOUSE");
      ESP_LOGI(TAG, "move       dx=%d dy=%d", in->move.dx, in->move.dy);
      break;
    case AIRKVM_FRAME_MOUSE_BUTTON:
      hid_state_set_buttons(&s_state, in->button_mask);
      ESP_LOGI(TAG, "buttons    0x%02x", in->button_mask);
      break;
    case AIRKVM_FRAME_MOUSE_SCROLL:
      ESP_LOGI(TAG, "scroll     v=%d h=%d", in->scroll.vertical,
               in->scroll.horizontal);
      break;
    case AIRKVM_FRAME_RELEASE_ALL:
      s_release_pending = true;
      break;
    case AIRKVM_FRAME_PING:
      ESP_LOGD(TAG, "ping");
      break;
    case AIRKVM_FRAME_WAKE:
      // net.c answers WAKE directly and never queues it; this arm exists so the
      // switch stays total.
      ESP_LOGI(TAG, "wake       (queued — unexpected, see net.c)");
      break;
  }
}

static void report_task(void *arg) {
  (void)arg;
  struct airkvm_intent intent;
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
  s_queue = xQueueCreate(64, sizeof(struct airkvm_intent));
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
