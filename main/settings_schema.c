#include "settings_schema.h"

#include <stdio.h>
#include <string.h>

#include "form.h"

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

enum settings_parse_result settings_parse_form(const struct settings_catalog *c,
                                               const char *body, size_t len,
                                               struct settings_update *out) {
  if (c == NULL || body == NULL || out == NULL) {
    return SETTINGS_PARSE_BAD;
  }
  struct settings_update scratch = {.theme = -1};
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
  jw_key(&w, "board");
  jw_str(&w, v->id.board);
  jw_raw(&w, ",");
  jw_key(&w, "chip");
  jw_str(&w, v->id.chip);
  jw_raw(&w, ",");
  jw_key(&w, "flash_mb");
  jw_int(&w, (long)v->id.flash_mb);
  jw_raw(&w, ",");
  jw_key(&w, "fw");
  jw_str(&w, v->id.fw);
  jw_raw(&w, ",");
  jw_key(&w, "idf");
  jw_str(&w, v->id.idf);
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
  jw_key(&w, "led");
  jw_str(&w, v->feat.led);
  jw_raw(&w, ",");
  jw_key(&w, "sdcard");
  jw_bool(&w, v->feat.sdcard);
  jw_raw(&w, ",");
  jw_key(&w, "usb_device");
  jw_bool(&w, v->feat.usb_device);
  jw_raw(&w, "},");

  // Each setting carries everything the page needs to render it, so a setting
  // added in a later image needs no page change: key, label, scope, type, the
  // current value, and what it may become.
  jw_key(&w, "settings");
  jw_raw(&w, "[");
  if (has_themes) {
    const int cur = (v->theme >= 0 && v->theme < cat->theme_count)
                        ? v->theme
                        : cat->theme_default;
    jw_raw(&w, "{");
    jw_key(&w, "key");
    jw_str(&w, "theme");
    jw_raw(&w, ",");
    jw_key(&w, "label");
    jw_str(&w, "Panel theme");
    jw_raw(&w, ",");
    jw_key(&w, "scope");
    jw_str(&w, "device");
    jw_raw(&w, ",");
    jw_key(&w, "type");
    jw_str(&w, "enum");
    jw_raw(&w, ",");
    jw_key(&w, "value");
    jw_str(&w, cat->themes[cur]);
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
  }
  jw_raw(&w, "]}");

  if (w.overflow) {
    buf[0] = '\0';
    return 0;
  }
  buf[w.len] = '\0';
  return w.len;
}
