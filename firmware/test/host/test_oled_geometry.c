#include <stdbool.h>

#include "oled_geometry.h"
#include "test_support.h"

// This exact arithmetic already cost one wasted flash-and-observe cycle to
// get right on real hardware (a rotated panel showing noise from unwritten
// pages, not a wiring fault) — see main/display.c's comment above
// OLED_PAGE_OFFSET. These are the specific board configs in the tree, plus a
// case with margin on both sides to exercise the general formula rather than
// just the boards' own zero-margin geometry.

static void c3_bench_board_unrotated(void) {
  // C3: 8 total pages, 5-page (40px) window, un-rotated, offset 3 pages
  // (24px) from page 0 — the offset passes through unchanged when not
  // rotating.
  CHECK_EQ_INT(OLED_PAGE_OFFSET_FOR(8, 5, 3, false), 3);
}

static void s3_dongle_rotated(void) {
  // S3: same 8/5/3 geometry as the C3, but this panel is mounted 180° from
  // what the layout assumes. The window sits flush against one end of the
  // page space (zero margin above, all the margin below) — rotating swaps
  // which end that margin is on, landing the compensated offset at 0, not 3.
  CHECK_EQ_INT(OLED_PAGE_OFFSET_FOR(8, 5, 3, true), 0);
}

static void centered_window_is_unaffected_by_rotation(void) {
  // A window with equal margin on both sides (8 total, 4-page window, 2-page
  // offset — 2 pages of margin on each end) is its own mirror image, so
  // rotating changes nothing: 8-4-2 == 2.
  CHECK_EQ_INT(OLED_PAGE_OFFSET_FOR(8, 4, 2, false), 2);
  CHECK_EQ_INT(OLED_PAGE_OFFSET_FOR(8, 4, 2, true), 2);
}

static void asymmetric_window_with_margin_on_both_sides(void) {
  // A window that isn't flush against either end (8 total, 3-page window, at
  // offset 1 — 1 page of margin above, 4 below) should land its rotated
  // mirror at the far side: 8-3-1 == 4.
  CHECK_EQ_INT(OLED_PAGE_OFFSET_FOR(8, 3, 1, false), 1);
  CHECK_EQ_INT(OLED_PAGE_OFFSET_FOR(8, 3, 1, true), 4);
}

int main(void) {
  RUN(c3_bench_board_unrotated);
  RUN(s3_dongle_rotated);
  RUN(centered_window_is_unaffected_by_rotation);
  RUN(asymmetric_window_with_margin_on_both_sides);
  return test_summary("oled_geometry");
}
