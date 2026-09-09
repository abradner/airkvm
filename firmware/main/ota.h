// Throwaway OTA test hook — proves esp_https_ota's fetch/write/switch/rollback
// primitive on real hardware. NOT the fleet delivery mechanism; see the help
// text on AIRKVM_OTA_TEST_URL in Kconfig.projbuild for why the two must not be
// conflated, and docs/design/fleet.md item 4 for what replaces this.
#pragma once

// Spawns a background task that, only if AIRKVM_OTA_TEST_URL is configured,
// waits for WiFi, confirms this boot healthy (cancelling any pending
// rollback), then polls for a different image to fetch and apply. A
// complete no-op — confirm included — when the URL is unset, which is every
// build except a deliberate local test.
void ota_start(void);
