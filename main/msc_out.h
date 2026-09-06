// The mass-storage function: TinyUSB's MSC callbacks, the one LUN, and the
// worker that does blocking block I/O off the USB task.
//
// Mirrors the HID rule in AGENTS.md §4: exactly one owner of LUN state
// (msc_lun.c, driven only from here), and the block sources and the sockets
// that feed them never link a tud_* symbol. Compiled only when
// CONFIG_TINYUSB_MSC_ENABLED is set.
#pragma once

#include "esp_err.h"
#include "msc_source.h"

// Creates the LUN (no medium), the I/O worker and its mailbox. Must run
// before tinyusb_driver_install(): the host's first INQUIRY / TEST UNIT READY
// can arrive as soon as enumeration completes, and the LUN has to answer.
// Never blocks on anything a source might do.
esp_err_t msc_out_start(void);

// Attach or detach a medium. Either may be called from any task. Detach is
// the storage analogue of release-all: total, and correct on every terminal
// event (eject, socket close, WiFi loss) — a transfer in flight against the
// old medium fails with NOT READY rather than delivering stale bytes.
void msc_out_bind(const struct msc_source *src);
void msc_out_unbind(void);
