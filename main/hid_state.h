// The single authoritative owner of what the target machine currently believes
// is held down. Every USB HID report originates here; the WebSocket layer
// parses frames into intents and hands them over, and never touches USB itself
// (AGENTS.md §4).
//
// Pure: no I/O, no allocation, no ESP-IDF headers, so the state machine that
// decides whether a key is held is host-tested. Pushing the resulting bytes to
// TinyUSB is usb_hid.c's job.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AIRHID_KEYBOARD_REPORT_LEN 8
#define AIRHID_MOUSE_REPORT_LEN 5

// A HID boot keyboard reports at most six non-modifier keys at once.
#define AIRHID_ROLLOVER_LIMIT 6

struct hid_state {
  uint8_t modifiers;                    // HID modifier byte, bit per modifier
  uint8_t keys[AIRHID_ROLLOVER_LIMIT];  // non-modifier usages, 0 = empty slot
  uint8_t buttons;                      // bit 0 left, 1 right, 2 middle
};

void hid_state_reset(struct hid_state *s);

// Press/release by HID usage code. Both are idempotent: pressing a held key or
// releasing an unheld one is a no-op rather than an error, because the network
// can duplicate and reorder, and neither case should be able to corrupt state.
// Usage 0 (keymap_lookup's "unknown") is ignored.
void hid_state_key_down(struct hid_state *s, uint8_t usage);
void hid_state_key_up(struct hid_state *s, uint8_t usage);

// Absolute button state, matching the wire format (see protocol.h).
void hid_state_set_buttons(struct hid_state *s, uint8_t mask);

// Clears every held key, modifier, and button. This is the safety action: any
// loss of confidence in the session runs it (AGENTS.md §2, principle 4). It is
// total by construction — one memset over the whole struct — rather than a
// sweep that a future field could be added outside of.
void hid_state_release_all(struct hid_state *s);

// True when nothing at all is held. Used to decide whether a release-all needs
// a report sent, and by the tests as a total-ness check.
bool hid_state_is_idle(const struct hid_state *s);

// Serializes current state into the 8-byte boot keyboard report:
//   [modifiers][reserved][key0..key5]
void hid_state_keyboard_report(const struct hid_state *s,
                               uint8_t out[AIRHID_KEYBOARD_REPORT_LEN]);

// Serializes a 5-byte mouse report: [buttons][dx][dy][vertical][horizontal].
// Movement and scroll are transient — they are arguments here rather than
// state, because a dropped move frame should simply not move, never repeat.
void hid_state_mouse_report(const struct hid_state *s, int8_t dx, int8_t dy,
                            int8_t scroll_v, int8_t scroll_h,
                            uint8_t out[AIRHID_MOUSE_REPORT_LEN]);
