// Wire format between the console and the dongle, and its decoder.
//
// Pure: no I/O, no allocation, no ESP-IDF headers. Compiles and is tested on
// the host (see test/host). AGENTS.md §4.
//
// Frames are binary WebSocket messages, little-endian:
//
//   [u8 type][payload...]
//
//   0x01 KEY_DOWN     [u8 len][len bytes of KeyboardEvent.code, not NUL-terminated]
//   0x02 KEY_UP       same payload as KEY_DOWN
//   0x03 MOUSE_MOVE   [i16 dx][i16 dy]
//   0x04 MOUSE_BUTTON [u8 mask]  absolute state, bit 0 left, 1 right, 2 middle
//   0x05 MOUSE_SCROLL [i8 vertical][i8 horizontal]
//   0x06 RELEASE_ALL  (no payload)
//   0x07 PING         (no payload)
//
// Mouse buttons are sent as an absolute mask rather than down/up events so that
// a dropped frame self-corrects on the next one, instead of leaving a button
// held (AGENTS.md §2, principle 4).
#pragma once

#include <stddef.h>
#include <stdint.h>

enum airkvm_frame_type {
  AIRKVM_FRAME_KEY_DOWN = 0x01,
  AIRKVM_FRAME_KEY_UP = 0x02,
  AIRKVM_FRAME_MOUSE_MOVE = 0x03,
  AIRKVM_FRAME_MOUSE_BUTTON = 0x04,
  AIRKVM_FRAME_MOUSE_SCROLL = 0x05,
  AIRKVM_FRAME_RELEASE_ALL = 0x06,
  AIRKVM_FRAME_PING = 0x07,
};

// Longest KeyboardEvent.code this accepts. The longest real one is
// "NumpadMultiply" / "AudioVolumeDown" at 15; 23 leaves room without letting a
// caller push an unbounded string into a stack buffer.
#define AIRKVM_MAX_CODE_LEN 23

struct airkvm_intent {
  enum airkvm_frame_type type;
  union {
    // KEY_DOWN / KEY_UP. NUL-terminated by the decoder so downstream code can
    // treat it as a normal C string.
    char code[AIRKVM_MAX_CODE_LEN + 1];
    struct {
      int16_t dx;
      int16_t dy;
    } move;
    uint8_t button_mask;
    struct {
      int8_t vertical;
      int8_t horizontal;
    } scroll;
  };
};

enum airkvm_decode_result {
  AIRKVM_DECODE_OK = 0,
  AIRKVM_DECODE_EMPTY,         // zero-length frame
  AIRKVM_DECODE_UNKNOWN_TYPE,  // type byte not in the enum above
  AIRKVM_DECODE_TRUNCATED,     // payload shorter than the type requires
  AIRKVM_DECODE_TOO_LONG,      // key code longer than AIRKVM_MAX_CODE_LEN
};

// Decodes one frame. `out` is only meaningful when the result is
// AIRKVM_DECODE_OK; on every other result it is left untouched, so a caller
// that ignores the return value acts on stale data rather than garbage — the
// tests pin this, because "partially decoded" is the shape that produces stuck
// keys.
//
// Trailing bytes beyond what the type requires are rejected rather than
// ignored: a frame we don't fully understand is a frame from a client we don't
// fully understand, and this path ends in keystrokes on someone's machine.
enum airkvm_decode_result airkvm_decode(const uint8_t *data, size_t len,
                                        struct airkvm_intent *out);
