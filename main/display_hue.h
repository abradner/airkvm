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
// Each hue carries a SECONDARY colour and an ACCENT colour — see the note on
// `hue` in theme.h. Most sections vary only the first; the network section is
// the one that varies both, because its accent IS the signal bar.
//
// Named for the sections ui.c actually uses them for rather than
// HUE_1/HUE_2/HUE_3, because a number would tell the next reader nothing about
// whether it is safe to reuse one somewhere else. display.h names the ink
// roles the same way and for the same reason.
enum display_hue {
  // The theme's plain secondary and accent. Also what every draw gets after a
  // clear, so a section that says nothing is quiet rather than accidentally
  // coloured.
  DISPLAY_HUE_NEUTRAL = 0,
  DISPLAY_HUE_IDENTITY,  // the header: which device this is, and who holds it
  DISPLAY_HUE_ADDRESS,   // the mDNS name — the other way to reach the console

  // The network row, in its three link states. Three hues rather than one
  // because the signal bar is this row's ACCENT, and colouring it is the whole
  // point: the SSID's colour is constant across all three, the bar's is not.
  //
  // Their SECONDARY is deliberately the theme's PLAIN quiet colour rather than
  // a personality tint, and that is a rendering constraint rather than taste.
  // The unfilled steps of the bar are drawn in SECONDARY so the empty part of
  // the scale stays visible, so a bright secondary here makes an empty step
  // read as a full one — three-of-four looked like four-of-four the first time
  // this was rendered. It also earns its keep: with the row's text plain, the
  // bar is the only coloured thing in it, which is exactly the emphasis a
  // gauge wants.
  //
  // They have to be hues rather than a separate mechanism because hue is
  // resolved per 8-row band and the SSID and the bar share a row — so whatever
  // colours the bar has to arrive by the same route that colours the text
  // beside it.
  //
  // WHY THE BAR IS ALLOWED TO CHANGE COLOUR AT ALL, since the rule until now
  // was that the panel does not duplicate the LED's alarm: this is not a
  // second channel saying what the light already says. The light answers
  // "should I be worried" from across a room; the bar answers "how good is the
  // link" at arm's length, and colouring it by the quantity it is ALREADY
  // displaying is redundant coding of one variable rather than a new claim.
  // The threshold is not re-derived either — it is indicator.h's, hysteresis
  // and all, so the two cannot disagree.
  //
  // The line that does hold: the bar never goes red. Red means fault on this
  // device (no WiFi, no HID), and one bar is degraded, not broken. The ramp
  // stops at amber, which is exactly the vocabulary the LED uses for this same
  // condition.
  DISPLAY_HUE_LINK,       // measured and healthy
  DISPLAY_HUE_LINK_WEAK,  // measured and weak enough to explain slowness
  DISPLAY_HUE_LINK_NONE,  // associated, no RSSI sample yet — say nothing

  DISPLAY_HUE_COUNT,
};
