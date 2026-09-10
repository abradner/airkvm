// Building the Location value for the HTTP->HTTPS redirect.
//
// Pure and free of esp_ headers, following keymap.c and protocol.c, because
// the interesting part of this is string handling on ATTACKER-CONTROLLED INPUT
// and that deserves to be tested rather than reviewed. The `Host` header is
// whatever the client sent; it reaches a response header, and a response
// header is exactly where a CRLF gets you a forged reply.
//
// What this is NOT worried about: someone setting `Host: evil.example` and
// being redirected there. They would only be redirecting themselves — an open
// redirect is a phishing tool because it borrows a *trusted* domain's
// reputation, and a LAN dongle with an operator-run CA has none to borrow.
//
// Note the allowlist does NOT prevent that: `evil.example` is entirely legal
// characters and produces `Location: https://evil.example/`. The behaviour is
// accepted on the argument above, not blocked — an earlier version of this
// comment claimed the charset closed it, which was simply false. The charset
// exists for header injection, which is a different risk and is closed.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Longest Host we will echo. A hostname is 253 bytes at most and this device's
// is ~16; the cap exists so a hostile client cannot make us assemble a header
// larger than the buffer that holds it.
#define REDIRECT_MAX_HOST 96

// Is this byte legal in a Host header value?
//
// Deliberately a strict allowlist rather than a CR/LF denylist. A denylist
// invites the next reader to wonder about NUL, about the 0x80-0xFF range that
// some parsers fold, and about whatever the next header-splitting trick turns
// out to be; an allowlist answers all of those at once. Brackets and colon are
// in because an IPv6 literal Host is `[::1]:443` and dropping those would break
// the one case that has no alternative spelling.
static inline bool redirect_host_char_ok(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':' ||
         c == '[' || c == ']';
}

// Writes "https://<host><uri>" into `out`, dropping any port from `host`.
//
// Returns false — and writes nothing usable — if the host is empty, too long,
// contains a byte outside the allowlist, or the result would not fit. Every
// one of those is a case where the honest answer is to not redirect at all:
// caller should serve an error rather than guess, because a redirect built
// from input we could not validate is worse than no redirect.
//
// The port is stripped because we are moving the client to 443, and echoing
// ":80" into an https:// URL would send it back to a port that does not serve
// TLS. An IPv6 literal keeps its brackets and loses only a port outside them.
static inline bool redirect_location(const char *host, const char *uri,
                                     char *out, size_t out_len) {
  static const char kScheme[] = "https://";
  if (host == NULL || uri == NULL || out == NULL || out_len == 0) {
    return false;
  }

  // Validate the ENTIRE value first, including whatever follows the port
  // colon. Stopping at the colon would accept `example.com:bad suffix` and
  // silently drop the tail — the tail never reaches Location, so nothing is
  // forged, but "we refuse anything we do not recognise" then would not be
  // true, and a fail-closed validator that quietly tolerates garbage is a
  // validator nobody can reason about.
  size_t total = 0;
  for (; host[total] != '\0'; total++) {
    if (total >= REDIRECT_MAX_HOST) {
      return false;
    }
    if (!redirect_host_char_ok(host[total])) {
      return false;
    }
  }

  // Now split off any ":port", and ENFORCE the grammar rather than tolerating
  // it. An earlier version only looked for the delimiter, which accepted
  // `example.com:bad` (silently dropping a suffix it had no opinion about) and
  // `[fe80::1]junk` — the latter emitting `https://[fe80::1]junk/`, an
  // authority no client can parse. Both were built entirely from allowlisted
  // characters, so the charset check could never have caught them: a
  // fail-closed validator has to know the shape, not just the alphabet.
  const bool bracketed = host[0] == '[';
  size_t i = 0;
  if (bracketed) {
    // IPv6 literal: everything up to the one closing bracket.
    for (i = 1; i < total && host[i] != ']'; i++) {
      if (host[i] == '[') {
        return false;  // nested bracket
      }
    }
    if (i >= total) {
      return false;  // never closed
    }
    i++;  // take the ']' with it
  } else {
    for (; i < total && host[i] != ':'; i++) {
      // A bracket outside a bracketed host is malformed wherever it appears.
      if (host[i] == '[' || host[i] == ']') {
        return false;
      }
    }
  }
  const size_t host_len = i;
  // "[]" is a closed bracket around nothing.
  if (host_len == 0 || (bracketed && host_len <= 2)) {
    return false;
  }

  // Whatever follows the host must be nothing at all, or a colon and at least
  // one digit and nothing but digits.
  if (i < total) {
    if (host[i] != ':') {
      return false;
    }
    if (++i >= total) {
      return false;  // "host:" with no port
    }
    for (; i < total; i++) {
      if (host[i] < '0' || host[i] > '9') {
        return false;
      }
    }
  }

  // The request target is generated by the server's own parser, not copied
  // from the wire verbatim, but it costs one loop to be sure it cannot carry
  // a newline into the header either.
  size_t uri_len = 0;
  for (size_t i = 0; uri[i] != '\0'; i++) {
    if (uri[i] == '\r' || uri[i] == '\n') {
      return false;
    }
    uri_len = i + 1;
  }

  // A request target is always absolute, but a client that sent something else
  // must not produce "https://hostapp.js" — so a leading slash may have to be
  // added, and it has to be counted here or the bounds check is one byte short
  // in exactly the case that adds a byte.
  const bool needs_slash = uri_len == 0 || uri[0] != '/';
  const size_t scheme_len = sizeof(kScheme) - 1;
  if (scheme_len + host_len + (needs_slash ? 1u : 0u) + uri_len + 1 > out_len) {
    return false;
  }
  size_t n = 0;
  for (size_t i = 0; i < scheme_len; i++) {
    out[n++] = kScheme[i];
  }
  for (size_t i = 0; i < host_len; i++) {
    out[n++] = host[i];
  }
  if (needs_slash) {
    out[n++] = '/';
  }
  for (size_t i = 0; i < uri_len; i++) {
    out[n++] = uri[i];
  }
  out[n] = '\0';
  return true;
}
