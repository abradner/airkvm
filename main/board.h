// Board pin map, in one place.
//
// Pins differ per board and are vendor-documented rather than
// datasheet-derived (AGENTS.md §6.1), so they live here instead of scattered
// through the modules that use them. Adding a board means adding a block here,
// not editing display.c and indicator.c.
//
// Selection is on CONFIG_AIRKVM_BOARD_*, not CONFIG_IDF_TARGET_*. The target
// is not a board: the ESP32-S3-0.42 dongle and the Waveshare LCD-1.47B are
// both esp32s3 and share nothing else. Keying off the target worked only while
// there was at most one board per chip, and a second S3 board collides on that
// arm and silently inherits the first one's pins.
#pragma once

#include "sdkconfig.h"

#if CONFIG_AIRKVM_BOARD_C3_EGG

// ESP32-C3 "egg" bench board. Values taken from a working ESPHome config for
// this exact board, not from a datasheet.
#define BOARD_HAS_DISPLAY 1
#define BOARD_DISPLAY_SSD1306 1
#define BOARD_I2C_SDA 5
#define BOARD_I2C_SCL 6
#define BOARD_OLED_ADDR 0x3C

// The panel is a 72x40 window inside an SSD1306's 128x64 address space, so
// everything drawn has to be offset. Getting this wrong shows as a picture
// that is shifted or wrapped rather than absent, which is easy to misread as
// "the driver works".
#define BOARD_OLED_WIDTH 72
#define BOARD_OLED_HEIGHT 40
#define BOARD_OLED_X_OFFSET 28
#define BOARD_OLED_Y_OFFSET 24

#define BOARD_PANEL_WIDTH BOARD_OLED_WIDTH
#define BOARD_PANEL_HEIGHT BOARD_OLED_HEIGHT
#define BOARD_CELL_W 4
#define BOARD_CELL_H 6
// Vertical-only enlargement: 18 columns cannot spare the width (see display.h).
#define BOARD_TALL_SX 1
#define BOARD_TALL_SY 2

#define BOARD_HAS_LED 1
#define BOARD_LED_PRIMARY 8  // blue "signal" LED, active-low
#define BOARD_LED_PRIMARY_INVERTED 1

// There is no second controllable LED. The red one is a hardwired power
// indicator: tools/led_scan walked every free GPIO in both polarities and it
// never changed, and the board's published pinout labels it "Power light".
// Settled — do not go looking for its pin again.

#define BOARD_BUTTON 9  // BOOT button, active-low with pull-up
#define BOARD_BUTTON_ACTIVE_LOW 1

// Note on I2C: published pinouts for this board family tag GPIO8/GPIO9 as
// "I2C SDA/SCL", but those are the chip's default alternate-function labels,
// not how the OLED is wired — GPIO8 is physically the LED here. The panel is on
// GPIO5/6 above, which is what a working ESPHome config for this board uses.

#elif CONFIG_AIRKVM_BOARD_S3_OLED042

// ESP32-S3-0.42 OLED supermini — the original dongle. I2C pins confirmed
// 2026-08-31 on real hardware with tools/i2c_scan: swept every ordered GPIO
// pair (756 in that run; the tool's exclusion list has since dropped one
// more strapping pin, so a rerun today sweeps 702 — see the tool's own
// comments), exactly one ACKed at 0x3C. This board's SDA/SCL are GPIO48/47, NOT
// the C3's GPIO5/6 — the C3 numbers were carried over here pending hardware
// and were wrong, exactly the trap AGENTS.md §6.1 warns against.
// WIDTH/HEIGHT/X_OFFSET/Y_OFFSET confirmed the same day: real firmware
// rendered legible status text (IP, mDNS name, link state) through this
// window on the actual panel.
#define BOARD_HAS_DISPLAY 1
#define BOARD_DISPLAY_SSD1306 1
#define BOARD_I2C_SDA 48
#define BOARD_I2C_SCL 47
#define BOARD_OLED_ADDR 0x3C
#define BOARD_OLED_WIDTH 72
#define BOARD_OLED_HEIGHT 40
#define BOARD_OLED_X_OFFSET 28
#define BOARD_OLED_Y_OFFSET 24
// This panel is mounted 180° from the orientation the status layout assumes
// (confirmed against the physical unit, not a datasheet). display.c derives
// the compensated page offset from this flag — do not hand-adjust Y_OFFSET
// instead, see the comment in display.c for why that goes wrong.
#define BOARD_OLED_ROTATE_180 1

#define BOARD_PANEL_WIDTH BOARD_OLED_WIDTH
#define BOARD_PANEL_HEIGHT BOARD_OLED_HEIGHT
#define BOARD_CELL_W 4
#define BOARD_CELL_H 6
// Vertical-only enlargement: 18 columns cannot spare the width (see display.h).
#define BOARD_TALL_SX 1
#define BOARD_TALL_SY 2

#define BOARD_HAS_LED 0
#define BOARD_BUTTON 0
#define BOARD_BUTTON_ACTIVE_LOW 1

#elif CONFIG_AIRKVM_BOARD_S3_LCD147

// Waveshare ESP32-S3-LCD-1.47B. ESP32-S3R8 (8MB octal PSRAM) + discrete XMC
// 16MB flash, measured with `esptool flash_id` on 2026-09-06 — not a
// part-family assumption.
//
// Pins below are transcribed from Waveshare's own schematic for the *B*
// variant, not from a pinout table:
//   files.waveshare.com/wiki/ESP32-S3-LCD-1.47B/ESP32-S3-LCD-1.47B_schematic_diagram.pdf
// That distinction is load-bearing. Every widely-published pinout for this
// board family — Espressif's own espp/ws-s3-lcd-1-47 example included — gives
// the backlight as GPIO48. That is the *non-B* board. On the B, GPIO48 is
// IMU_SDA and the backlight is GPIO46, so using the published number drives
// the IMU's I2C data line as a PWM output: the panel stays dark AND the I2C
// bus wedges, two symptoms from one cause and neither pointing at the pin.
// AGENTS.md §6.1, twice over.
//
// VERIFIED on hardware 2026-09-06: the panel renders, WiFi associates, the
// HTTPS console serves and validates against our CA, and the board enumerates
// as a USB HID keyboard that delivered 17/17 and 60/60 characters measured
// with tools/hid_listen.py. WiFi and USB coexist here, so gotcha 10's
// net_start()-before-hid_out_start() ordering holds on this board too — which
// had to be re-tested rather than inherited.
#define BOARD_HAS_DISPLAY 1
#define BOARD_DISPLAY_ST7789 1

#define BOARD_LCD_CLK 40  // LCD_CLK
#define BOARD_LCD_DIN 45  // LCD_DIN (MOSI); the panel is write-only, no MISO
#define BOARD_LCD_CS 42   // LCD_CS
#define BOARD_LCD_DC 41   // LCD_DC
#define BOARD_LCD_RST 39  // LCD_RST
#define BOARD_LCD_BL 46   // LCD_BL — NOT 48, see above
#define BOARD_LCD_BL_ON_LEVEL 1

// The panel is 172x320 in its native orientation. We drive it landscape, so
// the logical framebuffer is 320x172 and swap_xy does the rotation.
//
// Landscape rather than portrait because of display.h's own rule: an IP that
// does not fit is worse than one that is small. Portrait's 172px of width is
// about five characters at a legible size, and display.h is explicit that
// wrapping an IP reads as two different numbers.
#define BOARD_LCD_NATIVE_WIDTH 172
#define BOARD_LCD_NATIVE_HEIGHT 320
#define BOARD_PANEL_WIDTH 320
#define BOARD_PANEL_HEIGHT 172

// An 8x16 font, not the 3x5 scaled up. This panel's pixels are *smaller* than
// the OLED's (0.103 vs 0.130 mm), so a naive port renders text physically
// smaller than the screen it replaces; and scaling the 3x5 by 4 gives a 0.41mm
// stroke on a 1.23mm glyph, which is legible and looks like a calculator.
// 8x16 costs ~1.5KB of rodata and gives normal proportions.
#define BOARD_CELL_W 8
#define BOARD_CELL_H 16
// 40 columns is room enough to enlarge both axes; a 15-character IP at 2x is
// 240 of 320 pixels.
#define BOARD_TALL_SX 2
#define BOARD_TALL_SY 2

// ST7789 is a 240x320 controller; a 172-wide panel sits offset inside it, so
// every write needs a gap — the exact analogue of BOARD_OLED_X_OFFSET above,
// and the same failure signature: a wrong gap renders a shifted picture, not
// no picture, which reads as a wiring fault.
//
// CONFIRMED on hardware 2026-09-06: text renders the right way round and in
// the right place, and nothing is displaced. x_gap stays 0 because the logical
// long axis maps to the controller's 320-row axis, which has no offset; the
// 34 lands on y_gap, which is the 172-in-240 window.
//
// Note the panel's corners are physically ROUNDED and simply have no pixels
// there. That is not a gap error — it displaces nothing — and must not be
// "fixed" by adjusting these values, which would shift the whole image to hide
// a few missing corner pixels. The layout keeps content out of the corners
// instead; see UI_SAFE_INSET in ui.c.
// Landscape. swap_xy transposes; a transpose on its own is a reflection about
// the diagonal, which on screen reads as "mirrored" — so a rotation is always
// swap_xy plus EXACTLY ONE mirror. mirror_x gives 90 degrees, mirror_y gives
// 270; the two differ by 180, so if the text is the right way round but
// upside down, swap which of these is 1. Confirmed mirrored-with-neither on
// hardware 2026-09-06, which is what identified the missing mirror.
#define BOARD_LCD_SWAP_XY 1
#define BOARD_LCD_MIRROR_X 1
#define BOARD_LCD_MIRROR_Y 0
#define BOARD_LCD_X_GAP 0
#define BOARD_LCD_Y_GAP 34

// The WS2812B-0807 on GPIO38 is not driven yet: indicator.c is LEDC-PWM
// monochrome and an addressable LED is a different driver (RMT), not a
// parameter. Left dark deliberately rather than half-implemented. If a
// backlight fade later wants LEDC, pick a channel other than LEDC_CHANNEL_0 —
// indicator.c claims that one, and only stays out of the way today because
// BOARD_HAS_LED is 0 here.
#define BOARD_HAS_LED 0

#define BOARD_BUTTON 0  // BOOT button, active-low with pull-up
#define BOARD_BUTTON_ACTIVE_LOW 1

// Not driven this pass, recorded so the next person does not re-derive them
// from the schematic: QMI8658 IMU on I2C SDA=48 SCL=47 (INT1=13, INT2=12),
// microSD on SCLK=14 MOSI=15 MISO=16 D2=17 D1=18 CS=21, RGB_IO=38.
// Note 47/48 are the same pins the 0.42" dongle uses for its OLED — same
// pins, different peripheral, different board.
//
// GPIO33-37 are unavailable on this part regardless of what any pinout says:
// the R8's octal PSRAM is bonded to them even with PSRAM disabled in software.

#else

// No board selected. This is deliberately fatal rather than defaulting to
// "no display", because AGENTS.md §6.1's first config trap makes the silent
// version indistinguishable from broken hardware: a Kconfig-only edit does not
// force a reconfigure, so CONFIG_AIRKVM_BOARD_* can be *absent* from a stale
// sdkconfig rather than "is not set" — and a headless image that builds green
// looks exactly like a dead panel. Fail loudly at compile time instead.
#error \
    "no board selected — set CONFIG_AIRKVM_BOARD_*; see AGENTS.md §6.1 on stale sdkconfig"

#endif
