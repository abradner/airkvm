// Wire format of the media socket (/ws-media), and its codec.
//
// Pure: no I/O, no allocation, no ESP-IDF headers. Compiles and is tested on
// the host (test/host/test_media_proto.c). AGENTS.md §4.
//
// A separate socket and a separate codec from the input path on purpose:
// a 16 KB block of disk image must never share a decoder, a buffer or a task
// with a keystroke. airkvm_decode and its 25-byte buffer are untouched, and a
// media frame arriving on /ws still trips that handler's oversized-frame
// teardown, which is the right answer.
//
// Frames are binary WebSocket messages, little-endian, [u8 type][payload]:
//
//   browser → dongle
//   0x10 MEDIA_ATTACH  [u16 block_size][u64 block_count][u8 flags]
//                      [u8 name_len][name_len bytes, not NUL-terminated]
//                      flags: bit 0 read-only (always set today)
//   0x11 MEDIA_DETACH  (no payload)
//   0x12 MEDIA_DATA    [u32 req_id][u32 chunk][u32 offset][u16 len][len bytes]
//                      one piece of the chunk the dongle asked for; pieces
//                      of a chunk arrive in order, each starting where the
//                      last ended (block_cache.h says why)
//   0x13 MEDIA_ERROR   [u32 req_id][u8 code]   the browser could not serve it
//
//   dongle → browser (bit 7 set, the same rule as protocol.h's WAKE_RESULT)
//   0x90 MEDIA_REQ     [u32 req_id][u32 chunk]   send me this chunk
//   0x91 MEDIA_STATUS  [u8 state][u32 bytes_served][u32 pending]
//
// A "chunk" is MEDIA_CHUNK_SIZE bytes of the image, chunk n starting at byte
// n * MEDIA_CHUNK_SIZE. The browser answers a MEDIA_REQ with as many
// MEDIA_DATA pieces as it likes, each at most MEDIA_DATA_MAX bytes so a
// piece's payload fits one inbound TLS record (the WebSocket and media
// headers spill into a second; that is fine, the point is that the record
// buffer never has to be larger than the default). Sizes are fixed so the
// decoder is total: unknown type, short payload, trailing bytes, a geometry
// the LUN cannot hold, or a piece that would overrun its chunk are rejected
// outright, and `out` is left untouched on rejection — the same contract as
// airkvm_decode, for the same reason.
//
// The offset is u32 although today a chunk is one piece and every piece
// starts at 0: a wider chunk later must not need a wire-format bump.
#pragma once

#include <stddef.h>
#include <stdint.h>

// 16 KB, which makes a chunk exactly one DATA piece. Larger chunks amortise
// the WiFi round trip over more USB reads, but every cache slot costs a chunk
// of RAM, and until PSRAM is enabled the cache lives in internal RAM beside
// two TLS sessions. The browser's copy of this number (web/app.js) ships in
// the same image, so the two cannot drift; raise both together with PSRAM.
#define MEDIA_CHUNK_SIZE 16384u
// The inbound TLS record payload (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN, pinned in
// sdkconfig.defaults so this cannot drift apart from it silently).
#define MEDIA_DATA_MAX 16384u
#define MEDIA_NAME_MAX 31u
#define MEDIA_FLAG_READ_ONLY 0x01u

enum media_frame_type {
  MEDIA_FRAME_ATTACH = 0x10,
  MEDIA_FRAME_DETACH = 0x11,
  MEDIA_FRAME_DATA = 0x12,
  MEDIA_FRAME_ERROR = 0x13,
};

enum media_down_type {
  MEDIA_DOWN_REQ = 0x90,
  MEDIA_DOWN_STATUS = 0x91,
};

enum media_state {
  MEDIA_STATE_NONE = 0,      // no medium attached
  MEDIA_STATE_ATTACHED = 1,  // this client's medium is the LUN's
  MEDIA_STATE_BUSY = 2,      // another client holds the medium (R-003's twin)
  // Refused: attaching would leave too little internal heap for the console's
  // next TLS handshake. Media degrades; the input socket must not.
  MEDIA_STATE_NO_MEMORY = 3,
};

enum media_error_code {
  MEDIA_ERROR_OUT_OF_RANGE = 1,  // chunk beyond the image
  MEDIA_ERROR_READ_FAILED = 2,   // the file went away, or the read threw
};

struct media_msg {
  enum media_frame_type type;
  union {
    struct {
      // Already narrowed to what msc_source.h promises the LUN: block_count
      // fits a uint32 and block_size is a non-zero multiple of 512. The wire
      // carries a u64 count so the format never has to change; the decoder
      // refuses anything this firmware cannot serve.
      uint16_t block_size;
      uint32_t block_count;
      uint8_t flags;
      char name[MEDIA_NAME_MAX + 1];  // NUL-terminated by the decoder
    } attach;
    struct {
      uint32_t req_id;
      uint32_t chunk;
      uint32_t offset;
      uint16_t len;
      const uint8_t *data;  // points into the frame; valid as long as it is
    } data;
    struct {
      uint32_t req_id;
      uint8_t code;
    } error;
  };
};

enum media_decode_result {
  MEDIA_DECODE_OK = 0,
  MEDIA_DECODE_EMPTY,
  MEDIA_DECODE_UNKNOWN_TYPE,
  MEDIA_DECODE_TRUNCATED,  // short, or trailing bytes (both are "not understood")
  MEDIA_DECODE_BAD_RANGE,  // piece overruns its chunk, or a geometry the LUN
                           // cannot serve (zero, count > 2^32-1, size not a
                           // multiple of 512)
};

enum media_decode_result media_decode(const uint8_t *frame, size_t len,
                                      struct media_msg *out);

// A byte range of the image expressed as pieces of chunks. A USB transfer is
// at most CONFIG_TINYUSB_MSC_BUFSIZE (4 KB) and a chunk is 16 KB, so a range
// touches at most two chunks; the second entry is unused when it does not.
// Pure arithmetic, host-tested, because getting an off-by-one here serves the
// wrong sector.
struct media_span {
  uint32_t chunk;
  uint32_t offset;  // within the chunk
  uint32_t len;
};
// Returns the number of spans filled (1 or 2); 0 if len is 0 or the range
// cannot be expressed (len > MEDIA_CHUNK_SIZE).
size_t media_split(uint64_t byte_start, uint32_t len, struct media_span out[2]);

// Encoders for the two downstream frames. Return the number of bytes written,
// or 0 if `cap` is too small.
#define MEDIA_DOWN_REQ_LEN 9
#define MEDIA_DOWN_STATUS_LEN 10
size_t media_encode_req(uint8_t *buf, size_t cap, uint32_t req_id,
                        uint32_t chunk);
size_t media_encode_status(uint8_t *buf, size_t cap, enum media_state state,
                           uint32_t bytes_served, uint32_t pending);
