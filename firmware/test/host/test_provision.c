#include <string.h>

#include "provision.h"
#include "test_support.h"

static bool parse(const char *body, struct provision_form *f) {
  return provision_parse_form(body, strlen(body), f);
}

static void parses_a_normal_form(void) {
  struct provision_form f;
  CHECK(parse("ssid=Home+WiFi&psk=correct%20horse", &f));
  CHECK_EQ_STR(f.ssid, "Home WiFi");
  CHECK_EQ_STR(f.psk, "correct horse");
  // Field order does not matter; unknown fields are ignored.
  CHECK(parse("psk=12345678&extra=1&ssid=x", &f));
  CHECK_EQ_STR(f.ssid, "x");
  CHECK_EQ_STR(f.psk, "12345678");
  // Open network: empty or absent psk.
  CHECK(parse("ssid=open&psk=", &f));
  CHECK_EQ_STR(f.psk, "");
  CHECK(parse("ssid=open", &f));
  CHECK_EQ_STR(f.psk, "");
  // Duplicate keys: last wins. A key with no '=' is ignored. Uppercase hex
  // and '+' decode in the passphrase too.
  CHECK(parse("ssid=first&ssid=second&junk&psk=pass+w%4Ard", &f));
  CHECK_EQ_STR(f.ssid, "second");
  CHECK_EQ_STR(f.psk, "pass wJrd");
}

static void refuses_what_a_network_cannot_use(void) {
  struct provision_form f;
  memset(&f, 0xAA, sizeof(f));
  CHECK(!parse("psk=12345678", &f));           // no ssid
  CHECK(!parse("ssid=&psk=12345678", &f));     // empty ssid
  CHECK(!parse("ssid=x&psk=1234567", &f));     // 7-char psk
  CHECK(!parse("ssid=x&psk=%2", &f));          // truncated escape
  CHECK(!parse("ssid=x&psk=%zz345678", &f));   // bad hex
  CHECK(!parse("ssid=x%", &f));                // '%' at end
  CHECK(!parse("ssid=x&psk=%00abcdefg", &f));  // a NUL would truncate
  CHECK(!parse("ssid=%00x", &f));
  // Over-long: 33-byte SSID, 64-char psk.
  char body[200];
  snprintf(body, sizeof(body), "ssid=%s", "123456789012345678901234567890123");
  CHECK(!parse(body, &f));
  snprintf(body, sizeof(body), "ssid=x&psk=%s",
           "1234567890123456789012345678901234567890123456789012345678901234");
  CHECK(!parse(body, &f));
  // Refusal left the output untouched.
  CHECK_EQ_INT((unsigned char)f.ssid[0], 0xAA);
  // Positive controls at the limits: 32-byte SSID, 8 and 63-char psk.
  snprintf(body, sizeof(body), "ssid=%s&psk=%s",
           "12345678901234567890123456789012",
           "123456789012345678901234567890123456789012345678901234567890123");
  CHECK(parse(body, &f));
  CHECK_EQ_INT(strlen(f.ssid), 32);
  CHECK_EQ_INT(strlen(f.psk), 63);
}

static uint32_t s_seq;
static uint32_t fake_rng(void) {
  return s_seq++;
}
static uint32_t big_rng(void) {
  return 0xFFFFFFFFu;
}

static void psk_is_eight_hex_chars(void) {
  char psk[PROVISION_PSK_LEN + 1];
  s_seq = 0;
  provision_psk_hex(fake_rng, psk);
  CHECK_EQ_STR(psk, "01234567");
  s_seq = 8;
  // Uppercase: the 3x5 panel font folds case, so what is shown must be what
  // was generated, and passphrases are case-sensitive.
  provision_psk_hex(fake_rng, psk);
  CHECK_EQ_STR(psk, "89ABCDEF");
  provision_psk_hex(big_rng, psk);
  CHECK_EQ_STR(psk, "FFFFFFFF");  // 0xFFFFFFFF % 16, every nibble in range
  CHECK_EQ_INT(strlen(psk), 8);
}

static void ap_ssid_comes_from_the_mac_tail(void) {
  char ssid[PROVISION_AP_SSID_LEN + 1];
  const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0x1A, 0x2B};
  provision_ap_ssid(mac, ssid);
  CHECK_EQ_STR(ssid, "airkvm-1A2B");
  CHECK_EQ_INT(strlen(ssid), PROVISION_AP_SSID_LEN);
}

int main(void) {
  RUN(parses_a_normal_form);
  RUN(refuses_what_a_network_cannot_use);
  RUN(psk_is_eight_hex_chars);
  RUN(ap_ssid_comes_from_the_mac_tail);
  return test_summary("provision");
}
