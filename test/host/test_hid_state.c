#include "hid_state.h"
#include "keymap.h"
#include "test_support.h"

static void starts_idle(void) {
  struct hid_state s;
  hid_state_reset(&s);
  CHECK(hid_state_is_idle(&s));

  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s, report);
  for (int i = 0; i < AIRKVM_KEYBOARD_REPORT_LEN; i++) {
    CHECK_EQ_INT(report[i], 0);
  }
}

static void key_down_and_up(void) {
  struct hid_state s;
  hid_state_reset(&s);
  const uint8_t a = keymap_lookup("KeyA");

  hid_state_key_down(&s, a);
  CHECK(!hid_state_is_idle(&s));
  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s, report);
  CHECK_EQ_INT(report[0], 0);  // no modifiers
  CHECK_EQ_INT(report[2], a);

  hid_state_key_up(&s, a);
  CHECK(hid_state_is_idle(&s));
}

static void modifiers_go_in_the_modifier_byte(void) {
  struct hid_state s;
  hid_state_reset(&s);
  hid_state_key_down(&s, keymap_lookup("ControlLeft"));
  hid_state_key_down(&s, keymap_lookup("ShiftLeft"));

  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s, report);
  CHECK_EQ_INT(report[0], 0x03);
  // ...and not in the key array, which is where a modifier ends up if the
  // is-modifier branch is ever dropped.
  CHECK_EQ_INT(report[2], 0);

  hid_state_key_up(&s, keymap_lookup("ControlLeft"));
  hid_state_keyboard_report(&s, report);
  CHECK_EQ_INT(report[0], 0x02);
}

static void repeated_and_spurious_events_are_harmless(void) {
  // The network can duplicate and reorder; neither should corrupt state.
  struct hid_state s;
  hid_state_reset(&s);
  const uint8_t a = keymap_lookup("KeyA");

  hid_state_key_down(&s, a);
  hid_state_key_down(&s, a);
  hid_state_key_down(&s, a);
  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s, report);
  CHECK_EQ_INT(report[2], a);
  CHECK_EQ_INT(report[3], 0);  // occupies exactly one slot

  hid_state_key_up(&s, a);
  hid_state_key_up(&s, a);  // already released
  CHECK(hid_state_is_idle(&s));

  hid_state_key_up(&s, keymap_lookup("KeyZ"));  // never pressed
  CHECK(hid_state_is_idle(&s));
}

static void unknown_usage_is_ignored(void) {
  struct hid_state s;
  hid_state_reset(&s);
  hid_state_key_down(&s, keymap_lookup("NoSuchKey"));  // resolves to 0
  CHECK(hid_state_is_idle(&s));
}

static void report_is_compacted(void) {
  // Releasing a key in the middle must not leave a hole: some targets stop
  // reading the array at the first zero and would lose every later key.
  struct hid_state s;
  hid_state_reset(&s);
  const uint8_t a = keymap_lookup("KeyA");
  const uint8_t b = keymap_lookup("KeyB");
  const uint8_t c = keymap_lookup("KeyC");
  hid_state_key_down(&s, a);
  hid_state_key_down(&s, b);
  hid_state_key_down(&s, c);
  hid_state_key_up(&s, b);

  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s, report);
  CHECK_EQ_INT(report[2], a);
  CHECK_EQ_INT(report[3], c);
  CHECK_EQ_INT(report[4], 0);
}

static void six_keys_is_a_full_valid_report(void) {
  // Six held keys is the boot keyboard's capacity, not an overflow. An earlier
  // version emitted ErrorRollOver (0x01 in every slot) here, which tells the
  // target the report is unreliable at the exact moment it is completely
  // reliable — the six keys would register as nothing at all.
  struct hid_state s;
  hid_state_reset(&s);
  const char *codes[] = {"KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF"};
  for (int i = 0; i < AIRKVM_ROLLOVER_LIMIT; i++) {
    hid_state_key_down(&s, keymap_lookup(codes[i]));
  }
  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s, report);
  for (int i = 0; i < AIRKVM_ROLLOVER_LIMIT; i++) {
    CHECK_EQ_INT(report[2 + i], keymap_lookup(codes[i]));
  }

  // A seventh key is dropped rather than evicting a held one, which would
  // produce a release the console never sent and never will.
  hid_state_key_down(&s, keymap_lookup("KeyG"));
  hid_state_keyboard_report(&s, report);
  for (int i = 0; i < AIRKVM_ROLLOVER_LIMIT; i++) {
    CHECK_EQ_INT(report[2 + i], keymap_lookup(codes[i]));
  }

  // ...and releasing one frees its slot for the next key pressed.
  hid_state_key_up(&s, keymap_lookup("KeyA"));
  hid_state_key_down(&s, keymap_lookup("KeyH"));
  hid_state_keyboard_report(&s, report);
  CHECK_EQ_INT(report[2], keymap_lookup("KeyH"));
  CHECK_EQ_INT(report[3], keymap_lookup("KeyB"));
  CHECK_EQ_INT(report[7], keymap_lookup("KeyF"));
}

static void release_all_is_total(void) {
  // The safety invariant (AGENTS.md §2, principle 4). Everything that can be
  // held is held first, so this fails if a future field escapes the reset.
  struct hid_state s;
  hid_state_reset(&s);
  hid_state_key_down(&s, keymap_lookup("ControlLeft"));
  hid_state_key_down(&s, keymap_lookup("AltLeft"));
  hid_state_key_down(&s, keymap_lookup("KeyA"));
  hid_state_key_down(&s, keymap_lookup("KeyB"));
  hid_state_set_buttons(&s, 0x07);
  CHECK(!hid_state_is_idle(&s));  // positive control: the state really is dirty

  hid_state_release_all(&s);
  CHECK(hid_state_is_idle(&s));

  uint8_t report[AIRKVM_KEYBOARD_REPORT_LEN];
  hid_state_keyboard_report(&s, report);
  for (int i = 0; i < AIRKVM_KEYBOARD_REPORT_LEN; i++) {
    CHECK_EQ_INT(report[i], 0);
  }
  uint8_t mouse[AIRKVM_MOUSE_REPORT_LEN];
  hid_state_mouse_report(&s, 0, 0, 0, 0, mouse);
  for (int i = 0; i < AIRKVM_MOUSE_REPORT_LEN; i++) {
    CHECK_EQ_INT(mouse[i], 0);
  }
}

static void mouse_report_carries_buttons_and_deltas(void) {
  struct hid_state s;
  hid_state_reset(&s);
  hid_state_set_buttons(&s, 0x01);

  uint8_t mouse[AIRKVM_MOUSE_REPORT_LEN];
  hid_state_mouse_report(&s, -2, 3, 1, -1, mouse);
  CHECK_EQ_INT(mouse[0], 0x01);
  CHECK_EQ_INT((int8_t)mouse[1], -2);
  CHECK_EQ_INT((int8_t)mouse[2], 3);
  CHECK_EQ_INT((int8_t)mouse[3], 1);
  CHECK_EQ_INT((int8_t)mouse[4], -1);

  // Movement is transient, not state: the next report with no delta must not
  // repeat the previous movement.
  hid_state_mouse_report(&s, 0, 0, 0, 0, mouse);
  CHECK_EQ_INT(mouse[1], 0);
  CHECK_EQ_INT(mouse[2], 0);
  CHECK_EQ_INT(mouse[0], 0x01);  // ...but the held button persists
}

static void button_mask_ignores_undefined_bits(void) {
  struct hid_state s;
  hid_state_reset(&s);
  hid_state_set_buttons(&s, 0xFF);
  uint8_t mouse[AIRKVM_MOUSE_REPORT_LEN];
  hid_state_mouse_report(&s, 0, 0, 0, 0, mouse);
  CHECK_EQ_INT(mouse[0], 0x07);
}

int main(void) {
  RUN(starts_idle);
  RUN(key_down_and_up);
  RUN(modifiers_go_in_the_modifier_byte);
  RUN(repeated_and_spurious_events_are_harmless);
  RUN(unknown_usage_is_ignored);
  RUN(report_is_compacted);
  RUN(six_keys_is_a_full_valid_report);
  RUN(release_all_is_total);
  RUN(mouse_report_carries_buttons_and_deltas);
  RUN(button_mask_ignores_undefined_bits);
  return test_summary("hid_state");
}
