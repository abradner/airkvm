# Mobile console — design proposal

> **Status: proposal, nothing built.** Written for review under AGENTS.md §8, which requires that
> a new UI shape be proposed rather than invented in passing. Every behaviour described here is
> intent, not proof. Tracks issue #11.

## Why

Controlling a TV-attached computer from a phone is a primary use case, and today **the console is
completely non-functional for input on a phone.** Not degraded — non-functional. That is worth
stating precisely, because the issue title ("soft keyboards cannot drive the position-based
keymap") describes only one of four independent reasons.

1. **Capture never engages.** Entering capture requires Pointer Lock (`web/app.js:117-144`), which
   iOS Safari does not implement. Every keyboard and mouse handler begins `if (!captured) return;`
   and `captured` is set only from `pointerlockchange`. So all of them early-return, forever.
2. **There are no touch handlers at all.** Motion comes from `mousemove` + `movementX/Y`
   (`app.js:214-223`). Zero `touchstart`/`touchmove`/`pointerdown` listeners exist.
3. **A soft keyboard produces no `event.code`.** It reports `""`/`Unidentified`, and `event.code`
   is exactly what the wire protocol carries. This is the part the issue names.
4. **The layout assumes a mouse and a big screen.** `.stage` is locked to `aspect-ratio: 16/9`
   (`style.css:73`), wasting most of a portrait viewport on a black box; tap targets are ~0.4rem
   padding, well under the 44px minimum; there is exactly one media query in the whole stylesheet
   (`style.css:65-69`).

The parts that *do* work on a phone today are the sidebar buttons and the type-text box, because
they call `sendKey` directly without consulting `captured`.

## What already exists and must not be rebuilt

**`CHAR_MAP` (`web/app.js:271-297`) is the character → (`code`, shift) table this design needs.**
It covers a–z, A–Z, 0–9, and the full US punctuation set including shifted forms.
`el.sendText` (`app.js:299-320`) already walks a string through it, wraps shifted characters in
`ShiftLeft` down/up, refuses unmapped characters wholesale rather than typing half of them, and
paces at 8ms. Roughly the entire character path exists. Anyone implementing this should lift it
out of the send-text handler and share it — **not write a second table.**

Likewise `data-combo` (`index.html:39-45`, `app.js:254-264`) is already the mechanism for
on-screen keys: a `+`-joined list of `event.code` values, pressed in order and released in
reverse. The F-key and special-key rows are more of these, not a new concept.

## Layout

Single screen, portrait, no video. Bottom-to-top, because the thumb is at the bottom and the
things you touch most should be nearest it:

```
┌─────────────────────────────┐
│ toolbar: status · release   │  ← connection state, RELEASE ALL
├─────────────────────────────┤
│ macros: Ctrl-Alt-Del  …     │  ← existing data-combo buttons
├─────────────────────────────┤
│                             │
│      trackpad surface       │  ← touch: move, tap, two-finger scroll
│                             │
├─────────────────────────────┤
│ Esc F1..F12 ↑↓←→ Tab …      │  ← horizontally scrollable key rows
├─────────────────────────────┤
│ [ hidden input — soft kbd ] │  ← focus target; OS keyboard opens over this
└─────────────────────────────┘
```

Video is omitted rather than hidden: `<video>` has no source until "Show capture" is pressed, so
the default state is already no-video. **A mobile layout is a subtraction, not a new mode.**

## The four problems this design has to solve

### 1. Getting a `code` out of a soft keyboard

A hidden `<input>` holds focus, which is what makes the OS keyboard appear. Its value is diffed on
each `input` event and the delta is mapped through the existing `CHAR_MAP` to `code` + shift.

**`event.key` must never be used** (AGENTS.md §4): it is layout- and modifier-shifted, and sending
it would type the wrong key on any non-US layout. The diff approach keeps us on characters, which
`CHAR_MAP` already converts to physical positions.

Consequences to design for, not discover later:

- **Autocorrect, autocapitalise, and predictive text must be off** —
  `autocorrect="off" autocapitalize="none" autocomplete="off" spellcheck="false"` — or the browser
  will rewrite what the operator typed *after* we have sent it.
- **Backspace is a deletion in the diff, not a character.** It maps to `Backspace`.
- **IME composition must be ignored until committed** (`compositionstart`/`compositionend`), or
  every intermediate candidate gets typed.
- The input's value must be reset periodically, or the diff grows unbounded.

### 2. Not double-typing

The position path (`keydown`/`keyup` on `document`) and the character path must never both be
live. Today both handlers are unconditional and gated only on `captured`.

Proposal: **one explicit input mode**, not two overlapping ones —
`direct` (a real keyboard, position codes, requires Pointer Lock) or `soft` (hidden input,
character diffing). The mobile UI is always `soft`. Mode is chosen once and the inactive path's
listeners are removed, not merely skipped, so the exclusion is structural rather than a flag
someone can forget to check.

### 3. Latching modifiers

You cannot hold Ctrl and tap C with one finger. `data-combo` fires press+release with no dwell,
so it cannot express "Ctrl is down while I press something else".

Proposal: modifier keys latch. Tap `Ctrl` → it highlights and a `KEY_DOWN` goes out. The next
non-modifier key press sends its own down/up, then all latched modifiers are released. Tapping a
latched modifier again clears it. Long-press locks it until tapped again (for drag operations).

**This interacts with release-all and must be got right:** any release-all — from the toolbar
button, a session teardown, or a reconnect — must clear the latch UI too, or the screen will show
Ctrl held while the device believes nothing is. The latch is a *view* of device state, and there
is no way to read that state back (see below), so the UI must treat every release-all as
authoritative and reset itself.

### 4. The session dies when the phone backgrounds — by design

`app.js:56-69` sends a PING every 1000ms, and the device tears the session down after ~3s of
silence (`net.c`, `SESSION_TIMEOUT_MS`). **A backgrounded mobile browser throttles `setInterval`
to roughly once per minute.** So switching apps, taking a call, or locking the screen ends the
session, and `window.addEventListener("blur", exitCapture)` fires constantly on mobile besides.

This is not a bug to fix in the console. It is principle 4 working: the device cannot distinguish
"phone in a pocket" from "operator gone", and it prefers a spurious release-all to a held key.

What the UI owes the operator is therefore **honesty and fast recovery**, not concealment:

- Show session state prominently in the toolbar; do not let a dead session look live.
- Reconnect automatically on `visibilitychange` → visible, and say so.
- Never restore latched modifiers across a reconnect — that would re-press keys the device has
  deliberately released.

## Constraint: the protocol is one-directional

There is no `onmessage` handler in `app.js` and `net.c` never calls `httpd_ws_send_frame`. The
device never tells the console anything.

So a mobile UI **cannot**: know it is the spectator rather than the owner (R-003), read back caps
lock or num lock state, learn that a frame was rejected, or confirm a keystroke was delivered.
Every indicator it shows is a claim about what it *sent*, and it should be worded that way.
Changing this means adding a device→console message, which is a protocol change and a separate
proposal.

## What is deliberately not being decided here

- **Whether to serve a second page or adapt the existing one.** Both are defensible; the console
  is embedded via `EMBED_FILES` and adding an asset means touching two `foreach` lists in
  `main/CMakeLists.txt` plus the `extern`/handler/URI trio in `net.c`. Worth deciding with the
  implementer, not in advance.
- **Trackpad tuning** — acceleration, tap-to-click timing, two-finger scroll thresholds. These
  need a real phone against a real target, and guessing them here would be exactly the kind of
  asserted-not-measured claim principle 6 rejects.

## Verification, when it is built

- A real phone, both iOS Safari and Android Chrome, against a flashed board. The premise
  (`event.code` empty on mobile) is checkable in ten minutes against any web page and should be
  confirmed first.
- `tools/hid_listen.py` gives the ground truth for what actually reached the target, so "typed on
  the phone, arrived on the target" can be machine-checked rather than eyeballed.
- The US-layout assumption must become visible in the UI, not just a code comment
  (`app.js:301-303`) — a non-US operator otherwise gets wrong characters with no explanation.
