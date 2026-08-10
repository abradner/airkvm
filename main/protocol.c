#include "protocol.h"

#include <string.h>

static enum airkvm_decode_result decode_key(const uint8_t *payload, size_t len,
                                            struct airkvm_intent *out) {
  if (len < 1) {
    return AIRKVM_DECODE_TRUNCATED;
  }
  const size_t code_len = payload[0];
  // Length bound before the frame-size equality: checked the other way round,
  // TOO_LONG is unreachable for any caller that caps its receive buffer, and an
  // over-long code reports as merely truncated. Same rejection either way — the
  // difference is whether the log tells you which malformed thing arrived.
  if (code_len > AIRKVM_MAX_CODE_LEN) {
    return AIRKVM_DECODE_TOO_LONG;
  }
  if (len != 1 + code_len) {
    return AIRKVM_DECODE_TRUNCATED;
  }
  memcpy(out->code, payload + 1, code_len);
  out->code[code_len] = '\0';
  return AIRKVM_DECODE_OK;
}

enum airkvm_decode_result airkvm_decode(const uint8_t *data, size_t len,
                                        struct airkvm_intent *out) {
  if (data == NULL || out == NULL || len == 0) {
    return AIRKVM_DECODE_EMPTY;
  }

  const uint8_t type = data[0];
  const uint8_t *payload = data + 1;
  const size_t payload_len = len - 1;

  // Decoded into a scratch struct and only copied out on success, so a rejected
  // frame cannot leave `out` half-written (see the contract in protocol.h).
  struct airkvm_intent scratch;
  memset(&scratch, 0, sizeof(scratch));
  scratch.type = (enum airkvm_frame_type)type;

  switch (type) {
    case AIRKVM_FRAME_KEY_DOWN:
    case AIRKVM_FRAME_KEY_UP: {
      const enum airkvm_decode_result r =
          decode_key(payload, payload_len, &scratch);
      if (r != AIRKVM_DECODE_OK) {
        return r;
      }
      break;
    }

    case AIRKVM_FRAME_MOUSE_MOVE:
      if (payload_len != 4) {
        return AIRKVM_DECODE_TRUNCATED;
      }
      scratch.move.dx =
          (int16_t)((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));
      scratch.move.dy =
          (int16_t)((uint16_t)payload[2] | ((uint16_t)payload[3] << 8));
      break;

    case AIRKVM_FRAME_MOUSE_BUTTON:
      if (payload_len != 1) {
        return AIRKVM_DECODE_TRUNCATED;
      }
      scratch.button_mask = payload[0];
      break;

    case AIRKVM_FRAME_MOUSE_SCROLL:
      if (payload_len != 2) {
        return AIRKVM_DECODE_TRUNCATED;
      }
      scratch.scroll.vertical = (int8_t)payload[0];
      scratch.scroll.horizontal = (int8_t)payload[1];
      break;

    case AIRKVM_FRAME_RELEASE_ALL:
    case AIRKVM_FRAME_PING:
      if (payload_len != 0) {
        return AIRKVM_DECODE_TRUNCATED;
      }
      break;

    default:
      return AIRKVM_DECODE_UNKNOWN_TYPE;
  }

  *out = scratch;
  return AIRKVM_DECODE_OK;
}
