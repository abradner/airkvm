#include "test_support.h"
#include "wake_policy.h"

static void decide_orders_reasons_by_usefulness(void) {
  // No host outranks everything: a suspended, armed bus with nothing
  // enumerated is not a thing, and "not suspended" would mislead.
  CHECK_EQ_INT(wake_policy_decide(false, true, true), AIRKVM_WAKE_NO_HOST);
  CHECK_EQ_INT(wake_policy_decide(false, false, false), AIRKVM_WAKE_NO_HOST);
  // Awake target: say so even if a stale armed flag is lying around.
  CHECK_EQ_INT(wake_policy_decide(true, false, true),
               AIRKVM_WAKE_NOT_SUSPENDED);
  // Suspended but the host did not ask to be woken by us.
  CHECK_EQ_INT(wake_policy_decide(true, true, false),
               AIRKVM_WAKE_NOT_ENABLED_BY_HOST);
  // Positive control: all three hold.
  CHECK_EQ_INT(wake_policy_decide(true, true, true), AIRKVM_WAKE_REQUESTED);
}

static void input_classification_is_total(void) {
  CHECK(wake_policy_is_operator_input(AIRKVM_FRAME_KEY_DOWN));
  CHECK(wake_policy_is_operator_input(AIRKVM_FRAME_KEY_UP));
  CHECK(wake_policy_is_operator_input(AIRKVM_FRAME_MOUSE_BUTTON));
  CHECK(wake_policy_is_operator_input(AIRKVM_FRAME_MOUSE_SCROLL));
  // Not input: a console sends these while nobody is doing anything, and
  // keyed on them a target could never stay asleep with a dongle plugged in.
  CHECK(!wake_policy_is_operator_input(AIRKVM_FRAME_PING));
  CHECK(!wake_policy_is_operator_input(AIRKVM_FRAME_RELEASE_ALL));
  CHECK(!wake_policy_is_operator_input(AIRKVM_FRAME_WAKE));
  // Motion is excluded on purpose (see wake_policy.c).
  CHECK(!wake_policy_is_operator_input(AIRKVM_FRAME_MOUSE_MOVE));
}

static void pulse_refuses_when_not_suspended(void) {
  struct wake_policy p;
  wake_policy_reset(&p);
  CHECK_EQ_INT(wake_policy_pulse(&p, 1000000, false), WAKE_PULSE_NO);
  CHECK_EQ_INT(wake_policy_pulse(&p, 1000000, true), WAKE_PULSE_NO);
}

static void pulse_waits_five_ms_after_suspend(void) {
  struct wake_policy p;
  wake_policy_suspend(&p, 1000000);
  CHECK_EQ_INT(wake_policy_pulse(&p, 1000000, false), WAKE_PULSE_DEFER);
  CHECK_EQ_INT(wake_policy_pulse(&p, 1000000 + 4999, false), WAKE_PULSE_DEFER);
  // Deferring must not have started the spacing clock.
  CHECK_EQ_INT(p.pulses, 0);
  CHECK_EQ_INT(wake_policy_pulse(&p, 1000000 + 5000, false), WAKE_PULSE_NOW);
  CHECK_EQ_INT(p.pulses, 1);
}

static void pulses_are_spaced(void) {
  struct wake_policy p;
  wake_policy_suspend(&p, 0);
  CHECK_EQ_INT(wake_policy_pulse(&p, 10000, false), WAKE_PULSE_NOW);
  CHECK_EQ_INT(wake_policy_pulse(&p, 10000 + 99999, false), WAKE_PULSE_DEFER);
  CHECK_EQ_INT(wake_policy_pulse(&p, 10000 + 100000, false), WAKE_PULSE_NOW);
  CHECK_EQ_INT(p.pulses, 2);
}

static void implicit_pulses_are_capped_but_explicit_are_not(void) {
  struct wake_policy p;
  wake_policy_suspend(&p, 0);
  int64_t t = 10000;
  for (unsigned i = 0; i < WAKE_POLICY_MAX_IMPLICIT_PULSES; i++) {
    CHECK_EQ_INT(wake_policy_pulse(&p, t, true), WAKE_PULSE_NOW);
    t += WAKE_POLICY_MIN_INTERVAL_US;
  }
  // Budget spent: input no longer pulses, and stays that way.
  CHECK_EQ_INT(wake_policy_pulse(&p, t, true), WAKE_PULSE_NO);
  CHECK_EQ_INT(wake_policy_pulse(&p, t + 1000000, true), WAKE_PULSE_NO);
  // The operator's own request is still honoured, spacing permitting.
  CHECK_EQ_INT(wake_policy_pulse(&p, t, false), WAKE_PULSE_NOW);
  CHECK_EQ_INT(wake_policy_pulse(&p, t + 1000, false), WAKE_PULSE_DEFER);
  // A new suspend episode restores the budget.
  wake_policy_suspend(&p, t + 2000000);
  CHECK_EQ_INT(wake_policy_pulse(&p, t + 2000000 + 5000, true), WAKE_PULSE_NOW);
}

static void reset_ends_the_episode(void) {
  struct wake_policy p;
  wake_policy_suspend(&p, 0);
  CHECK_EQ_INT(wake_policy_pulse(&p, 10000, true), WAKE_PULSE_NOW);
  wake_policy_reset(&p);
  CHECK(!p.suspended);
  CHECK_EQ_INT(p.pulses, 0);
  CHECK_EQ_INT(wake_policy_pulse(&p, 20000, true), WAKE_PULSE_NO);
}

int main(void) {
  RUN(decide_orders_reasons_by_usefulness);
  RUN(input_classification_is_total);
  RUN(pulse_refuses_when_not_suspended);
  RUN(pulse_waits_five_ms_after_suspend);
  RUN(pulses_are_spaced);
  RUN(implicit_pulses_are_capped_but_explicit_are_not);
  RUN(reset_ends_the_episode);
  return test_summary("wake_policy");
}
