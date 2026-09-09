// application/x-www-form-urlencoded, decoded one pair at a time. Pure: no
// I/O, no ESP-IDF headers, host-tested through the parsers built on it
// (test_provision.c, test_settings.c). AGENTS.md §4.
//
// Extracted from provision.c when a second form arrived (settings.c). Two
// copies of a percent-decoder is the deletion test failing on input handling,
// which is the one place it must not (AGENTS.md §8).
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct form_pair {
  const char *key;  // not NUL-terminated; key_len bytes
  size_t key_len;
  const char *value;  // raw, still encoded; value_len bytes (0 if no '=')
  size_t value_len;
};

// Walks `body`: each call yields the next key[=value] pair and advances *pos.
// Returns false when the body is exhausted. A bare key with no '=' yields an
// empty value; empty segments ("a=1&&b=2") are skipped.
bool form_next(const char *body, size_t len, size_t *pos,
               struct form_pair *out);

// Decodes one value ('+' as space, %XX) into dst (cap bytes including NUL).
// Returns the decoded length, or -1 on bad encoding, an embedded NUL, or
// overflow — a value that does not fit is refused, never truncated, because a
// truncated value downstream is a different value that looks valid.
int form_decode(const char *src, size_t len, char *dst, size_t cap);

// True when the pair's key is exactly `name`.
bool form_key_is(const struct form_pair *p, const char *name);
