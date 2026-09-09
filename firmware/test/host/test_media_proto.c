#include <string.h>

#include "media_proto.h"
#include "test_support.h"

// Builds an ATTACH frame the way the browser will: little-endian fields.
static size_t attach_frame(uint8_t *f, uint16_t bs, uint64_t count,
                           uint8_t flags, const char *name) {
  const size_t nl = strlen(name);
  f[0] = MEDIA_FRAME_ATTACH;
  f[1] = (uint8_t)bs;
  f[2] = (uint8_t)(bs >> 8);
  for (int i = 0; i < 8; i++) {
    f[3 + i] = (uint8_t)(count >> (8 * i));
  }
  f[11] = flags;
  f[12] = (uint8_t)nl;
  memcpy(f + 13, name, nl);
  return 13 + nl;
}

#define DATA_HDR 15  // type + req + chunk + u32 offset + u16 len

static size_t data_frame(uint8_t *f, uint32_t req, uint32_t chunk, uint32_t off,
                         uint16_t len) {
  f[0] = MEDIA_FRAME_DATA;
  for (int i = 0; i < 4; i++) {
    f[1 + i] = (uint8_t)(req >> (8 * i));
    f[5 + i] = (uint8_t)(chunk >> (8 * i));
    f[9 + i] = (uint8_t)(off >> (8 * i));
  }
  f[13] = (uint8_t)len;
  f[14] = (uint8_t)(len >> 8);
  for (uint16_t i = 0; i < len; i++) {
    f[DATA_HDR + i] = (uint8_t)i;
  }
  return DATA_HDR + len;
}

static void decodes_attach(void) {
  uint8_t f[64];
  struct media_msg m;
  const size_t n =
      attach_frame(f, 2048, 0x12345678ull, MEDIA_FLAG_READ_ONLY, "debian.iso");
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_OK);
  CHECK_EQ_INT(m.type, MEDIA_FRAME_ATTACH);
  CHECK_EQ_INT(m.attach.block_size, 2048);
  CHECK_EQ_INT(m.attach.block_count, 0x12345678u);
  CHECK_EQ_INT(m.attach.flags, MEDIA_FLAG_READ_ONLY);
  CHECK_EQ_STR(m.attach.name, "debian.iso");
  // Empty name is fine; a zero geometry is not a medium.
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 512, 1, 0, ""), &m),
               MEDIA_DECODE_OK);
  CHECK_EQ_STR(m.attach.name, "");
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 0, 1, 0, "x"), &m),
               MEDIA_DECODE_BAD_RANGE);
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 512, 0, 0, "x"), &m),
               MEDIA_DECODE_BAD_RANGE);
  // What the LUN cannot serve is refused here, where it is host-tested: a
  // block size that is not a multiple of 512, and a count past uint32.
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 513, 1, 0, "x"), &m),
               MEDIA_DECODE_BAD_RANGE);
  CHECK_EQ_INT(
      media_decode(f, attach_frame(f, 512, 0x100000000ull, 0, "x"), &m),
      MEDIA_DECODE_BAD_RANGE);
  // Positive controls at both edges.
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 512, 0xFFFFFFFFull, 0, "x"), &m),
               MEDIA_DECODE_OK);
  CHECK_EQ_INT(m.attach.block_count, 0xFFFFFFFFu);
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 2048, 1, 0, "x"), &m),
               MEDIA_DECODE_OK);
}

static void attach_name_is_bounded(void) {
  uint8_t f[80];
  struct media_msg m;
  char name[MEDIA_NAME_MAX + 2];
  memset(name, 'n', sizeof(name) - 1);
  name[MEDIA_NAME_MAX + 1] = '\0';  // one too long
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 512, 1, 0, name), &m),
               MEDIA_DECODE_TRUNCATED);
  // Positive control: exactly the maximum is accepted and terminated.
  name[MEDIA_NAME_MAX] = '\0';
  CHECK_EQ_INT(media_decode(f, attach_frame(f, 512, 1, 0, name), &m),
               MEDIA_DECODE_OK);
  CHECK_EQ_INT(strlen(m.attach.name), MEDIA_NAME_MAX);
  // Declared name length longer than the bytes present.
  size_t n = attach_frame(f, 512, 1, 0, "abc");
  f[12] = 9;
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_TRUNCATED);
  // Shorter than the fixed part at all.
  CHECK_EQ_INT(media_decode(f, 5, &m), MEDIA_DECODE_TRUNCATED);
}

static void decodes_data_and_points_into_frame(void) {
  static uint8_t f[DATA_HDR + MEDIA_DATA_MAX + 2];
  struct media_msg m;
  const size_t n = data_frame(f, 7, 3, 1024, 100);
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_OK);
  CHECK_EQ_INT(m.data.req_id, 7);
  CHECK_EQ_INT(m.data.chunk, 3);
  CHECK_EQ_INT(m.data.offset, 1024);
  CHECK_EQ_INT(m.data.len, 100);
  CHECK(m.data.data == f + DATA_HDR);  // no copy: the piece stays in the frame
  CHECK_EQ_INT(m.data.data[99], 99);
  // Length mismatches either way are refused, and so is a header cut short.
  CHECK_EQ_INT(media_decode(f, n - 1, &m), MEDIA_DECODE_TRUNCATED);
  CHECK_EQ_INT(media_decode(f, n + 1, &m), MEDIA_DECODE_TRUNCATED);
  CHECK_EQ_INT(media_decode(f, DATA_HDR - 1, &m), MEDIA_DECODE_TRUNCATED);
  CHECK_EQ_INT(media_decode(f, n, NULL), MEDIA_DECODE_EMPTY);
}

static void data_piece_must_fit_its_chunk(void) {
  static uint8_t f[DATA_HDR + MEDIA_DATA_MAX + 2];
  struct media_msg m;
  // Last legal piece: ends exactly at the chunk boundary.
  size_t n = data_frame(f, 1, 0, MEDIA_CHUNK_SIZE - MEDIA_DATA_MAX,
                        (uint16_t)MEDIA_DATA_MAX);
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_OK);
  // One byte further: overruns the slot.
  n = data_frame(f, 1, 0, MEDIA_CHUNK_SIZE - MEDIA_DATA_MAX + 1,
                 (uint16_t)MEDIA_DATA_MAX);
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_BAD_RANGE);
  // Zero-length piece: refused, not "accepted and useless".
  n = data_frame(f, 1, 0, 0, 0);
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_BAD_RANGE);
  // A piece larger than one record payload, even though it fits the chunk.
  n = data_frame(f, 1, 0, 0, (uint16_t)(MEDIA_DATA_MAX + 1));
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_BAD_RANGE);
  // An offset chosen so that a 32-bit sum would wrap back inside the chunk.
  n = data_frame(f, 1, 0, 0xFFFFFFF0u, 32);
  CHECK_EQ_INT(media_decode(f, n, &m), MEDIA_DECODE_BAD_RANGE);
}

static void decodes_detach_and_error(void) {
  struct media_msg m;
  const uint8_t detach[] = {MEDIA_FRAME_DETACH};
  CHECK_EQ_INT(media_decode(detach, sizeof(detach), &m), MEDIA_DECODE_OK);
  CHECK_EQ_INT(m.type, MEDIA_FRAME_DETACH);
  const uint8_t fat_detach[] = {MEDIA_FRAME_DETACH, 0};
  CHECK_EQ_INT(media_decode(fat_detach, sizeof(fat_detach), &m),
               MEDIA_DECODE_TRUNCATED);
  const uint8_t err[] = {MEDIA_FRAME_ERROR,      0x2A, 0, 0, 0,
                         MEDIA_ERROR_READ_FAILED};
  CHECK_EQ_INT(media_decode(err, sizeof(err), &m), MEDIA_DECODE_OK);
  CHECK_EQ_INT(m.error.req_id, 0x2A);
  CHECK_EQ_INT(m.error.code, MEDIA_ERROR_READ_FAILED);
  CHECK_EQ_INT(media_decode(err, sizeof(err) - 1, &m), MEDIA_DECODE_TRUNCATED);
}

static void rejects_unknown_empty_and_reflected(void) {
  struct media_msg m;
  const uint8_t unknown[] = {0x7F};
  CHECK_EQ_INT(media_decode(unknown, 1, &m), MEDIA_DECODE_UNKNOWN_TYPE);
  // Our own downstream types coming back upstream are not requests.
  const uint8_t reflected[] = {MEDIA_DOWN_REQ, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK_EQ_INT(media_decode(reflected, sizeof(reflected), &m),
               MEDIA_DECODE_UNKNOWN_TYPE);
  // Input-socket frames are not media frames either (0x01 KEY_DOWN).
  const uint8_t key[] = {0x01, 4, 'K', 'e', 'y', 'A'};
  CHECK_EQ_INT(media_decode(key, sizeof(key), &m), MEDIA_DECODE_UNKNOWN_TYPE);
  CHECK_EQ_INT(media_decode(unknown, 0, &m), MEDIA_DECODE_EMPTY);
  CHECK_EQ_INT(media_decode(NULL, 3, &m), MEDIA_DECODE_EMPTY);
}

static void rejection_leaves_output_untouched(void) {
  struct media_msg m;
  const uint8_t err[] = {MEDIA_FRAME_ERROR, 9, 0, 0, 0, 1};
  CHECK_EQ_INT(media_decode(err, sizeof(err), &m), MEDIA_DECODE_OK);
  uint8_t f[64];
  const size_t n = attach_frame(f, 512, 1, 0, "later");
  CHECK_EQ_INT(media_decode(f, n - 2, &m), MEDIA_DECODE_TRUNCATED);
  CHECK_EQ_INT(m.type, MEDIA_FRAME_ERROR);
  CHECK_EQ_INT(m.error.req_id, 9);
}

static void encodes_downstream_frames(void) {
  uint8_t buf[16];
  CHECK_EQ_INT(media_encode_req(buf, sizeof(buf), 0x01020304, 0x0A0B0C0D),
               MEDIA_DOWN_REQ_LEN);
  const uint8_t want_req[] = {MEDIA_DOWN_REQ, 4,    3,    2,   1,
                              0x0D,           0x0C, 0x0B, 0x0A};
  CHECK(memcmp(buf, want_req, sizeof(want_req)) == 0);
  CHECK_EQ_INT(media_encode_req(buf, MEDIA_DOWN_REQ_LEN - 1, 1, 2), 0);
  CHECK_EQ_INT(media_encode_req(NULL, 99, 1, 2), 0);

  CHECK_EQ_INT(
      media_encode_status(buf, sizeof(buf), MEDIA_STATE_BUSY, 0x100, 0x2),
      MEDIA_DOWN_STATUS_LEN);
  const uint8_t want_st[] = {MEDIA_DOWN_STATUS, 2, 0, 1, 0, 0, 2, 0, 0, 0};
  CHECK(memcmp(buf, want_st, sizeof(want_st)) == 0);
  CHECK_EQ_INT(media_encode_status(NULL, 99, MEDIA_STATE_NONE, 0, 0), 0);
  CHECK_EQ_INT(media_encode_status(buf, MEDIA_DOWN_STATUS_LEN - 1,
                                   MEDIA_STATE_NONE, 0, 0),
               0);
}

static void splits_ranges_across_chunks(void) {
  struct media_span sp[2];
  // Inside one chunk.
  CHECK_EQ_INT(media_split(100, 50, sp), 1);
  CHECK_EQ_INT(sp[0].chunk, 0);
  CHECK_EQ_INT(sp[0].offset, 100);
  CHECK_EQ_INT(sp[0].len, 50);
  // Ending exactly on the boundary stays in one chunk.
  CHECK_EQ_INT(media_split(MEDIA_CHUNK_SIZE - 512, 512, sp), 1);
  CHECK_EQ_INT(sp[0].len, 512);
  // One byte over the boundary: two pieces that add up.
  CHECK_EQ_INT(media_split(MEDIA_CHUNK_SIZE - 512, 513, sp), 2);
  CHECK_EQ_INT(sp[0].chunk, 0);
  CHECK_EQ_INT(sp[0].len, 512);
  CHECK_EQ_INT(sp[1].chunk, 1);
  CHECK_EQ_INT(sp[1].offset, 0);
  CHECK_EQ_INT(sp[1].len, 1);
  // Starting on a boundary.
  CHECK_EQ_INT(media_split(3ull * MEDIA_CHUNK_SIZE, 4096, sp), 1);
  CHECK_EQ_INT(sp[0].chunk, 3);
  CHECK_EQ_INT(sp[0].offset, 0);
  // Deep into a large image: 64-bit byte offsets, 32-bit chunk numbers.
  CHECK_EQ_INT(media_split(0x1FFFF0000ull, 8, sp), 1);
  CHECK_EQ_INT(sp[0].chunk, 0x1FFFF0000ull / MEDIA_CHUNK_SIZE);
  CHECK_EQ_INT(sp[0].offset, 0);
  // And one byte before that boundary, 8 bytes straddle it.
  CHECK_EQ_INT(media_split(0x1FFFEFFFFull, 8, sp), 2);
  CHECK_EQ_INT(sp[0].len, 1);
  CHECK_EQ_INT(sp[1].len, 7);
  // Degenerate: nothing, or more than a chunk.
  CHECK_EQ_INT(media_split(0, 0, sp), 0);
  CHECK_EQ_INT(media_split(0, MEDIA_CHUNK_SIZE + 1, sp), 0);
  // A whole chunk starting mid-chunk is the largest two-piece case.
  CHECK_EQ_INT(media_split(1, MEDIA_CHUNK_SIZE, sp), 2);
  CHECK_EQ_INT(sp[0].len + sp[1].len, MEDIA_CHUNK_SIZE);
}

int main(void) {
  RUN(splits_ranges_across_chunks);
  RUN(decodes_attach);
  RUN(attach_name_is_bounded);
  RUN(decodes_data_and_points_into_frame);
  RUN(data_piece_must_fit_its_chunk);
  RUN(decodes_detach_and_error);
  RUN(rejects_unknown_empty_and_reflected);
  RUN(rejection_leaves_output_untouched);
  RUN(encodes_downstream_frames);
  return test_summary("media_proto");
}
