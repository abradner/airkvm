#include "protocol.h"
#include "test_support.h"

static void decodes_key_down(void) {
  const uint8_t frame[] = {AIRKVM_FRAME_KEY_DOWN, 4, 'K', 'e', 'y', 'A'};
  struct airkvm_intent out;
  CHECK_EQ_INT(airkvm_decode(frame, sizeof(frame), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_INT(out.type, AIRKVM_FRAME_KEY_DOWN);
  CHECK_EQ_STR(out.code, "KeyA");
}

static void decodes_mouse_move_including_negatives(void) {
  // -3 in little-endian two's complement, to pin the sign handling.
  const uint8_t frame[] = {AIRKVM_FRAME_MOUSE_MOVE, 0x05, 0x00, 0xFD, 0xFF};
  struct airkvm_intent out;
  CHECK_EQ_INT(airkvm_decode(frame, sizeof(frame), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_INT(out.move.dx, 5);
  CHECK_EQ_INT(out.move.dy, -3);
}

static void decodes_scroll_and_buttons(void) {
  const uint8_t scroll[] = {AIRKVM_FRAME_MOUSE_SCROLL, 0xFF, 0x01};
  struct airkvm_intent out;
  CHECK_EQ_INT(airkvm_decode(scroll, sizeof(scroll), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_INT(out.scroll.vertical, -1);
  CHECK_EQ_INT(out.scroll.horizontal, 1);

  const uint8_t buttons[] = {AIRKVM_FRAME_MOUSE_BUTTON, 0x03};
  CHECK_EQ_INT(airkvm_decode(buttons, sizeof(buttons), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_INT(out.button_mask, 0x03);
}

static void decodes_payloadless_frames(void) {
  const uint8_t release[] = {AIRKVM_FRAME_RELEASE_ALL};
  struct airkvm_intent out;
  CHECK_EQ_INT(airkvm_decode(release, sizeof(release), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_INT(out.type, AIRKVM_FRAME_RELEASE_ALL);

  const uint8_t ping[] = {AIRKVM_FRAME_PING};
  CHECK_EQ_INT(airkvm_decode(ping, sizeof(ping), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_INT(out.type, AIRKVM_FRAME_PING);

  const uint8_t wake[] = {AIRKVM_FRAME_WAKE};
  CHECK_EQ_INT(airkvm_decode(wake, sizeof(wake), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_INT(out.type, AIRKVM_FRAME_WAKE);
}

static void rejects_malformed_frames(void) {
  struct airkvm_intent out;

  const uint8_t empty[] = {0};
  CHECK_EQ_INT(airkvm_decode(empty, 0, &out), AIRKVM_DECODE_EMPTY);
  CHECK_EQ_INT(airkvm_decode(NULL, 4, &out), AIRKVM_DECODE_EMPTY);

  const uint8_t unknown[] = {0x7F, 0x00};
  CHECK_EQ_INT(airkvm_decode(unknown, sizeof(unknown), &out),
               AIRKVM_DECODE_UNKNOWN_TYPE);

  // A downstream (dongle → console) type reflected back upstream is not an
  // input frame and must not be mistaken for one (protocol.h, bit-7 rule).
  const uint8_t reflected[] = {AIRKVM_DOWN_WAKE_RESULT, 0x00};
  CHECK_EQ_INT(airkvm_decode(reflected, sizeof(reflected), &out),
               AIRKVM_DECODE_UNKNOWN_TYPE);

  // Declared length exceeds the bytes actually present.
  const uint8_t short_key[] = {AIRKVM_FRAME_KEY_DOWN, 4, 'K', 'e'};
  CHECK_EQ_INT(airkvm_decode(short_key, sizeof(short_key), &out),
               AIRKVM_DECODE_TRUNCATED);

  const uint8_t short_move[] = {AIRKVM_FRAME_MOUSE_MOVE, 0x01, 0x00};
  CHECK_EQ_INT(airkvm_decode(short_move, sizeof(short_move), &out),
               AIRKVM_DECODE_TRUNCATED);

  // Trailing bytes are rejected, not ignored (see protocol.h).
  const uint8_t long_move[] = {AIRKVM_FRAME_MOUSE_MOVE, 1, 0, 1, 0, 0xAA};
  CHECK_EQ_INT(airkvm_decode(long_move, sizeof(long_move), &out),
               AIRKVM_DECODE_TRUNCATED);

  const uint8_t fat_release[] = {AIRKVM_FRAME_RELEASE_ALL, 0x00};
  CHECK_EQ_INT(airkvm_decode(fat_release, sizeof(fat_release), &out),
               AIRKVM_DECODE_TRUNCATED);

  // WAKE has no payload either; a trailing byte is a client we don't
  // understand asking to wake a machine, and is refused the same way.
  const uint8_t fat_wake[] = {AIRKVM_FRAME_WAKE, 0x00};
  CHECK_EQ_INT(airkvm_decode(fat_wake, sizeof(fat_wake), &out),
               AIRKVM_DECODE_TRUNCATED);
}

static void rejects_overlong_key_code(void) {
  // A code longer than the buffer must be refused, not truncated into it.
  uint8_t frame[2 + AIRKVM_MAX_CODE_LEN + 1];
  frame[0] = AIRKVM_FRAME_KEY_DOWN;
  frame[1] = AIRKVM_MAX_CODE_LEN + 1;
  for (int i = 0; i < AIRKVM_MAX_CODE_LEN + 1; i++) {
    frame[2 + i] = 'X';
  }
  struct airkvm_intent out;
  CHECK_EQ_INT(airkvm_decode(frame, sizeof(frame), &out),
               AIRKVM_DECODE_TOO_LONG);

  // Positive control: one byte shorter is accepted, so the check above is
  // rejecting the length rather than the shape.
  frame[1] = AIRKVM_MAX_CODE_LEN;
  CHECK_EQ_INT(airkvm_decode(frame, sizeof(frame) - 1, &out), AIRKVM_DECODE_OK);
}

static void rejection_leaves_output_untouched(void) {
  // The contract in protocol.h: a caller that ignores the return value acts on
  // stale data, never on a half-decoded frame. Partial decode is the shape that
  // produces stuck keys, so it is pinned here.
  struct airkvm_intent out;
  const uint8_t good[] = {AIRKVM_FRAME_KEY_DOWN, 4, 'K', 'e', 'y', 'B'};
  CHECK_EQ_INT(airkvm_decode(good, sizeof(good), &out), AIRKVM_DECODE_OK);
  CHECK_EQ_STR(out.code, "KeyB");

  const uint8_t bad[] = {AIRKVM_FRAME_KEY_DOWN, 9, 'K', 'e', 'y'};
  CHECK_EQ_INT(airkvm_decode(bad, sizeof(bad), &out), AIRKVM_DECODE_TRUNCATED);
  CHECK_EQ_INT(out.type, AIRKVM_FRAME_KEY_DOWN);
  CHECK_EQ_STR(out.code, "KeyB");
}

int main(void) {
  RUN(decodes_key_down);
  RUN(decodes_mouse_move_including_negatives);
  RUN(decodes_scroll_and_buttons);
  RUN(decodes_payloadless_frames);
  RUN(rejects_malformed_frames);
  RUN(rejects_overlong_key_code);
  RUN(rejection_leaves_output_untouched);
  return test_summary("protocol");
}
