#include "keymap.h"
#include "test_support.h"

static void unknown_codes_map_to_zero(void) {
  // 0 is HID's "no key", so an unmapped code is inert rather than something the
  // caller must remember to check.
  CHECK_EQ_INT(keymap_lookup("NoSuchKey"), 0);
  CHECK_EQ_INT(keymap_lookup(""), 0);
  CHECK_EQ_INT(keymap_lookup(NULL), 0);

  // event.key values, not event.code values. Mapping these would be the
  // layout-dependence bug AGENTS.md §4 forbids, so they must not resolve.
  CHECK_EQ_INT(keymap_lookup("a"), 0);
  CHECK_EQ_INT(keymap_lookup("A"), 0);
  CHECK_EQ_INT(keymap_lookup("Control"), 0);
}

static void letters_and_digits(void) {
  CHECK_EQ_INT(keymap_lookup("KeyA"), 0x04);
  CHECK_EQ_INT(keymap_lookup("KeyZ"), 0x1D);
  CHECK_EQ_INT(keymap_lookup("Digit1"), 0x1E);
  CHECK_EQ_INT(keymap_lookup("Digit9"), 0x26);
  // HID puts 0 after 9, not before 1 — the easy off-by-one in this table.
  CHECK_EQ_INT(keymap_lookup("Digit0"), 0x27);
}

static void lookup_is_case_sensitive(void) {
  // The browser always emits this exact casing; accepting variants would mean
  // guessing at what a non-console client meant.
  CHECK_EQ_INT(keymap_lookup("keya"), 0);
  CHECK_EQ_INT(keymap_lookup("KEYA"), 0);
}

static void modifier_bits_match_hid_order(void) {
  const struct {
    const char *code;
    uint8_t usage;
    uint8_t bit;
  } cases[] = {
      {"ControlLeft", 0xE0, 0x01},  {"ShiftLeft", 0xE1, 0x02},
      {"AltLeft", 0xE2, 0x04},      {"MetaLeft", 0xE3, 0x08},
      {"ControlRight", 0xE4, 0x10}, {"ShiftRight", 0xE5, 0x20},
      {"AltRight", 0xE6, 0x40},     {"MetaRight", 0xE7, 0x80},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const uint8_t usage = keymap_lookup(cases[i].code);
    CHECK_EQ_INT(usage, cases[i].usage);
    CHECK(keymap_is_modifier(usage));
    CHECK_EQ_INT(keymap_modifier_bit(usage), cases[i].bit);
  }
}

static void non_modifiers_are_not_modifiers(void) {
  CHECK(!keymap_is_modifier(keymap_lookup("KeyA")));
  CHECK(!keymap_is_modifier(keymap_lookup("CapsLock")));
  CHECK(!keymap_is_modifier(0));
  // Positive control: the predicate can return true, so the checks above are
  // testing the predicate rather than a stuck false.
  CHECK(keymap_is_modifier(keymap_lookup("ControlLeft")));
}

static void no_duplicate_usages(void) {
  // Two codes mapping to one usage means one physical key can never be sent.
  // Walks the printable-ASCII-named entries by re-looking-up a representative
  // set; a full table walk would need the table exported, which it deliberately
  // is not.
  const char *codes[] = {
      "KeyA",      "KeyZ",      "Digit0",        "Digit1",      "Enter",
      "Escape",    "Space",     "Tab",           "Minus",       "Equal",
      "F1",        "F12",       "Home",          "End",         "ArrowUp",
      "ArrowDown", "Numpad0",   "Numpad1",       "NumpadEnter", "NumLock",
      "Delete",    "Backspace", "IntlBackslash", "ContextMenu", "ControlLeft",
      "MetaRight",
  };
  const size_t n = sizeof(codes) / sizeof(codes[0]);
  for (size_t i = 0; i < n; i++) {
    for (size_t j = i + 1; j < n; j++) {
      const uint8_t a = keymap_lookup(codes[i]);
      const uint8_t b = keymap_lookup(codes[j]);
      CHECK(a != 0);
      CHECK(a != b);
    }
  }
}

int main(void) {
  RUN(unknown_codes_map_to_zero);
  RUN(letters_and_digits);
  RUN(lookup_is_case_sensitive);
  RUN(modifier_bits_match_hid_order);
  RUN(non_modifiers_are_not_modifiers);
  RUN(no_duplicate_usages);
  return test_summary("keymap");
}
