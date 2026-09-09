// Browser KeyboardEvent.code -> USB HID usage code (Usage Page 0x07).
//
// Pure: no I/O, no allocation, no ESP-IDF headers. Compiles and is tested on
// the host (see test/host). AGENTS.md §4.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// HID usage codes for the eight modifier keys, in the bit order the HID boot
// keyboard report's modifier byte expects (LeftCtrl = bit 0 .. RightGUI = bit
// 7).
#define AIRKVM_USAGE_MOD_FIRST 0xE0
#define AIRKVM_USAGE_MOD_LAST 0xE7

// Returns 0 for anything unmapped. 0 is "no key" in HID, so an unknown code is
// inert by construction rather than something the caller must remember to
// check.
uint8_t keymap_lookup(const char *code);

// True if the usage is one of the eight modifiers, which the HID report carries
// in its modifier byte rather than in the six-key rollover array.
static inline bool keymap_is_modifier(uint8_t usage) {
  return usage >= AIRKVM_USAGE_MOD_FIRST && usage <= AIRKVM_USAGE_MOD_LAST;
}

// Bit for a modifier usage in the HID modifier byte. Undefined unless
// keymap_is_modifier(usage).
static inline uint8_t keymap_modifier_bit(uint8_t usage) {
  return (uint8_t)(1u << (usage - AIRKVM_USAGE_MOD_FIRST));
}
