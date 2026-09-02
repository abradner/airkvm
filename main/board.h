// Board pin map, in one place.
//
// Pins differ per board and are vendor-documented rather than
// datasheet-derived (AGENTS.md §6.1), so they live here instead of scattered
// through the modules that use them. Adding a board means adding a block here,
// not editing display.c and indicator.c.
#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C3

// ESP32-C3 "egg" bench board. Values taken from a working ESPHome config for
// this exact board, not from a datasheet.
#define BOARD_HAS_DISPLAY 1
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

#elif CONFIG_IDF_TARGET_ESP32S3

// ESP32-S3-0.42 OLED supermini — the real dongle. I2C pins confirmed
// 2026-08-31 on real hardware with tools/i2c_scan: swept all 756 ordered GPIO
// pairs, exactly one ACKed at 0x3C. This board's SDA/SCL are GPIO48/47, NOT
// the C3's GPIO5/6 — the C3 numbers were carried over here pending hardware
// and were wrong, exactly the trap AGENTS.md §6.1 warns against.
// WIDTH/HEIGHT/X_OFFSET/Y_OFFSET confirmed the same day: real firmware
// rendered legible status text (IP, mDNS name, link state) through this
// window on the actual panel.
#define BOARD_HAS_DISPLAY 1
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

#define BOARD_HAS_LED 0
#define BOARD_BUTTON 0
#define BOARD_BUTTON_ACTIVE_LOW 1

#else

#define BOARD_HAS_DISPLAY 0
#define BOARD_HAS_LED 0

#endif
