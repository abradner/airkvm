// The device's runtime settings: the one owner of what is stored in NVS, what
// is currently applied, and the /settings document the console renders from.
// Policy — what a value means, what is refused, what defaults — lives in
// settings_schema.h and is host-tested; this file is the I/O around it.
//
// First direct NVS key/value use in the firmware (esp_wifi's credential store
// was the only tenant before), so the shape is a proposal (AGENTS.md §8):
// namespace "airkvm", one key per setting, never a packed blob. One key per
// setting is what makes the safety net structural rather than careful — a key
// nothing reads is ignored because nothing reads it, and a missing key is the
// normal NOT_FOUND path rather than a parse failure inside a blob.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Reads every stored setting, resolves each against this image's catalogue
// (unknown or missing -> default, stored bytes left alone), and applies them.
// Idempotent on NVS init. Call once, after status_init() and before any task
// that could serve /settings; the display need not exist yet — a theme applied
// before display_init() is parked and painted at the first flush.
esp_err_t settings_init(void);

// The operator's panel brightness, percent. Read by ui_task every tick, which
// is also where setup mode overrides it to full (R-009): the override lives
// with the consumer so there is exactly one place that decides what the panel
// is actually driven at.
int settings_screen_brightness(void);

// Whether the USB composition IN EFFECT SINCE BOOT includes the disk
// interface. Reboot-scoped: the stored value can differ until the next boot,
// and this is the one hid_out_usb.c, the SD probe and the media socket act
// on, so the descriptor, the LUN and everything that feeds it agree.
bool settings_usb_disk_enabled(void);

// Writes the GET /settings document. Returns the length, or 0 if `cap` is too
// small for the whole thing (see settings_json in settings_schema.h).
size_t settings_document(char *buf, size_t cap);

enum settings_apply_result {
  SETTINGS_APPLIED,      // changed, applied, and stored
  SETTINGS_UNCHANGED,    // already those values: no write, no rate-limit charge
  SETTINGS_BAD_REQUEST,  // refused by the schema (400)
  SETTINGS_TOO_SOON,     // a write landed less than a second ago (429)
  SETTINGS_STORE_FAILED,  // applied to RAM, but NVS refused it (500)
  SETTINGS_RESTARTING,    // stored, and a restart is scheduled (200, then gone)
};

// Applies a POST /settings body. Runs on the HTTP task; the NVS commit is a
// flash erase on that task and is bounded by the rate limit, which is what
// stops an unauthenticated caller (R-001 applies here unchanged) from wearing
// the partition out. Every changed key in one body is one commit.
enum settings_apply_result settings_apply_form(const char *body, size_t len);
