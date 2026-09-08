// The console: mDNS, and the HTTP/WebSocket server that serves and drives it.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Brings up the network (wifi.c), advertises over mDNS, and starts the HTTP
// server. Returns once the server is listening; WiFi association continues in
// the background, because USB HID must not wait on a network that may never
// come up (docs/repo-map.md, boot flow).
esp_err_t net_start(void);

// Did the TCP/IP stack come up? True from the moment wifi_start() returns, and
// the precondition for binding any socket at all.
bool net_netif_ready(void);

// Is the console actually listening? False when TLS refused to start, which is
// fatal to the console by design (R-007) but not to the device — and is the
// exact failure the log tap exists to let you read about.
bool net_console_up(void);

// Ends the console session, if one is live, and releases every held key and
// button on the target. The release is unconditional: a spurious release-all is
// always preferable to a plausible held key (AGENTS.md §2, principle 4).
//
// This is the only place in the firmware where a session ending has effects.
// Anything that learns a session cannot continue calls this rather than
// releasing HID itself, so ownership and released state cannot drift apart.
void net_end_session(const char *why);
