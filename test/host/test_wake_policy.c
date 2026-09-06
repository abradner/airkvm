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

int main(void) {
  RUN(decide_orders_reasons_by_usefulness);
  return test_summary("wake_policy");
}
