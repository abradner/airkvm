// Which intents may be applied on top of a report that has not gone out yet.
//
// The report task drains every queued intent before emitting, so a burst
// collapses into the fewest reports that still convey it. That is only safe
// for intents whose intermediate states carry no information, and two kinds
// share one queue:
//
//   ACCUMULATING — MOUSE_MOVE, MOUSE_SCROLL. apply() does `+=`, so the sum is
//     the whole of the information and collapsing ten deltas loses nothing.
//   ABSOLUTE STATE — KEY_DOWN/KEY_UP, MOUSE_BUTTON. apply() overwrites, so an
//     intermediate state IS the information. A down and its up applied in one
//     pass leave the state exactly as it started, and the report that follows
//     says "nothing pressed" — the keypress, or the click, is gone.
//
// The keyboard half of that is AGENTS.md §9 entry 11, found when the first
// real typing test delivered 3 characters of 17. The button half was the same
// bug left standing (issue #58): a human click outlasts a drain pass by orders
// of magnitude, so only the phone console's synthesised taps could reach it,
// and those dodged it with a dwell rather than hitting it.
//
// Pure and host-tested (test/host/test_report_drain.c) so the classification
// is pinned rather than re-derived at each call site. The switch is total with
// no `default:` on purpose: a new frame type then fails the build under
// -Werror=switch instead of silently taking "safe to coalesce", which is both
// the wrong default and the one a `default:` would pick. That is §10 meta-rule
// 5 aimed at entry 19's general rule — when a guard is added for one instance
// of a shape, the other instances get enumerated rather than assumed.
#pragma once

#include <stdbool.h>

#include "protocol.h"

// Which report's state an intent writes, and so which report must have been
// emitted before it may be applied.
enum report_channel {
  REPORT_CHANNEL_NONE = 0,       // accumulates, or touches no report state
  REPORT_CHANNEL_KEYBOARD,       // the keyboard bitmap and modifier byte
  REPORT_CHANNEL_MOUSE_BUTTONS,  // the mouse button mask
};

static inline enum report_channel report_drain_channel(
    enum airkvm_frame_type type) {
  switch (type) {
    case AIRKVM_FRAME_KEY_DOWN:
    case AIRKVM_FRAME_KEY_UP:
      return REPORT_CHANNEL_KEYBOARD;
    case AIRKVM_FRAME_MOUSE_BUTTON:
      return REPORT_CHANNEL_MOUSE_BUTTONS;
    case AIRKVM_FRAME_MOUSE_MOVE:
    case AIRKVM_FRAME_MOUSE_SCROLL:
      // Accumulating: the deltas add and no intermediate is observable.
      return REPORT_CHANNEL_NONE;
    case AIRKVM_FRAME_RELEASE_ALL:
      // Writes both channels, but is never coalesced with anything: apply()
      // raises a flag that the top of the loop services by resetting the
      // queue, so it cannot sit behind an unreported state.
      return REPORT_CHANNEL_NONE;
    case AIRKVM_FRAME_PING:
    case AIRKVM_FRAME_WAKE:
    case AIRKVM_FRAME_STATUS_REQ:
      // No report state at all.
      return REPORT_CHANNEL_NONE;
  }
  return REPORT_CHANNEL_NONE;  // unreachable: the switch above is total
}

// True when this intent must wait for its channel's report to go out. The
// caller pumps that channel and, if the report is still owed, leaves the
// intent queued in order for the next pass.
static inline bool report_drain_blocked(enum airkvm_frame_type type,
                                        bool keyboard_owed, bool buttons_owed) {
  switch (report_drain_channel(type)) {
    case REPORT_CHANNEL_KEYBOARD:
      return keyboard_owed;
    case REPORT_CHANNEL_MOUSE_BUTTONS:
      return buttons_owed;
    case REPORT_CHANNEL_NONE:
      break;
  }
  return false;
}
