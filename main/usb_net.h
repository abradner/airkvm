// A USB network device (CDC-NCM) for setup mode, and nothing else.
//
// In setup mode the dongle presents itself to whatever it is plugged into as
// a USB Ethernet adapter on a private link, 192.168.7.1/24, with its own DHCP
// server, so the machine the dongle is plugged into can reach the setup page
// at http://192.168.7.1/ without joining the softAP. Setup mode runs no HID
// and no mass storage, which is what makes this possible at all: HID + MSC
// already use three of the S3's four IN endpoints, and NCM needs two more.
//
// Deliberately NOT a bridge and NOT a route (docs/repo-map.md, R-009; issue
// #38 stays parked): the DHCP offer carries no router option and no DNS
// option, so the host gains an address on this link and nothing else — no
// default route through the dongle, no resolver hijack. The captive DNS
// stays on the softAP link (captive_dns.h); a USB user types the address
// that is on the panel.
//
// Only compiled with CONFIG_TINYUSB_NET_MODE_NCM, which the S3 images set
// and the C3 bench cannot (no USB OTG). Unverified on hardware: whether the
// softAP and NCM coexist on the BBPLL (AGENTS.md §9, gotcha 10) is the first
// thing the hardware test answers, and the sta2eth example's one-core
// TinyUSB pinning is the first suspect if NCM misbehaves.
#pragma once

#include "esp_err.h"

// The address the DHCP server hands out from, as text, for the panel.
#define USB_NET_IP_STR "192.168.7.1"

// Installs TinyUSB with an NCM-only descriptor and brings up the link.
// Call after esp_wifi_start(): on the S3 the RF PHY init needs the BBPLL
// before USB is clocked off it (gotcha 10). Failure leaves USB uninstalled
// and the caller carries on with the softAP alone.
esp_err_t usb_net_start(void);
