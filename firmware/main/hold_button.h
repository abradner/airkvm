// Turns a stream of debounced button levels into the events the UI acts on:
// a short press, a long hold with progress feedback, the moment the hold
// completes, or a hold abandoned partway.
//
// Pure: no I/O, no ESP-IDF headers, host-tested (test/host/test_hold_button.c).
// The caller feeds it every tick with the debounced level and a millisecond
// clock; debouncing stays in ui.c, where the GPIO is.
//
// Why a hold, and why these numbers: holding BOOT at power-on is the ROM's
// download strap, so "hold at boot" can never be a firmware gesture. A 5 s
// hold while running is long enough to be unmistakably deliberate and short
// enough to be discoverable, and feedback from 1 s tells the person holding
// it that something is listening. Release before 5 s cancels with no action
// at all — the accidental-brush case is a short press, which keeps its old
// meaning, and everything between is a cancel.
//
// And why the action happens on RELEASE, not at the 5 s mark: the action is
// a restart, and the button is that same strap pin. A restart with BOOT
// still held re-samples the strap low and lands in the ROM downloader, where
// nothing of ours runs. So at 5 s the hold is ARMED (bar full, "release to
// restart") and the release edge fires. A reviewer caught this before it
// reached hardware.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HOLD_BUTTON_FEEDBACK_MS 1000u
#define HOLD_BUTTON_FIRE_MS 5000u

enum hold_button_event {
  HOLD_BUTTON_NONE,
  HOLD_BUTTON_SHORT_RELEASE,  // released before feedback began: a press
  HOLD_BUTTON_FEEDBACK,       // held past 1 s; *fraction says how far to 5 s
  HOLD_BUTTON_ARMED,          // held 5 s or more; releasing now will fire
  HOLD_BUTTON_FIRE,           // released while armed; exactly once per hold
  HOLD_BUTTON_CANCEL,         // released after feedback began, before armed
};

struct hold_button {
  bool pressed;
  bool feedback;  // feedback has begun for this hold
  bool armed;     // the 5 s mark passed during this hold
  uint32_t pressed_at_ms;
};

void hold_button_init(struct hold_button *b);

// One tick. `fraction` (0..255, may be NULL) is written for FEEDBACK only:
// 0 at 1 s, 254 just before 5 s (ARMED takes over at 5 s). Millisecond clock
// compared by unsigned difference, so its ~49-day wrap is a non-event.
enum hold_button_event hold_button_step(struct hold_button *b, bool pressed,
                                        uint32_t now_ms, uint8_t *fraction);
