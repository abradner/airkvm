#include "captive_dns.h"

#include <stdbool.h>
#include <string.h>

#define DNS_HDR 12
#define QR_FLAG 0x8000u
#define OPCODE_MASK 0x7800u
#define QTYPE_A 1
#define QCLASS_IN 1
#define ANSWER_LEN 16  // name ptr(2) type(2) class(2) ttl(4) rdlen(2) ip(4)
#define TTL_SECONDS 60u

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static void wr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// Skips a QNAME starting at `off`, bounds-checked. Returns the offset just
// past the terminating zero label, or 0 on overrun / a compression pointer
// (which no sane question uses, and which we do not follow).
static size_t skip_name(const uint8_t *p, size_t len, size_t off) {
  while (off < len) {
    const uint8_t l = p[off];
    if (l == 0) {
      return off + 1;
    }
    if (l & 0xC0) {
      return 0;
    }
    off += 1u + l;
  }
  return 0;
}

size_t captive_dns_answer(const uint8_t *req, size_t len, uint8_t *resp,
                          size_t cap, uint32_t ip_be) {
  if (req == NULL || resp == NULL || len < DNS_HDR || len > cap) {
    return 0;
  }
  const uint16_t flags = rd16(req + 2);
  if ((flags & QR_FLAG) || (flags & OPCODE_MASK)) {
    return 0;  // a response, or not a standard query
  }
  const uint16_t qdcount = rd16(req + 4);
  if (qdcount == 0) {
    return 0;
  }

  // Walk the questions first so a malformed packet is refused whole rather
  // than half-answered, and count the A/IN ones.
  size_t off = DNS_HDR;
  uint16_t answers = 0;
  for (uint16_t q = 0; q < qdcount; q++) {
    const size_t after_name = skip_name(req, len, off);
    if (after_name == 0 || after_name + 4 > len) {
      return 0;
    }
    if (rd16(req + after_name) == QTYPE_A &&
        rd16(req + after_name + 2) == QCLASS_IN) {
      answers++;
    }
    off = after_name + 4;
  }
  const size_t out_len = off + (size_t)answers * ANSWER_LEN;
  if (out_len > cap) {
    return 0;
  }

  // Header + questions copied verbatim (anything after the questions is
  // dropped: EDNS OPT records are not needed for a lie this simple).
  memcpy(resp, req, off);
  wr16(resp + 2, (uint16_t)(QR_FLAG | 0x0100u));  // response, recursion desired
  wr16(resp + 6, answers);                        // ANCOUNT
  wr16(resp + 8, 0);                              // NSCOUNT
  wr16(resp + 10, 0);                             // ARCOUNT

  // One answer per A question, pointing back at its name.
  uint8_t *a = resp + off;
  size_t qoff = DNS_HDR;
  for (uint16_t q = 0; q < qdcount; q++) {
    const size_t after_name = skip_name(req, len, qoff);
    const bool is_a = rd16(req + after_name) == QTYPE_A &&
                      rd16(req + after_name + 2) == QCLASS_IN;
    if (is_a) {
      wr16(a, (uint16_t)(0xC000u | qoff));
      wr16(a + 2, QTYPE_A);
      wr16(a + 4, QCLASS_IN);
      wr32(a + 6, TTL_SECONDS);
      wr16(a + 10, 4);
      memcpy(a + 12, &ip_be, 4);
      a += ANSWER_LEN;
    }
    qoff = after_name + 4;
  }
  return out_len;
}
