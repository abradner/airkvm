// WiFi station, mDNS, and the HTTP/WebSocket server that carries the console.
#pragma once

#include "esp_err.h"

// Brings up WiFi in station mode using credentials from Kconfig/NVS, advertises
// over mDNS, and starts the HTTP server. Returns once the server is listening;
// WiFi association continues in the background, because USB HID must not wait
// on a network that may never come up (docs/repo-map.md, boot flow).
esp_err_t net_start(void);
