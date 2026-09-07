// Setup mode: a softAP captive portal that collects WiFi credentials.
//
// Entered only by a reboot that the 5 s BOOT hold requested (setup_flag.h).
// While in it the dongle is a WiFi access point named airkvm-XXXX with an
// 8-digit passphrase shown on the panel, serving one plain-HTTP page that
// writes the credentials through esp_wifi_set_config() — the same NVS store
// wifi.c reads first on a normal boot — and restarts. Nothing else runs: no
// HID, no mass storage, no web console, no mDNS, no TinyUSB — which means the
// USB-Serial/JTAG log console IS present on whatever the dongle is plugged
// into, so the log must never carry the passphrase. It ends on save, on a
// second 5 s hold, or after five minutes.
//
// Boundary (docs/repo-map.md, R-009): anyone within WiFi range who can read
// the panel can re-point the dongle at a network of their choosing, for five
// minutes after someone with their hand on the button asked for it. That is
// the physical-presence model again, and the on-screen passphrase is the
// hurdle that makes "in range" insufficient on its own. The captive DNS runs
// on the AP link only (captive_dns.h says why).
#pragma once

// Runs setup mode on the calling task and never returns: every exit is a
// restart into normal mode.
void setup_mode_run(void) __attribute__((noreturn));
