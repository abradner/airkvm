#include "provision.h"

#include <string.h>

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes one form value in place into dst (cap bytes incl. NUL). Returns the
// decoded length, or -1 on bad encoding or overflow.
static int decode_value(const char *src, size_t len, char *dst, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    char c = src[i];
    if (c == '+') {
      c = ' ';
    } else if (c == '%') {
      if (i + 2 >= len) {
        return -1;  // "%x" or "%" at the end: not an encoding
      }
      const int hi = hexval(src[i + 1]);
      const int lo = hexval(src[i + 2]);
      if (hi < 0 || lo < 0) {
        return -1;
      }
      c = (char)((hi << 4) | lo);
      i += 2;
    }
    if (c == '\0' || n + 1 >= cap) {
      return -1;  // a NUL would silently truncate the value downstream
    }
    dst[n++] = c;
  }
  dst[n] = '\0';
  return (int)n;
}

bool provision_parse_form(const char *body, size_t len,
                          struct provision_form *out) {
  if (body == NULL || out == NULL) {
    return false;
  }
  struct provision_form scratch;
  memset(&scratch, 0, sizeof(scratch));
  bool have_ssid = false;
  bool have_psk = false;

  size_t i = 0;
  while (i < len) {
    // key=value&key=value ...
    size_t key_start = i;
    while (i < len && body[i] != '=' && body[i] != '&') {
      i++;
    }
    const size_t key_len = i - key_start;
    size_t val_start = i;
    size_t val_len = 0;
    if (i < len && body[i] == '=') {
      val_start = ++i;
      while (i < len && body[i] != '&') {
        i++;
      }
      val_len = i - val_start;
    }
    if (i < len && body[i] == '&') {
      i++;
    }
    if (key_len == 4 && memcmp(body + key_start, "ssid", 4) == 0) {
      const int n = decode_value(body + val_start, val_len, scratch.ssid,
                                 sizeof(scratch.ssid));
      if (n < 1) {
        return false;  // empty, over-long, or bad encoding
      }
      have_ssid = true;
    } else if (key_len == 3 && memcmp(body + key_start, "psk", 3) == 0) {
      const int n = decode_value(body + val_start, val_len, scratch.psk,
                                 sizeof(scratch.psk));
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
