// A block source whose bytes live in the operator's browser.
//
// The browser holds the disk image (File API); the dongle asks for chunks
// over /ws-media as the target reads them, and keeps a small cache so
// sequential reads do not pay a WiFi round trip per USB transfer. This module
// is the source half: it implements msc_source for msc_out.c, owns the cache,
// and blocks the msc_io worker until a chunk arrives, times out, or the medium
// goes away. The socket half (media_ws.c) feeds it and never touches the LUN
// or TinyUSB (AGENTS.md §4, "One LUN owner").
//
// No path here writes: the image is read-only by construction (a browser
// cannot hand out a writable file). msc_lun consults is_present/is_writable
// and then refuses WRITE with DATA PROTECT; read and write are never asked.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "msc_source.h"

// How the source asks the transport for a chunk. Returns false if it could
// not even be sent (no client); the read then fails without waiting.
typedef bool (*browser_request_fn)(uint32_t req_id, uint32_t chunk);

// How the source tells the transport the client has gone silent: several
// requests in a row went unanswered. The transport treats it as a terminal
// event and drops the medium — the storage twin of the 3 s keepalive on /ws.
// Without it a laptop that sleeps while attached holds the medium until TCP
// gives up, and the woken operator is told their own zombie is "busy".
typedef void (*browser_stalled_fn)(void);

// Sets up the synchronisation. Call once, before any attach. The cache itself
// is allocated per attach (see browser_source_attach) so an idle dongle pays
// nothing for a feature nobody is using.
esp_err_t browser_source_init(browser_request_fn request,
                              browser_stalled_fn stalled);

const struct msc_source *browser_source(void);

// --- driven by media_ws.c, the transport ---------------------------------

// A client presented an image. block_size is a multiple of 512 and
// block_count fits uint32 (media_proto.c enforces both). `name` is copied.
// Allocates the chunk cache — PSRAM if present, internal RAM otherwise — and
// refuses (false) if that would leave internal heap below the floor the
// console needs for its next TLS handshake. Losing media is recoverable;
// losing /ws is not, and heap exhaustion presents as "the console does not
// load", three layers away from the cause (issue #24's shape).
bool browser_source_attach(uint32_t block_count, uint16_t block_size,
                           const char *name);

// The image is gone: socket closed, DETACH, WiFi lost. Total: cache dropped,
// generation bumped, any blocked read woken to fail. Safe to call when
// nothing is attached.
void browser_source_detach(void);

// A chunk arrived. Refused (false) if it was not asked for, belongs to a
// previous image, or does not fit — the transport logs and moves on.
bool browser_source_deliver(uint32_t req_id, uint32_t chunk, uint32_t offset,
                            const uint8_t *data, uint32_t len);

// The client could not serve a request. The waiting read fails now rather
// than at its timeout.
void browser_source_fail(uint32_t req_id);

// For MEDIA_STATUS.
uint32_t browser_source_bytes_served(void);
uint32_t browser_source_pending(void);
