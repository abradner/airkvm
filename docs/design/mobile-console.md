# Mobile console

> **Status: implemented, on the existing page** (branch
> `feature/airkvm-mobile-trackpad-keyboard-da2454`, closes issue #11). The layout and every
> mechanism below are built and were exercised once on an iPhone against a real target (see
> "Measured on hardware"); the gesture constants were used unchanged in that run and are
> usable but not tuned (#59), and the iOS-specific behaviours still open are listed under
> "Unverified". The "Why" and the four problems are kept as written, because they are the
> reasons the code is shaped the way it is.

## Why

Controlling a TV-attached computer from a phone is a primary use case, and before this work **the
console was completely non-functional for input on a phone.** Not degraded — non-functional. That
is worth stating precisely, because the issue title ("soft keyboards cannot drive the
position-based keymap") describes only one of four independent reasons.

1. **Capture never engaged.** Entering capture required Pointer Lock, which iOS Safari does not
   implement. Every keyboard and mouse handler began `if (!captured) return;` and `captured` was
   set only from `pointerlockchange`. So all of them early-returned, forever.
2. **There were no touch handlers at all.** Motion came from `mousemove` + `movementX/Y`.
3. **A soft keyboard produces no `event.code`.** It reports `""`/`Unidentified`, and `event.code`
   is exactly what the wire protocol carries. This is the part the issue names.
4. **The layout assumed a mouse and a big screen.** `.stage` was locked to `aspect-ratio: 16/9`,
   wasting most of a portrait viewport on a black box; tap targets were ~0.4rem padding, well
   under the 44px minimum; there was exactly one media query in the whole stylesheet.

The parts that *did* work on a phone were the sidebar buttons and the type-text box, because they
called `sendKey` directly without consulting `captured`.

## What was reused rather than rebuilt

**`CHAR_MAP` (`web/app.js`) is the character → (`code`, shift) table.** It covers a–z, A–Z, 0–9,
and the full US punctuation set including shifted forms. The paste-box handler already walked a
string through it, wrapped shifted characters in `ShiftLeft` down/up, refused unmapped characters
wholesale rather than typing half of them, and paced at 8 ms. That path is now `pressKey` /
`typeChar` / `typeString`, and the soft keyboard calls the same functions — there is one table.

Likewise `data-combo` is the mechanism for on-screen keys: a `+`-joined list of `event.code`
values, pressed in order and released in reverse. The edit, navigation and F-key rows are more of
these. Modifiers are the one new attribute, `data-mod`, because they need to latch (below).

## Decisions

| Question | Decision | Why |
|---|---|---|
| One page or two | **One page.** `body.soft` switches the CSS; `main`/`.panel` become `display: contents` so sections can be re-ordered as flex siblings of the header. | No new embedded asset, no new route, one release-all code path. |
| Choosing the mode | `soft` iff `!("requestPointerLock" in Element.prototype) \|\| matchMedia("(pointer: coarse)").matches`; `?mode=direct\|soft` overrides. | Feature detection catches iOS/iPadOS Safari (no Pointer Lock at all, so `direct` could never capture there); the coarse-pointer clause catches Android Chrome, which exposes `requestPointerLock` on a touchscreen. |
| Structural exclusion | The direct listeners live in `installDirectInput()`, the soft ones in `installSoftInput()`; exactly one runs, before `connect()`. | The inactive path has no listeners, so it cannot forward a keystroke; a flag someone could forget to check is not the mechanism. |
| Touch events | Pointer Events for the logic; `touchstart`/`touchmove` listeners that only `preventDefault()`. | Pointer Events give per-finger ids (iOS 13+) and unify an iPad trackpad; a cancelled `touchmove` is what reliably stops elastic overscroll and pinch-zoom on iOS. |
| Keyboard element | A visually hidden `<textarea rows=1>`, not an `<input>`. | Return in a single-line input is a submit gesture and fires no `beforeinput`; in a textarea it arrives as `insertLineBreak`. |
| Character delivery | `beforeinput` with `preventDefault()`; the `input` handler only restores the box and reports leaks. | `beforeinput` says what happened (`insertText`, `deleteContentBackward`, `insertLineBreak`) instead of making the page infer it from a value diff, and cancelling it keeps the box fixed. The `input` handler never sends, so a browser that ignores the cancel can leave residue but cannot type twice. |
| The box's contents | Always exactly one zero-width space, caret after it. | Backspace on an empty field fires no `beforeinput`; a space sentinel would trip iOS's double-space-to-period rewrite and type a Backspace on the target. |
| Tap → click | `BUTTON 1`, dwell `TAP_CLICK_HOLD_MS`, `BUTTON 0`. | The dongle's report task drains every queued intent before emitting, and only keyboard intents are guarded against being applied on top of an unreported state (`main/hid_out_usb.c`, AGENTS.md §9 entry 19, issue #58). Two button frames inside the mouse endpoint's busy interval collapse into one report with nothing pressed. A human click never hit that. |
| Scroll direction | Natural (finger down → content down), behind `SCROLL_NATURAL`. | It is how the phone itself scrolls; the constant is the two-way door. |
| What the phone hides | Video and the stage, and the disk image. Kept: macros, target/wake, sensitivity (doubles as trackpad gain), the paste box — the last three folded under "More". | A phone has no capture dongle; presenting boot media from a phone is a separate proposal. |

## The four problems, as solved

### 1. Getting a `code` out of a soft keyboard

The hidden textarea holds focus, which is what makes the OS keyboard appear (only from a user
gesture on iOS: the "Keyboard" button). `beforeinput` is cancelled and its `inputType` dispatched:
`insertText` → `typeChar` per character; `insertLineBreak`/`insertParagraph` → Enter;
`deleteContentBackward` → Backspace; `deleteContentForward` → Delete; `insertFromPaste` →
`typeString` (paced, refused whole if any character is unmapped); anything else is reported in
the status line by name rather than guessed at. `compositionstart`/`compositionend` gate all of
it: nothing is sent while an IME composes, and the committed text goes through `typeString`.

**`event.key` is never used** (AGENTS.md §4): it is layout- and modifier-shifted. Characters go
through `CHAR_MAP` to physical positions, which is a US-layout-on-the-target assumption, and the
page says so next to the Keyboard button.

Autocorrect, autocapitalise and predictive text are off on the element
(`autocorrect="off" autocapitalize="none" autocomplete="off" spellcheck="false"`). If one gets
through anyway it arrives as `insertReplacementText`, which is the "reported, not typed" branch.

### 2. Not double-typing

One mode per page load; the other mode's listeners are never registered. See the decisions
table. The `input` handler is the second line: it never sends.

### 3. Latching modifiers

Tap a modifier → `KEY_DOWN`, highlighted. The next non-modifier key — on-screen or typed — sends
its own down/up, then every latched-but-not-locked modifier is released in reverse order. Tap a
latched modifier again → released. Long-press (`LATCH_LOCK_MS`) → locked until tapped.

The latch is a *view* of what the page believes the dongle holds, and there is no way to read
that back, so every release-all — the button, a socket close, the page going hidden — clears it
without sending anything. It is never restored across a reconnect: that would re-press keys the
dongle deliberately released. A latched Shift is not wrapped a second time by `pressKey`.

### 4. The session dies when the phone backgrounds — by design

The console pings once a second and the device tears the session down after ~3 s of silence
(`main/net.c`, `SESSION_TIMEOUT_MS`). A backgrounded mobile browser throttles timers, so
switching apps, taking a call or locking the screen ends the session. That is principle 4
working: the device cannot distinguish "phone in a pocket" from "operator gone", and it prefers a
spurious release-all to a held key.

What the page does about it: going hidden aborts any gesture in progress and sends release-all
itself rather than making the dongle wait out the deadline; coming back sends one frame on an
open socket (the dongle re-claims a session on any frame from the owner) or reconnects at once on
a closed one, and the status line says which.

## What the dongle does and does not say back

An earlier version of this document said the protocol was one-directional. It no longer is: the
dongle answers `WAKE` with `WAKE_RESULT` (0x80) and `STATUS_REQ` with `STATUS` (0x81)
(`main/protocol.h`). It still sends no keystroke acknowledgement, no notice that a session is the
spectator rather than the owner (R-003), and no caps-lock or modifier read-back. So every
indicator on the phone — the latch highlights, the dragging border — is a claim about what was
*sent*, and is worded that way. Changing that is a protocol change and a separate proposal.

## Constants that are guesses

All in one block at the top of `installSoftInput()` in `web/app.js`, each marked as unmeasured
(principle 6). Tune them on a phone against a target with `tools/hid_listen.py --mouse` running,
and record what was measured here (issue #59).

| Constant | Start | What it does |
|---|---|---|
| `TOUCH.GAIN` | 2 | finger px → mouse counts, times the sensitivity slider |
| `TOUCH.TAP_MAX_MS` | 250 | down→up faster than this with little travel is a tap |
| `TOUCH.TAP_SLOP_PX` | 10 | travel allowed inside a tap |
| `TOUCH.LONG_PRESS_MS` | 400 | one still finger this long holds the left button |
| `TOUCH.TAP_CLICK_HOLD_MS` | 40 | dwell between the two button frames of a click |
| `TOUCH.SCROLL_PX_PER_NOTCH` | 20 | finger travel per wheel notch |
| `TOUCH.SCROLL_MAX_PER_FLUSH` | 3 | notches per animation frame; the rest carries |
| `TOUCH.SCROLL_NATURAL` | true | finger down → content down |
| `LATCH_LOCK_MS` | 500 | long-press that locks a modifier |

Latency, stated rather than measured: a tap's click goes out when the finger lifts (no timer
waits for it) and is held for `TAP_CLICK_HOLD_MS`; a second tap inside that dwell waits it out; a drag costs `LONG_PRESS_MS` before the button goes down; motion
and scroll are coalesced per animation frame exactly as the desktop path already is; typed
characters go out on `beforeinput` with no pacing, paste at the existing 8 ms per character.

## Measured on hardware, 2026-09-08

iPhone (iOS Safari) → dongle → Raspberry Pi target, read back with `tools/hid_listen.py` and a
raw evdev transcript. Every line below is an observation, not a claim:

- **Typing:** `hello, World! 123` + Return arrived exactly (18 of 18, `--expect` match), so
  `beforeinput` fires for `insertText`, the Shift wrap and `insertLineBreak` on this iOS.
- **Trackpad:** one tap → `BTN_LEFT` down/up, seen 15 ms apart at the target although the page
  sends them `TAP_CLICK_HOLD_MS` = 40 ms apart — the difference is WiFi and endpoint jitter
  between two independently delivered frames, worth knowing when reading these numbers; a 5 cm drag → 43 axis records (the tool then counted `REL_X`/`REL_Y`
  records, not `SYN_REPORT` boundaries, so a diagonal step counted twice; it now counts reports),
  dx +451; two-finger 5 cm drag → six `REL_WHEEL +1` events over 0.5 s (natural direction);
  two-finger tap → `BTN_RIGHT` down/up. Presses and releases balanced (2/2).
- **Latch:** tap Ctrl, type `c` → Ctrl down, c down/up, Ctrl up 10 ms later. Long-press Shift,
  type `abc`, tap Shift → Shift held across all three, released on the tap.
- **Backgrounding:** Ctrl latched, phone locked → Ctrl released 4.9 s after the latch tap, about
  3 s after the lock — the dongle's deadline, not the page's own hidden-path release-all, which
  this trace cannot distinguish (whether iOS delivers `visibilitychange` before suspending the
  socket is the open question; #59). The key was released in every case.
- **Layout:** the first render scrolled the key rows sideways and hid Enter and Space off the
  edge; replaced with wrapped clusters (inverted-T arrows, a 3×2 nav block, F keys 6×2).
  Trackpad gestures did not scroll or zoom the page; the keyboard opened from the button.
- **Long-press quirk:** a long press on a modifier raised iOS text-selection handles over the
  label; the lock itself still engaged. `-webkit-user-select: none`, even page-wide, did not
  stop it; cancelling `touchstart` on the modifier buttons did (safe there because modifiers run
  on pointer events, never on click). Confirmed gone on the phone the same evening.
- **"More" overflow:** opening the folded section on the phone pushed its contents below a
  body that could not scroll, and flex shrinking crushed the macro row to make room. Fixed: the
  fixed-height sections no longer shrink, and "More" opens in place of the trackpad and keys,
  scrolling in that space with the header, status and Release-all still above it. Page scrolling was tried first and failed: a fixed, transformed body does
  not scroll on iOS, and every drag became Safari's pull-to-refresh. Touch moves outside the
  sheet and the text boxes are now cancelled so pull-to-refresh cannot fire on the page.
- **Desktop path (Mac, pointer lock):** `desktop ok` typed exactly, motion, one click and the
  trackpad's inertial wheel burst all arrived as before; the header read "pointer lock" and the
  operator reported the layout as unchanged (the desktop sidebar now sits inside a `<details>`
  styled as a flex column, which is what that check covers).
- **Macro row:** was a sideways-scrolling row; with the page cancelling `touchmove`, its tail
  was unreachable. Now a wrapping grid. "More" opens below the Release-all bar and macros, so the
  status line, where its own buttons report, stays visible.

The gesture constants above were used unchanged and felt usable in this session; they are still
starting values, not tuned (#59).

## Unverified on a real phone

Each of these is a hypothesis until a phone says otherwise, and the code comments say so at the
point where it matters:

- That cancelling `pointerdown` on the on-screen keys keeps the phone's keyboard open in every
  case (the latch test typed after tapping a modifier, so it held at least there). The fallback,
  if it does not, is `touchend` + `preventDefault()` with the action invoked from there.
- How iOS Safari moves a `position: fixed` body when the keyboard opens; the page sizes itself
  from `visualViewport` on resize only (a scroll handler that forced the window to the top
  fought the sheet's own scrolling and was removed).
- `beforeinput` firing for Backspace against the zero-width-space sentinel on iOS (not
  exercised in the session above).
- Android Chrome, entirely.

## Verification

- `tools/hid_listen.py --expect "…"` on the target for typed text; `tools/hid_listen.py --mouse`
  for clicks, drags and wheel notches — so "typed on the phone, arrived on the target" is a
  machine check, and a button that goes down and never comes up is an exit code, not a thing
  someone has to notice.
- The desktop path in both Chrome and Safari must behave as it did: `git diff -w --color-moved`
  over the first commit of this work shows its listener bodies moved into a function, not
  changed.
- `?mode=soft` on a desktop browser shows the phone UI for layout work without a phone.
