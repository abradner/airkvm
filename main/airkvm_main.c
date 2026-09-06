// Boot order: network before USB HID. The order is load-bearing — see the
// comment at net_start() below, and AGENTS.md §9 for the incident. The C3
// bench board verified the missing-credentials path (2026-08-10): net_start()
// fails cleanly and HID still comes up, which the ordering below preserves
// explicitly rather than by accident.

#include "esp_log.h"
#include "hid_out.h"
#include "logtap.h"
#if CONFIG_AIRKVM_MSC_SD
#include "msc_src_sd.h"
#endif
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

  // Diagnostic builds only; an inline no-op stub otherwise (logtap.h). Started
  // before net_start() so a failure *inside* net_start — which is what issue
  // #24 is about — is captured rather than missed.
  if (logtap_capture() != ESP_OK) {
    // Not fatal — but say so, because a diagnostic channel that is silently
    // missing is worse than one that is obviously absent.
    ESP_LOGW(TAG, "log capture unavailable");
  }

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
  // HID does not wait on the *network* — net_start() returns as soon as
  // association is *started* (wifi.h), never blocking on an access point that
  // may never answer — and it starts even when net_start() fails, because a
  // dongle with no network must still be a keyboard (docs/repo-map.md, boot
  // flow).
  //
  // It does, however, wait on everything net_start() does synchronously, which
  // is more than WiFi init: mDNS registration, the keepalive timer, and the
  // HTTP(S) server coming up, the last of which parses an RSA certificate.
  // Only the WiFi part is required to precede HID for the PHY reason above,
  // so if that delay ever matters, the fix is to split net_start() and start
  // HID after wifi_start() rather than to revert this order. Not measured, so
  // not asserted either way — issue #26.
  const esp_err_t err = net_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "network failed to start (%s) — starting USB HID anyway",
             esp_err_to_name(err));
  }
  ESP_ERROR_CHECK(hid_out_start());
#if CONFIG_AIRKVM_MSC_SD
  // After USB, so the LUN exists to bind to; never blocks on a card (the
  // probe runs on its own task). SDMMC slot 1 is on the GPIO matrix and clocks
  // from the CPU's PLL, not the USB PHY enable gotcha 10 is about — a check,
  // not an assumption: WiFi must still associate with this in place.
  if (msc_src_sd_start() != ESP_OK) {
    ESP_LOGE(TAG, "microSD failed to start — no card support this boot");
  }
#endif

  // Only now is there a network stack to bind to (esp_netif_init happens
  // inside net_start). No-op unless AIRKVM_LOG_TAP is set.
  if (logtap_serve() != ESP_OK) {
    ESP_LOGW(TAG, "log tap failed to serve");
  }

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "airkvm ready");
  }
}
