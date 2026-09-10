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
#include "display_hue.h"
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
// channel — red for a fault, amber for degraded — so the panel spends colour
// on hierarchy rather than alarm, and a screen that alarmed in parallel with
// the light would be two things saying it worse.
//
// ONE exception, added with the signal gauge: the bar goes amber when the link
// is weak. That is not the panel taking over the alarm — it is a gauge
// colouring itself by the quantity it is already displaying, on the same
// threshold the light uses (indicator_link_is_weak). It stops at amber; red
// stays the light's alone. display_hue.h carries the full argument.
enum display_ink {
  DISPLAY_INK_PRIMARY = 1,    // what you read first: the address
  DISPLAY_INK_SECONDARY = 2,  // present but quieter: labels, SSID, name
  DISPLAY_INK_ACCENT = 3,     // the signal bar; the only non-text element
};
void display_set_ink(enum display_ink ink);

// Hue for subsequent draws: which of the theme's complementary colours the
// SECONDARY text in this section is drawn in. `enum display_hue` and the
// reasoning live in display_hue.h.
//
// A no-op on a single-colour panel, exactly like display_set_ink, so ui.c can
// set it unconditionally and stay one draw() for both boards.
//
// Two things a caller must know, because both are load-bearing:
//
//   - It is a property of the ROW, not of the draw call. The colour panel
//     stores one hue per 8-pixel band rather than per pixel (a third bit plane
//     is 7KB of heap this board has already spent), so two draws landing on
//     the same text row share a hue and the last one set wins. Every row ui.c
//     draws is a whole number of bands, and the backend asserts that at
//     compile time.
//   - display_clear() resets every row to DISPLAY_HUE_NEUTRAL. A section that
//     forgets to set a hue is quiet, never whatever the previous frame left
//     there.
void display_set_hue(enum display_hue hue);

// Switches the panel to a theme, at runtime, from any task. A no-op on a
// panel with no palette, like the two setters above.
//
// Applied at the next flush rather than immediately, and that is not laziness:
// the palette and the dirty-band shadow are ui_task's, and display.h's own
// single-task rule holds for them. A palette rewritten from the HTTP task
// mid-flush would paint one mixed frame — tolerable — but a repaint flag set
// from there could be cleared by the flush already in progress, leaving every
// unchanged band in the OLD colours with nothing to disturb them. That is the
// stale-patch failure the dirty check exists to prevent, arriving by a route
// it cannot see: page_crc() hashes pixels and hue, not the palette. So the
// request is parked and ui_task consumes it at the top of its own flush,
// where it can both reload the palette and force the full repaint atomically.
struct display_theme;
void display_set_theme(const struct display_theme *theme);

// Panel brightness, percent, from any task; parked and applied at the next
// flush for the same reason as the theme. What "brightness" drives is the
// board's business — a PWM backlight on the LCD, the contrast register on the
// OLED — and 100 is what each board shipped at, so the default is
// byte-identical to the panel before this existed. The floor is not enforced
// here: settings_schema.h refuses anything below SETTINGS_SCREEN_MIN on the
// wire and defaults it from flash, and ui.c forces 100 in setup mode (R-009).
void display_set_brightness(int percent);

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

// Draws a signal gauge of `width` cells with `filled` of them lit, at a
// character cell.
//
// HOW it is drawn is the board's call, and the two panels genuinely differ:
// the LCD draws ascending steps, solid when filled and hollow when not, so the
// reading survives someone who cannot separate the two colours. The 0.42" OLED
// draws equal outlined boxes, because at a five-pixel glyph four ascending
// steps are one-pixel increments and a 1bpp panel has no quieter ink for the
// hollow ones. Callers pass the same numbers either way.
void display_bar(int col, int row, int width, int filled);

// Pushes the framebuffer to the panel. Nothing appears until this is called.
void display_flush(void);
