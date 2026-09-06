#include "wake_policy.h"

#include <string.h>

enum airkvm_wake_result wake_policy_decide(bool mounted, bool suspended,
                                           bool armed) {
  if (!mounted) {
    return AIRKVM_WAKE_NO_HOST;
  }
  if (!suspended) {
    return AIRKVM_WAKE_NOT_SUSPENDED;
  }
  if (!armed) {
    return AIRKVM_WAKE_NOT_ENABLED_BY_HOST;
  }
  return AIRKVM_WAKE_REQUESTED;
}

void wake_policy_reset(struct wake_policy *p) {
  memset(p, 0, sizeof(*p));
}

void wake_policy_suspend(struct wake_policy *p, int64_t now_us) {
  // A fresh episode: counters start over, the spacing clock too.
  wake_policy_reset(p);
  p->suspended = true;
  p->suspended_at_us = now_us;
}

enum wake_pulse wake_policy_pulse(struct wake_policy *p, int64_t now_us,
                                  bool implicit) {
  if (!p->suspended) {
    return WAKE_PULSE_NO;
  }
  if (now_us - p->suspended_at_us < WAKE_POLICY_MIN_AFTER_SUSPEND_US) {
    return WAKE_PULSE_DEFER;
  }
  if (p->pulses > 0 &&
      now_us - p->last_pulse_us < WAKE_POLICY_MIN_INTERVAL_US) {
    return WAKE_PULSE_DEFER;
  }
  if (implicit && p->implicit_pulses >= WAKE_POLICY_MAX_IMPLICIT_PULSES) {
    return WAKE_PULSE_NO;
  }
  p->last_pulse_us = now_us;
  p->pulses++;
  if (implicit) {
    p->implicit_pulses++;
  }
  return WAKE_PULSE_NOW;
}

bool wake_policy_is_operator_input(enum airkvm_frame_type type) {
  switch (type) {
    case AIRKVM_FRAME_KEY_DOWN:
    case AIRKVM_FRAME_KEY_UP:
    case AIRKVM_FRAME_MOUSE_BUTTON:
    case AIRKVM_FRAME_MOUSE_SCROLL:
      return true;
    case AIRKVM_FRAME_MOUSE_MOVE:
      // Deliberately not. The operator who just typed a sleep chord is, by
      // definition, captured with a hand on the mouse, and the console emits
      // motion frames from sub-pixel drift. A key, a click or a scroll is a
      // decision; a twitch is not.
      return false;
    case AIRKVM_FRAME_RELEASE_ALL:
    case AIRKVM_FRAME_PING:
    case AIRKVM_FRAME_WAKE:
      return false;
  }
  return false;
}
