// What the device's settings MEAN: the catalogue it advertises, how a stored or
// posted value resolves against it, and the document GET /settings serves.
// Pure — no I/O, no ESP-IDF headers — so every policy here is host-tested
// (test/host/test_settings.c). settings.c is the owner that reads and writes
// flash; this file is what it consults. AGENTS.md §4.
//
// The two halves have deliberately different tolerance, and the difference is
// the safety net the operator asked for:
//
//   from flash  -> DEFAULT.  A key that is missing, or holds a value this image
//                 does not know or cannot use, resolves to the compile-time
//                 default and the stored bytes are left alone. A panel with no
//                 theme is not a failure, so this is the cosmetic exception to
//                 §8's fail-fast rule, and it is what lets an image be
//                 downgraded without leaving the device on a blank screen.
//   from the    -> REFUSE.   A posted value the catalogue cannot use is a 400,
//   network        never silently corrected: the page is politeness and the
//                 enforcement is here (setup_mode.c states the same rule).
//
// Unknown KEYS are ignored on both paths, by construction: nothing reads them.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "status.h"  // STATUS_DEBUG_TIMEOUT_MS: the debug timeout's ceiling

// Longest theme name accepted on the wire or from flash, NUL excluded. Names
// are kebab-case ASCII (test_theme.c asserts it), so this bounds a buffer, not
// a design.
#define SETTINGS_THEME_NAME_MAX 31

// --- range settings -----------------------------------------------------------
//
// Two of these bounds are security parameters, not taste, and the tests pin
// them as such:
//
//   SETTINGS_SCREEN_MIN  is the floor below which the panel cannot be sent from
//                        the network. R-009 rests on the setup passphrase being
//                        readable OFF THE PANEL — reading it is the proof of
//                        physical presence — so a caller who could blank it
//                        could blank the proof. ui.c also forces full
//                        brightness while setup mode runs, whatever is stored.
//   SETTINGS_DEBUG_TIMEOUT_MAX_S is the compile-time default itself: the
//                        network may SHORTEN R-006's exposure window and never
//                        lengthen it. Without that, a caller could pre-set the
//                        maximum and wait for someone to press the button —
//                        the press stays physical, the window it opens would
//                        not. status.h's clamp is the enforcement; this
//                        table's refusal is the politeness (§8, deletion test).
#define SETTINGS_SCREEN_MIN 10
#define SETTINGS_SCREEN_MAX 100
#define SETTINGS_SCREEN_DEFAULT 100
#define SETTINGS_LED_MIN 5
#define SETTINGS_LED_MAX 100
#define SETTINGS_LED_DEFAULT 100
#define SETTINGS_DEBUG_TIMEOUT_MIN_S 10
#define SETTINGS_DEBUG_TIMEOUT_MAX_S (STATUS_DEBUG_TIMEOUT_MS / 1000)

enum settings_range_id {
  SETTINGS_RANGE_SCREEN,         // panel brightness, percent
  SETTINGS_RANGE_LED,            // status LED brightness, percent
  SETTINGS_RANGE_DEBUG_TIMEOUT,  // R-006 debug row auto-off, seconds
  SETTINGS_RANGE_COUNT,
};

struct settings_range {
  const char *key;  // wire and NVS key, <= 15 bytes (the NVS limit)
  const char *label;
  const char *unit;         // "%" or "s", for the page
  int min, max, step, def;  // inclusive bounds; def is the fresh-flash value
  bool present;             // this board has the thing it controls
};

// Fills the range table for a board. The bounds live in this one pure function
// so the test that pins the security properties above reads exactly the table
// the device serves, not a copy of it.
void settings_ranges_for_board(struct settings_range out[SETTINGS_RANGE_COUNT],
                               bool has_screen, bool has_led);

// What this image can offer. A board with no palette advertises zero themes,
// and the page then shows no theme control — the OLED dongle is not given a
// setting that provably does nothing (AGENTS.md §4, implied support).
struct settings_catalog {
  const char *const *themes;  // kebab-case names, theme_count of them
  int theme_count;
  int theme_default;  // index: the fresh-flash value (Kconfig chooses it)
  struct settings_range ranges[SETTINGS_RANGE_COUNT];
};

// Exact-name lookup; -1 when absent.
int settings_theme_index(const struct settings_catalog *c, const char *name);

// The theme a STORED value selects: the matching entry, or the default when
// the value is NULL, empty, or names nothing this image has. Never -1.
int settings_resolve_theme(const struct settings_catalog *c,
                           const char *stored);

// The value a STORED range setting resolves to: the stored value when there is
// one and it is within bounds, else the default. Out of bounds is not clamped,
// because a value outside the table is a value some other image wrote, and
// clamping would turn "I do not know this" into a number that looks chosen.
int settings_resolve_range(const struct settings_catalog *c,
                           enum settings_range_id id, int stored, bool have);

struct settings_update {
  int theme;                        // catalogue index, or -1 if not mentioned
  int range[SETTINGS_RANGE_COUNT];  // value, or -1 if not mentioned
};

enum settings_parse_result {
  SETTINGS_PARSE_OK,       // at least one known key, every one of them usable
  SETTINGS_PARSE_NOTHING,  // no key this image knows: a 400, not a silent 200
  SETTINGS_PARSE_BAD,      // a known key with a value the catalogue refuses
};

// Parses a POST body (application/x-www-form-urlencoded). `out` is untouched
// unless the result is OK — the airkvm_decode contract, so a refused body
// cannot leave half an update behind. A range value must be a plain decimal
// integer within its bounds; a key for a range this board does not have is
// refused, not ignored, because the page should never have offered it.
enum settings_parse_result settings_parse_form(const struct settings_catalog *c,
                                               const char *body, size_t len,
                                               struct settings_update *out);

// --- the GET document --------------------------------------------------------

struct settings_identity {
  const char *board;  // human name from board.h
  const char *chip;   // "esp32s3"
  unsigned flash_mb;
  const char *fw;   // app version string
  const char *idf;  // ESP-IDF version string
};

struct settings_features {
  const char *panel;  // "ssd1306", "st7789", or NULL for none
  int panel_w, panel_h;
  bool palette;     // the panel has colours a theme can change
  bool backlight;   // a PWM backlight, as opposed to a contrast register
  const char *led;  // "none", "single", "rgb"
  bool sdcard;
  bool usb_device;  // native USB OTG: the board can be a keyboard
};

struct settings_view {
  struct settings_identity id;
  struct settings_features feat;
  const struct settings_catalog *cat;
  int theme;  // current catalogue index; ignored when theme_count is 0
  int range_value[SETTINGS_RANGE_COUNT];  // current values; ignored if absent
};

// Serialises the view as the /settings JSON document. Returns the length
// written (NUL not counted), or 0 when `cap` cannot hold the whole document:
// truncated JSON is a different document rather than a shorter one, and the
// page would render it as such.
size_t settings_json(const struct settings_view *v, char *buf, size_t cap);
