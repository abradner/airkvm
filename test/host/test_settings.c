#include <stdio.h>
#include <string.h>

#include "settings_schema.h"
#include "test_support.h"

static const char *const THEMES[] = {"tokyo-night", "monokai-machine",
                                     "solarized-dark"};
static struct settings_catalog CAT;  // the LCD147: palette, backlight, LED
static struct settings_catalog NO_PALETTE;  // the OLED dongle: panel, no LED
static struct settings_catalog HEADLESS;    // nothing but the button

static void build_catalogues(void) {
  CAT = (struct settings_catalog){
      .themes = THEMES, .theme_count = 3, .theme_default = 1};
  settings_ranges_for_board(CAT.ranges, true, true);
  NO_PALETTE = (struct settings_catalog){
      .themes = NULL, .theme_count = 0, .theme_default = 0};
  settings_ranges_for_board(NO_PALETTE.ranges, true, false);
  HEADLESS = (struct settings_catalog){
      .themes = NULL, .theme_count = 0, .theme_default = 0};
  settings_ranges_for_board(HEADLESS.ranges, false, false);
}

static enum settings_parse_result parse(const struct settings_catalog *c,
                                        const char *body,
                                        struct settings_update *u) {
  return settings_parse_form(c, body, strlen(body), u);
}

static struct settings_update untouched(void) {
  struct settings_update u = {.theme = 99};
  for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
    u.range[i] = 99;
  }
  return u;
}

// --- the bounds themselves are the security properties ---------------------------

static void the_screen_has_a_visible_floor(void) {
  // R-009: the panel is the proof of presence, so it cannot be sent to zero.
  const struct settings_range *r = &CAT.ranges[SETTINGS_RANGE_SCREEN];
  CHECK(r->min >= 10);
  CHECK(r->min <= r->def);
  CHECK_EQ_INT(r->def, 100);  // byte-identical to the panel before this existed
  CHECK_EQ_STR(r->key, "screen");
  CHECK(strlen(r->key) <= 15);  // the NVS key limit
}

static void the_debug_timeout_can_only_be_shortened(void) {
  // R-006/R-010: the ceiling IS the default, so "shorten-only" is a property
  // of one number. status.h's clamp is the enforcement; this table's refusal
  // (below) is the politeness, and this pins that they agree.
  const struct settings_range *r = &CAT.ranges[SETTINGS_RANGE_DEBUG_TIMEOUT];
  CHECK_EQ_INT(r->max, r->def);
  CHECK_EQ_INT(r->max, STATUS_DEBUG_TIMEOUT_MS / 1000);
  CHECK(r->min >= 10);
  CHECK(r->present);  // every board: the button and the row are universal
  CHECK_EQ_STR(r->key, "dbgtimeout");
  CHECK(strlen(r->key) <= 15);
}

static void presence_follows_the_board(void) {
  CHECK(CAT.ranges[SETTINGS_RANGE_SCREEN].present);
  CHECK(CAT.ranges[SETTINGS_RANGE_LED].present);
  CHECK(NO_PALETTE.ranges[SETTINGS_RANGE_SCREEN].present);
  CHECK(!NO_PALETTE.ranges[SETTINGS_RANGE_LED].present);
  CHECK(!HEADLESS.ranges[SETTINGS_RANGE_SCREEN].present);
  CHECK(HEADLESS.ranges[SETTINGS_RANGE_DEBUG_TIMEOUT].present);
}

// --- from flash: default --------------------------------------------------------

static void a_stored_value_resolves_or_defaults(void) {
  CHECK_EQ_INT(settings_resolve_theme(&CAT, "solarized-dark"), 2);
  CHECK_EQ_INT(settings_resolve_theme(&CAT, "tokyo-night"), 0);
  // The three ways flash can fail to name a theme this image has, and all of
  // them land on the default rather than on -1 or on a blank panel: a key
  // that was never written, one written empty, and one written by an image
  // with a theme this one lacks.
  CHECK_EQ_INT(settings_resolve_theme(&CAT, NULL), 1);
  CHECK_EQ_INT(settings_resolve_theme(&CAT, ""), 1);
  CHECK_EQ_INT(settings_resolve_theme(&CAT, "gruvbox"), 1);
  // Positive control on the lookup itself, so the defaults above are not
  // "everything returns 1".
  CHECK_EQ_INT(settings_theme_index(&CAT, "gruvbox"), -1);
  CHECK_EQ_INT(settings_theme_index(&CAT, "monokai-machine"), 1);
}

static void a_stored_range_resolves_or_defaults(void) {
  CHECK_EQ_INT(settings_resolve_range(&CAT, SETTINGS_RANGE_SCREEN, 40, true),
               40);
  CHECK_EQ_INT(settings_resolve_range(&CAT, SETTINGS_RANGE_SCREEN, 10, true),
               10);
  // Missing -> default; out of bounds -> default, NOT clamped: a 3 written
  // by some other image is "I do not know this", not "very dim".
  CHECK_EQ_INT(settings_resolve_range(&CAT, SETTINGS_RANGE_SCREEN, 0, false),
               100);
  CHECK_EQ_INT(settings_resolve_range(&CAT, SETTINGS_RANGE_SCREEN, 3, true),
               100);
  CHECK_EQ_INT(settings_resolve_range(&CAT, SETTINGS_RANGE_SCREEN, 101, true),
               100);
  // A stored timeout longer than the ceiling reads as the ceiling's default,
  // never as the longer value.
  const int max_s = STATUS_DEBUG_TIMEOUT_MS / 1000;
  CHECK_EQ_INT(settings_resolve_range(&CAT, SETTINGS_RANGE_DEBUG_TIMEOUT,
                                      max_s + 1, true),
               max_s);
  CHECK_EQ_INT(
      settings_resolve_range(&CAT, SETTINGS_RANGE_DEBUG_TIMEOUT, 30, true), 30);
}

// --- from the network: refuse ------------------------------------------------------

static void a_posted_theme_is_accepted_only_if_the_catalogue_has_it(void) {
  struct settings_update u = untouched();
  CHECK_EQ_INT(parse(&CAT, "theme=tokyo-night", &u), SETTINGS_PARSE_OK);
  CHECK_EQ_INT(u.theme, 0);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_SCREEN], -1);  // not mentioned
  // Percent-encoding and unknown keys, both ignored as a browser would need.
  CHECK_EQ_INT(parse(&CAT, "extra=1&theme=solarized%2Ddark&x=", &u),
               SETTINGS_PARSE_OK);
  CHECK_EQ_INT(u.theme, 2);

  // A name this image does not have is refused, not defaulted: on the wire
  // the page is politeness and this is the enforcement.
  u = untouched();
  CHECK_EQ_INT(parse(&CAT, "theme=gruvbox", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(u.theme, 99);  // untouched on refusal
  CHECK_EQ_INT(parse(&CAT, "theme=", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "theme=%zz", &u), SETTINGS_PARSE_BAD);
  // Over-long: SETTINGS_THEME_NAME_MAX + 1 characters cannot be a name.
  char big[SETTINGS_THEME_NAME_MAX + 2 + 6];
  memcpy(big, "theme=", 6);
  memset(big + 6, 'a', SETTINGS_THEME_NAME_MAX + 1);
  big[6 + SETTINGS_THEME_NAME_MAX + 1] = '\0';
  CHECK_EQ_INT(parse(&CAT, big, &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(u.theme, 99);
}

static void a_posted_range_is_accepted_only_within_its_bounds(void) {
  struct settings_update u = untouched();
  CHECK_EQ_INT(parse(&CAT, "screen=40", &u), SETTINGS_PARSE_OK);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_SCREEN], 40);
  CHECK_EQ_INT(u.theme, -1);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_LED], -1);
  // Several in one body: all or none.
  u = untouched();
  CHECK_EQ_INT(
      parse(&CAT, "screen=10&led=5&dbgtimeout=60&theme=tokyo-night", &u),
      SETTINGS_PARSE_OK);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_SCREEN], 10);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_LED], 5);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_DEBUG_TIMEOUT], 60);
  CHECK_EQ_INT(u.theme, 0);

  // Below the floor, above the ceiling, not a number: refused, and the
  // whole update with it — a body with one bad value leaves nothing behind.
  u = untouched();
  CHECK_EQ_INT(parse(&CAT, "screen=9", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=0", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=101", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=50%25", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=+50", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=-5", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=1e2", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "screen=99999999", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&CAT, "led=100&screen=9", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_LED], 99);  // untouched
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_SCREEN], 99);

  // The debug timeout: shorter is fine, the ceiling is fine, longer is not.
  const int max_s = STATUS_DEBUG_TIMEOUT_MS / 1000;
  char body[32];
  snprintf(body, sizeof(body), "dbgtimeout=%d", max_s);
  CHECK_EQ_INT(parse(&CAT, body, &u), SETTINGS_PARSE_OK);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_DEBUG_TIMEOUT], max_s);
  snprintf(body, sizeof(body), "dbgtimeout=%d", max_s + 1);
  u = untouched();
  CHECK_EQ_INT(parse(&CAT, body, &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_DEBUG_TIMEOUT], 99);
}

static void a_body_naming_nothing_we_know_is_not_a_success(void) {
  struct settings_update u = untouched();
  // A page from a newer image posting a key this one lacks must not be told
  // it worked — nor must an empty body.
  CHECK_EQ_INT(parse(&CAT, "usb=hid", &u), SETTINGS_PARSE_NOTHING);
  CHECK_EQ_INT(parse(&CAT, "", &u), SETTINGS_PARSE_NOTHING);
  CHECK_EQ_INT(parse(&CAT, "&&", &u), SETTINGS_PARSE_NOTHING);
  CHECK_EQ_INT(u.theme, 99);
}

static void a_board_refuses_a_setting_for_a_thing_it_lacks(void) {
  struct settings_update u = untouched();
  // Known keys, nothing to apply them to. Refused rather than ignored,
  // because the page should never have offered the control.
  CHECK_EQ_INT(parse(&NO_PALETTE, "theme=tokyo-night", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&NO_PALETTE, "led=50", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(parse(&HEADLESS, "screen=50", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(u.theme, 99);
  CHECK_EQ_INT(u.range[SETTINGS_RANGE_LED], 99);
  // And accepts what it does have.
  CHECK_EQ_INT(parse(&NO_PALETTE, "screen=50", &u), SETTINGS_PARSE_OK);
  CHECK_EQ_INT(parse(&HEADLESS, "dbgtimeout=60", &u), SETTINGS_PARSE_OK);
  CHECK_EQ_INT(settings_resolve_theme(&NO_PALETTE, "anything"), 0);
}

// --- the GET document ----------------------------------------------------------

static struct settings_view view(const struct settings_catalog *c) {
  struct settings_view v = {
      .id = {.board = "Waveshare ESP32-S3-LCD-1.47B",
             .chip = "esp32s3",
             .flash_mb = 16,
             .fw = "0.3.0",
             .idf = "v5.5"},
      .feat = {.panel = "st7789",
               .panel_w = 320,
               .panel_h = 172,
               .palette = true,
               .backlight = true,
               .led = "rgb",
               .sdcard = true,
               .usb_device = true},
      .cat = c,
      .theme = 2,
  };
  v.range_value[SETTINGS_RANGE_SCREEN] = 60;
  v.range_value[SETTINGS_RANGE_LED] = 25;
  v.range_value[SETTINGS_RANGE_DEBUG_TIMEOUT] = 120;
  return v;
}

static int count(const char *s, char c) {
  int n = 0;
  for (; *s; s++) n += *s == c;
  return n;
}

static void the_document_is_whole_and_balanced(void) {
  char buf[2048];
  struct settings_view v = view(&CAT);
  const size_t n = settings_json(&v, buf, sizeof(buf));
  CHECK(n > 0);
  CHECK_EQ_INT((long)n, (long)strlen(buf));
  CHECK(buf[0] == '{');
  CHECK(buf[n - 1] == '}');
  CHECK_EQ_INT(count(buf, '{'), count(buf, '}'));
  CHECK_EQ_INT(count(buf, '['), count(buf, ']'));
  // The parts the page keys on. Substring checks, since there is no parser
  // on the host; the structural checks above are what make these meaningful.
  CHECK(strstr(buf, "\"board\":\"Waveshare ESP32-S3-LCD-1.47B\"") != NULL);
  CHECK(strstr(buf, "\"flash_mb\":16") != NULL);
  CHECK(strstr(buf, "\"palette\":true") != NULL);
  CHECK(strstr(buf, "\"led\":\"rgb\"") != NULL);
  CHECK(strstr(buf, "\"key\":\"theme\"") != NULL);
  CHECK(strstr(buf, "\"scope\":\"device\"") != NULL);
  CHECK(strstr(buf, "\"value\":\"solarized-dark\"") != NULL);
  CHECK(strstr(buf,
               "\"values\":[\"tokyo-night\",\"monokai-machine\","
               "\"solarized-dark\"]") != NULL);
  // The ranges, with their bounds, in catalogue order after the theme.
  CHECK(strstr(buf,
               "\"key\":\"screen\",\"label\":\"Screen brightness\",\"unit\":"
               "\"%\",\"scope\":\"device\",\"type\":\"range\",\"value\":60,"
               "\"min\":10,\"max\":100,\"step\":5}") != NULL);
  CHECK(strstr(buf, "\"key\":\"led\"") != NULL);
  CHECK(strstr(buf, "\"value\":25") != NULL);
  CHECK(strstr(buf, "\"key\":\"dbgtimeout\"") != NULL);
  CHECK(strstr(buf, "\"unit\":\"s\"") != NULL);
  CHECK(strstr(buf, "\"value\":120") != NULL);
  // Every element is comma-separated: "}{" would be two objects touching.
  CHECK(strstr(buf, "}{") == NULL);
}

static void a_board_advertises_only_what_it_has(void) {
  char buf[2048];
  struct settings_view v = view(&NO_PALETTE);
  v.feat.palette = false;
  v.feat.panel = "ssd1306";
  v.feat.led = "none";
  CHECK(settings_json(&v, buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "theme") == NULL);
  CHECK(strstr(buf, "\"key\":\"led\"") == NULL);
  CHECK(strstr(buf, "\"key\":\"screen\"") != NULL);
  CHECK(strstr(buf, "\"key\":\"dbgtimeout\"") != NULL);
  CHECK(strstr(buf, "\"settings\":[{\"key\":\"screen\"") !=
        NULL);  // no leading comma
  CHECK(strstr(buf, "}{") == NULL);

  v = view(&HEADLESS);
  v.feat.panel = NULL;
  CHECK(settings_json(&v, buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "\"settings\":[{\"key\":\"dbgtimeout\"") != NULL);
  CHECK(strstr(buf, "\"key\":\"screen\"") == NULL);
}

static void a_current_value_out_of_range_reads_as_the_default(void) {
  // The value in the view comes from settings.c's own state, but the document
  // must still be honest if that state is ever wrong: never index past the
  // catalogue, never emit a value the page cannot select.
  char buf[2048];
  struct settings_view v = view(&CAT);
  v.theme = 42;
  v.range_value[SETTINGS_RANGE_SCREEN] = 3;
  CHECK(settings_json(&v, buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "\"value\":\"monokai-machine\"") != NULL);
  CHECK(strstr(buf, "\"value\":100,\"min\":10") != NULL);
  CHECK(strstr(buf, "\"value\":3,") == NULL);
}

static void strings_are_escaped(void) {
  char buf[2048];
  struct settings_view v = view(&CAT);
  v.id.board = "a \"quoted\" \\ name\tend";
  CHECK(settings_json(&v, buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "\"board\":\"a \\\"quoted\\\" \\\\ name\\u0009end\"") !=
        NULL);
  // Positive control: an unescaped quote inside the value would have produced
  // this, which must be absent.
  CHECK(strstr(buf, "\"a \"quoted\"") == NULL);
  v.feat.panel = NULL;
  CHECK(settings_json(&v, buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "\"panel\":null") != NULL);
}

static void a_buffer_too_small_yields_nothing_rather_than_a_prefix(void) {
  char buf[2048];
  struct settings_view v = view(&CAT);
  const size_t full = settings_json(&v, buf, sizeof(buf));
  CHECK(full > 0);
  // Exactly the length is not enough (the NUL needs a byte); one more is.
  CHECK_EQ_INT((long)settings_json(&v, buf, full), 0);
  CHECK_EQ_STR(buf, "");
  CHECK_EQ_INT((long)settings_json(&v, buf, full + 1), (long)full);
  // And a comically small one, so this is not only an off-by-one check.
  CHECK_EQ_INT((long)settings_json(&v, buf, 8), 0);
  CHECK_EQ_STR(buf, "");
  // net.c's buffer must hold the biggest document any board serves; the
  // LCD147 view above is that board. Pin it against a generous margin, so a
  // grown catalogue fails here rather than as a 500 on the device.
  CHECK(full < 1024 - 128);
}

int main(void) {
  build_catalogues();
  RUN(the_screen_has_a_visible_floor);
  RUN(the_debug_timeout_can_only_be_shortened);
  RUN(presence_follows_the_board);
  RUN(a_stored_value_resolves_or_defaults);
  RUN(a_stored_range_resolves_or_defaults);
  RUN(a_posted_theme_is_accepted_only_if_the_catalogue_has_it);
  RUN(a_posted_range_is_accepted_only_within_its_bounds);
  RUN(a_body_naming_nothing_we_know_is_not_a_success);
  RUN(a_board_refuses_a_setting_for_a_thing_it_lacks);
  RUN(the_document_is_whole_and_balanced);
  RUN(a_board_advertises_only_what_it_has);
  RUN(a_current_value_out_of_range_reads_as_the_default);
  RUN(strings_are_escaped);
  RUN(a_buffer_too_small_yields_nothing_rather_than_a_prefix);
  return test_summary("settings");
}
