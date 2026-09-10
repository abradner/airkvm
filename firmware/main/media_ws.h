// The media socket: /ws-media, on its own HTTPS instance.
//
// Its own instance, not a second URI on the console server, because one
// esp_http_server instance is one task: a 16 KB image piece sitting in recv()
// on the console's task would hold every keystroke behind it and eat into the
// 3 s keepalive (AGENTS.md §9 entry 9). Principle 6 wants input latency
// unaffected by design, so the media path gets a task of its own.
//
// One client at a time, enforced at the socket: the instance has a single
// client slot, so a second media client cannot connect while one is
// connected. (MEDIA_STATE_BUSY exists in the protocol for a build that can
// afford a spectator slot; today it is unreachable.)
//
// Trust: the same as /ws — none (R-001). A LAN peer that can reach this port
// can present arbitrary boot media to the target. That is R-008 in
// docs/repo-map.md, a real widening within the physical-presence model, and
// it is accepted with the same argument as R-001: plugging the dongle in is
// the act of trust, and unplugging revokes it completely.
#pragma once

#include "esp_err.h"

// Starts the media server. Non-fatal to the console if it fails: the caller
// logs and carries on without mass-storage media.
esp_err_t media_ws_start(void);
