// The device's colour vocabulary: named Solarized tones, and the LED hues.
//
// == What moved, and why the panel is no longer here ==
//
// This file used to hold the screen's four roles as well, on the argument that
// keeping both encodings together stopped the LED and the panel drifting into
// two slightly different reds. The screen's roles now live in theme.h, because
// the panel became themeable and the LED deliberately did not (see below), so
// there is no longer a shared red to keep in step — that argument dissolved
// rather than being overruled. What is left here is the vocabulary theme.h
// names its Solarized entry from, plus the LED's own hues.
//
// == The LED is NOT themed, and that is a decision, not an omission ==
//
// A theme changes what the panel looks like. It must not change what the light
// MEANS. Red is a fault on every airkvm board and in every theme; if a theme
// could reassign it, the one channel on this device that is readable from
// across a room without reading would stop being a fact and start being a
// preference. The hues below are also tuned for a point source rather than for
// text — see the note above SOL_LED_OK — and a theme transcribed from an
// editor screenshot would re-break exactly what that tuning fixed.
#pragma once

#include <stdint.h>

#define RGB24(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (b))

// 5-6-5, in natural order. The panel's byte order is a separate question from
// this packing and is answered in display_st7789.c (BE16), not here: black and
// white are palindromic in RGB565, so the monochrome build never noticed, and
// the first coloured frame came out with the background vivid green until the
// swap went in (hardware, 2026-09-07). Colours are stored the way they are
// written; a transport swaps them if its panel wants that.
#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// RGB565 from one of the comma-triple macros below, e.g. RGB565_OF(SOL_BASE03).
// The extra level of indirection is required rather than decorative: macro
// arguments are separated on commas BEFORE they are expanded, so a direct
// RGB565(SOL_BASE03) passes one argument to a three-parameter macro and fails
// to compile. Going through a one-parameter macro expands the triple first and
// the rescan then splits it.
#define RGB565_OF(triple) RGB565(triple)

// Backgrounds and text, darkest to lightest.
#define SOL_BASE03 0x00, 0x2B, 0x36  // background
#define SOL_BASE02 0x07, 0x36, 0x42  // background highlight
#define SOL_BASE01 0x58, 0x6E, 0x75  // secondary content
#define SOL_BASE0 0x83, 0x94, 0x96   // body text
#define SOL_BASE1 0x93, 0xA1, 0xA1   // emphasised content

// LED hues. Separate from the screen's themed roles in theme.h, and the reason
// is that the two have genuinely different jobs.
//
// Solarized's palette is tuned for TEXT: its accents are desaturated and share
// a lightness so none dominates a page. That is exactly wrong for a status
// light, which is a single point of colour at low brightness where the only
// question is "which state is this". Solarized green (#859900) is (133,153,0)
// — red and green almost equal — so on an LED it reads YELLOW, confirmed on
// hardware 2026-09-07. Its blue (#268bd2) has enough green to read cyan, which
// would have made "bound" and "suspended" nearly the same light.
//
// So these keep Solarized's softness (nothing here is a full-gamut primary)
// while pulling the hues apart far enough to name at a glance.
#define SOL_LED_OK 0x21, 0x99, 0x00     // green: red cut to a quarter of green
#define SOL_LED_BOUND 0x1A, 0x5F, 0xD2  // blue: green cut so it is not cyan
#define SOL_LED_SUSPEND 0x2A, 0xA1, 0x98
#define SOL_LED_WARN 0xB5, 0x89, 0x00
#define SOL_LED_FAULT 0xDC, 0x32, 0x2F
#define SOL_LED_DEBUG 0xD3, 0x36, 0x82

// Accents. Only these carry meaning; everything else is text.
#define SOL_YELLOW 0xB5, 0x89, 0x00
#define SOL_ORANGE 0xCB, 0x4B, 0x16
#define SOL_RED 0xDC, 0x32, 0x2F
#define SOL_BLUE 0x26, 0x8B, 0xD2
#define SOL_CYAN 0x2A, 0xA1, 0x98
#define SOL_GREEN 0x85, 0x99, 0x00
#define SOL_MAGENTA 0xD3, 0x36, 0x82
