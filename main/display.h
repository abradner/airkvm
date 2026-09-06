// The status panel, as the rest of the firmware sees it.
//
// Two backends implement this: display_ssd1306.c (72x40 mono OLED over I2C)
// and display_st7789.c (172x320 IPS LCD over SPI). CMake compiles exactly one,
// selected by CONFIG_AIRKVM_BOARD_*. Callers do not know which.
//
// The shared drawing layer lives in canvas.h. What differs between backends is
// the transport and the geometry, not the rendering.
#pragma once

#include <stdbool.h>

#include "board.h"
#include "esp_err.h"

// Panel size in pixels, after any rotation the backend applies. ui.c uses
// these for anything it positions itself, rather than reaching past this
// header into the board's panel-specific symbols.
#define DISPLAY_WIDTH BOARD_PANEL_WIDTH
#define DISPLAY_HEIGHT BOARD_PANEL_HEIGHT

// Character cell, and the grid it implies. Derived from the board rather than
// fixed, because the two panels use different fonts: the 72x40 OLED has a 4x6
// cell (a 3x5 glyph plus a pixel of spacing) giving 18x6, and the LCD has an
// 8x16 cell giving 40x10.
//
// The 4x6 choice is worth keeping in view, because it is the constraint that
// shaped this whole interface: a 5x7 font would read better but fits only 12
// columns, and "192.168.100.100" is 15 characters — an IP that doesn't fit is
// worse than one that is small, because the whole point of the screen is
// telling you where to point a browser.
#define DISPLAY_CELL_W BOARD_CELL_W
#define DISPLAY_CELL_H BOARD_CELL_H
#define DISPLAY_COLS (DISPLAY_WIDTH / DISPLAY_CELL_W)
#define DISPLAY_ROWS (DISPLAY_HEIGHT / DISPLAY_CELL_H)

// Ink for subsequent draws. A no-op on a single-colour panel, which is why
// callers may set it unconditionally: on the 0.42" OLED every ink renders as
// "lit", which is the honest rendering of colour on a mono display.
//
// Deliberately only three inks plus none. The RGB LED is this product's status
// channel — red for a fault, amber for degraded — so the panel is free to
// spend colour on hierarchy rather than alarm, and a screen that alarms in
// parallel with the light would just be two things saying it worse.
enum display_ink {
  DISPLAY_INK_PRIMARY = 1,    // what you read first: the address
  DISPLAY_INK_SECONDARY = 2,  // present but quieter: labels, SSID, name
  DISPLAY_INK_ACCENT = 3,     // the signal bar; the only non-text element
};
void display_set_ink(enum display_ink ink);

esp_err_t display_init(void);

// True when init succeeded. Every draw call is a no-op otherwise, so a board
// with no panel (or a mis-wired one) degrades to "no display" instead of
// blocking or erroring on every refresh.
bool display_present(void);

void display_clear(void);

// Sets one pixel. Out-of-bounds coordinates are silently clipped, same as
// every other draw call — a caller doing bounce-off-the-edge math is expected
// to occasionally land one pixel past the edge.
//
// Like every draw call here, there is no lock: today's only caller is ui_task,
// and that single-task assumption is load-bearing, not incidental — a second
// task drawing concurrently would race display_flush() with no guard.
void display_pixel(int x, int y, bool on);

// Draws text at a character cell. Text is clipped, never wrapped: wrapping an
// IP address onto a second line reads as two different numbers.
void display_text(int col, int row, const char *text);

// Draws text enlarged, positioned by pixel row, for the one or two things
// actually read at a glance. The column origin is not scaled, so a large line
// starts where a normal one would.
//
// How it enlarges is the board's call (BOARD_TALL_SX/SY), because the reason
// differs per panel. On the 72x40 OLED it is vertical-only: at 18 columns,
// doubling the width too would cut the line to 9 characters, and
// "192.168.100.100" is 15 — so the legible version of the IP would be the one
// that no longer fits. Tall-and-narrow digits stay readable in a way
// tall-and-narrow prose would not. On the LCD's 40 columns that constraint
// does not bind, and it scales both axes.
void display_text_tall(int col, int y_px, const char *text);

// Draws a horizontal bar in the given cell row, `filled` of `width` cells.
// Used for signal strength, where a shape is readable at a glance and a number
// is not.
void display_bar(int col, int row, int width, int filled);

// Pushes the framebuffer to the panel. Nothing appears until this is called.
void display_flush(void);
