#include "settings_schema.h"

#include <stdio.h>
#include <string.h>

#include "form.h"

void settings_ranges_for_board(struct settings_range out[SETTINGS_RANGE_COUNT],
                               bool has_screen, bool has_led) {
  out[SETTINGS_RANGE_SCREEN] = (struct settings_range){
      .key = "screen",
      .label = "Screen brightness",
      .unit = "%",
      .min = SETTINGS_SCREEN_MIN,
      .max = SETTINGS_SCREEN_MAX,
      .step = 5,
      .def = SETTINGS_SCREEN_DEFAULT,
      .present = has_screen,
  };
  out[SETTINGS_RANGE_LED] = (struct settings_range){
      .key = "led",
      .label = "Status LED brightness",
      .unit = "%",
      .min = SETTINGS_LED_MIN,
      .max = SETTINGS_LED_MAX,
      .step = 5,
      .def = SETTINGS_LED_DEFAULT,
      .present = has_led,
  };
  out[SETTINGS_RANGE_DEBUG_TIMEOUT] = (struct settings_range){
      .key = "dbgtimeout",
      .label = "Keystroke display auto-off",
      .unit = "s",
      .min = SETTINGS_DEBUG_TIMEOUT_MIN_S,
      .max = SETTINGS_DEBUG_TIMEOUT_MAX_S,
      .step = 10,
      .def = SETTINGS_DEBUG_TIMEOUT_MAX_S,
      .present = true,  // every board has the button and the row
  };
}

static const char *const USB_VALUES[] = {"keyboard+mouse+disk",
                                         "keyboard+mouse"};

void settings_choices_for_board(
    struct settings_choice out[SETTINGS_CHOICE_COUNT], bool usb_device) {
  out[SETTINGS_CHOICE_USB] = (struct settings_choice){
      .key = "usb",
      .label = "USB interfaces",
      .apply = "reboot",
      .values = USB_VALUES,
      .count = 2,
      .def = SETTINGS_USB_WITH_DISK,
      .present = usb_device,
  };
}

int settings_choice_index(const struct settings_catalog *c,
                          enum settings_choice_id id, const char *name) {
  if (c == NULL || name == NULL) {
    return -1;
  }
  const struct settings_choice *ch = &c->choices[id];
  for (int i = 0; i < ch->count; i++) {
    if (strcmp(ch->values[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

int settings_resolve_choice(const struct settings_catalog *c,
                            enum settings_choice_id id, const char *stored) {
  const int i = settings_choice_index(c, id, stored);
  return i >= 0 ? i : c->choices[id].def;
}

int settings_theme_index(const struct settings_catalog *c, const char *name) {
  if (c == NULL || name == NULL) {
    return -1;
  }
  for (int i = 0; i < c->theme_count; i++) {
    if (strcmp(c->themes[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

int settings_resolve_theme(const struct settings_catalog *c,
                           const char *stored) {
  const int i = settings_theme_index(c, stored);
  return i >= 0 ? i : c->theme_default;
}

int settings_resolve_range(const struct settings_catalog *c,
                           enum settings_range_id id, int stored, bool have) {
  const struct settings_range *r = &c->ranges[id];
  if (have && stored >= r->min && stored <= r->max) {
    return stored;
  }
  return r->def;
}

// A plain decimal integer and nothing else: no sign, no spaces, no suffix. The
// values here are all non-negative, and "10%" or " 10" from a hand-written
// request should be a 400 rather than a guess.
static bool parse_uint(const char *s, int *out) {
  if (*s == '\0' || strlen(s) > 6) {
    return false;
  }
  int v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') {
      return false;
    }
    v = v * 10 + (*s - '0');
  }
  *out = v;
  return true;
}

enum settings_parse_result settings_parse_form(const struct settings_catalog *c,
                                               const char *body, size_t len,
                                               struct settings_update *out) {
  if (c == NULL || body == NULL || out == NULL) {
    return SETTINGS_PARSE_BAD;
  }
  struct settings_update scratch = {.theme = -1};
  for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
    scratch.range[i] = -1;
  }
  for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
    scratch.choice[i] = -1;
  }
  bool any = false;

  size_t pos = 0;
  struct form_pair p;
  while (form_next(body, len, &pos, &p)) {
    if (form_key_is(&p, "theme")) {
      char name[SETTINGS_THEME_NAME_MAX + 1];
      if (form_decode(p.value, p.value_len, name, sizeof(name)) < 1) {
        return SETTINGS_PARSE_BAD;  // empty, over-long, or bad encoding
      }
      // A board with no palette knows this key and cannot honour it: refuse,
      // rather than ignore, because the page should not have offered it.
      const int i = settings_theme_index(c, name);
      if (i < 0) {
        return SETTINGS_PARSE_BAD;
      }
      scratch.theme = i;
      any = true;
      continue;
    }
    if (form_key_is(&p, "restart")) {
      char text[4];
      if (form_decode(p.value, p.value_len, text, sizeof(text)) != 1 ||
          text[0] != '1') {
        return SETTINGS_PARSE_BAD;
      }
      scratch.restart = true;
      any = true;
      continue;
    }
    bool matched = false;
    for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
      const struct settings_choice *ch = &c->choices[i];
      if (!form_key_is(&p, ch->key)) {
        continue;
      }
      char name[SETTINGS_THEME_NAME_MAX + 1];
      if (!ch->present ||
          form_decode(p.value, p.value_len, name, sizeof(name)) < 1) {
        return SETTINGS_PARSE_BAD;
      }
      const int v = settings_choice_index(c, (enum settings_choice_id)i, name);
      if (v < 0) {
        return SETTINGS_PARSE_BAD;
      }
      scratch.choice[i] = v;
      any = true;
      matched = true;
      break;
    }
    if (matched) {
      continue;
    }
    for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
      const struct settings_range *r = &c->ranges[i];
      if (!form_key_is(&p, r->key)) {
        continue;
      }
      char text[8];
      int v;
      if (!r->present ||
          form_decode(p.value, p.value_len, text, sizeof(text)) < 1 ||
          !parse_uint(text, &v) || v < r->min || v > r->max) {
        return SETTINGS_PARSE_BAD;
      }
      scratch.range[i] = v;
      any = true;
      break;
    }
    // Unknown keys are ignored: a page from a newer image may post more.
  }
  if (!any) {
    return SETTINGS_PARSE_NOTHING;
  }
  *out = scratch;
  return SETTINGS_PARSE_OK;
}

// --- JSON writer ---------------------------------------------------------------
//
// Just enough to emit this one document. Every string goes through jw_str, so a
// board name or version with a quote in it cannot break the page; anything
// below 0x20 is escaped as \u00XX. Overflow is sticky and reported once at the
// end, so the callers stay linear.

struct jw {
  char *buf;
  size_t cap;
  size_t len;
  bool overflow;
};

static void jw_raw(struct jw *w, const char *s) {
  const size_t n = strlen(s);
  if (w->overflow || w->len + n >= w->cap) {
    w->overflow = true;
    return;
  }
  memcpy(w->buf + w->len, s, n);
  w->len += n;
}

static void jw_char(struct jw *w, char c) {
  if (w->overflow || w->len + 1 >= w->cap) {
    w->overflow = true;
    return;
  }
  w->buf[w->len++] = c;
}

static void jw_str(struct jw *w, const char *s) {
  jw_char(w, '"');
  for (; s != NULL && *s != '\0'; s++) {
    const unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      jw_char(w, '\\');
      jw_char(w, (char)c);
    } else if (c < 0x20) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      jw_raw(w, esc);
    } else {
      jw_char(w, (char)c);
    }
  }
  jw_char(w, '"');
}

static void jw_int(struct jw *w, long v) {
  char num[24];
  snprintf(num, sizeof(num), "%ld", v);
  jw_raw(w, num);
}

static void jw_bool(struct jw *w, bool v) {
  jw_raw(w, v ? "true" : "false");
}

// "key": — the comma before it is the caller's, so a first member is not
// special-cased everywhere.
static void jw_key(struct jw *w, const char *k) {
  jw_str(w, k);
  jw_char(w, ':');
}

static void jw_kv_str(struct jw *w, const char *k, const char *v) {
  jw_key(w, k);
  jw_str(w, v);
}

size_t settings_json(const struct settings_view *v, char *buf, size_t cap) {
  if (v == NULL || buf == NULL || cap == 0) {
    return 0;
  }
  struct jw w = {.buf = buf, .cap = cap};
  const struct settings_catalog *cat = v->cat;
  const bool has_themes = cat != NULL && cat->theme_count > 0;

  jw_raw(&w, "{");
  jw_key(&w, "identity");
  jw_raw(&w, "{");
  jw_kv_str(&w, "board", v->id.board);
  jw_raw(&w, ",");
  jw_kv_str(&w, "chip", v->id.chip);
  jw_raw(&w, ",");
  jw_key(&w, "flash_mb");
  jw_int(&w, (long)v->id.flash_mb);
  jw_raw(&w, ",");
  jw_kv_str(&w, "fw", v->id.fw);
  jw_raw(&w, ",");
  jw_kv_str(&w, "idf", v->id.idf);
  jw_raw(&w, "},");

  jw_key(&w, "features");
  jw_raw(&w, "{");
  jw_key(&w, "panel");
  if (v->feat.panel != NULL) {
    jw_str(&w, v->feat.panel);
  } else {
    jw_raw(&w, "null");
  }
  jw_raw(&w, ",");
  jw_key(&w, "panel_w");
  jw_int(&w, v->feat.panel_w);
  jw_raw(&w, ",");
  jw_key(&w, "panel_h");
  jw_int(&w, v->feat.panel_h);
  jw_raw(&w, ",");
  jw_key(&w, "palette");
  jw_bool(&w, v->feat.palette);
  jw_raw(&w, ",");
  jw_key(&w, "backlight");
  jw_bool(&w, v->feat.backlight);
  jw_raw(&w, ",");
  jw_kv_str(&w, "led", v->feat.led);
  jw_raw(&w, ",");
  jw_key(&w, "sdcard");
  jw_bool(&w, v->feat.sdcard);
  jw_raw(&w, ",");
  jw_key(&w, "usb_device");
  jw_bool(&w, v->feat.usb_device);
  jw_raw(&w, ",");
  jw_key(&w, "usb_disk");
  jw_bool(&w, v->feat.usb_disk);
  jw_raw(&w, "},");

  // Each setting carries everything the page needs to render it, so a setting
  // added in a later image needs no page change: key, label, scope, type, the
  // current value, and what it may become.
  jw_key(&w, "settings");
  jw_raw(&w, "[");
  bool emitted = false;
  if (has_themes) {
    const int cur = (v->theme >= 0 && v->theme < cat->theme_count)
                        ? v->theme
                        : cat->theme_default;
    jw_raw(&w, "{");
    jw_kv_str(&w, "key", "theme");
    jw_raw(&w, ",");
    jw_kv_str(&w, "label", "Panel theme");
    jw_raw(&w, ",");
    jw_kv_str(&w, "scope", "device");
    jw_raw(&w, ",");
    jw_kv_str(&w, "type", "enum");
    jw_raw(&w, ",");
    jw_kv_str(&w, "value", cat->themes[cur]);
    jw_raw(&w, ",");
    jw_key(&w, "values");
    jw_raw(&w, "[");
    for (int i = 0; i < cat->theme_count; i++) {
      if (i > 0) {
        jw_raw(&w, ",");
      }
      jw_str(&w, cat->themes[i]);
    }
    jw_raw(&w, "]}");
    emitted = true;
  }
  if (cat != NULL) {
    for (int i = 0; i < SETTINGS_RANGE_COUNT; i++) {
      const struct settings_range *r = &cat->ranges[i];
      if (!r->present) {
        continue;
      }
      // The value in the view comes from settings.c's own state; the document
      // is honest even if that state is ever wrong — never a value the page
      // cannot select.
      int cur = v->range_value[i];
      if (cur < r->min || cur > r->max) {
        cur = r->def;
      }
      if (emitted) {
        jw_raw(&w, ",");
      }
      jw_raw(&w, "{");
      jw_kv_str(&w, "key", r->key);
      jw_raw(&w, ",");
      jw_kv_str(&w, "label", r->label);
      jw_raw(&w, ",");
      jw_kv_str(&w, "unit", r->unit);
      jw_raw(&w, ",");
      jw_kv_str(&w, "scope", "device");
      jw_raw(&w, ",");
      jw_kv_str(&w, "type", "range");
      jw_raw(&w, ",");
      jw_key(&w, "value");
      jw_int(&w, cur);
      jw_raw(&w, ",");
      jw_key(&w, "min");
      jw_int(&w, r->min);
      jw_raw(&w, ",");
      jw_key(&w, "max");
      jw_int(&w, r->max);
      jw_raw(&w, ",");
      jw_key(&w, "step");
      jw_int(&w, r->step);
      jw_raw(&w, "}");
      emitted = true;
    }
    for (int i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
      const struct settings_choice *ch = &cat->choices[i];
      if (!ch->present) {
        continue;
      }
      int cur = v->choice_value[i];
      if (cur < 0 || cur >= ch->count) {
        cur = ch->def;
      }
      int run = v->choice_running[i];
      if (run < 0 || run >= ch->count) {
        run = ch->def;
      }
      if (emitted) {
        jw_raw(&w, ",");
      }
      jw_raw(&w, "{");
      jw_kv_str(&w, "key", ch->key);
      jw_raw(&w, ",");
      jw_kv_str(&w, "label", ch->label);
      jw_raw(&w, ",");
      jw_kv_str(&w, "scope", "device");
      jw_raw(&w, ",");
      jw_kv_str(&w, "type", "enum");
      if (ch->apply != NULL) {
        jw_raw(&w, ",");
        jw_kv_str(&w, "apply", ch->apply);
        jw_raw(&w, ",");
        jw_kv_str(&w, "running", ch->values[run]);
      }
      jw_raw(&w, ",");
      jw_kv_str(&w, "value", ch->values[cur]);
      jw_raw(&w, ",");
      jw_key(&w, "values");
      jw_raw(&w, "[");
      for (int j = 0; j < ch->count; j++) {
        if (j > 0) {
          jw_raw(&w, ",");
        }
        jw_str(&w, ch->values[j]);
      }
      jw_raw(&w, "]}");
      emitted = true;
    }
  }
  jw_raw(&w, "]}");

  if (w.overflow) {
    buf[0] = '\0';
    return 0;
  }
  buf[w.len] = '\0';
  return w.len;
}
