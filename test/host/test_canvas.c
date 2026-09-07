// Tests for canvas.h — the shared 1bpp drawing layer.
//
// The point of the golden below is narrow and specific: canvas.h was extracted
// from display.c, and the 0.42" dongle's rendering was confirmed legible on
// physical hardware on 2026-08-31. An extraction that quietly changed a glyph
// would still build, still run, and only show up as a slightly wrong character
// on a 9mm screen. So the golden was produced by compiling the ORIGINAL
// display.c rendering path (lifted from git before the change) and capturing
// its framebuffer — it is an independent oracle, not this implementation's own
// output blessed after the fact.
#include "canvas.h"
#include "test_support.h"

#define OLED_W 72
#define OLED_H 40

static uint8_t g_bits[OLED_W * ((OLED_H + 7) / 8)];
static struct canvas g_oled = {
    .bits = g_bits,
    .w = OLED_W,
    .h = OLED_H,
    .font = &canvas_font3x5,
};

// Byte-for-byte output of the pre-extraction display.c for the draw calls in
// oled_render_matches_pre_extraction_golden() below. Regenerating this by
// running the new code would defeat its entire purpose.
static const uint8_t k_golden_oled[sizeof(g_bits)] = {
    0x1F, 0x85, 0x1F, 0x00, 0x91, 0x9F, 0x91, 0x00, 0x9F, 0x85, 0x97, 0x00,
    0x1F, 0x04, 0x1B, 0x00, 0x0F, 0x90, 0x0F, 0x00, 0x9F, 0x82, 0x9F, 0x00,
    0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x80, 0x00, 0x1F, 0x15, 0x1E, 0x00,
    0x1F, 0x91, 0x1F, 0x00, 0x9F, 0x90, 0x9F, 0x00, 0x9F, 0x86, 0x9F, 0x00,
    0x1F, 0x11, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x86, 0xFF, 0x80, 0x00, 0x9F, 0x99, 0xFF, 0x00, 0xF9, 0x99, 0x9F, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x86, 0xFF, 0x80, 0x00, 0xFF, 0x99, 0xF9, 0x00,
    0xFF, 0x99, 0xFF, 0x00, 0x00, 0x80, 0x00, 0x00, 0x86, 0xFF, 0x80, 0x00,
    0xFF, 0x81, 0xFF, 0x00, 0xFF, 0x81, 0xFF, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x86, 0xFF, 0x80, 0x00, 0xFF, 0x81, 0xFF, 0x00, 0xFF, 0x81, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x7D, 0x09, 0x7D, 0x00, 0x0D, 0x71, 0x0D, 0x00, 0x11, 0x11, 0x11, 0x00,
    0x7C, 0x21, 0x7C, 0x00, 0x45, 0x7D, 0x45, 0x00, 0x7D, 0x15, 0x15, 0x00,
    0x45, 0x7D, 0x45, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00,
    0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x7D, 0x7D, 0x7D, 0x00,
    0x7C, 0x7C, 0x7C, 0x00, 0x7C, 0x44, 0x7C, 0x00, 0x7C, 0x44, 0x7C, 0x00,
    0xDF, 0x10, 0xDF, 0x00, 0xD7, 0x55, 0x5D, 0x00, 0xDF, 0x15, 0xDE, 0x00,
    0xC0, 0x40, 0xC0, 0x00, 0x5F, 0x51, 0xDF, 0x00, 0x5F, 0x44, 0xDB, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x1F, 0x10, 0x00,
    0x1D, 0x15, 0x17, 0x00, 0x15, 0x15, 0x1F, 0x00, 0x07, 0x04, 0x1F, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x01, 0x06, 0x00, 0x07, 0x05, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00,
    0x07, 0x01, 0x07, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Mirrors what ui.c's draw() actually asks for, plus the awkward cases: a
// string long enough to clip, lowercase (which this font folds), a character
// outside the table, and the last glyph in it.
static void render_reference_screen(struct canvas *c) {
  canvas_clear(c);
  canvas_text(c, 0 * 4, 0 * 6, "AIRKVM", 1, 1);
  canvas_text(c, 11 * 4, 0 * 6, "BOUND", 1, 1);
  canvas_text(c, 0 * 4, 7, "192.168.100.100", 1, 2);
  canvas_text(c, 0 * 4, 3 * 6, "my-wifi", 1, 1);
  canvas_bar(c, 14, 3, 4, 2, 1, 1);
  canvas_text(c, 0 * 4, 4 * 6, "USB OK  1234", 1, 1);
  canvas_text(c, 0 * 4, 5 * 6, "KeyA?~", 1, 1);
}

static void oled_render_matches_pre_extraction_golden(void) {
  render_reference_screen(&g_oled);
  int mismatches = 0;
  for (size_t i = 0; i < sizeof(g_bits); i++) {
    if (g_bits[i] != k_golden_oled[i]) {
      if (mismatches < 5) {
        fprintf(stderr, "    byte %zu: got 0x%02X want 0x%02X\n", i, g_bits[i],
                k_golden_oled[i]);
      }
      mismatches++;
    }
  }
  CHECK_EQ_INT(mismatches, 0);

  // Positive control: a golden of all zeroes would also "match" a renderer
  // that drew nothing, so assert the reference screen actually put ink down.
  int ink = 0;
  for (size_t i = 0; i < sizeof(g_bits); i++) {
    if (g_bits[i] != 0) {
      ink++;
    }
  }
  CHECK(ink > 100);
}

static bool pixel_at(const struct canvas *c, int x, int y) {
  if (x < 0 || x >= c->w || y < 0 || y >= c->h) {
    return false;
  }
  return (c->bits[(y / 8) * c->w + x] & (1u << (y % 8))) != 0;
}

// The ink ROLE at a pixel, reassembled from the two planes the way
// display_st7789.c's flush does. pixel_at above answers "is it lit", which is
// the only question the monochrome board has; anything checking colour has to
// read both planes or it cannot tell SECONDARY from ACCENT.
static uint8_t role_at(const struct canvas *c, int x, int y) {
  if (x < 0 || x >= c->w || y < 0 || y >= c->h) {
    return CANVAS_INK_NONE;
  }
  const size_t off = (size_t)(y / 8) * (size_t)c->w + (size_t)x;
  const uint8_t bit = (uint8_t)(1u << (y % 8));
  uint8_t role = (c->bits[off] & bit) ? 1u : 0u;
  if (c->bits_hi != NULL && (c->bits_hi[off] & bit)) {
    role |= 2u;
  }
  return role;
}

// The only new branch the second board introduces into shared code.
static void scaling_replicates_each_pixel_as_a_block(void) {
  static uint8_t one_bits[64 * 8];
  static uint8_t big_bits[64 * 8];
  struct canvas one = {
      .bits = one_bits, .w = 64, .h = 64, .font = &canvas_font3x5};
  struct canvas big = {
      .bits = big_bits, .w = 64, .h = 64, .font = &canvas_font3x5};
  canvas_clear(&one);
  canvas_clear(&big);
  canvas_text(&one, 0, 0, "A", 1, 1);
  canvas_text(&big, 0, 0, "A", 3, 3);

  for (int y = 0; y < 5; y++) {
    for (int x = 0; x < 3; x++) {
      const bool want = pixel_at(&one, x, y);
      for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
          CHECK(pixel_at(&big, x * 3 + dx, y * 3 + dy) == want);
        }
      }
    }
  }
}

// 172 is the first panel height in this tree that is not a whole number of
// 8-row pages (21.5), so the partial last page is a branch no existing board
// exercises. Getting it wrong overruns the buffer rather than misdrawing.
static void non_page_aligned_height_does_not_overrun(void) {
  enum { W = 320, H = 172, GUARD = 16 };
  static uint8_t buf[W * ((H + 7) / 8) + GUARD];
  memset(buf, 0xA5, sizeof(buf));
  struct canvas c = {.bits = buf, .w = W, .h = H, .font = &canvas_font3x5};

  CHECK_EQ_INT(canvas_pages(H), 22);
  CHECK_EQ_INT(canvas_bytes(W, H), W * 22);

  canvas_clear(&c);
  // Every row including the last, and one past it.
  for (int y = 0; y <= H; y++) {
    canvas_pixel(&c, 0, y, true);
    canvas_pixel(&c, W - 1, y, true);
  }
  canvas_text(&c, 0, H - 8, "EDGE", 1, 1);

  for (int i = 0; i < GUARD; i++) {
    CHECK_EQ_INT(buf[W * 22 + i], 0xA5);
  }
  CHECK(pixel_at(&c, 0, H - 1));
  CHECK(!pixel_at(&c, 0, H));
}

static void text_clips_and_never_wraps(void) {
  canvas_clear(&g_oled);
  // 18 columns fit; the 19th character must not appear on row 1.
  canvas_text(&g_oled, 0, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 1, 1);
  bool row1_ink = false;
  for (int x = 0; x < OLED_W; x++) {
    for (int y = 6; y < 12; y++) {
      row1_ink = row1_ink || pixel_at(&g_oled, x, y);
    }
  }
  CHECK(!row1_ink);
}

static void bar_keeps_an_outline_when_empty(void) {
  canvas_clear(&g_oled);
  canvas_bar(&g_oled, 0, 0, 4, 0, 1, 1);
  // Nothing filled, but each cell keeps its border so the scale stays visible.
  CHECK(pixel_at(&g_oled, 0, 0));
  CHECK(pixel_at(&g_oled, 2, 0));
  CHECK(pixel_at(&g_oled, 0, 4));
  CHECK(!pixel_at(&g_oled, 1, 2));  // interior of an unfilled cell
  canvas_clear(&g_oled);
  canvas_bar(&g_oled, 0, 0, 4, 1, 1, 1);
  CHECK(pixel_at(&g_oled, 1, 2));  // interior of a filled cell
}

// The bar has to read as separate cells on BOTH fonts. It does so for free on
// the 3x5 (glyph 3 on a 4 pitch); the 8x16 is 8 on 8, so without an explicit
// inset every cell border merges into one solid block — visible only by
// looking at a rendered screen, which is why it is pinned here.
static void bar_cells_stay_separated_on_both_fonts(void) {
  static uint8_t bits[320 * 22];
  struct canvas c = {
      .bits = bits, .w = 320, .h = 172, .font = &canvas_font8x16};
  canvas_clear(&c);
  canvas_bar(&c, 0, 0, 4, 4, 1, 1);
  // Column 7 is the last of the first cell's pitch; it must be blank so the
  // next cell starts against a gap rather than sharing an edge.
  bool gap_col_clear = true;
  for (int y = 0; y < canvas_font8x16.glyph_h; y++) {
    gap_col_clear =
        gap_col_clear && !pixel_at(&c, canvas_font8x16.advance - 1, y);
  }
  CHECK(gap_col_clear);
  // Positive control: the cell either side of that gap is drawn.
  CHECK(pixel_at(&c, 0, 0));
  CHECK(pixel_at(&c, canvas_font8x16.advance, 0));
}

// The stepped signal gauge (canvas_bar_steps). Shape carries the reading here,
// not just colour — which is what lets the light theme's bar lean on WCAG's 3:1
// graphical floor instead of 4.5 — so the shape is worth pinning.
static void bar_steps_ascend_and_sit_on_the_baseline(void) {
  static uint8_t lo[320 * 22], hi[320 * 22];
  struct canvas c = {.bits = lo,
                     .bits_hi = hi,
                     .w = 320,
                     .h = 172,
                     .font = &canvas_font8x16,
                     .ink = CANVAS_INK_ACCENT};
  canvas_clear(&c);
  canvas_bar_steps(&c, 0, 0, 4, 4, 1, 1, CANVAS_INK_SECONDARY);

  const int full = canvas_font8x16.glyph_h;  // 16
  int height[4];
  for (int i = 0; i < 4; i++) {
    const int x = i * canvas_font8x16.advance;
    height[i] = 0;
    for (int y = 0; y < full; y++) {
      if (role_at(&c, x, y) != CANVAS_INK_NONE) {
        height[i]++;
      }
    }
    // Bottom-aligned: the pixel directly above the row's baseline is set, the
    // one at the top of the cell is not (for anything but a full-height step).
    CHECK(role_at(&c, x, full - 1) != CANVAS_INK_NONE);
  }
  // Strictly ascending, and the last step fills the cell.
  CHECK(height[0] < height[1]);
  CHECK(height[1] < height[2]);
  CHECK(height[2] < height[3]);
  CHECK_EQ_INT(height[3], full);
  // The shortest step is never invisible — "one bar" must not look like "none".
  CHECK(height[0] >= 1);
}

// Filled steps take the pen; unfilled take ink_off. This is the "bold and
// feint" that keeps the empty part of the scale visible: an unfilled step must
// be present but quiet, never absent (absent reads as "no data") and never the
// pen colour (which reads as reached).
static void bar_steps_use_two_inks_and_restore_the_pen(void) {
  static uint8_t lo[320 * 22], hi[320 * 22];
  struct canvas c = {.bits = lo,
                     .bits_hi = hi,
                     .w = 320,
                     .h = 172,
                     .font = &canvas_font8x16,
                     .ink = CANVAS_INK_ACCENT};
  canvas_clear(&c);
  canvas_bar_steps(&c, 0, 0, 4, 2, 1, 1, CANVAS_INK_SECONDARY);

  const int full = canvas_font8x16.glyph_h;
  // Steps 0 and 1 are filled: pen ink.
  CHECK_EQ_INT(role_at(&c, 0, full - 1), CANVAS_INK_ACCENT);
  CHECK_EQ_INT(role_at(&c, canvas_font8x16.advance, full - 1),
               CANVAS_INK_ACCENT);
  // Steps 2 and 3 are not: present, but in the quiet ink.
  CHECK_EQ_INT(role_at(&c, 2 * canvas_font8x16.advance, full - 1),
               CANVAS_INK_SECONDARY);
  CHECK_EQ_INT(role_at(&c, 3 * canvas_font8x16.advance, full - 1),
               CANVAS_INK_SECONDARY);

  // The pen is restored, or the next draw silently changes colour — the sort
  // of bug that shows up three call sites away from its cause.
  CHECK_EQ_INT(c.ink, CANVAS_INK_ACCENT);
}

// Zero filled is a real state (associated, no RSSI sample yet) and must still
// draw the whole scale, quietly. An empty gauge and a missing gauge are
// different things.
static void bar_steps_with_nothing_filled_still_draws_the_scale(void) {
  static uint8_t lo[320 * 22], hi[320 * 22];
  struct canvas c = {.bits = lo,
                     .bits_hi = hi,
                     .w = 320,
                     .h = 172,
                     .font = &canvas_font8x16,
                     .ink = CANVAS_INK_ACCENT};
  canvas_clear(&c);
  canvas_bar_steps(&c, 0, 0, 4, 0, 1, 1, CANVAS_INK_SECONDARY);

  const int full = canvas_font8x16.glyph_h;
  for (int i = 0; i < 4; i++) {
    CHECK_EQ_INT(role_at(&c, i * canvas_font8x16.advance, full - 1),
                 CANVAS_INK_SECONDARY);
  }
  // Positive control: nothing was drawn in the pen ink anywhere.
  bool any_accent = false;
  for (int x = 0; x < 4 * canvas_font8x16.advance; x++) {
    for (int y = 0; y < full; y++) {
      any_accent = any_accent || role_at(&c, x, y) == CANVAS_INK_ACCENT;
    }
  }
  CHECK(!any_accent);
}

// Two planes give four roles, and a canvas with no second plane must collapse
// every ink to "lit" rather than dropping pixels — that is what keeps one
// draw() serving a colour panel and a monochrome one.
static void ink_planes_encode_four_roles(void) {
  static uint8_t lo[64 * 8], hi[64 * 8];
  struct canvas c = {.bits = lo,
                     .bits_hi = hi,
                     .w = 64,
                     .h = 64,
                     .font = &canvas_font3x5,
                     .ink = CANVAS_INK_PRIMARY};
  canvas_clear(&c);
  const uint8_t inks[] = {CANVAS_INK_PRIMARY, CANVAS_INK_SECONDARY,
                          CANVAS_INK_ACCENT};
  for (int i = 0; i < 3; i++) {
    canvas_set_ink(&c, inks[i]);
    canvas_pixel(&c, i, 0, true);
  }
  for (int i = 0; i < 3; i++) {
    CHECK_EQ_INT(canvas_ink_at(&c, i, 0), inks[i]);
  }
  // Clearing a pixel returns it to NONE whatever ink drew it.
  canvas_set_ink(&c, CANVAS_INK_ACCENT);
  canvas_pixel(&c, 2, 0, false);
  CHECK_EQ_INT(canvas_ink_at(&c, 2, 0), CANVAS_INK_NONE);
}

static void mono_canvas_collapses_every_ink_to_lit(void) {
  static uint8_t lo[64 * 8];
  struct canvas c = {.bits = lo,
                     .bits_hi = NULL,  // single-colour panel
                     .w = 64,
                     .h = 64,
                     .font = &canvas_font3x5,
                     .ink = CANVAS_INK_PRIMARY};
  canvas_clear(&c);
  // SECONDARY is plane-1-only. With no plane 1 a naive implementation would
  // drop it entirely and the text would silently vanish on the OLED — which is
  // exactly the bug this test exists to catch.
  canvas_set_ink(&c, CANVAS_INK_SECONDARY);
  canvas_text(&c, 0, 0, "A", 1, 1);
  int ink = 0;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      ink += pixel_at(&c, x, y) ? 1 : 0;
    }
  }
  CHECK(ink > 0);
}

static void cell_grid_matches_each_board(void) {
  // The 0.42" dongle's historical literals, which must survive the move to
  // font-derived metrics: 72/4 == 18 and 40/6 == 6.
  CHECK_EQ_INT(canvas_font3x5.advance, 4);
  CHECK_EQ_INT(canvas_font3x5.line_h, 6);
  CHECK_EQ_INT(OLED_W / canvas_font3x5.advance, 18);
  CHECK_EQ_INT(OLED_H / canvas_font3x5.line_h, 6);
  // And the LCD: 320/8 == 40, 172/16 == 10.
  CHECK_EQ_INT(canvas_font8x16.advance, 8);
  CHECK_EQ_INT(canvas_font8x16.line_h, 16);
  CHECK_EQ_INT(320 / canvas_font8x16.advance, 40);
  CHECK_EQ_INT(172 / canvas_font8x16.line_h, 10);
  // A 15-character IP at 2x must still fit the LCD's 320px width, which is the
  // constraint display.h's whole cell-size argument rests on.
  CHECK(15 * canvas_font8x16.advance * 2 <= 320);
}

static void font_tables_are_internally_consistent(void) {
  CHECK_EQ_INT(canvas_font3x5.last - canvas_font3x5.first + 1, 64);
  CHECK(canvas_font3x5.fold_lowercase);
  CHECK_EQ_INT(canvas_font8x16.first, 32);
  CHECK_EQ_INT(canvas_font8x16.last, 126);
  CHECK(!canvas_font8x16.fold_lowercase);
  // Real lowercase means 'a' and 'A' must differ, which is the thing the
  // folded table cannot do.
  const uint16_t *upper =
      &canvas_font8x16.cols[('A' - 32) * canvas_font8x16.glyph_w];
  const uint16_t *lower =
      &canvas_font8x16.cols[('a' - 32) * canvas_font8x16.glyph_w];
  CHECK(memcmp(upper, lower, canvas_font8x16.glyph_w * sizeof(uint16_t)) != 0);
}

int main(void) {
  RUN(oled_render_matches_pre_extraction_golden);
  RUN(scaling_replicates_each_pixel_as_a_block);
  RUN(non_page_aligned_height_does_not_overrun);
  RUN(text_clips_and_never_wraps);
  RUN(bar_keeps_an_outline_when_empty);
  RUN(bar_cells_stay_separated_on_both_fonts);
  RUN(bar_steps_ascend_and_sit_on_the_baseline);
  RUN(bar_steps_use_two_inks_and_restore_the_pen);
  RUN(bar_steps_with_nothing_filled_still_draws_the_scale);
  RUN(ink_planes_encode_four_roles);
  RUN(mono_canvas_collapses_every_ink_to_lit);
  RUN(cell_grid_matches_each_board);
  RUN(font_tables_are_internally_consistent);
  return test_summary("canvas");
}
