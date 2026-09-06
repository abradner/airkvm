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
//   0x08 WAKE         (no payload)  ask the dongle to signal USB remote wakeup
//
// The dongle also sends frames to the console, in the same [u8 type][payload]
// shape. Downstream types have bit 7 set, so the two directions can never be
// confused for one another even if a frame is ever reflected:
//
//   0x80 WAKE_RESULT  [u8 result]   reply to WAKE; see enum airkvm_wake_result
//
// This is the first downstream frame the protocol has had. No convention
// existed before it; the bit-7 split is a proposal (AGENTS.md §8), not settled
// law — codify or correct it in review.
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
  // Asks the dongle to signal USB remote wakeup. Meaningful only while the
  // target has suspended the bus with wakeup armed; every other state is
  // reported back rather than guessed at (see airkvm_wake_result).
  AIRKVM_FRAME_WAKE = 0x08,
};

// Dongle → console frame types. Bit 7 set, see the table above.
enum airkvm_downstream_type {
  AIRKVM_DOWN_WAKE_RESULT = 0x80,
};

// What happened to a WAKE. REQUESTED means the preconditions held and the
// wakeup pulse was handed to the USB stack — it does not mean the target woke;
// only the bus resuming is evidence of that. The other three name which
// precondition failed, so the console can say something more useful than "no".
enum airkvm_wake_result {
  AIRKVM_WAKE_REQUESTED = 0,
  AIRKVM_WAKE_NO_HOST = 1,        // not enumerated: nothing to wake
  AIRKVM_WAKE_NOT_SUSPENDED = 2,  // bus is active: the target is not asleep
  AIRKVM_WAKE_NOT_ENABLED_BY_HOST =
      3,  // target suspended without arming wakeup
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
