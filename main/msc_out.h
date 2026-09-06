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

// Attach or detach a medium, from any task. Two media can arrive
// independently (a browser image over the socket, a card in the slot), so
// both calls arbitrate atomically: bind_if_free takes the LUN only if it is
// empty and says whether it did; unbind_if releases it only if `src` is the
// holder. A loser never touches the winner's medium under the target.
//
// Detach is the storage analogue of release-all: total, and correct on every
// terminal event (eject, socket close, WiFi loss, card pulled) — a transfer in
// flight against the old medium fails with NOT READY rather than delivering
// stale bytes.
bool msc_out_bind_if_free(const struct msc_source *src);
bool msc_out_unbind_if(const struct msc_source *src);

// The source currently bound, or NULL. Informational (a status line, a log);
// never the basis for a bind decision — that is what bind_if_free is for.
const struct msc_source *msc_out_current(void);

// Called from tud_mount_cb and tud_umount_cb: a (re)enumeration means TinyUSB
// has forgotten any transfer in flight, so a completion for it must be
// dropped rather than delivered to whatever command comes next.
void msc_out_bus_reset(void);
