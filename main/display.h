// Minimal SSD1306 driver plus text rendering for the 0.42" status panel.
//
// Written rather than pulled in: the panel is a 72x40 window inside a 128x64
// controller, all this needs is a framebuffer and a font, and a graphics
// library would be more code to configure than to replace.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Character cell is 4x6 (a 3x5 glyph plus one pixel of spacing), giving 18
// columns by 6 rows on a 72x40 panel. A 5x7 font would read better but fits
// only 12 columns, and "192.168.100.100" is 15 characters — an IP that doesn't
// fit is worse than one that is small, because the whole point of the screen is
// telling you where to point a browser.
#define DISPLAY_COLS 18
#define DISPLAY_ROWS 6

// Pixel geometry, for callers that mix text sizes and therefore need to place
// things by pixel rather than by row.
#define DISPLAY_CELL_W 4
#define DISPLAY_CELL_H 6

esp_err_t display_init(void);

// True when init succeeded. Every draw call is a no-op otherwise, so a board
// with no panel (or a mis-wired one) degrades to "no display" instead of
// blocking or erroring on every refresh.
bool display_present(void);

void display_clear(void);

// Sets one pixel in the framebuffer. Out-of-bounds coordinates are silently
// clipped, same as every other draw call — a caller doing bounce-off-the-edge
// math is expected to occasionally land one pixel past the edge.
//
// Like every draw call here, the framebuffer has no lock: today's only
// caller is ui_task, and that single-task assumption is load-bearing, not
// incidental — a second task drawing concurrently would race display_flush()
// with no guard against it.
void display_pixel(int x, int y, bool on);

// Draws text at a character cell. Text is clipped, never wrapped: wrapping an
// IP address onto a second line reads as two different numbers.
void display_text(int col, int row, const char *text);

// Draws text at double height, positioned by pixel row, for the one or two
// things actually read at a glance.
//
// Stretched vertically only, not both axes. Doubling the width too would cut
// the line to 9 characters, and "192.168.100.100" is 15 — so the legible
// version of the IP would be the one that no longer fits. Tall-and-narrow
// digits stay readable in a way tall-and-narrow prose would not, which is why
// this is used for the address and not for labels.
void display_text_tall(int col, int y_px, const char *text);

// Draws a horizontal bar in the given cell row, `filled` of `width` cells.
// Used for signal strength, where a shape is readable at a glance and a number
// is not.
void display_bar(int col, int row, int width, int filled);

// Pushes the framebuffer to the panel. Nothing appears until this is called.
void display_flush(void);
