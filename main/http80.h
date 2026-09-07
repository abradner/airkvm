// The plain-HTTP server on port 80, and its single owner.
//
// Port 80 has two jobs on a TLS build and neither can have its own server,
// because two servers cannot bind one port. main/Kconfig.projbuild already
// warned about this — the log tap "requires AIRKVM_TLS, because it serves on
// port 80 and a non-TLS console is already there... without this dependency
// the two race for the same port and which one you get is a coin toss". Adding
// the HTTPS redirect makes that a three-way problem, so ownership moves here
// rather than being negotiated between modules.
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

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_AIRKVM_TLS

// Starts the port-80 server and registers every route that belongs on it.
// A failure is logged and returned but is never fatal: port 80 carries a
// convenience redirect and a diagnostic, and neither is worth refusing to
// serve the console over.
esp_err_t http80_start(void);

#endif  // CONFIG_AIRKVM_TLS
