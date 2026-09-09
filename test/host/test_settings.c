#include <string.h>

#include "settings_schema.h"
#include "test_support.h"

static const char *const THEMES[] = {"tokyo-night", "monokai-machine",
                                     "solarized-dark"};
static const struct settings_catalog CAT = {
    .themes = THEMES, .theme_count = 3, .theme_default = 1};
static const struct settings_catalog NO_PALETTE = {
    .themes = NULL, .theme_count = 0, .theme_default = 0};

static enum settings_parse_result parse(const struct settings_catalog *c,
                                        const char *body,
                                        struct settings_update *u) {
  return settings_parse_form(c, body, strlen(body), u);
}

// --- from flash: default -------------------------------------------------------

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

// --- from the network: refuse --------------------------------------------------

static void a_posted_theme_is_accepted_only_if_the_catalogue_has_it(void) {
  struct settings_update u = {.theme = 99};
  CHECK_EQ_INT(parse(&CAT, "theme=tokyo-night", &u), SETTINGS_PARSE_OK);
  CHECK_EQ_INT(u.theme, 0);
  // Percent-encoding and unknown keys, both ignored as a browser would need.
  CHECK_EQ_INT(parse(&CAT, "extra=1&theme=solarized%2Ddark&x=", &u),
               SETTINGS_PARSE_OK);
  CHECK_EQ_INT(u.theme, 2);

  // A name this image does not have is refused, not defaulted: on the wire
  // the page is politeness and this is the enforcement.
  u.theme = 99;
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

static void a_body_naming_nothing_we_know_is_not_a_success(void) {
  struct settings_update u = {.theme = 99};
  // A page from a newer image posting a key this one lacks must not be told
  // it worked — nor must an empty body.
  CHECK_EQ_INT(parse(&CAT, "brightness=50", &u), SETTINGS_PARSE_NOTHING);
  CHECK_EQ_INT(parse(&CAT, "", &u), SETTINGS_PARSE_NOTHING);
  CHECK_EQ_INT(parse(&CAT, "&&", &u), SETTINGS_PARSE_NOTHING);
  CHECK_EQ_INT(u.theme, 99);
}

static void a_board_without_a_palette_refuses_a_theme(void) {
  struct settings_update u = {.theme = 99};
  // Known key, nothing to apply it to. Refused rather than ignored, because
  // the page should never have offered the control (the catalogue is empty).
  CHECK_EQ_INT(parse(&NO_PALETTE, "theme=tokyo-night", &u), SETTINGS_PARSE_BAD);
  CHECK_EQ_INT(u.theme, 99);
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
  return v;
}

static int count(const char *s, char c) {
  int n = 0;
  for (; *s; s++) n += *s == c;
  return n;
}

static void the_document_is_whole_and_balanced(void) {
  char buf[1024];
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
}

static void a_board_without_a_palette_advertises_no_theme(void) {
  char buf[1024];
  struct settings_view v = view(&NO_PALETTE);
  v.feat.palette = false;
  v.feat.panel = "ssd1306";
  CHECK(settings_json(&v, buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "\"settings\":[]") != NULL);
  CHECK(strstr(buf, "theme") == NULL);
  CHECK(strstr(buf, "\"palette\":false") != NULL);
}

static void a_current_value_out_of_range_reads_as_the_default(void) {
  // The value in the view comes from settings.c's own state, but the document
  // must still be honest if that state is ever wrong: never index past the
  // catalogue, never emit a value the page cannot select.
  char buf[1024];
  struct settings_view v = view(&CAT);
  v.theme = 42;
  CHECK(settings_json(&v, buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "\"value\":\"monokai-machine\"") != NULL);
}

static void strings_are_escaped(void) {
  char buf[1024];
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
  char buf[1024];
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
}

int main(void) {
  RUN(a_stored_value_resolves_or_defaults);
  RUN(a_posted_theme_is_accepted_only_if_the_catalogue_has_it);
  RUN(a_body_naming_nothing_we_know_is_not_a_success);
  RUN(a_board_without_a_palette_refuses_a_theme);
  RUN(the_document_is_whole_and_balanced);
  RUN(a_board_without_a_palette_advertises_no_theme);
  RUN(a_current_value_out_of_range_reads_as_the_default);
  RUN(strings_are_escaped);
  RUN(a_buffer_too_small_yields_nothing_rather_than_a_prefix);
  return test_summary("settings");
}
