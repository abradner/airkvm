// UNVERIFIED — this file compiles and links for esp32s3, and nothing more.
// Nothing here has enumerated on a real USB host: descriptor layout, endpoint
// numbering, boot-protocol acceptance, and the report pacing below are all
// hypotheses until an S3 arrives (AGENTS.md §4, last rule). The rest of the
// firmware has been exercised on a C3 bench board, which by definition cannot
// test this file — do not let the project's general "it works" leak onto it.

#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_out.h"
#include "hid_state.h"
#include "keymap.h"
#include "status.h"
#include "tinyusb.h"

static const char *TAG = "usb_hid";

// Two separate HID interfaces rather than one interface with report IDs.
// Report IDs are the compact option, but a BIOS speaking boot protocol expects
// an 8-byte report with no report-ID prefix — sharing one interface is a known
// way to lose exactly the BIOS support this project exists for (README's
// central claim). The cost is one extra interface and endpoint.
enum {
  HID_ITF_KEYBOARD = 0,
  HID_ITF_MOUSE = 1,
  HID_ITF_COUNT,
};

#define EPNUM_HID_KEYBOARD 0x81
#define EPNUM_HID_MOUSE 0x82

static const uint8_t s_kbd_report_desc[] = {TUD_HID_REPORT_DESC_KEYBOARD()};
static const uint8_t s_mouse_report_desc[] = {TUD_HID_REPORT_DESC_MOUSE()};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static const uint8_t s_configuration_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, HID_ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, 100),
    // Boot protocol on the keyboard interface is what makes this work before an
    // OS loads its own HID driver.
    TUD_HID_DESCRIPTOR(HID_ITF_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_kbd_report_desc), EPNUM_HID_KEYBOARD,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_HID_DESCRIPTOR(HID_ITF_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(s_mouse_report_desc), EPNUM_HID_MOUSE,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    // Espressif's vendor ID with their generic HID product ID. Do not invent a
    // VID: unallocated VIDs collide, and a colliding VID/PID pair can make a
    // target load the wrong driver for a device it thinks it has seen before.
    .idVendor = 0x303A,
    .idProduct = 0x4004,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04},  // English (US)
    "airkvm",
    "airkvm keyboard+mouse",
    "000001",
    "airkvm keyboard",
    "airkvm mouse",
};

// --- state ------------------------------------------------------------------

static struct hid_state s_state;
static QueueHandle_t s_queue;
static volatile bool s_mounted;

// Set by any path that loses confidence in session state; the report task
// services it ahead of the queue.
static volatile bool s_release_pending;

bool hid_out_is_mounted(void) {
  return s_mounted;
}

void tud_mount_cb(void) {
  ESP_LOGI(TAG, "USB host enumerated the device");
  s_mounted = true;
  status_set_usb_mounted(true);
}

void tud_umount_cb(void) {
  ESP_LOGI(TAG, "USB host disconnected");
  s_mounted = false;
  status_set_usb_mounted(false);
  // Unplugged mid-session: drop everything so a replug does not resume a state
  // the console has since forgotten about.
  s_release_pending = true;
}

void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
  s_release_pending = true;
}

void tud_resume_cb(void) {}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
  return instance == HID_ITF_MOUSE ? s_mouse_report_desc : s_kbd_report_desc;
}

// Required by TinyUSB. airkvm never reads from the target: a GET_REPORT reply
// of zero length is the honest answer, and inventing report contents here would
// be data the target did not ask us to make up.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}

// The target sets keyboard LEDs (caps/num lock) through here. Ignored for now;
// surfacing lock state in the console is worth doing later, since a target
// whose caps lock disagrees with the operator's is a confusing failure.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
}

// --- report task ------------------------------------------------------------

// An interrupt endpoint accepts at most one report per bInterval (10 ms here),
// so intents must never be turned into reports one-for-one as they arrive: a
// six-frame combo lands in a single TCP segment, is applied in microseconds,
// and everything after the first report would be thrown away by
// tud_hid_n_ready() — Ctrl-down delivered, Ctrl-up discarded, modifier held on
// someone's machine. Instead the task accumulates and emits at endpoint pace.
//
// The two report types accumulate differently:
//   mouse     transient deltas, so dropping one loses distance — sum them.
//   keyboard  a *state*, not an event log — and that is exactly why it cannot
//             be coalesced the way this comment used to claim. It said "the
//             newest state supersedes any older one", which is true of the
//             bitmap and false of the keypress: apply a key-down and its
//             key-up in the same pass and the bitmap ends where it started,
//             so the single report that follows says "nothing pressed" and
//             the keystroke is lost outright. Over TCP a down/up pair almost
//             always arrives together, so this dropped ~75% of typed
//             characters on real hardware while the frames themselves all
//             arrived (AGENTS.md §9, entry 11). The drain below therefore
//             stops at a keyboard intent whenever a report is still pending,
//             which costs one endpoint interval per state change and loses
//             nothing.
static bool s_kbd_dirty;
static int32_t s_pending_dx, s_pending_dy;
static int32_t s_pending_scroll_v, s_pending_scroll_h;
static bool s_buttons_dirty;

static int8_t take_delta(int32_t *pending) {
  const int32_t v = *pending;
  const int8_t out = v > 127 ? 127 : (v < -127 ? -127 : (int8_t)v);
  *pending -= out;  // carry the remainder so a fast flick still travels
  return out;
}

static void pump_keyboard(void) {
  if (!s_kbd_dirty || !tud_hid_n_ready(HID_ITF_KEYBOARD)) {
    return;
  }
  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s_state, report);
  // report_id 0: no report ID in the descriptor, which is what boot protocol
  // requires (see the two-interface note above).
  if (tud_hid_n_report(HID_ITF_KEYBOARD, 0, report, sizeof(report))) {
    s_kbd_dirty = false;
  }
}

static void pump_mouse(void) {
  const bool has_motion =
      s_pending_dx || s_pending_dy || s_pending_scroll_v || s_pending_scroll_h;
  if ((!has_motion && !s_buttons_dirty) || !tud_hid_n_ready(HID_ITF_MOUSE)) {
    return;
  }
  const int8_t dx = take_delta(&s_pending_dx);
  const int8_t dy = take_delta(&s_pending_dy);
  const int8_t sv = take_delta(&s_pending_scroll_v);
  const int8_t sh = take_delta(&s_pending_scroll_h);
  uint8_t report[AIRKVM_MOUSE_REPORT_LEN];
  hid_state_mouse_report(&s_state, dx, dy, sv, sh, report);
  if (tud_hid_n_report(HID_ITF_MOUSE, 0, report, sizeof(report))) {
    s_buttons_dirty = false;
  }
}

static void apply(const struct airkvm_intent *in) {
  switch (in->type) {
    case AIRKVM_FRAME_KEY_DOWN:
      hid_state_key_down(&s_state, keymap_lookup(in->code));
      status_note_event(in->code);
      s_kbd_dirty = true;
      break;
    case AIRKVM_FRAME_KEY_UP:
      hid_state_key_up(&s_state, keymap_lookup(in->code));
      status_note_event(in->code);
      s_kbd_dirty = true;
      break;
    case AIRKVM_FRAME_MOUSE_MOVE:
      status_note_event("MOUSE");
      s_pending_dx += in->move.dx;
      s_pending_dy += in->move.dy;
      break;
    case AIRKVM_FRAME_MOUSE_BUTTON:
      hid_state_set_buttons(&s_state, in->button_mask);
      s_buttons_dirty = true;
      break;
    case AIRKVM_FRAME_MOUSE_SCROLL:
      s_pending_scroll_v += in->scroll.vertical;
      s_pending_scroll_h += in->scroll.horizontal;
      break;
    case AIRKVM_FRAME_RELEASE_ALL:
      s_release_pending = true;
      break;
    case AIRKVM_FRAME_PING:
      break;
  }
}

static void report_task(void *arg) {
  (void)arg;
  struct airkvm_intent intent;
  for (;;) {
    if (s_release_pending) {
      s_release_pending = false;
      // Drain first: queued intents predate the release and would re-press keys
      // the release was meant to clear.
      xQueueReset(s_queue);
      hid_state_release_all(&s_state);
      // Pending motion dies with the release; only the state report matters.
      s_pending_dx = s_pending_dy = 0;
      s_pending_scroll_v = s_pending_scroll_h = 0;
      s_kbd_dirty = true;
      s_buttons_dirty = true;
    }

    // Apply everything already queued before emitting, so a burst collapses
    // into the fewest reports that still convey it faithfully — with one
    // exception that is load-bearing: a keyboard intent is never applied on
    // top of a keyboard state whose report has not gone out yet. Doing so
    // silently destroys the keypress (see the note above pump_keyboard).
    // Peek before receiving, so the intent we decline to apply stays queued
    // in order for the next pass.
    while (xQueuePeek(s_queue, &intent, 0) == pdTRUE) {
      const bool is_keyboard = intent.type == AIRKVM_FRAME_KEY_DOWN ||
                               intent.type == AIRKVM_FRAME_KEY_UP;
      if (is_keyboard && s_kbd_dirty) {
        break;  // emit the pending state first; this intent waits its turn
      }
      (void)xQueueReceive(s_queue, &intent, 0);
      apply(&intent);
      if (s_release_pending) {
        break;  // handled at the top of the next iteration, ahead of the queue
      }
    }

    pump_keyboard();
    pump_mouse();

    // Short sleep rather than blocking on the queue: there may be nothing new
    // to receive and still something pending to send, either because the
    // endpoint was busy last time round or because a delta exceeded one report.
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// --- lifecycle --------------------------------------------------------------

bool hid_out_submit(const struct airkvm_intent *intent) {
  if (s_queue == NULL) {
    return false;
  }
  // Never block: this runs on the HTTP task, and a stalled network task holding
  // a key down is the failure this design exists to prevent.
  return xQueueSend(s_queue, intent, 0) == pdTRUE;
}

void hid_out_release_all(void) {
  s_release_pending = true;
}

esp_err_t hid_out_start(void) {
  s_queue = xQueueCreate(64, sizeof(struct airkvm_intent));
  if (s_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }
  hid_state_reset(&s_state);

  const tinyusb_config_t cfg = {
      .device_descriptor = &s_device_desc,
      .string_descriptor = s_string_desc,
      .string_descriptor_count =
          sizeof(s_string_desc) / sizeof(s_string_desc[0]),
      .external_phy = false,
      .configuration_descriptor = s_configuration_desc,
  };
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG,
                      "tinyusb_driver_install failed");

  if (xTaskCreate(report_task, "hid_report", 3072, NULL, 5, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "USB HID started (keyboard itf %d, mouse itf %d)",
           HID_ITF_KEYBOARD, HID_ITF_MOUSE);
  return ESP_OK;
}
