// A 1bpp page-packed drawing surface, plus text and bar rendering on top of it.
//
// This is the half of the old display.c that was never board-specific. Its
// draw_text() and display_bar() were written entirely in terms of
// display_pixel(), so the font, the glyph blitting, the clip rule and the bar
// shape are the same code on any panel — they just used to live in the same
// file as one particular SSD1306 transport.
//
// Header-only and free of esp_ headers on purpose, following oled_geometry.h:
// that is what lets test/host build it and check the rendering without a
// board. The font *tables* are separate translation units (font3x5.c,
// font8x16.c) so a board links only the one it uses.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// One glyph is `glyph_w` column bitmaps, bit 0 = top row. Column-major rather
// than row-major because that is the shape the original hand-built 3x5 table
// was written in, and a rasterised 8x16 costs the same either way (16 bytes).
struct canvas_font {
  const uint16_t *cols;  // glyph_w entries per glyph, consecutive
  uint8_t glyph_w;
  uint8_t glyph_h;
  uint8_t advance;  // horizontal cell pitch: glyph_w plus letter spacing
  uint8_t line_h;   // vertical cell pitch: glyph_h plus line spacing
  uint8_t first;    // inclusive ASCII range covered by the table
  uint8_t last;
  // The 3x5 table folds lowercase to uppercase because hand-building 64
  // glyphs was already the expensive part. A generated table has no such
  // reason and carries real lowercase.
  bool fold_lowercase;
};

extern const struct canvas_font canvas_font3x5;
extern const struct canvas_font canvas_font8x16;

// Pixels are packed exactly as an SSD1306 wants them — 8 rows per byte,
// stride `w` — so that backend can hand its buffer to the panel unmodified.
// A panel that wants a different layout expands at flush time; at these sizes
// (320x172 is 7040 bytes) that is far cheaper than the alternative, which is a
// 110KB RGB565 framebuffer competing with mbedTLS for the same heap.
// Ink roles. Four, because two 1bpp planes give four combinations and that is
// exactly the vocabulary a status screen needs — see display.h for what each
// one means. Role 0 is "no ink" and is how a pixel is cleared.
//
// Two planes rather than a repacked 2bpp buffer, deliberately. The packing
// stays byte-for-byte what the SSD1306 wants, so that backend hands plane 0
// to the panel unmodified and its rendering is provably unchanged — the golden
// test in test_canvas.c holds that to account. A repack would have made every
// existing pixel path suspect for a feature only one board uses.
#define CANVAS_INK_NONE 0
#define CANVAS_INK_PRIMARY 1
#define CANVAS_INK_SECONDARY 2
#define CANVAS_INK_ACCENT 3

struct canvas {
  uint8_t *bits;  // plane 0
  // Plane 1. NULL on a single-colour panel, which is not an error: with no
  // second plane every non-zero ink collapses to "on", which is the honest
  // rendering of colour on a monochrome display and costs that board nothing.
  uint8_t *bits_hi;
  int w;
  int h;
  const struct canvas_font *font;
  uint8_t ink;  // role applied by subsequent draws; 0 means "use PRIMARY"
};

static inline int canvas_pages(int h) {
  return (h + 7) / 8;
}

static inline int canvas_bytes(int w, int h) {
  return w * canvas_pages(h);
}

static inline void canvas_clear(struct canvas *c) {
  const size_t n = (size_t)canvas_bytes(c->w, c->h);
  memset(c->bits, 0, n);
  if (c->bits_hi != NULL) {
    memset(c->bits_hi, 0, n);
  }
}

// Sets the role for subsequent draws. Stateful, like a pen colour, because the
// alternative is threading an ink argument through every call site including
// the ones that have no opinion — and the callers that do care set it for one
// line and put it back.
static inline void canvas_set_ink(struct canvas *c, uint8_t ink) {
  c->ink = ink;
}

// Out-of-bounds writes are silently clipped, same as every other draw call —
// a caller doing bounce-off-the-edge math is expected to occasionally land one
// pixel past the edge.
static inline void canvas_pixel(struct canvas *c, int x, int y, bool on) {
  if (x < 0 || x >= c->w || y < 0 || y >= c->h) {
    return;
  }
  const size_t off = (size_t)(y / 8) * (size_t)c->w + (size_t)x;
  const uint8_t bit = (uint8_t)(1u << (y % 8));
  // Ink 0 from a caller that never set one means PRIMARY, so every existing
  // call site keeps working untouched and a zeroed struct is not a trap.
  const uint8_t ink = c->ink == CANVAS_INK_NONE ? CANVAS_INK_PRIMARY : c->ink;

  // On a single-plane canvas, plane 0 means "lit", not "bit 0 of the role" —
  // otherwise SECONDARY (role 2, bit 0 clear) writes nothing and the text
  // silently disappears on the monochrome board. That is not hypothetical: it
  // is what this did when first written, and since ui.c draws almost
  // everything SECONDARY it would have blanked the shipping dongle's screen
  // while the colour board looked perfect.
  const bool lit = on && (c->bits_hi != NULL ? (ink & 1u) != 0 : ink != 0);
  if (lit) {
    c->bits[off] |= bit;
  } else {
    c->bits[off] &= (uint8_t)~bit;
  }
  if (c->bits_hi != NULL) {
    if (on && (ink & 2u)) {
      c->bits_hi[off] |= bit;
    } else {
      c->bits_hi[off] &= (uint8_t)~bit;
    }
  }
}

// The role at a pixel, 0..3. Used by a colour backend at flush time; a
// monochrome one just reads plane 0.
static inline uint8_t canvas_ink_at(const struct canvas *c, int x, int y) {
  if (x < 0 || x >= c->w || y < 0 || y >= c->h) {
    return CANVAS_INK_NONE;
  }
  const size_t off = (size_t)(y / 8) * (size_t)c->w + (size_t)x;
  const uint8_t bit = (uint8_t)(1u << (y % 8));
  uint8_t ink = (c->bits[off] & bit) ? 1u : 0u;
  if (c->bits_hi != NULL && (c->bits_hi[off] & bit)) {
    ink |= 2u;
  }
  return ink;
}

// Draws text at a pixel position, scaled independently in each axis. Text is
// clipped, never wrapped: wrapping an IP address onto a second line reads as
// two different numbers.
//
// Independent sx/sy exists because the 0.42" panel wants tall-but-not-wide —
// at 18 columns, doubling the width would cut a line to 9 characters and
// "192.168.100.100" is 15. A panel with room to spare scales both.
static inline void canvas_text(struct canvas *c, int x, int y, const char *text,
                               int sx, int sy) {
  const struct canvas_font *f = c->font;
  if (text == NULL || f == NULL || sx < 1 || sy < 1) {
    return;
  }
  for (const char *p = text; *p; p++) {
    if (x + f->glyph_w * sx > c->w) {
      return;  // clip rather than wrap
    }
    unsigned char ch = (unsigned char)*p;
    if (f->fold_lowercase && ch >= 'a' && ch <= 'z') {
      ch = (unsigned char)(ch - 'a' + 'A');
    }
    if (ch < f->first || ch > f->last) {
      ch = '?';
    }
    const uint16_t *glyph = &f->cols[(ch - f->first) * f->glyph_w];
    for (int gx = 0; gx < f->glyph_w; gx++) {
      for (int gy = 0; gy < f->glyph_h; gy++) {
        if (!(glyph[gx] & (1u << gy))) {
          continue;
        }
        for (int ry = 0; ry < sy; ry++) {
          for (int rx = 0; rx < sx; rx++) {
            canvas_pixel(c, x + gx * sx + rx, y + gy * sy + ry, true);
          }
        }
      }
    }
    x += f->advance * sx;
  }
}

// A horizontal bar, `filled` of `width` cells, positioned in character cells.
// Used for signal strength, where a shape is readable at a glance and a number
// is not.
//
// Cell metrics come from the font rather than being hardcoded: display.c used
// to write `col * 4` and `row * 6` as literals here while display_text() went
// through the DISPLAY_CELL_* macros, so the two would have silently disagreed
// the moment a board changed cell size. On the 72x40 board they happen to be
// the same numbers, which is why it never showed.
static inline void canvas_bar(struct canvas *c, int col, int row, int width,
                              int filled, int sx, int sy) {
  const struct canvas_font *f = c->font;
  if (f == NULL) {
    return;
  }
  const int x0 = col * f->advance * sx;
  const int y0 = row * f->line_h * sy;
  // One pixel narrower than the cell pitch, so consecutive cells never touch.
  // The gap is what makes the bar read as four boxes rather than one block —
  // and it has to be derived, not assumed: the 3x5 font gets it for free
  // because its glyph is 3 wide on a 4 pitch, but the 8x16 font is 8 on 8, and
  // using glyph_w there merges every border into a solid bar. (advance - 1 is
  // exactly glyph_w for the 3x5 font, so the OLED output is unchanged — the
  // golden render in test_canvas.c holds that to account.)
  const int bw = (f->advance - 1) * sx;
  const int bh = f->glyph_h * sy;
  for (int i = 0; i < width; i++) {
    const bool on = i < filled;
    for (int gx = 0; gx < bw; gx++) {
      for (int gy = 0; gy < bh; gy++) {
        // Unfilled cells keep an outline so the bar's full length stays
        // visible; an empty gap reads as "no signal data" rather than "weak".
        const bool edge = gy == 0 || gy == bh - 1 || gx == 0 || gx == bw - 1;
        canvas_pixel(c, x0 + i * f->advance * sx + gx, y0 + gy, on || edge);
      }
    }
  }
}

// A signal gauge: ascending steps, left to right, bottom-aligned — the shape
// every phone and laptop uses for exactly this quantity, which is the point.
// canvas_bar()'s equal boxes above say "four of something"; these say "a scale,
// and this much of it".
//
// Filled steps are SOLID in the pen ink; unfilled ones are an OUTLINE in
// `ink_off`. Both differences matter and neither is decoration:
//
//   - the outline keeps the empty part of the scale visible, so an absent step
//     reads as "not reached" rather than "no data" (canvas_bar's reasoning);
//   - solid-vs-outline means the reading does not depend on seeing colour.
//
// The second one was got wrong first time round: every step was drawn solid and
// only the ink differed, which put the whole reading on hue. Measured on the
// RGB565-quantised colours, an unfilled step against a filled one is 1.02:1 on
// the default theme and 1.00:1 on solarized-dark — the ramp colours are chosen
// to share a lightness, so to anyone who cannot separate those hues the gauge
// was a solid block. Shape is what makes it legible without colour, and
// test_canvas.c now asserts the two are drawn differently.
//
// The pen is restored before returning: this is the only drawing call that
// touches two inks, and a caller that set PRIMARY and got SECONDARY back would
// be a genuinely horrible bug to find.
//
// `ink_off` must not be CANVAS_INK_NONE. canvas_pixel reads ink 0 as "the
// caller never set one" and substitutes PRIMARY, so passing NONE to mean "draw
// nothing for unfilled steps" draws them in the BRIGHTEST ink instead. Pass an
// ink you actually want.
//
// Not used by the 0.42" OLED — see the note at display_bar in
// display_st7789.c for why the shape does not survive a 3x5 font.
static inline void canvas_bar_steps(struct canvas *c, int col, int row,
                                    int width, int filled, int sx, int sy,
                                    uint8_t ink_off) {
  const struct canvas_font *f = c->font;
  if (f == NULL || width <= 0) {
    return;
  }
  const int x0 = col * f->advance * sx;
  const int y0 = row * f->line_h * sy;
  // One pixel narrower than the pitch, so steps never touch — same derivation
  // as canvas_bar(), and same reason (gotcha 14c).
  const int bw = (f->advance - 1) * sx;
  const int full_h = f->glyph_h * sy;
  const uint8_t pen = c->ink;

  for (int i = 0; i < width; i++) {
    // Ascending, and never zero: the shortest step still has to be visible,
    // or "one bar" and "no bars" look the same.
    int h = full_h * (i + 1) / width;
    if (h < 1) {
      h = 1;
    }
    const bool lit = i < filled;
    canvas_set_ink(c, lit ? pen : ink_off);
    const int top = full_h - h;
    for (int gx = 0; gx < bw; gx++) {
      for (int gy = top; gy < full_h; gy++) {
        // A one-pixel border of the step's own rectangle. At the shortest step
        // (4px on this panel) that still leaves a visible hollow, which is why
        // the outline is drawn at the step's height rather than as a fixed
        // stub — the height has to keep meaning "position in the ramp".
        const bool edge =
            gx == 0 || gx == bw - 1 || gy == top || gy == full_h - 1;
        if (lit || edge) {
          canvas_pixel(c, x0 + i * f->advance * sx + gx, y0 + gy, true);
        }
      }
    }
  }
  canvas_set_ink(c, pen);
}
