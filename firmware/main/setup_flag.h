// The "reboot into setup mode" request, carried across a software reset.
//
// Setup mode needs a different USB identity from normal operation, and USB
// descriptors are fixed at driver install, so the only clean way from one to
// the other is a reboot. The request is a magic word in noinit RAM, honoured
// only when the reset reason says software reset: uninitialised RAM after a
// power-on or a brown-out holds anything, and "anything" must never read as
// a request to open a provisioning AP. (The pattern IDF's own sta2eth example
// uses; RTC_DATA_ATTR would be wrong — it is reloaded from the image on boot.)
#pragma once

#include <stdbool.h>

// Records the request and restarts. Does not return.
void setup_flag_request_and_restart(void);

// True exactly once after a restart that setup_flag_request_and_restart()
// caused; the flag is consumed, so a further reset from inside setup mode
// lands in normal mode.
bool setup_flag_take(void);
