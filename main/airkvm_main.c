// Boot order: network before USB HID. The order is load-bearing — see the
// comment at net_start() below, and AGENTS.md §9 for the incident. The C3
// bench board verified the missing-credentials path (2026-08-10): net_start()
// fails cleanly and HID still comes up, which the ordering below preserves
// explicitly rather than by accident.

#include "esp_log.h"
#include "hid_out.h"
#include "http80.h"
#include "logtap.h"
#include "settings.h"
#if CONFIG_AIRKVM_MSC_SD
#include "msc_src_sd.h"
#endif
#include "net.h"
#include "ota.h"
#include "setup_flag.h"
#include "setup_mode.h"
#include "status.h"
#include "ui.h"

static const char *TAG = "airkvm";

void app_main(void) {
  // Status and front panel first, so the screen can report progress (and
  // failure) for everything that follows rather than staying dark until the
  // network happens to come up.
  status_init();
  // Stored preferences, before anything that could serve or paint them. The
  // theme it applies is parked until the display's first flush, so this need
  // not wait for the panel. Not fatal: a dongle with defaults is a dongle.
  if (settings_init() != ESP_OK) {
    ESP_LOGE(TAG, "settings unavailable — running on defaults");
  }
  ui_start();

  // A 5 s BOOT hold in the previous run asked for setup mode (setup_flag.h).
  // Everything below this line is the normal dongle; setup mode is a
  // different device (an access point serving one page) and never returns.
  // It brings up WiFi itself and starts no USB at all, so gotcha 10's
  // ordering does not arise there.
  if (setup_flag_take()) {
    setup_mode_run();
  }
  // The missing-credentials path was verified on the C3 bench on 2026-08-10
  // against Kconfig only; it now consults the radio's NVS store first
  // (wifi.c), which that run predates.

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
  // inside net_start). This is the plain-HTTP server on port 80: the redirect
  // to HTTPS, and the log tap's route if that build enabled it. Compiled out
  // entirely on a non-TLS build, where the console itself is on port 80 and
  // there is nothing to redirect to.
  //
  // Never fatal. Port 80 carries a convenience and a diagnostic; the console
  // is on 443 and has already started.
  //
  // Gated on the TCP/IP STACK being up, not on net_start() having succeeded,
  // and the difference is the whole point. Two distinct failures hide behind
  // one error code:
  //
  //   no netif — httpd_start would bind with no stack at all, which logtap.h
  //     records as a reboot loop. Nothing can run here.
  //   netif up, console down — TLS refused to start (fatal to the console by
  //     design, R-007; a cert that will not parse has done this before,
  //     gotcha 6). The redirect must NOT be registered, since it would point
  //     at a port with nothing behind it. But this is exactly the state the
  //     log tap exists to let you read about, so the server still starts to
  //     carry /log.
  //
  // An earlier version gated both on `err == ESP_OK`, and so deleted the
  // network diagnostic on precisely the failure it was built for.
#if CONFIG_AIRKVM_TLS
  if (net_netif_ready() && http80_start(net_console_up()) != ESP_OK) {
    ESP_LOGW(TAG, "port 80 not served");
  }
#endif

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "airkvm ready");
  }
}
