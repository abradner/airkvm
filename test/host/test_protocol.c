#include "protocol.h"
#include "test_support.h"

static void decodes_key_down(void) {
  const uint8_t frame[] = {AIRHID_FRAME_KEY_DOWN, 4, 'K', 'e', 'y', 'A'};
  struct airhid_intent out;
  CHECK_EQ_INT(airhid_decode(frame, sizeof(frame), &out), AIRHID_DECODE_OK);
  CHECK_EQ_INT(out.type, AIRHID_FRAME_KEY_DOWN);
  CHECK_EQ_STR(out.code, "KeyA");
}

static void decodes_mouse_move_including_negatives(void) {
  // -3 in little-endian two's complement, to pin the sign handling.
  const uint8_t frame[] = {AIRHID_FRAME_MOUSE_MOVE, 0x05, 0x00, 0xFD, 0xFF};
  struct airhid_intent out;
  CHECK_EQ_INT(airhid_decode(frame, sizeof(frame), &out), AIRHID_DECODE_OK);
  CHECK_EQ_INT(out.move.dx, 5);
  CHECK_EQ_INT(out.move.dy, -3);
}

static void decodes_scroll_and_buttons(void) {
  const uint8_t scroll[] = {AIRHID_FRAME_MOUSE_SCROLL, 0xFF, 0x01};
  struct airhid_intent out;
  CHECK_EQ_INT(airhid_decode(scroll, sizeof(scroll), &out), AIRHID_DECODE_OK);
  CHECK_EQ_INT(out.scroll.vertical, -1);
  CHECK_EQ_INT(out.scroll.horizontal, 1);

  const uint8_t buttons[] = {AIRHID_FRAME_MOUSE_BUTTON, 0x03};
  CHECK_EQ_INT(airhid_decode(buttons, sizeof(buttons), &out), AIRHID_DECODE_OK);
  CHECK_EQ_INT(out.button_mask, 0x03);
}

static void decodes_payloadless_frames(void) {
  const uint8_t release[] = {AIRHID_FRAME_RELEASE_ALL};
  struct airhid_intent out;
  CHECK_EQ_INT(airhid_decode(release, sizeof(release), &out), AIRHID_DECODE_OK);
  CHECK_EQ_INT(out.type, AIRHID_FRAME_RELEASE_ALL);

  const uint8_t ping[] = {AIRHID_FRAME_PING};
  CHECK_EQ_INT(airhid_decode(ping, sizeof(ping), &out), AIRHID_DECODE_OK);
  CHECK_EQ_INT(out.type, AIRHID_FRAME_PING);
}

static void rejects_malformed_frames(void) {
  struct airhid_intent out;

  const uint8_t empty[] = {0};
  CHECK_EQ_INT(airhid_decode(empty, 0, &out), AIRHID_DECODE_EMPTY);
  CHECK_EQ_INT(airhid_decode(NULL, 4, &out), AIRHID_DECODE_EMPTY);

  const uint8_t unknown[] = {0x7F, 0x00};
  CHECK_EQ_INT(airhid_decode(unknown, sizeof(unknown), &out),
               AIRHID_DECODE_UNKNOWN_TYPE);

  // Declared length exceeds the bytes actually present.
  const uint8_t short_key[] = {AIRHID_FRAME_KEY_DOWN, 4, 'K', 'e'};
  CHECK_EQ_INT(airhid_decode(short_key, sizeof(short_key), &out),
               AIRHID_DECODE_TRUNCATED);

  const uint8_t short_move[] = {AIRHID_FRAME_MOUSE_MOVE, 0x01, 0x00};
  CHECK_EQ_INT(airhid_decode(short_move, sizeof(short_move), &out),
               AIRHID_DECODE_TRUNCATED);

  // Trailing bytes are rejected, not ignored (see protocol.h).
  const uint8_t long_move[] = {AIRHID_FRAME_MOUSE_MOVE, 1, 0, 1, 0, 0xAA};
  CHECK_EQ_INT(airhid_decode(long_move, sizeof(long_move), &out),
               AIRHID_DECODE_TRUNCATED);

  const uint8_t fat_release[] = {AIRHID_FRAME_RELEASE_ALL, 0x00};
  CHECK_EQ_INT(airhid_decode(fat_release, sizeof(fat_release), &out),
               AIRHID_DECODE_TRUNCATED);
}

static void rejects_overlong_key_code(void) {
  // A code longer than the buffer must be refused, not truncated into it.
  uint8_t frame[2 + AIRHID_MAX_CODE_LEN + 1];
  frame[0] = AIRHID_FRAME_KEY_DOWN;
  frame[1] = AIRHID_MAX_CODE_LEN + 1;
  for (int i = 0; i < AIRHID_MAX_CODE_LEN + 1; i++) {
    frame[2 + i] = 'X';
  }
  struct airhid_intent out;
  CHECK_EQ_INT(airhid_decode(frame, sizeof(frame), &out),
               AIRHID_DECODE_TOO_LONG);

  // Positive control: one byte shorter is accepted, so the check above is
  // rejecting the length rather than the shape.
  frame[1] = AIRHID_MAX_CODE_LEN;
  CHECK_EQ_INT(airhid_decode(frame, sizeof(frame) - 1, &out), AIRHID_DECODE_OK);
}

static void rejection_leaves_output_untouched(void) {
  // The contract in protocol.h: a caller that ignores the return value acts on
  // stale data, never on a half-decoded frame. Partial decode is the shape that
  // produces stuck keys, so it is pinned here.
  struct airhid_intent out;
  const uint8_t good[] = {AIRHID_FRAME_KEY_DOWN, 4, 'K', 'e', 'y', 'B'};
  CHECK_EQ_INT(airhid_decode(good, sizeof(good), &out), AIRHID_DECODE_OK);
  CHECK_EQ_STR(out.code, "KeyB");

  const uint8_t bad[] = {AIRHID_FRAME_KEY_DOWN, 9, 'K', 'e', 'y'};
  CHECK_EQ_INT(airhid_decode(bad, sizeof(bad), &out), AIRHID_DECODE_TRUNCATED);
  CHECK_EQ_INT(out.type, AIRHID_FRAME_KEY_DOWN);
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
