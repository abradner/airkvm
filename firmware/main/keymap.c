#include "keymap.h"

#include <string.h>

// The mapping lives here, in C, rather than in the console's JavaScript, so
// that exactly one table defines it and that table is host-tested (AGENTS.md
// §4). The console therefore sends `event.code` strings on the wire; at human
// typing rates the extra bytes are irrelevant next to having a second copy of
// this table drift.
//
// `event.code` names a *physical key position*, which is precisely what a HID
// usage code means — so this mapping is layout-independent and stays correct on
// non-US keyboards. Mapping `event.key` instead would be a bug (AGENTS.md §4).
struct keymap_entry {
  const char *code;
  uint8_t usage;
};

// clang-format off
// Kept as a hand-aligned grid: this is a lookup table, and one entry per line
// makes a 110-row wall out of something you should be able to scan for a gap.
static const struct keymap_entry k_keymap[] = {
    // Letters (usage 0x04..0x1D, alphabetical by position name)
    {"KeyA", 0x04}, {"KeyB", 0x05}, {"KeyC", 0x06}, {"KeyD", 0x07},
    {"KeyE", 0x08}, {"KeyF", 0x09}, {"KeyG", 0x0A}, {"KeyH", 0x0B},
    {"KeyI", 0x0C}, {"KeyJ", 0x0D}, {"KeyK", 0x0E}, {"KeyL", 0x0F},
    {"KeyM", 0x10}, {"KeyN", 0x11}, {"KeyO", 0x12}, {"KeyP", 0x13},
    {"KeyQ", 0x14}, {"KeyR", 0x15}, {"KeyS", 0x16}, {"KeyT", 0x17},
    {"KeyU", 0x18}, {"KeyV", 0x19}, {"KeyW", 0x1A}, {"KeyX", 0x1B},
    {"KeyY", 0x1C}, {"KeyZ", 0x1D},

    // Number row. HID orders these 1..9 then 0, not in numeric order:
    // Digit0 is 0x27, after Digit9.
    {"Digit1", 0x1E}, {"Digit2", 0x1F}, {"Digit3", 0x20}, {"Digit4", 0x21},
    {"Digit5", 0x22}, {"Digit6", 0x23}, {"Digit7", 0x24}, {"Digit8", 0x25},
    {"Digit9", 0x26}, {"Digit0", 0x27},

    // Control and punctuation
    {"Enter", 0x28}, {"Escape", 0x29}, {"Backspace", 0x2A}, {"Tab", 0x2B},
    {"Space", 0x2C}, {"Minus", 0x2D}, {"Equal", 0x2E}, {"BracketLeft", 0x2F},
    {"BracketRight", 0x30}, {"Backslash", 0x31}, {"Semicolon", 0x33}, {"Quote", 0x34},
    {"Backquote", 0x35}, {"Comma", 0x36}, {"Period", 0x37}, {"Slash", 0x38},
    {"CapsLock", 0x39},

    // Function row
    {"F1", 0x3A}, {"F2", 0x3B}, {"F3", 0x3C}, {"F4", 0x3D},
    {"F5", 0x3E}, {"F6", 0x3F}, {"F7", 0x40}, {"F8", 0x41},
    {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45},

    // Navigation cluster
    {"PrintScreen", 0x46}, {"ScrollLock", 0x47}, {"Pause", 0x48}, {"Insert", 0x49},
    {"Home", 0x4A}, {"PageUp", 0x4B}, {"Delete", 0x4C}, {"End", 0x4D},
    {"PageDown", 0x4E}, {"ArrowRight", 0x4F}, {"ArrowLeft", 0x50}, {"ArrowDown", 0x51},
    {"ArrowUp", 0x52},

    // Keypad
    {"NumLock", 0x53}, {"NumpadDivide", 0x54}, {"NumpadMultiply", 0x55}, {"NumpadSubtract", 0x56},
    {"NumpadAdd", 0x57}, {"NumpadEnter", 0x58}, {"Numpad1", 0x59}, {"Numpad2", 0x5A},
    {"Numpad3", 0x5B}, {"Numpad4", 0x5C}, {"Numpad5", 0x5D}, {"Numpad6", 0x5E},
    {"Numpad7", 0x5F}, {"Numpad8", 0x60}, {"Numpad9", 0x61}, {"Numpad0", 0x62},
    {"NumpadDecimal", 0x63},

    // ISO layouts have a key between LeftShift and KeyZ that ANSI layouts lack.
    {"IntlBackslash", 0x64}, {"ContextMenu", 0x65},

    // Modifiers. Order here matches the HID modifier byte's bit order, which
    // keymap_modifier_bit() derives arithmetically — do not renumber.
    {"ControlLeft", 0xE0}, {"ShiftLeft", 0xE1}, {"AltLeft", 0xE2}, {"MetaLeft", 0xE3},
    {"ControlRight", 0xE4}, {"ShiftRight", 0xE5}, {"AltRight", 0xE6}, {"MetaRight", 0xE7},
};
// clang-format on

uint8_t keymap_lookup(const char *code) {
  if (code == NULL) {
    return 0;
  }
  // Linear scan over ~110 short strings, at human typing rates. A sorted table
  // plus bsearch would be faster and hand-sorting it would be a source of bugs;
  // this is not on any path where the difference is measurable.
  for (size_t i = 0; i < sizeof(k_keymap) / sizeof(k_keymap[0]); i++) {
    if (strcmp(code, k_keymap[i].code) == 0) {
      return k_keymap[i].usage;
    }
  }
  return 0;
}
