#include <string.h>

#include "captive_dns.h"
#include "test_support.h"

// Builds a query for one name: header + QNAME labels + type/class.
static size_t query(uint8_t *buf, uint16_t id, const char *name,
                    uint16_t qtype) {
  memset(buf, 0, 12);
  buf[0] = (uint8_t)(id >> 8);
  buf[1] = (uint8_t)id;
  buf[2] = 0x01;  // RD
  buf[5] = 1;     // QDCOUNT 1
  size_t off = 12;
  const char *p = name;
  while (*p) {
    const char *dot = strchr(p, '.');
    const size_t l = dot ? (size_t)(dot - p) : strlen(p);
    buf[off++] = (uint8_t)l;
    memcpy(buf + off, p, l);
    off += l;
    p = dot ? dot + 1 : p + l;
  }
  buf[off++] = 0;
  buf[off++] = (uint8_t)(qtype >> 8);
  buf[off++] = (uint8_t)qtype;
  buf[off++] = 0;
  buf[off++] = 1;  // IN
  return off;
}

static const uint32_t IP =
    0x0104A8C0u;  // 192.168.4.1 in network order (LE host)

static void answers_an_a_query_with_our_address(void) {
  uint8_t q[128], r[256];
  const size_t qlen = query(q, 0xBEEF, "captive.apple.com", 1);
  const size_t rlen = captive_dns_answer(q, qlen, r, sizeof(r), IP);
  CHECK_EQ_INT(rlen, qlen + 16);
  CHECK_EQ_INT(r[0], 0xBE);
  CHECK_EQ_INT(r[1], 0xEF);         // id echoed
  CHECK_EQ_INT(r[2] & 0x80, 0x80);  // QR
  CHECK_EQ_INT(r[7], 1);            // ANCOUNT
  CHECK_EQ_INT(r[9], 0);            // NSCOUNT
  CHECK_EQ_INT(r[11], 0);           // ARCOUNT
  // Question copied verbatim.
  CHECK(memcmp(r + 12, q + 12, qlen - 12) == 0);
  // Answer: pointer to the question name, A, IN, TTL, rdlen 4, our IP.
  const uint8_t *a = r + qlen;
  CHECK_EQ_INT(a[0], 0xC0);
  CHECK_EQ_INT(a[1], 12);
  CHECK_EQ_INT(a[3], 1);   // type A
  CHECK_EQ_INT(a[5], 1);   // class IN
  CHECK_EQ_INT(a[11], 4);  // rdlength
  CHECK(memcmp(a + 12, &IP, 4) == 0);
}

static void non_a_queries_get_an_empty_answer(void) {
  uint8_t q[128], r[256];
  const size_t qlen = query(q, 1, "example.com", 28);  // AAAA
  const size_t rlen = captive_dns_answer(q, qlen, r, sizeof(r), IP);
  CHECK_EQ_INT(rlen, qlen);         // header + question, no answer section
  CHECK_EQ_INT(r[2] & 0x80, 0x80);  // still a response
  CHECK_EQ_INT(r[7], 0);            // ANCOUNT 0
}

static void refuses_malformed_and_non_queries(void) {
  uint8_t q[128], r[256];
  const size_t qlen = query(q, 1, "a.b", 1);
  // Too short for a header; truncated inside the name; a response; opcode.
  CHECK_EQ_INT(captive_dns_answer(q, 11, r, sizeof(r), IP), 0);
  CHECK_EQ_INT(captive_dns_answer(q, qlen - 5, r, sizeof(r), IP), 0);
  // Name intact, but the type/class fields cut short: a different check.
  CHECK_EQ_INT(captive_dns_answer(q, qlen - 2, r, sizeof(r), IP), 0);
  q[2] |= 0x80;
  CHECK_EQ_INT(captive_dns_answer(q, qlen, r, sizeof(r), IP), 0);
  q[2] = 0x01 | 0x08;  // opcode 1 (IQUERY)
  CHECK_EQ_INT(captive_dns_answer(q, qlen, r, sizeof(r), IP), 0);
  q[2] = 0x01;
  // QDCOUNT 0; a compression pointer in a question; a response buffer too small.
  q[5] = 0;
  CHECK_EQ_INT(captive_dns_answer(q, qlen, r, sizeof(r), IP), 0);
  q[5] = 1;
  q[12] = 0xC0;
  CHECK_EQ_INT(captive_dns_answer(q, qlen, r, sizeof(r), IP), 0);
  q[12] = 1;
  CHECK_EQ_INT(captive_dns_answer(q, qlen, r, qlen + 15, IP), 0);
  // Positive control: the same packet with room answers.
  CHECK_EQ_INT(captive_dns_answer(q, qlen, r, qlen + 16, IP), qlen + 16);
  // A label whose length runs past the packet end.
  q[12] = 60;
  CHECK_EQ_INT(captive_dns_answer(q, qlen, r, sizeof(r), IP), 0);
}

static void edns_additional_record_is_dropped(void) {
  uint8_t q[128], r[256];
  size_t qlen = query(q, 3, "example.org", 1);
  // An OPT pseudo-record in the additional section, ARCOUNT 1.
  const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
  memcpy(q + qlen, opt, sizeof(opt));
  q[11] = 1;
  const size_t rlen =
      captive_dns_answer(q, qlen + sizeof(opt), r, sizeof(r), IP);
  // Header + question + one answer; the OPT record is not echoed and
  // ARCOUNT says so.
  CHECK_EQ_INT(rlen, qlen + 16);
  CHECK_EQ_INT(r[11], 0);
  CHECK_EQ_INT(r[7], 1);
}

static void answers_each_a_question_of_several(void) {
  uint8_t q[256], r[512];
  size_t qlen = query(q, 2, "one.test", 1);
  // Append a second question (AAAA) and a third (A) by hand.
  const uint8_t two[] = {3,   't', 'w', 'o', 4,  't', 'e',
                         's', 't', 0,   0,   28, 0,   1};
  const uint8_t three[] = {5, 't', 'h', 'r', 'e', 'e', 0, 0, 1, 0, 1};
  memcpy(q + qlen, two, sizeof(two));
  qlen += sizeof(two);
  memcpy(q + qlen, three, sizeof(three));
  qlen += sizeof(three);
  q[5] = 3;
  const size_t rlen = captive_dns_answer(q, qlen, r, sizeof(r), IP);
  CHECK_EQ_INT(rlen, qlen + 2 * 16);
  CHECK_EQ_INT(r[7], 2);
  // Second answer points at the third question's name, not the second's.
  const uint8_t *a2 = r + qlen + 16;
  const size_t third_off = qlen - sizeof(three);
  CHECK_EQ_INT(a2[0], 0xC0 | (third_off >> 8));
  CHECK_EQ_INT(a2[1], third_off & 0xFF);
}

int main(void) {
  RUN(answers_an_a_query_with_our_address);
  RUN(non_a_queries_get_an_empty_answer);
  RUN(refuses_malformed_and_non_queries);
  RUN(edns_additional_record_is_dropped);
  RUN(answers_each_a_question_of_several);
  return test_summary("captive_dns");
}
