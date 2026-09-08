// The captive-portal DNS answer: every A query gets our address.
//
// Pure packet arithmetic, host-tested (test/host/test_captive_dns.c); the
// socket lives in setup_mode.c. Only ever bound to the softAP link — never
// the USB link, where a catch-all resolver would hijack the laptop's name
// resolution (Windows queries all adapters in parallel). AGENTS.md §4.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Builds the response to one DNS request. Every A/IN question is answered
// with `ip_be` (network byte order); other question types get the QR flag
// and no answer, so the client moves on instead of retrying. Returns the
// response length, or 0 if the request is not a standard query, is
// malformed, or the response would not fit `cap`.
size_t captive_dns_answer(const uint8_t *req, size_t len, uint8_t *resp,
                          size_t cap, uint32_t ip_be);
