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

// ESP32-S3-0.42 OLED supermini — the real dongle. UNVERIFIED: no board in hand.
// The 0.42" panel is the same size and controller as the C3 board's, but this
// family's variants differ in I2C pins, so these must be confirmed against the
// vendor's own demo for the S3 variant before being trusted (AGENTS.md §6.1).
#define BOARD_HAS_DISPLAY 1
#define BOARD_I2C_SDA 5
#define BOARD_I2C_SCL 6
#define BOARD_OLED_ADDR 0x3C
#define BOARD_OLED_WIDTH 72
#define BOARD_OLED_HEIGHT 40
#define BOARD_OLED_X_OFFSET 28
#define BOARD_OLED_Y_OFFSET 24

#define BOARD_HAS_LED 0
#define BOARD_BUTTON 0
#define BOARD_BUTTON_ACTIVE_LOW 1

#else

#define BOARD_HAS_DISPLAY 0
#define BOARD_HAS_LED 0

#endif
