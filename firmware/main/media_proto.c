#include "media_proto.h"

#include <stdint.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

enum media_decode_result media_decode(const uint8_t *frame, size_t len,
                                      struct media_msg *out) {
  if (frame == NULL || out == NULL || len == 0) {
    return MEDIA_DECODE_EMPTY;
  }
  const uint8_t type = frame[0];
  const uint8_t *p = frame + 1;
  const size_t n = len - 1;

  struct media_msg scratch;
  memset(&scratch, 0, sizeof(scratch));
  scratch.type = (enum media_frame_type)type;

  switch (type) {
    case MEDIA_FRAME_ATTACH: {
      // Fixed part first, then the name whose length the fixed part carries.
      if (n < 2 + 8 + 1 + 1) {
        return MEDIA_DECODE_TRUNCATED;
      }
      const uint8_t name_len = p[11];
      if (name_len > MEDIA_NAME_MAX) {
        return MEDIA_DECODE_TRUNCATED;
      }
      if (n != 12u + name_len) {
        return MEDIA_DECODE_TRUNCATED;
      }
      const uint16_t block_size = rd16(p);
      const uint64_t block_count = rd64(p + 2);
      // What the LUN can serve (msc_source.h): a uint32 count and 512-byte
      // multiples. Refused here so the check is host-tested, not deferred to
      // the attach handler.
      if (block_size == 0 || block_size % 512 != 0 || block_count == 0 ||
          block_count > UINT32_MAX) {
        return MEDIA_DECODE_BAD_RANGE;
      }
      scratch.attach.block_size = block_size;
      scratch.attach.block_count = (uint32_t)block_count;
      scratch.attach.flags = p[10];
      memcpy(scratch.attach.name, p + 12, name_len);
      scratch.attach.name[name_len] = '\0';
      break;
    }

    case MEDIA_FRAME_DETACH:
      if (n != 0) {
        return MEDIA_DECODE_TRUNCATED;
      }
      break;

    case MEDIA_FRAME_DATA: {
      if (n < 14) {
        return MEDIA_DECODE_TRUNCATED;
      }
      const uint32_t offset = rd32(p + 8);
      const uint16_t dlen = rd16(p + 12);
      if (n != 14u + dlen) {
        return MEDIA_DECODE_TRUNCATED;
      }
      // A zero-length piece carries nothing and would let a client spin the
      // dongle without progress; a piece past the chunk end would write past
      // the slot. Both are refused, not clamped. 64-bit so a huge offset
      // cannot wrap the sum back inside the chunk.
      if (dlen == 0 || dlen > MEDIA_DATA_MAX ||
          (uint64_t)offset + dlen > MEDIA_CHUNK_SIZE) {
        return MEDIA_DECODE_BAD_RANGE;
      }
      scratch.data.req_id = rd32(p);
      scratch.data.chunk = rd32(p + 4);
      scratch.data.offset = offset;
      scratch.data.len = dlen;
      scratch.data.data = p + 14;
      break;
    }

    case MEDIA_FRAME_ERROR:
      if (n != 5) {
        return MEDIA_DECODE_TRUNCATED;
      }
      scratch.error.req_id = rd32(p);
      scratch.error.code = p[4];
      break;

    default:
      return MEDIA_DECODE_UNKNOWN_TYPE;
  }

  *out = scratch;
  return MEDIA_DECODE_OK;
}

size_t media_split(uint64_t byte_start, uint32_t len,
                   struct media_span out[2]) {
  if (len == 0 || len > MEDIA_CHUNK_SIZE || out == NULL) {
    return 0;
  }
  const uint64_t first_chunk = byte_start / MEDIA_CHUNK_SIZE;
  const uint32_t first_off = (uint32_t)(byte_start % MEDIA_CHUNK_SIZE);
  if (first_chunk > UINT32_MAX) {
    return 0;
  }
  out[0].chunk = (uint32_t)first_chunk;
  out[0].offset = first_off;
  if (first_off + len <= MEDIA_CHUNK_SIZE) {
    out[0].len = len;
    return 1;
  }
  out[0].len = MEDIA_CHUNK_SIZE - first_off;
  if (first_chunk + 1 > UINT32_MAX) {
    return 0;
  }
  out[1].chunk = (uint32_t)first_chunk + 1;
  out[1].offset = 0;
  out[1].len = len - out[0].len;
  return 2;
}

size_t media_encode_req(uint8_t *buf, size_t cap, uint32_t req_id,
                        uint32_t chunk) {
  if (buf == NULL || cap < MEDIA_DOWN_REQ_LEN) {
    return 0;
  }
  buf[0] = MEDIA_DOWN_REQ;
  wr32(buf + 1, req_id);
  wr32(buf + 5, chunk);
  return MEDIA_DOWN_REQ_LEN;
}

size_t media_encode_status(uint8_t *buf, size_t cap, enum media_state state,
                           uint32_t bytes_served, uint32_t pending) {
  if (buf == NULL || cap < MEDIA_DOWN_STATUS_LEN) {
    return 0;
  }
  buf[0] = MEDIA_DOWN_STATUS;
  buf[1] = (uint8_t)state;
  wr32(buf + 2, bytes_served);
  wr32(buf + 6, pending);
  return MEDIA_DOWN_STATUS_LEN;
}
