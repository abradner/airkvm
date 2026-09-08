// Front-panel LED behaviour.
//
//   no client      off
//   client bound   breathing, capped at 50% duty
//   input activity a brief blip, +25% above whatever the breath is doing
//
// The cap matters: this thing sits on a desk or in a rack next to a machine you
// are working on, and a status light at full brightness in a dark room is worse
// than no status light.
#pragma once

#include <stdbool.h>

#include "status.h"

// What the LED is saying, derived from status in exactly one place.
//
// Split out as a pure function because there are now two LED backends (a
// single-channel PWM one and an RGB one) and "which state are we in" must not
// be answered differently by each. What differs between them is how a state is
// *rendered*; what it *is* is shared, and host-tested.
enum indicator_state {
  // Setup mode: the dongle has rebooted into the provisioning portal. It
  // outranks everything, including FAULT, and that ordering is the point
  // rather than a tie-break — in setup mode there is no keyboard, no disk, no
  // console and no target. "Your keystrokes are going nowhere" is not a fault
  // to be diagnosed, it is the mode working as designed, and a light saying
  // FAULT would send the operator looking for a problem that does not exist.
  INDICATOR_SETUP,
  // Something the operator came here to do will not work.
  INDICATOR_FAULT,
  // The target is enumerated but its USB bus is suspended: keystrokes will go
  // nowhere until it is woken. Distinct from FAULT because nothing is broken
  // and there is a specific remedy (the console's Wake button), and distinct
  // from WARN because it is not degradation — it is a full stop with a fix.
  INDICATOR_SUSPENDED,
  // Works, but degraded in a way that explains slowness before someone goes
  // looking for a bug.
  INDICATOR_WARN,
  // A console holds the keyboard.
  INDICATOR_BOUND,
  // Idle and healthy.
  INDICATOR_IDLE,
};

// Weakest signal band, matching ui.c's one-bar threshold. Kept as the same
// number deliberately: the light and the screen should not disagree about
// whether the link is weak.
#define INDICATOR_WEAK_RSSI_DBM (-77)

// ...and the level it must climb back above to stop warning. The gap is
// hysteresis, and it is not decoration: RSSI is a noisy single sample, so a
// bare threshold makes one unlucky reading flip the light to amber and back.
// Observed on hardware 2026-09-07 — a link sitting around -62 dBm flashed
// yellow at least once while being watched.
//
// The light's whole job is "should I be worried", and an indicator that cries
// wolf on noise is worse than one slightly slow to notice, because people
// learn to ignore it. Erring toward not-warning is right for this signal —
// the opposite of principle 4's release-all bias, for the same underlying
// reason: choose the error that does not train the operator to look away.
#define INDICATOR_STRONG_RSSI_DBM (-72)

// Is the radio link weak enough to explain slowness?
//
// Extracted from indicator_state_for() below because the STATUS PANEL now
// colours its signal bar on the same question, and the header above already
// promised the light and the screen would not disagree about it. That promise
// was a matched pair of constants; it is now a shared function, which is the
// difference between a claim and a mechanism (AGENTS.md §10 meta-rule 5).
// Delete this and the two would drift on the next threshold tweak.
//
// `was_weak` is the caller's own last answer, which is what gives the
// hysteresis somewhere to live without this holding state — see the constants
// above for why a bare threshold is not enough. Callers keep their own latch
// and pass false on the first call; both are driven from the same snapshot at
// the same cadence, so they move together.
static inline bool indicator_link_is_weak(int8_t rssi, bool was_weak) {
  // rssi == 0 means "not measured yet", not "catastrophically weak".
  if (rssi == 0) {
    return false;
  }
  // Enter below the weak level; leave only once back above the strong one.
  return was_weak ? rssi < INDICATOR_STRONG_RSSI_DBM
                  : rssi <= INDICATOR_WEAK_RSSI_DBM;
}

// `prev` is the state last returned, so the weak-signal warning can have
// hysteresis without this function holding state of its own — it stays pure
// and host-testable, and the hysteresis cannot end up implemented differently
// in each LED backend. Pass INDICATOR_IDLE on the first call.
static inline enum indicator_state indicator_state_for(
    const struct status_snapshot *s, enum indicator_state prev) {
  // Precedence is deliberate: faults and degradation outrank "someone is
  // driving it", because they are the ones you would act on. The screen still
  // says BOUND, so that information is not lost — the light is answering
  // "should I be worried", not "what is happening".
  // Setup mode first, and specifically BEFORE the usb_mounted test below.
  //
  // On the S3 the portal is also served over a USB NCM network device, so
  // TinyUSB enumerates and tud_mount_cb fires: usb_mounted goes true while
  // setup_active is true. Ranked the other way round, the light would report a
  // healthy keyboard about a device that is a network adapter — and worse, it
  // would say so on the one channel the operator trusts to mean "your input is
  // reaching the target". The panel has the same bug for the same reason;
  // see issue #55.
  if (s->setup_active) {
    return INDICATOR_SETUP;
  }
  if (!s->wifi_connected || !s->usb_mounted) {
    return INDICATOR_FAULT;
  }
  // No credentials at all: not a fault, because nothing is broken and the
  // remedy is on the panel ("HOLD BOOT 5S:SETUP"). WARN is the state that
  // already means "works as far as it can, and here is why it is not doing
  // what you expected". A stable boolean set once at boot, so it cannot
  // flicker against the RSSI hysteresis below.
  if (s->no_credentials) {
    return INDICATOR_WARN;
  }
  // Ranked above WARN deliberately: if the target is asleep AND the link is
  // weak, being asleep is the thing actually stopping you. Note usb_mounted
  // stays true through a suspend, so this cannot be folded into the check
  // above — without it the light reports full health while nothing types,
  // which is the precise failure a status light exists to prevent.
  if (s->usb_suspended) {
    return INDICATOR_SUSPENDED;
  }
  if (indicator_link_is_weak(s->rssi, prev == INDICATOR_WARN)) {
    return INDICATOR_WARN;
  }
  return s->client_connected ? INDICATOR_BOUND : INDICATOR_IDLE;
}

void indicator_init(void);

// Call periodically; drives the LED from current status. Kept as a tick rather
// than its own task so the UI task owns all the timing in one place.
// Call periodically; drives the LED from `s`. Kept as a tick rather than its
// own task so the UI task owns all the timing in one place.
//
// Takes the CALLER'S snapshot rather than reading its own, and returns the
// state it just rendered, because the status panel derives the signal bar's
// colour from the same question this answers. Two independent status_get()
// calls microseconds apart can differ, and a difference inside the hysteresis
// band does not wash out on the next tick — it latches, and the light and the
// screen then disagree about the link indefinitely. One snapshot, one answer.
enum indicator_state indicator_tick(const struct status_snapshot *s);
