// Setup mode: a softAP captive portal that collects WiFi credentials.
//
// Entered only by a reboot that the 5 s BOOT hold requested (setup_flag.h).
// While in it the dongle is a WiFi access point named airkvm-XXXX, WPA3-SAE
// only, with an 8-character hex passphrase shown on the panel, serving one
// plain-HTTP page that
// writes the credentials through esp_wifi_set_config() — the same NVS store
// wifi.c reads first on a normal boot — and restarts. On the S3 the same
// page is also reachable over USB: the dongle enumerates as a USB Ethernet
// adapter on a private link (usb_net.h), so the machine it is plugged into
// can browse to it without joining the AP. Nothing else runs: no HID, no
// mass storage, no web console, no mDNS. On a board with no USB device
// controller there is no TinyUSB either, and the USB-Serial/JTAG log console
// IS present on whatever the dongle is plugged into — so the log must never
// carry the passphrase, on any board. It ends on save, on a second 5 s hold,
// or after five minutes.
//
// Boundary (docs/repo-map.md, R-009): anyone within WiFi range who can read
// the panel can re-point the dongle at a network of their choosing, for five
// minutes after someone with their hand on the button asked for it. That is
// the physical-presence model again, and the on-screen passphrase is what
// makes "in range" insufficient on its own — a barrier, since SAE removes
// the offline attack that made an 8-character key a mere hurdle under WPA2. The captive DNS runs
// on the AP link only (captive_dns.h says why).
#pragma once

// Runs setup mode on the calling task and never returns: every exit is a
// restart into normal mode.
void setup_mode_run(void) __attribute__((noreturn));
