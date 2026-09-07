// The status panel's colour scheme, as data rather than as a constant.
//
// == Why this exists ==
//
// The first coloured build hardcoded Solarized Dark, and on the panel it read
// as too low contrast. That was a fair verdict and a measurable one: Solarized
// is tuned for large areas of text in an editor, where low contrast is restful,
// and this is a 33mm status panel read at arm's length or across a desk. The
// number behind the complaint is below — Solarized Dark's SECONDARY role,
// which ui.c draws almost everything in, sat at 2.87:1 against its background.
//
// So a theme is a choice. Which one is compiled in is a Kconfig choice
// (AIRKVM_THEME), matching how the board itself is selected; see the proposal
// note at the bottom of this file for why it is build-time and not the button.
//
// == The four roles ==
//
// canvas.h gives exactly four ink roles, because two 1bpp planes give four
// combinations and a full RGB565 framebuffer does not fit beside mbedTLS
// (display_st7789.c explains that at length). ui.c spends them like this:
//
//   NONE       the background, i.e. every pixel not drawn
//   PRIMARY    the IP address, and nothing else
//   SECONDARY  labels, SSID, mDNS name, the USB/target row, the debug row
//   ACCENT     the RSSI bar, and nothing else
//
// A theme therefore needs a background, a strong text, a quiet text, and one
// accent. Themes with richer palettes have to be *reduced* to that, and doing
// that by eye off a screenshot is how the original went wrong — so each entry
// below names which colour of the source scheme it took and why, and the
// contrast ratio it produces is asserted by test/host/test_theme.c rather than
// asserted in prose.
//
// == Contrast, measured ==
//
// Ratios are WCAG relative luminance, computed on the RGB565-QUANTISED value —
// what the panel can actually show — not on the source hex. Quantisation moves
// them by a few percent and it would be dishonest to quote the unreachable
// number. Text here is unantialiased 8x16 on a 172x320 panel, so WCAG's normal
// -text 4.5:1 is a floor rather than a target.
//
//   theme                  PRIMARY  SECONDARY  ACCENT   PRI-SEC gap
//   tokyo-night             11.15     8.49     10.25      2.66
//   monokai-machine         13.52     7.93      8.43      5.59   (default)
//   one-dark-night-flat      8.47     4.83      7.57      3.64
//   monokai-light           14.43     5.02      3.61      9.41
//   solarized-dark           5.91     5.02      5.00      0.89   (SEC was 2.87)
//
// The per-section hues below are SECONDARY text in another colour, so they are
// held to the same floor and measure:
//
//   theme                IDENTITY  ADDRESS   LINK
//   tokyo-night              7.66     9.69  10.25
//   monokai-machine          6.38     9.35   8.43
//   one-dark-night-flat      6.17     9.01   7.57
//   monokai-light            5.11        -      -   (only one it can carry)
//   solarized-dark              -     4.90   5.00   (no violet that clears)
//
// A dash is the theme's plain secondary, i.e. that section is not coloured at
// all. Two of the five genuinely cannot fill every slot, which is recorded per
// theme below rather than papered over by lowering the floor for decoration.
//
// == Which one is the default, and why it is not the one the ranking picked ==
//
// The rule stated first was "highest weakest ratio", since the complaint was
// that the weakest role was unreadable rather than that the average was low.
// That rule picks tokyo-night: its worst element, 8.49:1, is the strongest
// worst element of the five.
//
// Then the themes were rendered (tools/preview_ui.py), and the picture
// disagreed. Tokyo Night's PRIMARY and SECONDARY are only 2.66 apart, so the
// IP address does not visibly outrank the context around it — the hierarchy
// the four ink roles exist to create is measurably present and visually
// almost absent. monokai-machine has more than twice that separation, the
// address reads as the thing to read, and its weakest element is still 7.93:1
// — 76% above the floor and nearly three times the 2.87:1 that failed.
//
// So the default is monokai-machine, chosen by looking rather than by
// computing. This is AGENTS.md §9 entry 14 exactly: for anything whose
// correctness is visual, the host renderer is the instrument, and it is
// allowed to overrule an a-priori metric. The metric is not discarded — it is
// still the floor every theme must clear (test/host/test_theme.c) — it just
// does not get to pick between themes that all clear it comfortably.
#pragma once

#include <stdint.h>

#include "canvas.h"
#include "display_hue.h"
#include "palette.h"

// Ordered by weakest-ratio-first, which is a computable fact about the table
// and so cannot go stale in the way a taste ordering would. Note the default
// is deliberately NOT the head of this list — see above. Not load-bearing
// either way: the Kconfig choice names a theme, it does not index this.
enum display_theme_id {
  DISPLAY_THEME_TOKYO_NIGHT,
  DISPLAY_THEME_MONOKAI_MACHINE,
  DISPLAY_THEME_ONE_DARK_NIGHT_FLAT,
  DISPLAY_THEME_MONOKAI_LIGHT,
  DISPLAY_THEME_SOLARIZED_DARK,
  DISPLAY_THEME_COUNT,
};

struct display_theme {
  // Kebab-case, matching the Kconfig option and logged at init so a board can
  // say which theme it is running without anyone reading its sdkconfig.
  const char *name;
  // Indexed by canvas ink role, in NATURAL RGB565 order. The panel's byte swap
  // is display_st7789.c's business and is deliberately not baked in here — a
  // theme says what a colour IS, a transport says how that colour is clocked
  // out. See the BE16 note in that file.
  uint16_t ink[4];
  // Per-section colours, indexed by `enum display_hue`. This is what stops the
  // panel being three shades of one colour: the schemes all carry six or so
  // complementary accents and the four ink roles could only ever reach one.
  //
  // Two colours per hue, because a section can hold both quiet text and the
  // one accent element. `secondary` is what almost every section varies;
  // `accent` is the theme's own everywhere EXCEPT the three link states, where
  // it is the signal bar and carries meaning — display_hue.h says why that is
  // allowed and where the ramp stops.
  //
  // [DISPLAY_HUE_NEUTRAL] is ink[SECONDARY]/ink[ACCENT] by construction, so
  // "no hue" needs no sentinel and no branch. A theme with no colour that
  // clears the contrast floor for a given section says so by putting its plain
  // secondary there — see monokai-light and solarized-dark, which genuinely
  // cannot fill every slot and do not pretend to.
  struct {
    uint16_t secondary;
    uint16_t accent;
  } hue[DISPLAY_HUE_COUNT];
};

// static const in a header: every translation unit that includes this gets its
// own copy, and exactly two do (the backend and the host test), so the
// duplication is one table rather than a link-order question. Same shape as
// canvas.h's static inlines, for the same reason — this has to build on the
// host with no ESP-IDF.
static const struct display_theme display_themes[DISPLAY_THEME_COUNT] = {
    // Tokyo Night (enkia.tokyo-night). Its editor.foreground (#a9b1d6) is the
    // SECONDARY here and its brighter variable colour (#c0caf5) the PRIMARY,
    // which is the unusual choice on this list and why it has the highest
    // floor: most schemes put a *comment* grey where SECONDARY goes, and
    // comment greys are exactly what failed. Tokyo Night's own comment
    // (#51597d) is 2.6:1 — Solarized's mistake in a different hue, and
    // deliberately unused.
    // ACCENT is the scheme's cyan rather than its blue: on a 4-cell bar the
    // brighter of the two is the one that still reads at a glance.
    [DISPLAY_THEME_TOKYO_NIGHT] =
        {"tokyo-night",
         {
             [CANVAS_INK_NONE] = RGB565(0x1A, 0x1B, 0x26),
             [CANVAS_INK_PRIMARY] = RGB565(0xC0, 0xCA, 0xF5),
             [CANVAS_INK_SECONDARY] = RGB565(0xA9, 0xB1, 0xD6),
             [CANVAS_INK_ACCENT] = RGB565(0x7D, 0xCF, 0xFF),
         },
         {
             // Purple, green and cyan from the scheme's own accents — 7.66, 9.69
             // and 10.25:1. The widest usable spread on this list: Tokyo
             // Night's accents are all bright enough to clear the floor, so
             // here the choice was aesthetic rather than forced.
             //
             // Signal ramp: the scheme's green, then its amber. No red — see
             // display_hue.h for where the ramp stops and why.
             [DISPLAY_HUE_NEUTRAL] = {RGB565(0xA9, 0xB1, 0xD6),
                                      RGB565(0x7D, 0xCF, 0xFF)},
             [DISPLAY_HUE_IDENTITY] = {RGB565(0xBB, 0x9A, 0xF7),
                                       RGB565(0x7D, 0xCF, 0xFF)},
             [DISPLAY_HUE_ADDRESS] = {RGB565(0x9E, 0xCE, 0x6A),
                                      RGB565(0x7D, 0xCF, 0xFF)},
             [DISPLAY_HUE_LINK] = {RGB565(0xA9, 0xB1, 0xD6),
                                   RGB565(0x9E, 0xCE, 0x6A)},
             [DISPLAY_HUE_LINK_WEAK] = {RGB565(0xA9, 0xB1, 0xD6),
                                        RGB565(0xE0, 0xAF, 0x68)},
             [DISPLAY_HUE_LINK_NONE] = {RGB565(0xA9, 0xB1, 0xD6),
                                        RGB565(0xA9, 0xB1, 0xD6)},
         }},

    // Monokai Pro, Filter Machine. background/text/dimmed1/accent5 straight
    // off the scheme's own named palette, which is the one source on this list
    // that publishes a graded dimmed ramp — so SECONDARY is dimmed1 (#b8c4c3)
    // rather than a guess. dimmed2 (#8b9798) was the tempting pick and lands
    // at 4.54:1, over the floor but with no margin; dimmed1 costs a little
    // hierarchy and buys 3.4 points of legibility.
    [DISPLAY_THEME_MONOKAI_MACHINE] =
        {"monokai-machine",
         {
             [CANVAS_INK_NONE] = RGB565(0x27, 0x31, 0x36),
             [CANVAS_INK_PRIMARY] = RGB565(0xF2, 0xFF, 0xFC),
             [CANVAS_INK_SECONDARY] = RGB565(0xB8, 0xC4, 0xC3),
             [CANVAS_INK_ACCENT] = RGB565(0x7C, 0xD5, 0xF1),
         },
         {
             // accent6, accent4, accent5 — 6.38, 9.35 and 8.43:1. Deliberately
             // NOT accent1 (red) anywhere: red is fault on this device, and no
             // section of a working panel should borrow it.
             //
             // Signal ramp: accent4 green, then accent2 orange for weak. It
             // stops there rather than reaching accent1 — see display_hue.h.
             [DISPLAY_HUE_NEUTRAL] = {RGB565(0xB8, 0xC4, 0xC3),
                                      RGB565(0x7C, 0xD5, 0xF1)},
             [DISPLAY_HUE_IDENTITY] = {RGB565(0xBA, 0xA0, 0xF8),
                                       RGB565(0x7C, 0xD5, 0xF1)},
             [DISPLAY_HUE_ADDRESS] = {RGB565(0xA2, 0xE5, 0x7B),
                                      RGB565(0x7C, 0xD5, 0xF1)},
             [DISPLAY_HUE_LINK] = {RGB565(0xB8, 0xC4, 0xC3),
                                   RGB565(0xA2, 0xE5, 0x7B)},
             [DISPLAY_HUE_LINK_WEAK] = {RGB565(0xB8, 0xC4, 0xC3),
                                        RGB565(0xFF, 0xB2, 0x70)},
             [DISPLAY_HUE_LINK_NONE] = {RGB565(0xB8, 0xC4, 0xC3),
                                        RGB565(0xB8, 0xC4, 0xC3)},
         }},

    // One Dark Pro Night Flat (zhuangtongfa.Material-theme). editor.background
    // and editor.foreground as given. SECONDARY is the scheme's comment grey
    // (#7f848e) and not its line-number grey (#667187), which would be 3.62:1
    // — under the floor, and the sort of near-miss that only shows up as
    // "squint at the SSID". This theme has the lowest SECONDARY of the four
    // dark ones and is kept because it is the one that was asked for; the
    // margin is real but thin.
    [DISPLAY_THEME_ONE_DARK_NIGHT_FLAT] =
        {"one-dark-night-flat",
         {
             [CANVAS_INK_NONE] = RGB565(0x16, 0x19, 0x1D),
             [CANVAS_INK_PRIMARY] = RGB565(0xAB, 0xB2, 0xBF),
             [CANVAS_INK_SECONDARY] = RGB565(0x7F, 0x84, 0x8E),
             [CANVAS_INK_ACCENT] = RGB565(0x61, 0xAF, 0xEF),
         },
         {
             // The scheme's purple, green and blue — 6.17, 9.01 and 7.57:1.
             //
             // Signal ramp: its green, then its yellow. That yellow is the
             // brightest colour the scheme owns (10.71:1) and went unused
             // until there was something for it to actually mean.
             [DISPLAY_HUE_NEUTRAL] = {RGB565(0x7F, 0x84, 0x8E),
                                      RGB565(0x61, 0xAF, 0xEF)},
             [DISPLAY_HUE_IDENTITY] = {RGB565(0xC6, 0x78, 0xDD),
                                       RGB565(0x61, 0xAF, 0xEF)},
             [DISPLAY_HUE_ADDRESS] = {RGB565(0x98, 0xC3, 0x79),
                                      RGB565(0x61, 0xAF, 0xEF)},
             [DISPLAY_HUE_LINK] = {RGB565(0x7F, 0x84, 0x8E),
                                   RGB565(0x98, 0xC3, 0x79)},
             [DISPLAY_HUE_LINK_WEAK] = {RGB565(0x7F, 0x84, 0x8E),
                                        RGB565(0xE5, 0xC0, 0x7B)},
             [DISPLAY_HUE_LINK_NONE] = {RGB565(0x7F, 0x84, 0x8E),
                                        RGB565(0x7F, 0x84, 0x8E)},
         }},

    // Monokai Pro Light. The only light theme, and the only one where the
    // background is the LIT state: on this panel every undrawn pixel is
    // ink role 0, so choosing this puts a 172x320 white rectangle on the desk.
    // That is a room decision, not a legibility one — it is the most readable
    // theme here in daylight and the worst one in a dark rack.
    //
    // ACCENT is the scheme's blue (#1c8ca8) at 3.61:1, the lowest ACCENT on
    // this list. That is inherent: an accent has to be darker than a light
    // background to show at all, and the saturated blues that read as "signal"
    // are mid-luminance. It clears the 3:1 that WCAG asks of a non-text
    // element, which the bar is, and it is the only role here that leans on
    // that lower floor.
    [DISPLAY_THEME_MONOKAI_LIGHT] =
        {"monokai-light",
         {
             [CANVAS_INK_NONE] = RGB565(0xFA, 0xF4, 0xF2),
             [CANVAS_INK_PRIMARY] = RGB565(0x29, 0x24, 0x2A),
             [CANVAS_INK_SECONDARY] = RGB565(0x70, 0x6B, 0x6E),
             [CANVAS_INK_ACCENT] = RGB565(0x1C, 0x8C, 0xA8),
         },
         {
             // This theme CANNOT carry the full set, and that is a fact about
             // light backgrounds rather than a gap to fill. A hue has to be
             // darker than its background to read, and saturated hues sit at
             // mid luminance, so against #faf4f2 the scheme's own accents
             // measure green 3.23, blue 3.61, red 3.62, orange 3.23 and
             // yellow 3.13 — every one under the 4.5:1 floor the rest of this
             // file exists to enforce. (Monokai Pro Light does ship those
             // colours on that background; the editor is simply not holding
             // itself to WCAG AA, and a 33mm panel is not an editor.)
             //
             // Only accent6, purple, clears it, at 5.11:1. So this theme takes
             // the one hue it can carry and leaves the rest plain. It is the
             // quietest of the five on purpose: lowering the floor to decorate
             // it would invert the point of the whole change.
             //
             // The signal bar is the exception, and a principled one. It is a
             // GRAPHICAL element, which WCAG holds to 3:1 rather than 4.5, and
             // it is the one place on this panel where colour carries meaning
             // rather than personality. So the ramp runs at the scheme's own
             // green (3.23) and yellow (3.13), both clear of 3:1 — and the
             // bar's SHAPE carries the same information for anyone the colour
             // fails, which is why leaning on the lower floor is defensible
             // here and would not be for a line of text.
             [DISPLAY_HUE_NEUTRAL] = {RGB565(0x70, 0x6B, 0x6E),
                                      RGB565(0x1C, 0x8C, 0xA8)},
             [DISPLAY_HUE_IDENTITY] = {RGB565(0x70, 0x58, 0xBE),
                                       RGB565(0x1C, 0x8C, 0xA8)},
             [DISPLAY_HUE_ADDRESS] = {RGB565(0x70, 0x6B, 0x6E),
                                      RGB565(0x1C, 0x8C, 0xA8)},
             [DISPLAY_HUE_LINK] = {RGB565(0x70, 0x6B, 0x6E),
                                   RGB565(0x26, 0x9D, 0x69)},
             [DISPLAY_HUE_LINK_WEAK] = {RGB565(0x70, 0x6B, 0x6E),
                                        RGB565(0xCC, 0x7A, 0x0A)},
             [DISPLAY_HUE_LINK_NONE] = {RGB565(0x70, 0x6B, 0x6E),
                                        RGB565(0x70, 0x6B, 0x6E)},
         }},

    // Solarized Dark — what shipped, kept so the comparison is honest and so
    // anyone who liked it can have it back.
    //
    // ONE role is changed, and it is the fix rather than a liberty: SECONDARY
    // was base01, which Solarized defines as the *comment* tone, at 2.87:1.
    // ui.c draws labels, the SSID, the mDNS name and the target row in
    // SECONDARY — content, not comments — and Solarized's own role for body
    // text on a dark background is base0. So the original was a role
    // misassignment, not a colour problem, and correcting it to base0 (#839496)
    // takes SECONDARY from 2.87:1 to 5.02:1 without leaving the scheme.
    //
    // PRIMARY stays base1, which is the brightest foreground Solarized Dark
    // defines; there is no in-scheme way to push it past 5.91:1. Solarized is
    // consequently still the flattest theme here — its PRIMARY and SECONDARY
    // are 0.89 apart where tokyo-night's are 2.66 — which is a property of the
    // scheme, and is why it is no longer the default.
    [DISPLAY_THEME_SOLARIZED_DARK] =
        {"solarized-dark",
         {
             [CANVAS_INK_NONE] = RGB565_OF(SOL_BASE03),
             [CANVAS_INK_PRIMARY] = RGB565_OF(SOL_BASE1),
             [CANVAS_INK_SECONDARY] = RGB565_OF(SOL_BASE0),
             [CANVAS_INK_ACCENT] = RGB565_OF(SOL_CYAN),
         },
         {
             // Two of three, for the same honest reason as monokai-light above.
             // Solarized's green (4.90:1) and cyan (5.00:1) clear the floor;
             // its violet is 3.56 and its magenta 3.47, so IDENTITY stays
             // plain.
             //
             // Be warned that the green is barely green: #859900 is
             // (133,153,0), red and green almost equal with no blue at all,
             // which is the exact fact palette.h records about why it reads
             // YELLOW on an LED. As text on a dark panel it reads olive rather
             // than amber, and a hostname is not a row anyone scans for alarm,
             // so it stays on the mDNS row.
             //
             // On the SIGNAL BAR the same colour would be a real problem,
             // because that row IS scanned for alarm and olive sitting next to
             // amber is not a ramp anyone can read. So this theme's healthy
             // bar stays cyan — its accent, exactly as before this feature —
             // and only the weak state moves, to yellow. A two-colour ramp
             // that reads is better than a three-colour one that does not.
             [DISPLAY_HUE_NEUTRAL] = {RGB565_OF(SOL_BASE0),
                                      RGB565_OF(SOL_CYAN)},
             [DISPLAY_HUE_IDENTITY] = {RGB565_OF(SOL_BASE0),
                                       RGB565_OF(SOL_CYAN)},
             [DISPLAY_HUE_ADDRESS] = {RGB565_OF(SOL_GREEN),
                                      RGB565_OF(SOL_CYAN)},
             [DISPLAY_HUE_LINK] = {RGB565_OF(SOL_BASE0), RGB565_OF(SOL_CYAN)},
             [DISPLAY_HUE_LINK_WEAK] = {RGB565_OF(SOL_BASE0),
                                        RGB565_OF(SOL_YELLOW)},
             [DISPLAY_HUE_LINK_NONE] = {RGB565_OF(SOL_BASE0),
                                        RGB565_OF(SOL_BASE0)},
         }},
};

// Which theme this image is built with.
//
// The final #else is for the host test, which compiles this header with no
// sdkconfig.h at all — and for the 0.42" OLED build, where the Kconfig choice
// is deliberately not offered because that panel is 1bpp and has no palette to
// theme. Neither is a silently-degraded required setting (AGENTS.md §8): the
// Kconfig choice has a `default`, so on the board that has a palette one of
// these is always defined.
#if defined(CONFIG_AIRKVM_THEME_TOKYO_NIGHT)
#define DISPLAY_THEME_SELECTED DISPLAY_THEME_TOKYO_NIGHT
#elif defined(CONFIG_AIRKVM_THEME_MONOKAI_MACHINE)
#define DISPLAY_THEME_SELECTED DISPLAY_THEME_MONOKAI_MACHINE
#elif defined(CONFIG_AIRKVM_THEME_ONE_DARK_NIGHT_FLAT)
#define DISPLAY_THEME_SELECTED DISPLAY_THEME_ONE_DARK_NIGHT_FLAT
#elif defined(CONFIG_AIRKVM_THEME_MONOKAI_LIGHT)
#define DISPLAY_THEME_SELECTED DISPLAY_THEME_MONOKAI_LIGHT
#elif defined(CONFIG_AIRKVM_THEME_SOLARIZED_DARK)
#define DISPLAY_THEME_SELECTED DISPLAY_THEME_SOLARIZED_DARK
#else
#define DISPLAY_THEME_SELECTED DISPLAY_THEME_MONOKAI_MACHINE
#endif

// PROPOSAL, per AGENTS.md §8's no-stated-convention rule. There is no stated
// convention for how a display preference is selected on this device, and this
// change picks one: BUILD TIME, via Kconfig, the same axis the board itself is
// chosen on. Codify or correct.
//
// The alternative considered and rejected for now is runtime selection, and
// the reason is not cost — it is the button. The BOOT button already does two
// things whose meanings are disambiguated by what is on screen at the moment
// of the press, and R-006 (docs/repo-map.md) leans on that press being latched
// at press time so a client HID event cannot retarget it. A third meaning is
// not a third `if`; it is a re-argument of a security mitigation. A console
// control avoids the button entirely but costs a protocol frame, a UI, and NVS
// persistence — real surface for a preference nobody has yet said they want to
// change without a rebuild.
//
// What this change deliberately does buy is the option: the table above is
// data, every theme is present in the image, and the backend reads a selected
// entry rather than a compile-time constant. Making the selection runtime is
// changing where the index comes from, not restructuring the colour path.
