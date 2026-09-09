// The R-006 debug-mode expiry.
//
// Only the decision is tested here, not the plumbing: status.c needs FreeRTOS
// and esp_timer, but the question "has debug mode been on long enough to turn
// itself off" is pure arithmetic and belongs where it can be checked without a
// board — same split as oled_geometry.h.
#include "status.h"
#include "test_support.h"

#define US_PER_MS 1000LL
#define TIMEOUT_US ((int64_t)STATUS_DEBUG_TIMEOUT_MS * US_PER_MS)

static void not_expired_before_the_deadline(void) {
  CHECK(!status_debug_is_expired(0, 0, STATUS_DEBUG_TIMEOUT_MS));
  CHECK(!status_debug_is_expired(0, 1, STATUS_DEBUG_TIMEOUT_MS));
  CHECK(!status_debug_is_expired(0, TIMEOUT_US - 1, STATUS_DEBUG_TIMEOUT_MS));
  // A minute in, on a five-minute timeout, is nowhere near.
  CHECK(!status_debug_is_expired(0, 60 * 1000 * US_PER_MS,
                                 STATUS_DEBUG_TIMEOUT_MS));
}

static void expired_at_and_after_the_deadline(void) {
  // The boundary itself expires — a row that shows keystrokes should err
  // toward off, the same way principle 4 errs toward releasing keys.
  CHECK(status_debug_is_expired(0, TIMEOUT_US, STATUS_DEBUG_TIMEOUT_MS));
  CHECK(status_debug_is_expired(0, TIMEOUT_US + 1, STATUS_DEBUG_TIMEOUT_MS));
  CHECK(status_debug_is_expired(0, 24LL * 3600 * 1000 * US_PER_MS,
                                STATUS_DEBUG_TIMEOUT_MS));
}

static void deadline_is_relative_to_when_it_was_enabled(void) {
  // Enabling late in the device's life must not read as instantly expired,
  // which is what a comparison against raw uptime would do.
  const int64_t late = 6LL * 3600 * 1000 * US_PER_MS;  // six hours up
  CHECK(!status_debug_is_expired(late, late, STATUS_DEBUG_TIMEOUT_MS));
  CHECK(!status_debug_is_expired(late, late + TIMEOUT_US - 1,
                                 STATUS_DEBUG_TIMEOUT_MS));
  CHECK(status_debug_is_expired(late, late + TIMEOUT_US,
                                STATUS_DEBUG_TIMEOUT_MS));
}

static void clock_going_backwards_reads_as_not_expired(void) {
  // esp_timer_get_time() is monotonic so this should not arise; assert the
  // safe direction anyway, because the unsafe one silently disables the row
  // rather than silently keeping it up.
  CHECK(!status_debug_is_expired(TIMEOUT_US * 2, TIMEOUT_US,
                                 STATUS_DEBUG_TIMEOUT_MS));
}

static void timeout_is_a_sane_duration(void) {
  // Long enough to answer "is my input reaching the target", short enough that
  // it does not outlive the operator standing there.
  CHECK(STATUS_DEBUG_TIMEOUT_MS >= 60 * 1000);
  CHECK(STATUS_DEBUG_TIMEOUT_MS <= 15 * 60 * 1000);
}

// R-010: the console may shorten the window and never lengthen it. The clamp
// is the enforcement (status.h); this pins that the ceiling IS the default,
// so "shorten-only" is a property of one number rather than of two agreeing.
static void the_timeout_can_only_be_shortened(void) {
  CHECK_EQ_INT(status_debug_timeout_clamp(STATUS_DEBUG_TIMEOUT_MS),
               STATUS_DEBUG_TIMEOUT_MS);
  CHECK_EQ_INT(status_debug_timeout_clamp(STATUS_DEBUG_TIMEOUT_MS + 1),
               STATUS_DEBUG_TIMEOUT_MS);
  CHECK_EQ_INT(status_debug_timeout_clamp(0xFFFFFFFFu),
               STATUS_DEBUG_TIMEOUT_MS);
  CHECK_EQ_INT(status_debug_timeout_clamp(10 * 1000), 10 * 1000);
  // And a shorter window really is shorter: expired at 10 s, not at 5 min.
  CHECK(status_debug_is_expired(0, 10 * 1000 * US_PER_MS, 10 * 1000));
  CHECK(!status_debug_is_expired(0, 10 * 1000 * US_PER_MS - 1, 10 * 1000));
}

int main(void) {
  RUN(the_timeout_can_only_be_shortened);
  RUN(not_expired_before_the_deadline);
  RUN(expired_at_and_after_the_deadline);
  RUN(deadline_is_relative_to_when_it_was_enabled);
  RUN(clock_going_backwards_reads_as_not_expired);
  RUN(timeout_is_a_sane_duration);
  return test_summary("status_debug");
}
