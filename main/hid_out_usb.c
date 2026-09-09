// PARTLY VERIFIED — read the split carefully before trusting anything here.
//
// Verified on hardware 2026-09-04 (ESP32-S3-0.42 OLED into a macOS host):
// the device enumerates, the descriptor layout and endpoint numbering below
// are accepted, and keyboard reports are delivered and typed correctly —
// 17 of 17 characters, in order, with the device's own event counter
// advancing by exactly the number of intents sent. The report pacing in this
// file was not merely unverified but wrong, and was corrected by that test
// (AGENTS.md §9, entry 11).
//
// Still hypotheses, and not to be treated otherwise (AGENTS.md §4, last
// rule): **boot-protocol acceptance by a BIOS/UEFI**, which is the claim the
// whole project rests on and which no OS-booted host can test; behaviour on
// any host other than macOS; and the mouse path, which has had no hardware
// exercise at all — only the keyboard has.
//
// The descriptor has changed since that verification: the configuration now
// advertises remote wakeup (bmAttributes 0xA0 rather than 0x80). Enumeration
// with that bit set, whether a host arms it, whether the wakeup pulse resumes
// a sleeping target, and whether advertising it makes hosts selectively
// suspend an idle dongle (which wake-on-input in the report task exists to
// survive) are all unverified until re-run on hardware.
//
// The C3 bench board cannot test any of this: its backend logs intents
// instead of sending reports, so it cannot even lose one. Do not let either
// the project's general "it works" or this file's new partial verification
// leak onto the lines above that are still guesses.

#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_out.h"
#include "hid_state.h"
#include "keymap.h"
#if CONFIG_TINYUSB_MSC_ENABLED
#include "msc_out.h"
#endif
#include "settings.h"
#include "status.h"
#include "tinyusb.h"
#include "wake_policy.h"

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
#if CONFIG_TINYUSB_MSC_ENABLED
  // Mass storage comes *after* both HID interfaces: interface 0 must stay the
  // boot keyboard, because that is what a BIOS looks at (AGENTS.md §4).
  ITF_MSC = HID_ITF_COUNT,
#endif
};

#define EPNUM_HID_KEYBOARD 0x81
#define EPNUM_HID_MOUSE 0x82
// Endpoint budget: the S3's DWC2 core has 4 usable IN endpoints. HID takes
// two, MSC one (plus one OUT); one IN is left, which is why HID + MSC + a
// network function can never coexist on this part.
#define EPNUM_MSC_OUT 0x03
#define EPNUM_MSC_IN 0x83

static const uint8_t s_kbd_report_desc[] = {TUD_HID_REPORT_DESC_KEYBOARD()};
static const uint8_t s_mouse_report_desc[] = {TUD_HID_REPORT_DESC_MOUSE()};

// Two compositions, both built, one chosen at start from the stored USB
// setting (settings.c, reboot-scoped): keyboard+mouse+disk and keyboard+mouse.
// Each is a whole static descriptor rather than one assembled at runtime,
// because a descriptor is a claim the host caches per VID/PID (below) and the
// two therefore also differ in product ID. Keyboard+mouse alone is the exact
// two-interface device that was verified on hardware first; the full one is
// what has shipped since MSC landed.
#define ITF_COUNT_HID HID_ITF_COUNT
#define CONFIG_TOTAL_LEN_HID (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)
#if CONFIG_TINYUSB_MSC_ENABLED
#define ITF_COUNT_FULL (HID_ITF_COUNT + 1)
#define CONFIG_TOTAL_LEN_FULL \
  (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)
#endif

// The two HID interfaces, identical in both compositions. Boot protocol on
// the keyboard interface is what makes this work before an OS loads its own
// HID driver.
#define HID_INTERFACE_DESCRIPTORS                                      \
  TUD_HID_DESCRIPTOR(HID_ITF_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,   \
                     sizeof(s_kbd_report_desc), EPNUM_HID_KEYBOARD,    \
                     CFG_TUD_HID_EP_BUFSIZE, 10),                      \
      TUD_HID_DESCRIPTOR(HID_ITF_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE,     \
                         sizeof(s_mouse_report_desc), EPNUM_HID_MOUSE, \
                         CFG_TUD_HID_EP_BUFSIZE, 10)

// Remote wakeup advertised in both: it is what lets a "Wake target" from the
// console resume a sleeping machine, and it is what a physical keyboard
// does. The host still has to arm it (SET_FEATURE DEVICE_REMOTE_WAKEUP)
// before suspending; tud_remote_wakeup() refuses otherwise. 100 mA is
// knowingly wrong (WiFi TX peaks well above it) and knowingly left alone:
// raising it is its own enumeration test, not something to bundle with
// a second descriptor change (issue #32).
static const uint8_t s_configuration_desc_hid[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT_HID, 0, CONFIG_TOTAL_LEN_HID,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    HID_INTERFACE_DESCRIPTORS,
};

#if CONFIG_TINYUSB_MSC_ENABLED
static const uint8_t s_configuration_desc_full[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT_FULL, 0, CONFIG_TOTAL_LEN_FULL,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    HID_INTERFACE_DESCRIPTORS,
    // Present for the whole boot, even with no medium: a static descriptor
    // means the target's USB topology never changes under it. Attaching
    // media later is a removable-disk "medium inserted", not a
    // re-enumeration — which would drop the keyboard mid-session and force
    // a release-all. 64 is the full-speed bulk packet size. UNVERIFIED on
    // hardware: whether a BIOS still accepts the boot keyboard beside an
    // empty card reader is the claim this whole project rests on, and no
    // OS-booted host can test it. Leaving the disk out of the composition
    // (settings.c) is the operator's answer when it does not.
    TUD_MSC_DESCRIPTOR(ITF_MSC, 6, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};
#endif

// Espressif's vendor ID with their generic product IDs. Do not invent a VID:
// unallocated VIDs collide, and a colliding VID/PID pair can make a target
// load the wrong driver for a device it thinks it has seen before.
//
// Espressif's PID map encodes the interface set (bit 2 HID, bit 1 MSC), and
// it exists because hosts cache descriptors per VID/PID: the same PID with a
// different interface list is how Windows ends up loading yesterday's driver
// for today's device. That is why the two compositions are two PIDs, and why
// a keyboard-only composition is not offered until it has one of its own.
#define DEVICE_DESCRIPTOR(pid)                   \
  {                                              \
      .bLength = sizeof(tusb_desc_device_t),     \
      .bDescriptorType = TUSB_DESC_DEVICE,       \
      .bcdUSB = 0x0200,                          \
      .bDeviceClass = 0x00,                      \
      .bDeviceSubClass = 0x00,                   \
      .bDeviceProtocol = 0x00,                   \
      .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, \
      .idVendor = 0x303A,                        \
      .idProduct = (pid),                        \
      .bcdDevice = 0x0100,                       \
      .iManufacturer = 0x01,                     \
      .iProduct = 0x02,                          \
      .iSerialNumber = 0x03,                     \
      .bNumConfigurations = 0x01,                \
  }

static const tusb_desc_device_t s_device_desc_hid = DEVICE_DESCRIPTOR(0x4004);
#if CONFIG_TINYUSB_MSC_ENABLED
static const tusb_desc_device_t s_device_desc_full = DEVICE_DESCRIPTOR(0x4006);
#endif

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04},  // English (US)
    "airkvm",
    "airkvm keyboard+mouse",
    "000001",
    "airkvm keyboard",
    "airkvm mouse",
    "airkvm disk",  // index 6, the MSC interface string
};

// --- state ------------------------------------------------------------------

static struct hid_state s_state;
static QueueHandle_t s_queue;
static volatile bool s_mounted;

// Set by any path that loses confidence in session state; the report task
// services it ahead of the queue.
static volatile bool s_release_pending;

// Bus suspend state. Two facts, held differently on purpose:
//
//   suspended  is TinyUSB's — read through tud_suspended() wherever a decision
//              depends on it. A mirror would go stale: a bus reset (target
//              rebooting out of sleep, or a replug after a cable pull that
//              looked like a suspend) clears TinyUSB's own flag inside
//              usbd_reset() without ever calling tud_resume_cb. The mirror
//              below exists only so the display and the log can react to the
//              transitions, and it is re-zeroed on mount for that reason.
//   armed      is whether the host enabled remote wakeup before suspending.
//              TinyUSB has no getter for that one, so the value handed to
//              tud_suspend_cb is the only copy.
//
// `s_wake_pending` is a flag rather than a queued intent for the same reason
// release-all is: while the bus is suspended tud_hid_n_ready() is false, so
// the report task's drain is parked on whatever keyboard intent is at the head
// of the queue (issue #27), and a wake behind it would wait for exactly the
// resume it exists to cause.
static volatile bool s_suspended;  // display/log mirror only, see above
static volatile bool s_remote_wakeup_en;
static volatile bool s_wake_pending;

// Wake-on-input. `s_input_arrived` is set by hid_out_submit() for operator
// input only — key, button, scroll (wake_policy_is_operator_input) — and
// consumed by the report task, which turns it into `s_implicit_wake_wanted`
// when the bus is suspended with wakeup armed. It is deliberately not derived
// from "is any output owed": the suspend itself leaves a release-all report
// owed, and a console keeps sending PINGs and the occasional RELEASE_ALL, none
// of which is a person asking the machine to wake up. Keyed on those, the
// target could never stay asleep with a dongle plugged in.
//
// The edge flag alone can lose a request: a submit landing between the read
// and the clear, or input that arrives after dwc2 has marked the bus suspended
// but before tud_suspend_cb has run. Both leave the keystroke parked at the
// head of the queue, so the drain below also re-raises the request whenever it
// finds itself parked on a keyboard intent during a suspend — level, not edge.
//
// Timing (5 ms after suspend, spacing, the per-episode budget) lives in
// wake_policy.c, where it is host-tested.
static volatile bool s_input_arrived;
static volatile bool s_implicit_wake_wanted;
static struct wake_policy s_wake;
static bool s_wake_budget_logged;  // one warning per episode, not one per pass

bool hid_out_is_mounted(void) {
  return s_mounted;
}

// TinyUSB device callbacks. These are weak-by-convention hooks that the
// application is expected to define — but esp_tinyusb's tusb_msc_storage.c
// defines tud_mount_cb and tud_umount_cb *strongly* too, and it is compiled
// whenever CONFIG_TINYUSB_MSC_ENABLED is set, which it now is.
//
// Two outcomes were possible: a duplicate-symbol link error, which is the
// good case because it is loud, or Espressif's implementation winning the
// link, which is the bad one: the firmware still builds and still enumerates,
// and the only symptom is that tud_umount_cb below never runs.
//
// That is a safety regression rather than a cosmetic one. tud_umount_cb sets
// s_release_pending, which is how an unplug releases every held key — §2
// principle 4, the dongle must never be the thing typing. A composite build
// that quietly lost it would leave keys held on the target after the cable
// was pulled, with nothing in any log to say so.
//
// What makes ours win deterministically: main is linked WHOLE_ARCHIVE
// (main/CMakeLists.txt), so every object here enters the link before any
// archive is searched and no tud_* reference survives to be satisfied by
// tusb_msc_storage.o; and tools/check_msc_symbols.cmake fails the build if
// that object made it into the ELF anyway (AGENTS.md §9). The hardware
// acceptance test is still the real proof: with a key held, pull the cable —
// the log must say "USB host disconnected" and the key must be released.
// Nothing else reveals it, not a green build and not a working mount.
void tud_mount_cb(void) {
  ESP_LOGI(TAG, "USB host enumerated the device");
  s_mounted = true;
  // A (re)enumeration ends any suspend, with or without a resume callback —
  // see the note on the suspend mirror above.
  s_suspended = false;
  s_remote_wakeup_en = false;
  s_wake_pending = false;
  s_implicit_wake_wanted = false;
  wake_policy_reset(&s_wake);
#if CONFIG_TINYUSB_MSC_ENABLED
  msc_out_bus_reset();
#endif
  status_set_usb_mounted(true);
  status_set_usb_suspended(false);
}

void tud_umount_cb(void) {
  ESP_LOGI(TAG, "USB host disconnected");
  s_mounted = false;
  s_suspended = false;
  s_wake_pending = false;
  s_implicit_wake_wanted = false;
  wake_policy_reset(&s_wake);
#if CONFIG_TINYUSB_MSC_ENABLED
  msc_out_bus_reset();
#endif
  status_set_usb_mounted(false);
  status_set_usb_suspended(false);
  // Unplugged mid-session: drop everything so a replug does not resume a state
  // the console has since forgotten about.
  s_release_pending = true;
}

// Fires on any bus suspend: the target sleeping, but also a host parking an
// idle device (selective suspend), and — on this board, which has no VBUS
// sense — possibly a cable pull, which TinyUSB's own comments warn can look
// like a suspend rather than an unmount. Whether that last one happens here is
// unobserved. Hence "suspended", never "asleep", anywhere this state is shown.
void tud_suspend_cb(bool remote_wakeup_en) {
  ESP_LOGI(TAG, "USB bus suspended (remote wakeup %s by host)",
           remote_wakeup_en ? "armed" : "not armed");
  s_suspended = true;
  s_remote_wakeup_en = remote_wakeup_en;
  wake_policy_suspend(&s_wake, esp_timer_get_time());
  s_wake_budget_logged = false;
  status_set_usb_suspended(true);
  s_release_pending = true;
}

// The only real evidence a wakeup worked: the host resumed the bus. Logged as
// such so a hardware test can tell "pulse sent" from "target woke".
void tud_resume_cb(void) {
  ESP_LOGI(TAG, "USB bus resumed");
  s_suspended = false;
  // Tidiness, not a guard: a wake request racing this line can still set the
  // flag afterwards, and the pulse then goes to tud_remote_wakeup(), which
  // refuses on its own because the bus is no longer suspended. That refusal is
  // the load-bearing check.
  s_wake_pending = false;
  s_implicit_wake_wanted = false;
  wake_policy_reset(&s_wake);
  status_set_usb_suspended(false);
}

enum airkvm_wake_result hid_out_wake_target(void) {
  // "Not enabled by host" is an honest answer, not a dongle fault: the host
  // went to sleep without asking to be woken by this device. On Linux that is
  // /sys/bus/usb/devices/<dev>/power/wakeup; on a PC it is usually a firmware
  // "wake on USB" setting.
  const enum airkvm_wake_result r =
      wake_policy_decide(s_mounted, tud_suspended(), s_remote_wakeup_en);
  if (r == AIRKVM_WAKE_REQUESTED) {
    s_wake_pending = true;
  }
  return r;
}

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
// How long the report task waits when it has nothing to do: two FreeRTOS ticks
// at CONFIG_FREERTOS_HZ=1000, which sdkconfig.defaults pins for this reason.
//
// The tick rate is load-bearing here, not incidental. At ESP-IDF's default
// 100Hz the smallest possible wait is one 10ms tick, which aliases against the
// endpoints' 10ms bInterval — a task waking every 10ms catches the endpoint's
// ready window only every other time, and typing throughput halves. Measured:
// a 60-character string at 10ms/char delivered 26 of 60 at 100Hz and 60 of 60
// at 1kHz. Waking every 2ms gives five chances per endpoint interval.
#define REPORT_TICK_MS 2

// The guard that would have caught the bug fixed in the report task's wait: a
// pdMS_TO_TICKS() value that rounds down to zero turns vTaskDelay into a bare
// yield, and a always-ready task then spins a core flat out. Structural, so it
// cannot silently come back if CONFIG_FREERTOS_HZ or this constant changes
// (AGENTS.md §10, meta-rule 5: prefer a mechanism to a sentence).
_Static_assert(pdMS_TO_TICKS(REPORT_TICK_MS) > 0,
               "REPORT_TICK_MS rounds to zero ticks at this CONFIG_FREERTOS_HZ "
               "— the report task would spin instead of sleeping");

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
      // Counts as activity for the same reason keys and motion do: it is the
      // operator doing something. Without it, clicking without moving leaves
      // the idle clock running and the radio drops to a saving tier in the
      // middle of active use (wifi_set_activity, ui.c).
      status_note_event("CLICK");
      hid_state_set_buttons(&s_state, in->button_mask);
      s_buttons_dirty = true;
      break;
    case AIRKVM_FRAME_MOUSE_SCROLL:
      // Likewise: reading a long page is scroll-only input, and it should not
      // look like an absent operator.
      status_note_event("SCROLL");
      s_pending_scroll_v += in->scroll.vertical;
      s_pending_scroll_h += in->scroll.horizontal;
      break;
    case AIRKVM_FRAME_RELEASE_ALL:
      s_release_pending = true;
      break;
    case AIRKVM_FRAME_PING:
      break;
    case AIRKVM_FRAME_WAKE:
    case AIRKVM_FRAME_STATUS_REQ:
      // net.c answers these directly rather than queueing them (see the note
      // on s_wake_pending). Queued, they would bypass every precondition and
      // produce no reply, so they are refused rather than half-honoured; the
      // arm exists to keep the switch total.
      ESP_LOGW(TAG, "request 0x%02x arrived through the queue — ignored",
               (unsigned)in->type);
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

    // Implicit wake: what a physical keyboard does when you tap a key on a
    // sleeping machine, and what makes advertising remote wakeup survivable
    // for typing. Hosts that selectively suspend an idle HID device (Windows
    // does by default for wake-capable devices; Linux when a power tool sets
    // power/control to auto — hypotheses until observed) would otherwise park
    // the bus while the target is fully awake, and every keystroke after that
    // would sit behind tud_hid_n_ready() == false until something else
    // resumed it.
    if (s_input_arrived) {
      s_input_arrived = false;
      if (tud_suspended() && s_remote_wakeup_en) {
        s_implicit_wake_wanted = true;
      }
    }

    if (s_wake_pending || s_implicit_wake_wanted) {
      // The operator's own request is never budget-capped; only input is.
      const bool implicit = !s_wake_pending;
      switch (wake_policy_pulse(&s_wake, esp_timer_get_time(), implicit)) {
        case WAKE_PULSE_NOW: {
          s_wake_pending = false;
          s_implicit_wake_wanted = false;
          // On this task, not the HTTP task: inside the dcd this is a
          // read-modify-write of DCTL and then dwc2's remote-wakeup delay,
          // vTaskDelay(pdMS_TO_TICKS(1)) — which waits until the next tick,
          // 0–1 ms at CONFIG_FREERTOS_HZ=1000, below the spec's 1 ms minimum
          // pulse width and, at the default 100 Hz, the zero-tick non-delay
          // of AGENTS.md §9 entry 12. Vendor code; cannot be static-asserted
          // from here. TinyUSB re-checks its own preconditions and returns
          // false if the bus is no longer suspended or wakeup was never armed.
          const bool issued = tud_remote_wakeup();
          ESP_LOGI(TAG, "remote wakeup %s (%s)",
                   issued ? "signalled" : "refused by stack",
                   implicit ? "on input" : "requested");
          break;
        }
        case WAKE_PULSE_DEFER:
          // Too soon after the suspend or the last pulse. Flags stay set; the
          // timed peek at the bottom of the loop brings us back within
          // REPORT_TICK_MS, the same way release-all gets serviced.
          break;
        case WAKE_PULSE_NO:
          s_wake_pending = false;
          s_implicit_wake_wanted = false;
          if (implicit && !s_wake_budget_logged) {
            s_wake_budget_logged = true;
            ESP_LOGW(TAG,
                     "target did not resume after %d wake-on-input pulses; "
                     "parked until it does or the operator asks",
                     WAKE_POLICY_MAX_IMPLICIT_PULSES);
          }
          break;
      }
    }

    // Apply everything already queued before emitting, so a burst collapses
    // into the fewest reports that still convey it faithfully — with one
    // exception that is load-bearing: a keyboard intent is never applied on
    // top of a keyboard state whose report has not gone out yet. Doing so
    // silently destroys the keypress (see the note above pump_keyboard).
    // Peek before receiving, so the intent we decline to apply stays queued
    // in order for the next pass.
    //
    // When blocked, flush here rather than waiting for the pump below: if the
    // endpoint is ready the report goes out immediately and the drain
    // continues, so a burst of typing empties at endpoint pace within one
    // pass instead of one intent per wakeup.
    //
    // KNOWN LIMITATION (issue #27). If the keyboard endpoint stays unready
    // while the mouse endpoint does not, this break leaves the keyboard
    // intent at the head of a queue *shared* with the mouse, so mouse intents
    // behind it are unreachable and the pointer freezes. Normally the block
    // lasts at most one endpoint interval, and the paths that would wedge it
    // for longer — unplug, suspend — already reset the queue via
    // tud_umount_cb/tud_suspend_cb. The real fix is separate keyboard and
    // mouse queues, which is a change to the single-queue design AGENTS.md §4
    // states, so it is proposed there rather than made here at the same time
    // as a bug fix.
    while (xQueuePeek(s_queue, &intent, 0) == pdTRUE) {
      const bool is_keyboard = intent.type == AIRKVM_FRAME_KEY_DOWN ||
                               intent.type == AIRKVM_FRAME_KEY_UP;
      if (is_keyboard && s_kbd_dirty) {
        pump_keyboard();
        if (s_kbd_dirty) {
          // Endpoint not ready; this intent waits its turn. If the reason is
          // a suspended bus, waiting is not enough — nothing will resume it
          // unless we ask (see the note on wake-on-input; this is the level
          // half of the trigger, and the policy's budget bounds it).
          if (tud_suspended() && s_remote_wakeup_en) {
            s_implicit_wake_wanted = true;
          }
          break;
        }
      }
      (void)xQueueReceive(s_queue, &intent, 0);
      apply(&intent);
      if (s_release_pending) {
        break;  // handled at the top of the next iteration, ahead of the queue
      }
    }

    pump_keyboard();
    pump_mouse();

    // Wait for there to be something to do. This used to be
    // `vTaskDelay(pdMS_TO_TICKS(2))`, which never slept at all: pdMS_TO_TICKS
    // is integer arithmetic, so at CONFIG_FREERTOS_HZ=100 it evaluates to
    // (2 * 100) / 1000 == 0, and vTaskDelay(0) is documented by the kernel as
    // "just forces a reschedule" — the task is never added to the delayed
    // list. This task is priority 5 and always ready, so it was rescheduled
    // immediately and pegged one core at 160MHz permanently, preventing the
    // idle task from ever gating that core's clock. It had done so since the
    // initial commit. See the static assert on REPORT_TICK_MS above.
    //
    // Two cases, because they want different waits:
    if (s_kbd_dirty || s_buttons_dirty || s_pending_dx || s_pending_dy ||
        s_pending_scroll_v || s_pending_scroll_h) {
      // Output still owed — the endpoint was busy, or a delta exceeded one
      // report. Nothing new needs to arrive for there to be work, so sleep a
      // tick and try to pump again.
      vTaskDelay(pdMS_TO_TICKS(REPORT_TICK_MS));
    } else {
      // Nothing owed: block until an intent arrives. A blocking peek wakes
      // the instant one does, so idling costs no added latency on the first
      // keystroke — better than a fixed delay, which would add up to a tick.
      // The timeout still gives a periodic wakeup, which release-all needs:
      // hid_out_release_all() sets a flag from another task without queueing
      // anything, so nothing would wake us otherwise.
      struct airkvm_intent waited;
      (void)xQueuePeek(s_queue, &waited, pdMS_TO_TICKS(REPORT_TICK_MS));
    }
  }
}

// --- lifecycle --------------------------------------------------------------

bool hid_out_submit(const struct airkvm_intent *intent) {
  if (s_queue == NULL) {
    return false;
  }
  // Never block: this runs on the HTTP task, and a stalled network task holding
  // a key down is the failure this design exists to prevent.
  if (xQueueSend(s_queue, intent, 0) != pdTRUE) {
    return false;
  }
  if (wake_policy_is_operator_input(intent->type)) {
    s_input_arrived = true;  // see the note on wake-on-input
  }
  return true;
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

  // Which composition this boot presents. Read once, here: the descriptor,
  // the LUN, the SD probe and the media socket all consult the same
  // settings_usb_disk_enabled(), so they cannot disagree about whether there
  // is a disk. Reboot-scoped by design — re-enumerating a live device is a
  // hardware re-test of gotcha 10 (the USB and RF PHYs share the BBPLL) and
  // is tracked, not attempted.
  const tusb_desc_device_t *device = &s_device_desc_hid;
  const uint8_t *configuration = s_configuration_desc_hid;
  int itf_count = ITF_COUNT_HID;
#if CONFIG_TINYUSB_MSC_ENABLED
  const bool with_disk = settings_usb_disk_enabled();
  if (with_disk) {
    // Before the driver: the host's first INQUIRY can land as soon as
    // enumeration completes, and the LUN has to be there to answer it. Never
    // blocks — nothing about a medium is touched here. Skipped entirely in
    // the other composition; every msc_out_* entry point tolerates that
    // (gotcha 16b), and the MSC callbacks are never invoked for an interface
    // the descriptor does not declare.
    ESP_RETURN_ON_ERROR(msc_out_start(), TAG, "msc_out_start failed");
    device = &s_device_desc_full;
    configuration = s_configuration_desc_full;
    itf_count = ITF_COUNT_FULL;
  }
#else
  const bool with_disk = false;
#endif

  const tinyusb_config_t cfg = {
      .device_descriptor = device,
      .string_descriptor = s_string_desc,
      .string_descriptor_count =
          sizeof(s_string_desc) / sizeof(s_string_desc[0]),
      .external_phy = false,
      .configuration_descriptor = configuration,
  };
  ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG,
                      "tinyusb_driver_install failed");

  if (xTaskCreate(report_task, "hid_report", 3072, NULL, 5, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG,
           "USB HID started (keyboard itf %d, mouse itf %d, %d itfs, PID "
           "0x%04X, disk %s)",
           HID_ITF_KEYBOARD, HID_ITF_MOUSE, itf_count, device->idProduct,
           with_disk ? "on" : "off");
  return ESP_OK;
}
