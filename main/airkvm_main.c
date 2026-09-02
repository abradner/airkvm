// Boot order verified on the ESP32-C3 bench board (2026-08-10), including the
// missing-credentials path: net_start() fails cleanly and the HID backend
// stays up rather than the device reboot-looping.

#include "esp_log.h"
#include "hid_out.h"
#include "net.h"
#include "ota.h"
#include "status.h"
#include "ui.h"

static const char *TAG = "airkvm";

void app_main(void) {
  // Status and front panel first, so the screen can report progress (and
  // failure) for everything that follows rather than staying dark until the
  // network happens to come up.
  status_init();
  ui_start();

  // USB first, and independent of the network. A dongle plugged into a target
  // must enumerate as a keyboard whether or not WiFi ever associates; the
  // reverse order would make a bad network look like broken hardware
  // (docs/repo-map.md, boot flow).
  ESP_ERROR_CHECK(hid_out_start());

  // Self-contained: waits for WiFi on its own rather than depending on
  // net_start() below. A no-op in every real build (dev-only OTA test hook,
  // gated on AIRKVM_OTA_TEST_URL — see main/ota.c); started unconditionally
  // here regardless, since when it IS configured, confirming this boot
  // healthy should not depend on the console also starting successfully.
  ota_start();

  const esp_err_t err = net_start();
  if (err != ESP_OK) {
    // Deliberately not fatal: HID is up, and a dongle that reboot-loops on a
    // missing access point is harder to diagnose than one that sits there
    // enumerated with no console.
    ESP_LOGE(TAG, "network failed to start (%s) — USB HID still active",
             esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "airkvm ready");
}
