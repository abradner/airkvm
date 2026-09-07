// WiFi station bring-up, and the NVS initialisation it depends on.
//
// Split out of net.c so that the console (HTTP, WebSocket, mDNS) and the
// network it rides on are separate concerns. Fleet mode will keep this file
// verbatim and replace the console entirely (docs/design/fleet.md), which is
// only possible if station bring-up does not live inside the HTTP module.
#pragma once

#include "esp_err.h"

// Initialises NVS, brings up WiFi in station mode from the stored credentials
// (NVS, written by setup mode or by a previous boot's Kconfig values) or, on a
// fresh flash, from Kconfig, and
// returns as soon as the association is *started* — not once it succeeds.
// Association continues in the background because USB HID must never wait on a
// network that may never come up (docs/repo-map.md, boot flow).
//
// Fails fast when no SSID is stored or configured rather than defaulting to anything
// (AGENTS.md §8, configuration).
esp_err_t wifi_start(void);

// How hard the radio should be working right now.
//
// Power save is not a single yes/no for this device. Principle 6 says latency
// is a feature, and it is — while someone is typing. It is not a feature while
// the dongle sits plugged into a machine nobody is driving, which is most of
// its life, and the cost of pretending otherwise is a permanently hot board.
// So the radio follows the operator instead of a constant.
enum wifi_activity {
  WIFI_ACTIVITY_ACTIVE,   // input within the last few seconds — full speed
  WIFI_ACTIVITY_WAITING,  // idle a while, but the console is still watched
  WIFI_ACTIVITY_IDLE,     // nobody is driving this; the screensaver is up
};

// Sets the radio's power-save mode to match. Cheap and idempotent — it only
// calls into esp_wifi when the tier actually changes — so callers can invoke
// it on a timer tick without tracking the previous value.
//
// Lives here rather than in the UI that computes the tier: the UI knows about
// idleness, but what to do about it is the radio's business (AGENTS.md §4,
// one owner per concern).
void wifi_set_activity(enum wifi_activity activity);
