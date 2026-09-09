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
//                 does not know, resolves to the compile-time default and the
//                 stored bytes are left alone. A panel with no theme is not a
//                 failure, so this is the cosmetic exception to §8's fail-fast
//                 rule, and it is what lets an image be downgraded without
//                 leaving the device on a blank screen.
//   from the    -> REFUSE.   A posted value the catalogue cannot use is a 400,
//   network        never silently corrected: the page is politeness and the
//                 enforcement is here (setup_mode.c states the same rule).
//
// Unknown KEYS are ignored on both paths, by construction: nothing reads them.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Longest theme name accepted on the wire or from flash, NUL excluded. Names
// are kebab-case ASCII (test_theme.c asserts it), so this bounds a buffer, not
// a design.
#define SETTINGS_THEME_NAME_MAX 31

// What this image can offer. A board with no palette advertises zero themes,
// and the page then shows no theme control — the OLED dongle is not given a
// setting that provably does nothing (AGENTS.md §4, implied support).
struct settings_catalog {
  const char *const *themes;  // kebab-case names, theme_count of them
  int theme_count;
  int theme_default;  // index: the fresh-flash value (Kconfig chooses it)
};

// Exact-name lookup; -1 when absent.
int settings_theme_index(const struct settings_catalog *c, const char *name);

// The theme a STORED value selects: the matching entry, or the default when
// the value is NULL, empty, or names nothing this image has. Never -1.
int settings_resolve_theme(const struct settings_catalog *c,
                           const char *stored);

struct settings_update {
  int theme;  // catalogue index, or -1 when the body did not mention it
};

enum settings_parse_result {
  SETTINGS_PARSE_OK,       // at least one known key, every one of them usable
  SETTINGS_PARSE_NOTHING,  // no key this image knows: a 400, not a silent 200
  SETTINGS_PARSE_BAD,      // a known key with a value the catalogue refuses
};

// Parses a POST body (application/x-www-form-urlencoded). `out` is untouched
// unless the result is OK — the airkvm_decode contract, so a refused body
// cannot leave half an update behind.
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
  bool backlight;   // PR 2: brightness
  const char *led;  // "none", "single", "rgb"
  bool sdcard;
  bool usb_device;  // native USB OTG: the board can be a keyboard
};

struct settings_view {
  struct settings_identity id;
  struct settings_features feat;
  const struct settings_catalog *cat;
  int theme;  // current catalogue index; ignored when theme_count is 0
};

// Serialises the view as the /settings JSON document. Returns the length
// written (NUL not counted), or 0 when `cap` cannot hold the whole document:
// truncated JSON is a different document rather than a shorter one, and the
// page would render it as such.
size_t settings_json(const struct settings_view *v, char *buf, size_t cap);
