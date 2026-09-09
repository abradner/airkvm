#include "hold_button.h"

#include <string.h>

void hold_button_init(struct hold_button *b) {
  memset(b, 0, sizeof(*b));
}

enum hold_button_event hold_button_step(struct hold_button *b, bool pressed,
                                        uint32_t now_ms, uint8_t *fraction) {
  if (pressed && !b->pressed) {
    // Press edge.
    b->pressed = true;
    b->feedback = false;
    b->armed = false;
    b->pressed_at_ms = now_ms;
    return HOLD_BUTTON_NONE;
  }
  if (!pressed && b->pressed) {
    // Release edge. What it means depends on how far the hold got.
    b->pressed = false;
    if (b->armed) {
      return HOLD_BUTTON_FIRE;
    }
    return b->feedback ? HOLD_BUTTON_CANCEL : HOLD_BUTTON_SHORT_RELEASE;
  }
  if (!pressed) {
    return HOLD_BUTTON_NONE;
  }
  const uint32_t held = now_ms - b->pressed_at_ms;
  if (held >= HOLD_BUTTON_FIRE_MS) {
    b->armed = true;
    return HOLD_BUTTON_ARMED;
  }
  if (held >= HOLD_BUTTON_FEEDBACK_MS) {
    b->feedback = true;
    if (fraction) {
      const uint32_t span = HOLD_BUTTON_FIRE_MS - HOLD_BUTTON_FEEDBACK_MS;
      *fraction = (uint8_t)(((held - HOLD_BUTTON_FEEDBACK_MS) * 255u) / span);
    }
    return HOLD_BUTTON_FEEDBACK;
  }
  return HOLD_BUTTON_NONE;
}
