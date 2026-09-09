#include "hold_button.h"
#include "test_support.h"

// Drive the machine with the debounced level at a given time.
#define STEP(b, p, t, f) hold_button_step(&(b), (p), (t), (f))

static void short_press_is_a_press(void) {
  struct hold_button b;
  hold_button_init(&b);
  CHECK_EQ_INT(STEP(b, true, 1000, NULL), HOLD_BUTTON_NONE);
  CHECK_EQ_INT(STEP(b, true, 1500, NULL), HOLD_BUTTON_NONE);  // 500 ms in
  CHECK_EQ_INT(STEP(b, true, 1999, NULL), HOLD_BUTTON_NONE);  // just under 1 s
  CHECK_EQ_INT(STEP(b, false, 2000, NULL), HOLD_BUTTON_SHORT_RELEASE);
  CHECK_EQ_INT(STEP(b, false, 2030, NULL), HOLD_BUTTON_NONE);
}

static void feedback_starts_at_one_second_and_fills_to_five(void) {
  struct hold_button b;
  uint8_t f = 99;
  hold_button_init(&b);
  CHECK_EQ_INT(STEP(b, true, 0, &f), HOLD_BUTTON_NONE);
  CHECK_EQ_INT(STEP(b, true, 999, &f), HOLD_BUTTON_NONE);
  CHECK_EQ_INT(f, 99);  // untouched until feedback
  CHECK_EQ_INT(STEP(b, true, 1000, &f), HOLD_BUTTON_FEEDBACK);
  CHECK_EQ_INT(f, 0);
  CHECK_EQ_INT(STEP(b, true, 3000, &f), HOLD_BUTTON_FEEDBACK);
  CHECK_EQ_INT(f, 127);  // halfway through the 1 s..5 s span
  CHECK_EQ_INT(STEP(b, true, 4999, &f), HOLD_BUTTON_FEEDBACK);
  CHECK_EQ_INT(f, 254);
}

static void release_during_feedback_cancels(void) {
  struct hold_button b;
  hold_button_init(&b);
  CHECK_EQ_INT(STEP(b, true, 0, NULL), HOLD_BUTTON_NONE);
  CHECK_EQ_INT(STEP(b, true, 3000, NULL), HOLD_BUTTON_FEEDBACK);
  CHECK_EQ_INT(STEP(b, false, 3030, NULL), HOLD_BUTTON_CANCEL);
  // A cancel is not a press: nothing else fires.
  CHECK_EQ_INT(STEP(b, false, 3060, NULL), HOLD_BUTTON_NONE);
}

static void arms_at_five_seconds_and_fires_on_release_only(void) {
  struct hold_button b;
  hold_button_init(&b);
  CHECK_EQ_INT(STEP(b, true, 10, NULL), HOLD_BUTTON_NONE);
  CHECK_EQ_INT(STEP(b, true, 5009, NULL), HOLD_BUTTON_FEEDBACK);
  // At 5 s: armed, not fired. The action is a restart and the button is the
  // boot strap; restarting with it held would land in the ROM downloader.
  CHECK_EQ_INT(STEP(b, true, 5010, NULL), HOLD_BUTTON_ARMED);
  CHECK_EQ_INT(STEP(b, true, 9000, NULL), HOLD_BUTTON_ARMED);  // still armed
  // Release is the trigger, exactly once.
  CHECK_EQ_INT(STEP(b, false, 9030, NULL), HOLD_BUTTON_FIRE);
  CHECK_EQ_INT(STEP(b, false, 9060, NULL), HOLD_BUTTON_NONE);
  // And the next hold starts clean.
  CHECK_EQ_INT(STEP(b, true, 9090, NULL), HOLD_BUTTON_NONE);
  CHECK_EQ_INT(STEP(b, false, 9120, NULL), HOLD_BUTTON_SHORT_RELEASE);
}

static void clock_wrap_is_a_non_event(void) {
  struct hold_button b;
  uint8_t f = 0;
  hold_button_init(&b);
  const uint32_t t0 = 0xFFFFFFFFu - 2000u;
  CHECK_EQ_INT(STEP(b, true, t0, &f), HOLD_BUTTON_NONE);
  CHECK_EQ_INT(STEP(b, true, t0 + 1000u, &f),
               HOLD_BUTTON_FEEDBACK);                            // pre-wrap
  CHECK_EQ_INT(STEP(b, true, 1000u, &f), HOLD_BUTTON_FEEDBACK);  // 3001 ms held
  CHECK_EQ_INT(STEP(b, true, 3000u, &f), HOLD_BUTTON_ARMED);     // 5001 ms
  CHECK_EQ_INT(STEP(b, false, 3030u, &f), HOLD_BUTTON_FIRE);
}

int main(void) {
  RUN(short_press_is_a_press);
  RUN(feedback_starts_at_one_second_and_fills_to_five);
  RUN(release_during_feedback_cancels);
  RUN(arms_at_five_seconds_and_fires_on_release_only);
  RUN(clock_wrap_is_a_non_event);
  return test_summary("hold_button");
}
