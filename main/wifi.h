// WiFi station bring-up, and the NVS initialisation it depends on.
//
// Split out of net.c so that the console (HTTP, WebSocket, mDNS) and the
// network it rides on are separate concerns. Fleet mode will keep this file
// verbatim and replace the console entirely (docs/design/fleet.md), which is
// only possible if station bring-up does not live inside the HTTP module.
#pragma once

#include "esp_err.h"

// Initialises NVS, brings up WiFi in station mode from Kconfig credentials, and
// returns as soon as the association is *started* — not once it succeeds.
// Association continues in the background because USB HID must never wait on a
// network that may never come up (docs/repo-map.md, boot flow).
//
// Fails fast when no SSID is configured rather than defaulting to anything
// (AGENTS.md §8, configuration).
esp_err_t wifi_start(void);
