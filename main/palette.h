// Solarized Dark, in one place, for anything on this device that shows colour.
//
// Chosen rather than saturated primaries because a status light and a status
// screen are things you sit next to for hours, and full-gamut red/green is
// exhausting at that distance. Solarized's accents are deliberately desaturated
// and share a lightness, so no single one grabs the eye harder than the others
// — which matters when the point is that only *one* of them should be on at a
// time.
//
// Both encodings live here so the LED and the panel cannot drift into two
// slightly different reds. The LED wants 8-bit-per-channel; the ST7789 wants
// RGB565.
#pragma once

#include <stdint.h>

#define RGB24(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (b))

// 5-6-5. Note the panel's byte order is a separate question from this packing:
// pure black and pure white are byte-order agnostic, which is why the
// monochrome build never had to care. The first real colour on screen is where
// that stops being true — expect to check `data_endian` when the palette
// reaches the LCD.
#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Backgrounds and text, darkest to lightest.
#define SOL_BASE03 0x00, 0x2B, 0x36  // background
#define SOL_BASE02 0x07, 0x36, 0x42  // background highlight
#define SOL_BASE01 0x58, 0x6E, 0x75  // secondary content
#define SOL_BASE0 0x83, 0x94, 0x96   // body text
#define SOL_BASE1 0x93, 0xA1, 0xA1   // emphasised content

// Screen roles, as RGB565. Four, matching canvas.h's ink roles.
//
// The scheme is restrained on purpose, and the reason is architectural rather
// than aesthetic: on this board the RGB LED is the primary status channel, so
// the panel does not have to alarm. It can spend colour on HIERARCHY instead —
// which is the thing a status screen actually lacks when everything is the
// same white.
//
//   PRIMARY    base1   the address: the one thing you read at a glance
//   SECONDARY  base01  labels, SSID, mDNS name — present, deliberately quieter
//   ACCENT     cyan    the signal bar, the only non-text element
//
// No red and no amber here. Those live on the LED, where they are visible from
// across a room and cannot be missed; duplicating them on the panel would make
// two things say the same thing and neither say it well.
#define SOL_SCREEN_BG RGB565(0x00, 0x2B, 0x36)
#define SOL_SCREEN_PRIMARY RGB565(0x93, 0xA1, 0xA1)
#define SOL_SCREEN_SECONDARY RGB565(0x58, 0x6E, 0x75)
#define SOL_SCREEN_ACCENT RGB565(0x2A, 0xA1, 0x98)

// LED hues. Separate from the screen entries above, and the reason is that the
// two have genuinely different jobs.
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
// Setup mode. Deliberately NOT another hue on the health ramp: setup mode is
// not a degree of working, it is the device doing a different job entirely
// (no keyboard, no disk, no console, no target). A warm white says "this is
// not the normal mode" without inviting the operator to rank it against green
// or amber, and it is unmistakable beside all six hues above.
#define SOL_LED_SETUP 0xEE, 0xE8, 0xD5

// Accents. Only these carry meaning; everything else is text.
#define SOL_YELLOW 0xB5, 0x89, 0x00
#define SOL_ORANGE 0xCB, 0x4B, 0x16
#define SOL_RED 0xDC, 0x32, 0x2F
#define SOL_BLUE 0x26, 0x8B, 0xD2
#define SOL_CYAN 0x2A, 0xA1, 0x98
#define SOL_GREEN 0x85, 0x99, 0x00
#define SOL_MAGENTA 0xD3, 0x36, 0x82
