// Per-section hues, in their own header so both sides can have them.
//
// This exists as a separate file for one structural reason: `display.h` pulls
// in `board.h` and `esp_err.h`, and `theme.h` must build on the host with no
// ESP-IDF at all (that is what lets test/host assert every theme's contrast
// without a board). One enum shared by both, and nothing else in here, so
// neither file has to include the other.
#pragma once

// Which of the theme's complementary colours subsequent draws use.
//
// Orthogonal to `enum display_ink`, and the two answer different questions:
// ink says how LOUD an element is (the address is read first, labels are
// context), hue says which colour FAMILY the section belongs to. Ink is the
// hierarchy; hue is the personality.
//
// It applies to SECONDARY text only — see the note on `hue` in theme.h.
//
// Named for the sections ui.c actually uses them for rather than
// HUE_1/HUE_2/HUE_3, because a number would tell the next reader nothing about
// whether it is safe to reuse one somewhere else. display.h names the ink
// roles the same way and for the same reason.
enum display_hue {
  // The theme's plain secondary. Also what every draw gets after a clear, so
  // a section that says nothing is quiet rather than accidentally coloured.
  DISPLAY_HUE_NEUTRAL = 0,
  DISPLAY_HUE_IDENTITY,  // the header: which device this is, and who holds it
  DISPLAY_HUE_ADDRESS,   // the mDNS name — the other way to reach the console
  DISPLAY_HUE_LINK,      // SSID and signal: everything about the radio
  DISPLAY_HUE_COUNT,
};
