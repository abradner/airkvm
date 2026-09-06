#include "wake_policy.h"

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
