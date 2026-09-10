#include "hid_state.h"

#include <string.h>

#include "keymap.h"

void hid_state_reset(struct hid_state *s) {
  memset(s, 0, sizeof(*s));
}

void hid_state_release_all(struct hid_state *s) {
  memset(s, 0, sizeof(*s));
}

bool hid_state_is_idle(const struct hid_state *s) {
  if (s->modifiers != 0 || s->buttons != 0) {
    return false;
  }
  for (int i = 0; i < AIRKVM_ROLLOVER_LIMIT; i++) {
    if (s->keys[i] != 0) {
      return false;
    }
  }
  return true;
}

void hid_state_key_down(struct hid_state *s, uint8_t usage) {
  if (usage == 0) {
    return;
  }
  if (keymap_is_modifier(usage)) {
    s->modifiers |= keymap_modifier_bit(usage);
    return;
  }
  int free_slot = -1;
  for (int i = 0; i < AIRKVM_ROLLOVER_LIMIT; i++) {
    if (s->keys[i] == usage) {
      return;  // already held; repeats are the host's job, not ours
    }
    if (s->keys[i] == 0 && free_slot < 0) {
      free_slot = i;
    }
  }
  if (free_slot >= 0) {
    s->keys[free_slot] = usage;
  }
  // No free slot: drop it. The seventh simultaneous key is not a real scenario
  // for a remote console, and dropping is safer than evicting a held key, which
  // would produce a key-up the console never sent and never sends again.
}

void hid_state_key_up(struct hid_state *s, uint8_t usage) {
  if (usage == 0) {
    return;
  }
  if (keymap_is_modifier(usage)) {
    s->modifiers &= (uint8_t)~keymap_modifier_bit(usage);
    return;
  }
  for (int i = 0; i < AIRKVM_ROLLOVER_LIMIT; i++) {
    if (s->keys[i] == usage) {
      s->keys[i] = 0;
      return;
    }
  }
}

void hid_state_set_buttons(struct hid_state *s, uint8_t mask) {
  s->buttons = mask & 0x07;
}

void hid_state_keyboard_report(const struct hid_state *s,
                               uint8_t out[AIRKVM_KEYBOARD_REPORT_LEN]) {
  out[0] = s->modifiers;
  out[1] = 0;  // reserved by the HID boot keyboard spec

  // No ErrorRollOver is ever emitted, deliberately. Six held keys is a *full
  // and valid* boot-keyboard report, not an overflow — reporting 0x01 in every
  // slot there would tell the target "this report is unreliable" at the one
  // moment it is completely reliable, and none of the six would register. The
  // seventh key is dropped at key_down time instead (see above), so the report
  // always describes exactly what the target should believe is held.
  //
  // Compacted: freeing a slot in the middle must not leave a gap, because some
  // targets stop reading the array at the first zero.
  int w = 2;
  for (int i = 0; i < AIRKVM_ROLLOVER_LIMIT; i++) {
    if (s->keys[i] != 0) {
      out[w++] = s->keys[i];
    }
  }
  while (w < AIRKVM_KEYBOARD_REPORT_LEN) {
    out[w++] = 0;
  }
}

void hid_state_mouse_report(const struct hid_state *s, int8_t dx, int8_t dy,
                            int8_t scroll_v, int8_t scroll_h,
                            uint8_t out[AIRKVM_MOUSE_REPORT_LEN]) {
  out[0] = s->buttons;
  out[1] = (uint8_t)dx;
  out[2] = (uint8_t)dy;
  out[3] = (uint8_t)scroll_v;
  out[4] = (uint8_t)scroll_h;
}
