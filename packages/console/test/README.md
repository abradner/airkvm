# Console checks

Loads the real `index.html` in headless Firefox with a stubbed `WebSocket` and a
fake clock, drives it with synthetic events, and asserts the **exact bytes** it
puts on the wire against the layout in `firmware/main/protocol.h`.

```sh
mise run console:test        # the suite
mise run console:self-test   # plant a bug; require the suite to catch it
```

## What it proves, and what it does not

It proves the console *encodes* correctly: that a key press is
`01 <len> <code>`, that a 300px flick becomes three ±127 frames of int16 counts
rather than one, that a combo releases in reverse order, that release-all is a
single `06`.

It proves **nothing** about the firmware decoding those bytes, nothing about USB
enumeration or HID delivery, and nothing about latency — none of which a browser
can observe (AGENTS.md §4, §7). A green run here plus a green firmware build
still means no one has typed on a target.

## Design notes

**Nothing here names `app.js`.** `runner.py` serves the console directory and
injects `harness.js` into `<head>` (so the stubs exist before any console script
runs) and `tests.js` before `</body>` (so the checks run after all of them).
Both positions are structural, so splitting `app.js` into several concatenated
files changes nothing in this directory. The files on disk are never modified —
injection happens at serve time.

**Any 404 fails the run.** Several checks assert that *no* frame was sent. A page
that half-loaded because a script was missing also sends nothing, so without this
guard a broken probe would read as success (AGENTS.md §8). It earned its keep
immediately: it caught the console's `fetch("/settings")` on the very first run,
which is why `settings.json` exists. `/favicon.ico` is answered rather than
exempted — the browser asks for it unprompted, so a 404 there says nothing about
the page being intact, and loosening the guard would blunt it.

**Expected bytes are written out literally**, not derived from `app.js`'s own
`FRAME` table. A test that computes its expectation from the code under test
agrees with that code by construction, including when both are wrong.

**One page load per mode.** The console picks `direct` or `soft` once and never
revisits it (AGENTS.md §4), so a single load testing both would be testing a
state the console cannot be in. The exclusion is checked in both directions: a
touchpad gesture must send nothing in `direct`, and a document key event must
send nothing in `soft` — each one's positive control is the same input producing
frames in the other mode.

**Time is explicit.** The console schedules keepalive on an interval, motion
flushes on `requestAnimationFrame`, and paced typing on short timeouts.
`harness.js` replaces those with a clock nothing advances but a check, so the
suite is neither slow when it passes nor flaky when it fails.

## Browser constraints (AGENTS.md §6.1)

- **Firefox, not Chromium.** Chromium headless hangs on this hardware.
- **A fresh profile directory every run.** Reusing one makes Firefox silently
  produce nothing at all, which is indistinguishable from a hang.
- **Pointer lock cannot be acquired headlessly** — it needs a real user gesture,
  and a synthetic click is not one. `tests.js` sets the console's `captured` flag
  directly instead; without that every direct-mode handler early-returns and the
  suite would pass by testing nothing.
