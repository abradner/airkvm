// The plain-HTTP server on port 80, and its single owner.
//
// Port 80 has two jobs on a TLS build and neither can have its own server,
// because two servers cannot bind one port. main/Kconfig.projbuild already
// warned about the shape — the log tap "requires AIRKVM_TLS, because it serves
// on port 80 and a non-TLS console is already there... without this dependency
// the two race for the same port and which one you get is a coin toss". That
// dependency held, so no race ever shipped; the redirect is simply the second
// occupant that makes a written-down rule insufficient, so ownership moves
// here rather than being negotiated between modules.
//
// This is the same shape as hid_state.c owning HID reports and msc_lun.c
// owning the LUN: one module answers "what is on port 80", and the alternative
// is a race whose symptom is a feature that silently is not there.
//
// Route precedence is registration order in esp_http_server, and the redirect
// is a catch-all, so ordering is not a convention between callers — every
// route is registered by one function in this file, in an order you can read
// on one screen.
//
// Nothing here exists on a non-TLS build. There the console itself is on 80
// (net.c), there is nothing to redirect to, and the log tap is unavailable by
// Kconfig dependency.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_AIRKVM_TLS

// Starts the port-80 server and registers every route that belongs on it.
//
// `console_up` says whether there is anything on 443 to send people to. When
// it is false the redirect is NOT registered — pointing a browser at a port
// with nothing behind it reproduces the refused connection this feature exists
// to remove — but the server still starts if the log tap is compiled in,
// because a console that failed to start is precisely what the tap is for
// (logtap.h). With neither route to serve, nothing is started at all.
//
// A failure is logged and returned but is never fatal: port 80 carries a
// convenience and a diagnostic, and neither is worth refusing to serve the
// console over.
esp_err_t http80_start(bool console_up);

#endif  // CONFIG_AIRKVM_TLS
