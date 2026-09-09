#include "provision.h"

#include <string.h>

#include "form.h"

bool provision_parse_form(const char *body, size_t len,
                          struct provision_form *out) {
  if (body == NULL || out == NULL) {
    return false;
  }
  struct provision_form scratch;
  memset(&scratch, 0, sizeof(scratch));
  bool have_ssid = false;
  bool have_psk = false;

  size_t pos = 0;
  struct form_pair p;
  while (form_next(body, len, &pos, &p)) {
    if (form_key_is(&p, "ssid")) {
      const int n =
          form_decode(p.value, p.value_len, scratch.ssid, sizeof(scratch.ssid));
      if (n < 1) {
        return false;  // empty, over-long, or bad encoding
      }
      have_ssid = true;
    } else if (form_key_is(&p, "psk")) {
      const int n =
          form_decode(p.value, p.value_len, scratch.psk, sizeof(scratch.psk));
      if (n < 0 || (n > 0 && n < PROVISION_PSK_MIN)) {
        return false;
      }
      have_psk = true;
    }
    // Unknown keys are ignored: a browser may add its own.
  }
  if (!have_ssid) {
    return false;
  }
  (void)have_psk;  // absent psk means open network, same as empty
  *out = scratch;
  return true;
}

void provision_psk_hex(uint32_t (*rng)(void), char out[PROVISION_PSK_LEN + 1]) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 0; i < PROVISION_PSK_LEN; i++) {
    out[i] = hex[rng() % 16u];
  }
  out[PROVISION_PSK_LEN] = '\0';
}

void provision_ap_ssid(const uint8_t mac[6],
                       char out[PROVISION_AP_SSID_LEN + 1]) {
  static const char hex[] = "0123456789ABCDEF";
  memcpy(out, "airkvm-", 7);
  out[7] = hex[mac[4] >> 4];
  out[8] = hex[mac[4] & 0xF];
  out[9] = hex[mac[5] >> 4];
  out[10] = hex[mac[5] & 0xF];
  out[11] = '\0';
}
