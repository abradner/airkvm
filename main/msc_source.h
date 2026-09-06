// A block source: where a mass-storage LUN's bytes come from.
//
// Pure header, no ESP-IDF (AGENTS.md §4) — msc_lun.c is host-tested against a
// fake one. Two real backends are planned behind it: an image streamed from
// the operator's browser, and the microSD slot on the LCD board. The LUN and
// the SCSI glue in msc_out.c never know which.
//
// Every function takes the source's own ctx. read/write/flush block and are
// called from the msc_io worker task only, never from a TinyUSB callback: a
// blocking read inside tud_msc_read10_cb would hold up the endpoint that
// clears the keyboard's busy flag for the whole duration (principle 6).
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct msc_geometry {
  uint32_t block_count;
  uint16_t block_size;  // 512 for a disk; 2048 for the CD-ROM LUN (issue #34)
};

struct msc_source {
  const char *name;  // for logs and the console
  // Present *and* what size — one call, so a card yanked between two calls
  // cannot report yesterday's geometry as today's.
  bool (*is_present)(void *ctx, struct msc_geometry *out);
  bool (*is_writable)(void *ctx);
  // Byte-addressed by (lba, offset) so the USB buffer can be smaller than a
  // block. len is a multiple of 512 and never exceeds the USB buffer. Return
  // 0 on success, negative on a medium error.
  int (*read)(void *ctx, uint32_t lba, uint32_t offset, void *buf,
              uint32_t len);
  int (*write)(void *ctx, uint32_t lba, uint32_t offset, const void *buf,
               uint32_t len);  // NULL: read-only medium
  int (*flush)(void *ctx);     // NULL: nothing to flush
  void *ctx;
};
