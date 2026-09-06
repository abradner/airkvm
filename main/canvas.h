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
struct canvas {
  uint8_t *bits;
  int w;
  int h;
  const struct canvas_font *font;
};

static inline int canvas_pages(int h) {
  return (h + 7) / 8;
}

static inline int canvas_bytes(int w, int h) {
  return w * canvas_pages(h);
}

static inline void canvas_clear(struct canvas *c) {
  memset(c->bits, 0, (size_t)canvas_bytes(c->w, c->h));
}

// Out-of-bounds writes are silently clipped, same as every other draw call —
// a caller doing bounce-off-the-edge math is expected to occasionally land one
// pixel past the edge.
static inline void canvas_pixel(struct canvas *c, int x, int y, bool on) {
  if (x < 0 || x >= c->w || y < 0 || y >= c->h) {
    return;
  }
  uint8_t *cell = &c->bits[(y / 8) * c->w + x];
  const uint8_t bit = (uint8_t)(1u << (y % 8));
  if (on) {
    *cell |= bit;
  } else {
    *cell &= (uint8_t)~bit;
  }
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
