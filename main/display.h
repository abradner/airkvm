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

esp_err_t display_init(void);

// True when init succeeded. Every draw call is a no-op otherwise, so a board
// with no panel (or a mis-wired one) degrades to "no display" instead of
// blocking or erroring on every refresh.
bool display_present(void);

void display_clear(void);

// Draws text at a character cell. Text is clipped, never wrapped: wrapping an
// IP address onto a second line reads as two different numbers.
void display_text(int col, int row, const char *text);

// Draws a horizontal bar in the given cell row, `filled` of `width` cells.
// Used for signal strength, where a shape is readable at a glance and a number
// is not.
void display_bar(int col, int row, int width, int filled);

// Pushes the framebuffer to the panel. Nothing appears until this is called.
void display_flush(void);
