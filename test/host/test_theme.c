// Tests for theme.h — the panel's colour themes.
//
// This suite exists because of a specific failure: the first coloured build
// hardcoded Solarized Dark, looked fine in a hex editor, and read as too low
// contrast on the actual panel. Nothing in the build could have caught that,
// because "is this legible" was never a number anywhere. It is now, and this
// is where the number is checked — so a future theme, or a future tweak to an
// existing one, cannot reintroduce the same class of bug by being transcribed
// from a screenshot.
//
// Everything here is computed on the RGB565-QUANTISED colour, which is what
// the panel can actually show. Asserting against the source hex would pass on
// a colour the hardware cannot produce.
#include <math.h>

#include "test_support.h"
#include "theme.h"

// WCAG 2.x floors. Text is unantialiased 8x16 on a 172x320 panel read at arm's
// length, which is NOT the case WCAG was calibrated for, so these are a floor
// and not a target — theme.h records the actual ratios, which are mostly well
// above.
//
// 4.5 is the normal-text minimum (AA). The 8x16 cell is under WCAG's
// large-text threshold, so the large-text 3:1 does not apply to the two text
// roles.
#define MIN_TEXT_CONTRAST 4.5
// 3.0 is the non-text minimum, and the RSSI bar is the one non-text element on
// the panel (ui.c draws nothing else in ACCENT).
#define MIN_ACCENT_CONTRAST 3.0

// RGB565 back to 8 bits per channel the way a panel does it: replicate the
// high bits into the low ones, so 0x1F becomes 0xFF rather than 0xF8.
static void expand565(uint16_t c, double out[3]) {
  const unsigned r5 = (c >> 11) & 0x1Fu;
  const unsigned g6 = (c >> 5) & 0x3Fu;
  const unsigned b5 = c & 0x1Fu;
  out[0] = (double)((r5 << 3) | (r5 >> 2));
  out[1] = (double)((g6 << 2) | (g6 >> 4));
  out[2] = (double)((b5 << 3) | (b5 >> 2));
}

static double luminance(uint16_t c) {
  double ch[3];
  expand565(c, ch);
  for (int i = 0; i < 3; i++) {
    const double v = ch[i] / 255.0;
    ch[i] = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
  }
  return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}

static double contrast(uint16_t a, uint16_t b) {
  const double la = luminance(a);
  const double lb = luminance(b);
  const double hi = la > lb ? la : lb;
  const double lo = la > lb ? lb : la;
  return (hi + 0.05) / (lo + 0.05);
}

// Positive control for the metric itself. Every assertion below is of the form
// "this ratio is big enough", and a luminance function that returned a
// constant, or a contrast function that returned 21, would satisfy all of them
// while measuring nothing. So pin the two ends and one known middle first: if
// this test fails, no other result in this file means anything.
static void contrast_metric_is_sane(void) {
  const uint16_t black = RGB565(0x00, 0x00, 0x00);
  const uint16_t white = RGB565(0xFF, 0xFF, 0xFF);

  // The two extremes of the scale, exactly.
  CHECK(fabs(contrast(black, white) - 21.0) < 0.001);
  // A colour against itself has no contrast at all.
  CHECK(fabs(contrast(white, white) - 1.0) < 0.001);
  CHECK(fabs(contrast(black, black) - 1.0) < 0.001);
  // Symmetric: which one is "foreground" cannot change the answer.
  CHECK(fabs(contrast(black, white) - contrast(white, black)) < 1e-12);

  // A known middle value, so a function that only got the endpoints right is
  // still caught — and a deliberately awkward one. #808080 on black is 5.32:1
  // by the WCAG formula, but 5.51:1 here, because RGB565 cannot store #808080:
  // it quantises to (132,130,132), which is 3.6% more contrast than the colour
  // that was asked for. 5.32 is the number a reader will expect and the wrong
  // one; asserting 5.51 is what pins this file to what the PANEL shows rather
  // than to what the source hex says, which is the entire premise here.
  CHECK(fabs(contrast(RGB565(0x80, 0x80, 0x80), black) - 5.51) < 0.01);

  // And the quantisation is genuinely being applied: 0x01 has no bit left in
  // RGB565's 5-bit red channel, so it must land on the same colour as 0x00.
  CHECK(RGB565(0x01, 0x01, 0x01) == RGB565(0x00, 0x00, 0x00));
}

// The bug this whole file exists for, held as a fact rather than as a comment.
//
// Solarized base01 is the scheme's COMMENT tone, and it was used for ui.c's
// SECONDARY role, which draws labels, the SSID, the mDNS name and the target
// row — content, not comments. That is the misassignment that shipped, and
// this asserts it really was under the floor, so the floor is demonstrably
// sensitive enough to have caught it.
static void the_original_solarized_secondary_would_fail(void) {
  const uint16_t bg = RGB565_OF(SOL_BASE03);
  const uint16_t base01 = RGB565_OF(SOL_BASE01);
  const uint16_t base0 = RGB565_OF(SOL_BASE0);

  CHECK(contrast(base01, bg) < MIN_TEXT_CONTRAST);
  // ...and that the replacement clears it, from the same scheme.
  CHECK(contrast(base0, bg) >= MIN_TEXT_CONTRAST);
  // The shipped theme uses the corrected one.
  CHECK(
      display_themes[DISPLAY_THEME_SOLARIZED_DARK].ink[CANVAS_INK_SECONDARY] ==
      base0);
}

static void every_theme_is_complete_and_named(void) {
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    const struct display_theme *t = &display_themes[i];
    // A designated initialiser missed for one enum value leaves a zeroed
    // entry, which is a valid-looking all-black theme. The name catches it.
    CHECK(t->name != NULL);
    if (t->name == NULL) {
      continue;
    }
    CHECK(t->name[0] != '\0');
    for (const char *c = t->name; *c; c++) {
      // Kebab-case, because the name is the Kconfig option's identity and is
      // logged at init; a stray space or capital makes it a different string
      // to whatever reads it back.
      CHECK((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-');
    }
    for (int j = 0; j < i; j++) {
      CHECK(strcmp(display_themes[j].name, t->name) != 0);
    }
  }
}

static void every_theme_clears_the_contrast_floor(void) {
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    const struct display_theme *t = &display_themes[i];
    const uint16_t bg = t->ink[CANVAS_INK_NONE];
    const double pri = contrast(t->ink[CANVAS_INK_PRIMARY], bg);
    const double sec = contrast(t->ink[CANVAS_INK_SECONDARY], bg);
    const double acc = contrast(t->ink[CANVAS_INK_ACCENT], bg);

    if (pri < MIN_TEXT_CONTRAST || sec < MIN_TEXT_CONTRAST ||
        acc < MIN_ACCENT_CONTRAST) {
      // Print before failing: "which theme, which role, by how much" is the
      // whole diagnostic, and a bare CHECK line gives none of it.
      fprintf(stderr, "  theme %s: primary %.2f secondary %.2f accent %.2f\n",
              t->name, pri, sec, acc);
    }
    CHECK(pri >= MIN_TEXT_CONTRAST);
    CHECK(sec >= MIN_TEXT_CONTRAST);
    CHECK(acc >= MIN_ACCENT_CONTRAST);
  }
}

// The roles are a HIERARCHY, not three interchangeable colours: ui.c draws the
// IP address in PRIMARY and everything else in SECONDARY specifically so the
// address is the thing read first. A theme whose secondary out-contrasts its
// primary inverts that, still clears every floor above, and looks merely
// "wrong" on hardware in a way nobody would trace back to a colour table.
// A hue is SECONDARY text in a different colour, so it is text, so it is held
// to the text floor. This is the assertion that stops "a little more
// personality" from quietly undoing the change this whole file exists for: it
// is exactly the pressure that produced the 2.87:1 row in the first place.
static void every_hue_clears_the_text_floor(void) {
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    const struct display_theme *t = &display_themes[i];
    const uint16_t bg = t->ink[CANVAS_INK_NONE];
    for (int h = 0; h < DISPLAY_HUE_COUNT; h++) {
      const double c = contrast(t->hue[h], bg);
      if (c < MIN_TEXT_CONTRAST) {
        fprintf(stderr, "  theme %s: hue %d is %.2f, under the %.1f floor\n",
                t->name, h, c, MIN_TEXT_CONTRAST);
      }
      CHECK(c >= MIN_TEXT_CONTRAST);
    }
  }
}

// NEUTRAL is not a colour a theme chooses, it is the theme's own SECONDARY.
// Holding that structurally is what lets "this theme has no hue it can carry
// for this section" be expressed by assignment rather than by a sentinel the
// backend would have to branch on — and monokai-light and solarized-dark both
// genuinely need to express it.
static void the_neutral_hue_is_the_theme_secondary(void) {
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    const struct display_theme *t = &display_themes[i];
    CHECK(t->hue[DISPLAY_HUE_NEUTRAL] == t->ink[CANVAS_INK_SECONDARY]);
  }
}

// A theme that declines a hue is fine; a theme that declines all of them is a
// theme with no personality, which is what this change exists to fix. Checked
// per theme so the failure names the one that went flat — with monokai-light
// exempt, because a near-white background genuinely cannot carry a second
// legible hue and pretending otherwise is how the floor gets lowered.
static void every_dark_theme_carries_at_least_one_hue(void) {
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    if (i == DISPLAY_THEME_MONOKAI_LIGHT) {
      continue;
    }
    const struct display_theme *t = &display_themes[i];
    int distinct = 0;
    for (int h = DISPLAY_HUE_NEUTRAL + 1; h < DISPLAY_HUE_COUNT; h++) {
      if (t->hue[h] != t->ink[CANVAS_INK_SECONDARY]) {
        distinct++;
      }
    }
    if (distinct == 0) {
      fprintf(stderr, "  theme %s has no hue distinct from its secondary\n",
              t->name);
    }
    CHECK(distinct > 0);
  }
}

static void primary_outranks_secondary_in_every_theme(void) {
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    const struct display_theme *t = &display_themes[i];
    const uint16_t bg = t->ink[CANVAS_INK_NONE];
    const double pri = contrast(t->ink[CANVAS_INK_PRIMARY], bg);
    const double sec = contrast(t->ink[CANVAS_INK_SECONDARY], bg);
    if (pri <= sec) {
      fprintf(stderr,
              "  theme %s: primary %.2f does not outrank secondary %.2f\n",
              t->name, pri, sec);
    }
    CHECK(pri > sec);
  }
}

// Two roles that resolve to the same colour are a theme with three roles, and
// the one place it shows is the RSSI bar disappearing into the text next to
// it. Cheap to assert, impossible to see in a hex table.
static void every_role_in_a_theme_is_distinct(void) {
  for (int i = 0; i < DISPLAY_THEME_COUNT; i++) {
    const struct display_theme *t = &display_themes[i];
    for (int a = 0; a < 4; a++) {
      for (int b = a + 1; b < 4; b++) {
        CHECK(t->ink[a] != t->ink[b]);
      }
    }
  }
}

// The build-time selection has to name a theme that exists. An enum value out
// of range would index past the table and draw from whatever follows it.
static void the_selected_theme_is_in_range(void) {
  CHECK(DISPLAY_THEME_SELECTED >= 0);
  CHECK(DISPLAY_THEME_SELECTED < DISPLAY_THEME_COUNT);
  CHECK(display_themes[DISPLAY_THEME_SELECTED].name != NULL);
}

// theme.h quotes a ratio table in its header comment, and a comment that
// drifts from the code is worse than none (AGENTS.md principle 1). These are
// the five numbers it quotes; if a colour moves, this fails and the comment
// has to move with it.
static void the_documented_ratios_are_the_real_ones(void) {
  static const struct {
    int id;
    double primary, secondary, accent;
  } documented[] = {
      {DISPLAY_THEME_TOKYO_NIGHT, 11.15, 8.49, 10.25},
      {DISPLAY_THEME_MONOKAI_MACHINE, 13.52, 7.93, 8.43},
      {DISPLAY_THEME_ONE_DARK_NIGHT_FLAT, 8.47, 4.83, 7.57},
      {DISPLAY_THEME_MONOKAI_LIGHT, 14.43, 5.02, 3.61},
      {DISPLAY_THEME_SOLARIZED_DARK, 5.91, 5.02, 5.00},
  };
  CHECK_EQ_INT(sizeof(documented) / sizeof(documented[0]), DISPLAY_THEME_COUNT);

  for (size_t i = 0; i < sizeof(documented) / sizeof(documented[0]); i++) {
    const struct display_theme *t = &display_themes[documented[i].id];
    const uint16_t bg = t->ink[CANVAS_INK_NONE];
    const double pri = contrast(t->ink[CANVAS_INK_PRIMARY], bg);
    const double sec = contrast(t->ink[CANVAS_INK_SECONDARY], bg);
    const double acc = contrast(t->ink[CANVAS_INK_ACCENT], bg);
    if (fabs(pri - documented[i].primary) > 0.005 ||
        fabs(sec - documented[i].secondary) > 0.005 ||
        fabs(acc - documented[i].accent) > 0.005) {
      fprintf(
          stderr,
          "  theme %s: measured %.2f/%.2f/%.2f, theme.h says %.2f/%.2f/%.2f\n",
          t->name, pri, sec, acc, documented[i].primary,
          documented[i].secondary, documented[i].accent);
    }
    CHECK(fabs(pri - documented[i].primary) <= 0.005);
    CHECK(fabs(sec - documented[i].secondary) <= 0.005);
    CHECK(fabs(acc - documented[i].accent) <= 0.005);
  }
}

int main(void) {
  RUN(contrast_metric_is_sane);
  RUN(the_original_solarized_secondary_would_fail);
  RUN(every_theme_is_complete_and_named);
  RUN(every_theme_clears_the_contrast_floor);
  RUN(every_hue_clears_the_text_floor);
  RUN(the_neutral_hue_is_the_theme_secondary);
  RUN(every_dark_theme_carries_at_least_one_hue);
  RUN(primary_outranks_secondary_in_every_theme);
  RUN(every_role_in_a_theme_is_distinct);
  RUN(the_selected_theme_is_in_range);
  RUN(the_documented_ratios_are_the_real_ones);
  return test_summary("theme");
}
