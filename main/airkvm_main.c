// Boot order: network before USB HID. The order is load-bearing — see the
// comment at net_start() below, and AGENTS.md §9 for the incident. The C3
// bench board verified the missing-credentials path (2026-08-10): net_start()
// fails cleanly and HID still comes up, which the ordering below preserves
// explicitly rather than by accident.

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

  // Self-contained: waits for WiFi on its own rather than depending on
  // net_start() below. A no-op in every real build (dev-only OTA test hook,
  // gated on AIRKVM_OTA_TEST_URL — see main/ota.c).
  ota_start();

  // Network first, then USB HID — and the order is load-bearing. On the
  // ESP32-S3 the RF PHY and the USB PHY share the BBPLL (soc_caps.h:
  // SOC_WIFI_PHY_NEEDS_USB_WORKAROUND; esp_phy's init writes the PLL's USB
  // enable via phy_bbpll_en_usb() before handing the PLL to the closed
  // calibration blob). With TinyUSB already clocked off that PLL, WiFi's
  // init returns ESP_OK but never posts WIFI_EVENT_STA_START, so the radio
  // never engages: "NO WIFI" forever, at idle current, with nothing logged.
  // Verified on hardware (2026-09-04): HID-first never associates; this order
  // — WiFi first, HID immediately after — associates and both run side by
  // side; an already-established connection also survives HID starting 20s
  // later. What the blob does when it finds the PLL already owned is not
  // observable; the fix is empirical.
  //
  // This does not make HID wait on the network. net_start() returns as soon
  // as association is *started* (wifi.h), so HID is delayed by WiFi's
  // synchronous init only — and it starts regardless of whether that init
  // succeeded, because a dongle with no access point must still be a
  // keyboard (docs/repo-map.md, boot flow).
  const esp_err_t err = net_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "network failed to start (%s) — starting USB HID anyway",
             esp_err_to_name(err));
  }
  ESP_ERROR_CHECK(hid_out_start());

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "airkvm ready");
  }
}
