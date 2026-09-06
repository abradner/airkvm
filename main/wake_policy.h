// The decisions behind USB remote wakeup, kept pure so they are host-tested.
//
// No I/O, no ESP-IDF headers (AGENTS.md §4). hid_out_usb.c feeds this the
// bus facts TinyUSB reports and acts on what comes back; nothing here touches
// the transport. Time is int64 microseconds from whatever clock the caller
// has (esp_timer_get_time() in firmware, a counter in tests).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

// Why a wake request can or cannot proceed, in the order the console should
// hear about it: no host at all beats "not suspended", which beats "the host
// never armed wakeup". These are the console's reasons only — TinyUSB refuses
// a pulse on its own when its preconditions fail, so deleting this function
// would change the message, never what is possible (AGENTS.md §8, layered
// checks).
enum airkvm_wake_result wake_policy_decide(bool mounted, bool suspended,
                                           bool armed);

// --- pulse timing -----------------------------------------------------------

// USB 2.0 §7.1.7.7: a device must not signal remote wakeup until the bus has
// been idle for at least 5 ms after the suspend. dwc2 reports suspend after
// 3 ms of idle, so a keystroke already queued when the suspend lands would
// otherwise be pulsed inside the forbidden window.
#define WAKE_POLICY_MIN_AFTER_SUSPEND_US 5000

// Spacing between pulses in one suspend episode. A host resuming the bus after
// a selective suspend takes tens of milliseconds; one that is genuinely asleep
// takes seconds and one pulse is enough. 100 ms is well clear of the first and
// makes repeated asking for the second cheap rather than a storm.
#define WAKE_POLICY_MIN_INTERVAL_US 100000

// Input-triggered pulses per suspend episode before giving up. A host that has
// not resumed after three asks is not going to for the fourth; the operator's
// explicit Wake is never capped.
#define WAKE_POLICY_MAX_IMPLICIT_PULSES 3

struct wake_policy {
  bool suspended;
  int64_t suspended_at_us;
  int64_t last_pulse_us;
  unsigned pulses;           // in this suspend episode, any trigger
  unsigned implicit_pulses;  // the input-triggered subset
};

// Any end of a suspend episode: resume, (re)enumeration, unplug.
void wake_policy_reset(struct wake_policy *p);
void wake_policy_suspend(struct wake_policy *p, int64_t now_us);

enum wake_pulse {
  WAKE_PULSE_NOW,    // issue tud_remote_wakeup() now; counters advanced
  WAKE_PULSE_DEFER,  // too soon; ask again next pass, keep the request
  WAKE_PULSE_NO,     // not suspended, or implicit budget spent; drop it
};

// `implicit` is true for wake-on-input, false for the operator's Wake button.
enum wake_pulse wake_policy_pulse(struct wake_policy *p, int64_t now_us,
                                  bool implicit);

// Which frames are a person doing something to the target — and therefore a
// reason to wake it — as opposed to housekeeping (PING, RELEASE_ALL) or the
// wake request itself.
bool wake_policy_is_operator_input(enum airkvm_frame_type type);
