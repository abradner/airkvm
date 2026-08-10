// Boot order verified on the ESP32-C3 bench board (2026-08-10), including the
// missing-credentials path: net_start() fails cleanly and the HID backend
// stays up rather than the device reboot-looping.

#include "esp_log.h"
#include "hid_out.h"
#include "net.h"

static const char *TAG = "airhid";

void app_main(void) {
  // USB first, and independent of the network. A dongle plugged into a target
  // must enumerate as a keyboard whether or not WiFi ever associates; the
  // reverse order would make a bad network look like broken hardware
  // (docs/repo-map.md, boot flow).
  ESP_ERROR_CHECK(hid_out_start());

  const esp_err_t err = net_start();
  if (err != ESP_OK) {
    // Deliberately not fatal: HID is up, and a dongle that reboot-loops on a
    // missing access point is harder to diagnose than one that sits there
    // enumerated with no console.
    ESP_LOGE(TAG, "network failed to start (%s) — USB HID still active",
             esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "airhid ready");
}
