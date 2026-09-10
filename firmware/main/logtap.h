// Diagnostic only — serves the firmware's own log over plain HTTP.
//
// Exists because this board can reach a state with no readable log at all: a
// USB HID build has TinyUSB on the only port, and after a soft reboot the USB
// PHY does not return to USB-Serial/JTAG without a physical power cycle. When
// the thing being debugged is *why the console will not start* (issue #24),
// the network is the only channel left that demonstrably works.
//
// NEVER enable in anything resembling a real build. It serves unfiltered
// firmware logs to any unauthenticated LAN client — strictly worse than R-001,
// which merely lets someone type at the target. The keystroke debug row
// (R-006) is gated on physical presence for exactly this reason. Gated on
// AIRKVM_LOG_TAP, default off, set only in a local uncommitted sdkconfig.
#pragma once

#include "esp_err.h"

#define TAG_LOGTAP "logtap"

// Split in two, and the split is load-bearing. Capture must start as early as
// possible to catch the very failure being investigated, but the HTTP server
// cannot start until there is a network stack to bind to — esp_netif_init()
// happens inside net_start(). Starting the server first put the board into a
// reboot loop (2026-09-05), recovered by OTA rollback.
//
// The serving half no longer starts a server: port 80 has more than one
// occupant now and http80.c owns it, registering this handler alongside the
// HTTPS redirect in a fixed order. What is left here is the ring buffer and
// the handler that renders it.
#if CONFIG_AIRKVM_LOG_TAP

#include "esp_http_server.h"

// Installs the log hook. Safe to call before the network exists; call first.
esp_err_t logtap_capture(void);

// Renders the captured buffer as text/plain. Registered at /log by http80.c —
// this module deliberately does not own a server, so there is exactly one
// answer to "what is listening on port 80".
esp_err_t logtap_http_get(httpd_req_t *req);

#else
static inline esp_err_t logtap_capture(void) {
  return ESP_OK;
}
#endif
