// The decisions behind USB remote wakeup, kept pure so they are host-tested.
//
// No I/O, no ESP-IDF headers (AGENTS.md §4). hid_out_usb.c feeds this the
// bus facts TinyUSB reports and acts on what comes back; nothing here touches
// the transport. Time is int64 microseconds from whatever clock the caller
// has (esp_timer_get_time() in firmware, a counter in tests).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

// Why a wake request can or cannot proceed, in the order the console should
// hear about it: no host at all beats "not suspended", which beats "the host
// never armed wakeup". These are the console's reasons only — TinyUSB refuses
// a pulse on its own when its preconditions fail, so deleting this function
// would change the message, never what is possible (AGENTS.md §8, layered
// checks).
enum airkvm_wake_result wake_policy_decide(bool mounted, bool suspended,
                                           bool armed);
