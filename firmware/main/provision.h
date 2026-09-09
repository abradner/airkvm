// The pure parts of setup mode: what the portal form means, and how the AP
// names itself. No I/O, no ESP-IDF headers, host-tested
// (test/host/test_provision.c). AGENTS.md §4.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// WiFi limits: an SSID is up to 32 bytes; a WPA2/WPA3 passphrase is 8 to 63
// characters, or empty for an open network.
#define PROVISION_SSID_MAX 32
#define PROVISION_PSK_MIN 8
#define PROVISION_PSK_MAX 63

struct provision_form {
  char ssid[PROVISION_SSID_MAX + 1];
  char psk[PROVISION_PSK_MAX + 1];
};

// Parses an application/x-www-form-urlencoded body with `ssid` and `psk`
// fields (percent-decoding, '+' as space). Refuses anything a network cannot
// use: empty or over-long SSID, a passphrase of 1–7 characters, a byte that
// is not valid percent-encoding, unknown fields are ignored. `out` is
// untouched on refusal, the airkvm_decode contract.
bool provision_parse_form(const char *body, size_t len,
                          struct provision_form *out);

// Eight uppercase hex characters from the given RNG, NUL-terminated into
// out[9]: 32 bits, still eight characters to read off a panel. Uppercase
// because the 3x5 font folds case and passphrases do not. With the AP on
// WPA3-SAE (setup_mode.c) a captured handshake cannot be attacked offline,
// so the size only has to beat online guessing at the AP's own pace —
// which decimal digits did not, and were never meant to: they were a
// hurdle behind WPA2, and this is the barrier the map (R-009) now claims.
#define PROVISION_PSK_LEN 8
void provision_psk_hex(uint32_t (*rng)(void), char out[PROVISION_PSK_LEN + 1]);

// "airkvm-XXXX" from the last two bytes of the MAC, so two dongles in range
// of each other do not collide. out must hold PROVISION_AP_SSID_LEN + 1.
#define PROVISION_AP_SSID_LEN 11
void provision_ap_ssid(const uint8_t mac[6],
                       char out[PROVISION_AP_SSID_LEN + 1]);
