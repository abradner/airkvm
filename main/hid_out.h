// The HID output backend interface.
//
// Everything above this line — the WebSocket handler, the protocol decoder —
// deals in decoded intents and never touches a HID transport directly
// (AGENTS.md §4). Exactly one backend is compiled in, chosen by
// CONFIG_AIRKVM_HID_BACKEND:
//
//   usb    real USB HID via TinyUSB (hid_out_usb.c). Requires an ESP32-S3/S2:
//          native USB OTG is a hardware prerequisite, not a build flag.
//   log    no HID at all (hid_out_log.c). Decoded intents go to the log, so the
//          network, console, and protocol paths can be exercised on a board that
//          physically cannot be a keyboard. A bench instrument, never a product.
//
// A CH9329 UART backend would slot in here as a third implementation, which is
// how non-OTG boards would eventually drive a real target. Not built.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "protocol.h"

// Starts the backend. Does not wait for a host to enumerate: a dongle that is
// not plugged into anything yet must still boot, serve its console, and report
// its address.
esp_err_t hid_out_start(void);

// Queues one decoded intent. Safe to call from the HTTP task; the caller never
// touches the transport itself. Returns false if the queue is full — see the
// caller in net.c for why that is not automatically a release-all.
bool hid_out_submit(const struct airkvm_intent *intent);

// Clears every held key and button. Called on every terminal session event.
// Bypasses the queue: the whole point is that it still works when the queue is
// the thing that failed.
void hid_out_release_all(void);

// True once a USB host has enumerated and configured the device — i.e. the
// target is actually listening. The log backend reports false: nothing is.
bool hid_out_is_mounted(void);
