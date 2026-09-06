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

// Accents. Only these carry meaning; everything else is text.
#define SOL_YELLOW 0xB5, 0x89, 0x00
#define SOL_ORANGE 0xCB, 0x4B, 0x16
#define SOL_RED 0xDC, 0x32, 0x2F
#define SOL_BLUE 0x26, 0x8B, 0xD2
#define SOL_CYAN 0x2A, 0xA1, 0x98
#define SOL_GREEN 0x85, 0x99, 0x00
#define SOL_MAGENTA 0xD3, 0x36, 0x82
