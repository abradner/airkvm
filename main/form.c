#include "form.h"

#include <string.h>

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int form_decode(const char *src, size_t len, char *dst, size_t cap) {
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

bool form_next(const char *body, size_t len, size_t *pos,
               struct form_pair *out) {
  size_t i = *pos;
  while (i < len) {
    // key=value&key=value ...
    const size_t key_start = i;
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
    if (key_len == 0) {
      continue;  // "&&", or a leading '&': nothing to yield
    }
    out->key = body + key_start;
    out->key_len = key_len;
    out->value = body + val_start;
    out->value_len = val_len;
    *pos = i;
    return true;
  }
  *pos = i;
  return false;
}

bool form_key_is(const struct form_pair *p, const char *name) {
  const size_t n = strlen(name);
  return p->key_len == n && memcmp(p->key, name, n) == 0;
}
