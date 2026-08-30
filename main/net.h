// The console: mDNS, and the HTTP/WebSocket server that serves and drives it.
#pragma once

#include "esp_err.h"

// Brings up the network (wifi.c), advertises over mDNS, and starts the HTTP
// server. Returns once the server is listening; WiFi association continues in
// the background, because USB HID must not wait on a network that may never
// come up (docs/repo-map.md, boot flow).
esp_err_t net_start(void);
