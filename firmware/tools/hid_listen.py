#!/usr/bin/env python3
"""Read back what the dongle actually delivered as a USB HID keyboard.

This closes the loop that `poke.py` alone cannot. poke.py proves frames were
*sent*; the device's on-screen event counter proves intents were *applied*.
Neither proves a keystroke reached the target — and the gap between those three
is exactly where this project's worst bug lived (AGENTS.md §9, entry 11: 3
characters of 17 delivered while every frame arrived and every intent applied).
Until now the only way to close it was to ask a human what appeared on screen.

Run the dongle into this machine and it enumerates as an ordinary USB keyboard,
so the kernel will tell us precisely what it typed:

    ./tools/hid_listen.py --seconds 5
    ./tools/hid_listen.py --expect "hello from airkvm" &
    ./tools/poke.py <dongle-ip> type "hello from airkvm"
    ./tools/hid_listen.py --mouse --seconds 10   # buttons, motion, wheel

`--mouse` reads the dongle's mouse node instead, so "a tap is one click" and
"two fingers are a wheel" are checked the same way as typed text is.

**Scope, stated plainly because this is a keylogger-shaped tool.** It opens the
airkvm dongle's own input node and nothing else — located by its USB identity
under /dev/input/by-id/, never by scanning every device. It cannot see the
operator's real keyboard, and it exists so that HID claims are measured rather
than eyeballed (AGENTS.md §8, "verify the output, not the instrument"). Needs
membership of the `input` group; it deliberately does not ask for root.

Linux only, by construction — it reads the kernel's evdev interface.
"""

from __future__ import annotations

import argparse
import fcntl
import glob
import os
import struct
import sys
import time

# EVIOCGRAB: _IOW('E', 0x90, int). Grabbing takes the device exclusively, so
# events go ONLY to this process and never reach the desktop session.
#
# That matters here beyond tidiness. The dongle is often plugged into the same
# machine that is developing it, which makes it a real keyboard attached to the
# operator's own session — so a typing test types into whatever window has
# focus, and a stuck modifier is the operator's problem rather than the
# target's. Grabbing turns the test into something safe to run unattended.
#
# The grab lasts only while the fd is open, and the kernel drops it on exit,
# including on a crash. Not the default: an un-grabbed listen is the honest
# observation of what a real target would receive, and silently intercepting a
# keyboard is not something a tool should do unless asked.
EVIOCGRAB = 0x40044590

# struct input_event: struct timeval (2 x long), __u16 type, __u16 code,
# __s32 value. 24 bytes on 64-bit; asserted at run time rather than assumed,
# since a 32-bit host would lay it out differently.
EVENT_FORMAT = "@llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)

EV_SYN = 0x00
EV_KEY = 0x01
EV_REL = 0x02
KEY_RELEASE, KEY_PRESS = 0, 1

# Relative axes (input-event-codes.h). The hi-res wheel axes the kernel adds
# alongside REL_WHEEL are ignored: one notch is one REL_WHEEL event, and that
# is the unit the dongle sends.
REL_AXES = {0: "x", 1: "y", 6: "hwheel", 8: "wheel"}
BUTTONS = {0x110: "BTN_LEFT", 0x111: "BTN_RIGHT", 0x112: "BTN_MIDDLE"}

# Linux keycodes (include/uapi/linux/input-event-codes.h) for the characters
# poke.py can type. Deliberately explicit and small, mirroring poke.py's own
# CHAR_MAP: a third copy of a key table would be worse, but this one maps a
# *different* namespace — the kernel's codes, not HID usages or browser
# event.code strings — so it cannot be shared with either.
KEYCODES = {
    2: ("1", "!"),
    3: ("2", "@"),
    4: ("3", "#"),
    5: ("4", "$"),
    6: ("5", "%"),
    7: ("6", "^"),
    8: ("7", "&"),
    9: ("8", "*"),
    10: ("9", "("),
    11: ("0", ")"),
    12: ("-", "_"),
    13: ("=", "+"),
    14: ("\b", "\b"),
    15: ("\t", "\t"),
    16: ("q", "Q"),
    17: ("w", "W"),
    18: ("e", "E"),
    19: ("r", "R"),
    20: ("t", "T"),
    21: ("y", "Y"),
    22: ("u", "U"),
    23: ("i", "I"),
    24: ("o", "O"),
    25: ("p", "P"),
    26: ("[", "{"),
    27: ("]", "}"),
    28: ("\n", "\n"),
    30: ("a", "A"),
    31: ("s", "S"),
    32: ("d", "D"),
    33: ("f", "F"),
    34: ("g", "G"),
    35: ("h", "H"),
    36: ("j", "J"),
    37: ("k", "K"),
    38: ("l", "L"),
    39: (";", ":"),
    40: ("'", '"'),
    41: ("`", "~"),
    43: ("\\", "|"),
    44: ("z", "Z"),
    45: ("x", "X"),
    46: ("c", "C"),
    47: ("v", "V"),
    48: ("b", "B"),
    49: ("n", "N"),
    50: ("m", "M"),
    51: (",", "<"),
    52: (".", ">"),
    53: ("/", "?"),
    57: (" ", " "),
}

# Modifiers, named rather than mapped to characters.
MODIFIERS = {
    29: "Ctrl",
    42: "Shift",
    54: "Shift",
    56: "Alt",
    97: "Ctrl",
    100: "Alt",
    125: "Meta",
    126: "Meta",
}
SHIFT_KEYS = (42, 54)

# Non-character keys worth naming in a transcript — everything the console's
# on-screen key rows can send.
NAMED = {
    1: "Esc",
    111: "Delete",
    110: "Insert",
    102: "Home",
    107: "End",
    104: "PageUp",
    109: "PageDown",
    103: "Up",
    108: "Down",
    105: "Left",
    106: "Right",
    59: "F1",
    60: "F2",
    61: "F3",
    62: "F4",
    63: "F5",
    64: "F6",
    65: "F7",
    66: "F8",
    67: "F9",
    68: "F10",
    87: "F11",
    88: "F12",
}


def find_keyboard(wait_s: float = 0.0, kind: str = "kbd") -> str:
    """The dongle's own keyboard (or mouse) node, by USB identity — never by scanning.

    Waits for it to appear, because the node legitimately goes away and comes
    back: an OTA update reboots the device, which re-enumerates it. Without
    this a test firing during an update fails as "no keyboard" rather than
    waiting the couple of seconds the device needs.
    """
    deadline = time.time() + wait_s
    while True:
        matches = sorted(glob.glob(f"/dev/input/by-id/*airkvm*event-{kind}"))
        if matches:
            return matches[0]
        if time.time() >= deadline:
            raise SystemExit(
                f"no airkvm {kind} node found under /dev/input/by-id/ — is the "
                "dongle plugged into this machine and running a USB HID "
                "build? (pass --wait N to allow for an OTA reboot)"
            )
        time.sleep(0.25)


def _set_grab(fd: int, on: bool) -> None:
    fcntl.ioctl(fd, EVIOCGRAB, 1 if on else 0)


def listen(path: str, deadline: float, quiet: bool, grab: bool = False) -> str:
    """Collect typed characters until the deadline. Returns what was typed."""
    typed: list[str] = []
    # A *set* of held shift keycodes, not a boolean. With a boolean, holding
    # both shifts and releasing either one clears it, so a following KeyA is
    # transcribed as "a" while the target really typed "A" — a false pass in
    # the direction that matters, since this tool exists to be believed.
    shift_held: set[int] = set()
    fd = _open(path, grab, quiet)
    try:
        while time.time() < deadline:
            try:
                data = os.read(fd, EVENT_SIZE * 64)
            except BlockingIOError:
                time.sleep(0.005)
                continue
            except OSError:
                # The device went away mid-listen — an OTA reboot does this.
                # Report what was captured rather than losing it to a
                # traceback; the caller's expectation check will fail loudly
                # enough if the disappearance actually mattered.
                break
            for off in range(0, len(data) - EVENT_SIZE + 1, EVENT_SIZE):
                _, _, etype, code, value = struct.unpack_from(EVENT_FORMAT, data, off)
                if etype != EV_KEY:
                    continue
                if code in SHIFT_KEYS:
                    if value == KEY_RELEASE:
                        shift_held.discard(code)
                    else:
                        shift_held.add(code)
                    continue
                # Autorepeat (value 2) is a real character on the target: a key
                # held long enough repeats, and this tool must not report one
                # "a" when the target received twenty. Repeats therefore count.
                # That is also the repeat-storm failure principle 4 names, so a
                # verifier that silently swallowed them would hide exactly the
                # bug it should catch.
                if value == KEY_RELEASE:
                    continue
                if code in KEYCODES:
                    typed.append(KEYCODES[code][1 if shift_held else 0])
                elif code in MODIFIERS:
                    if not quiet:
                        print(f"  [{MODIFIERS[code]} down]", file=sys.stderr)
                else:
                    typed.append(f"<{NAMED.get(code, f'key{code}')}>")
    finally:
        os.close(fd)
    return "".join(typed)


def _open(path: str, grab: bool, quiet: bool) -> int:
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except PermissionError:
        raise SystemExit(
            f"cannot read {path} — this needs membership of the 'input' group "
            "(and a fresh login to pick it up). Deliberately not asking for root."
        )
    if grab:
        try:
            _set_grab(fd, True)
        except OSError as exc:
            os.close(fd)
            print(
                f"could not grab {path} exclusively ({exc}) — refusing to "
                "continue, because the caller asked for input to be trapped "
                "and it would instead reach the desktop",
                file=sys.stderr,
            )
            raise SystemExit(2) from exc
        if not quiet:
            print("  [grabbed: input will NOT reach this session]", file=sys.stderr)
    return fd


def listen_mouse(
    path: str, deadline: float, grab: bool = False, quiet: bool = False
) -> dict:
    """Transcribe the mouse node: buttons as they happen, motion in bursts.

    Consecutive relative motion is folded into one line per burst — a burst
    ends at a button or wheel event, or at a pause longer than BURST_GAP_S —
    because the interesting facts about a finger-drag are its total travel and
    what the buttons did around it, not sixty lines of dx=1. Wheel notches are
    printed one line per event: the console coalesces scroll per animation
    frame, and how many notches arrive per event is exactly what that tuning
    needs to see.

    Returns totals, so a caller can assert on them.
    """
    BURST_GAP_S = 0.1
    totals = {"x": 0, "y": 0, "wheel": 0, "hwheel": 0, "presses": 0, "releases": 0}
    burst = {"x": 0, "y": 0, "events": 0, "open": False, "start": 0.0, "last": 0.0}

    def flush_burst() -> None:
        if burst["events"]:
            dur = burst["last"] - burst["start"]
            print(
                f"  move dx={burst['x']:+d} dy={burst['y']:+d} "
                f"({burst['events']} reports over {dur:.2f}s)"
            )
            burst.update(x=0, y=0, events=0, open=False)

    fd = _open(path, grab, quiet)
    t0 = time.time()
    try:
        while time.time() < deadline:
            try:
                data = os.read(fd, EVENT_SIZE * 64)
            except BlockingIOError:
                if burst["events"] and time.time() - t0 - burst["last"] > BURST_GAP_S:
                    flush_burst()
                time.sleep(0.005)
                continue
            except OSError:
                break
            now = time.time() - t0
            for off in range(0, len(data) - EVENT_SIZE + 1, EVENT_SIZE):
                _, _, etype, code, value = struct.unpack_from(EVENT_FORMAT, data, off)
                if etype == EV_KEY and code in BUTTONS:
                    flush_burst()
                    edge = "up" if value == KEY_RELEASE else "down"
                    totals["releases" if value == KEY_RELEASE else "presses"] += 1
                    print(f"  {now:7.3f}s {BUTTONS[code]} {edge}")
                elif etype == EV_REL and code in REL_AXES:
                    axis = REL_AXES[code]
                    totals[axis] += value
                    if axis in ("wheel", "hwheel"):
                        flush_burst()
                        print(f"  {now:7.3f}s {axis} {value:+d}")
                    else:
                        if not burst["events"] and not burst["open"]:
                            burst["start"] = now
                        burst[axis] += value
                        burst["open"] = True
                        burst["last"] = now
                elif etype == EV_SYN and code == 0 and burst["open"]:
                    # One device report ends here, however many axes it moved;
                    # counting axes would call a diagonal step two reports.
                    burst["open"] = False
                    burst["events"] += 1
        flush_burst()
    finally:
        os.close(fd)
    print(
        f"totals: dx={totals['x']:+d} dy={totals['y']:+d} wheel={totals['wheel']:+d} "
        f"hwheel={totals['hwheel']:+d} presses={totals['presses']} "
        f"releases={totals['releases']}"
    )
    return totals


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=5.0, help="how long to listen")
    parser.add_argument(
        "--expect",
        help="text that should arrive; exit 1 with a diff if it does not",
    )
    parser.add_argument("--device", help="override the input node")
    parser.add_argument(
        "--mouse",
        action="store_true",
        help="read the dongle's mouse node instead: buttons, motion bursts, "
        "wheel notches, and totals",
    )
    parser.add_argument(
        "--wait",
        type=float,
        default=0.0,
        help="seconds to wait for the device to appear, for use across an OTA "
        "reboot (default: fail immediately)",
    )
    parser.add_argument(
        "--quiet", action="store_true", help="suppress modifier chatter"
    )
    parser.add_argument(
        "--grab",
        action="store_true",
        help="take the device exclusively so keystrokes do NOT reach this "
        "machine's desktop session — use when the dongle is plugged into the "
        "same machine you are working on",
    )
    args = parser.parse_args()

    if EVENT_SIZE != 24:
        print(
            f"unexpected struct input_event size {EVENT_SIZE} — this tool "
            "assumes a 64-bit kernel ABI",
            file=sys.stderr,
        )
        return 2

    if args.mouse and args.expect is not None:
        print(
            "--expect is for typed text; --mouse prints a transcript", file=sys.stderr
        )
        return 2

    path = args.device or find_keyboard(args.wait, "mouse" if args.mouse else "kbd")
    if not args.quiet:
        print(f"listening on {path} for {args.seconds}s", file=sys.stderr)

    if args.mouse:
        totals = listen_mouse(path, time.time() + args.seconds, args.grab, args.quiet)
        # A button that went down and never came up is the failure this
        # project exists to prevent, and a transcript should not let it pass
        # quietly.
        if totals["presses"] != totals["releases"]:
            print(
                "WARNING: button presses and releases do not balance", file=sys.stderr
            )
            return 1
        return 0

    typed = listen(path, time.time() + args.seconds, args.quiet, args.grab)

    if args.expect is None:
        print(typed)
        return 0

    if typed == args.expect:
        print(f"OK: delivered exactly {len(typed)} chars: {typed!r}")
        return 0

    # Report the first divergence rather than two opaque strings — with a
    # dropped keystroke the useful fact is *where* the streams parted.
    print(
        f"MISMATCH\n  expected ({len(args.expect)}): {args.expect!r}", file=sys.stderr
    )
    print(f"  actual   ({len(typed)}): {typed!r}", file=sys.stderr)
    for i, (want, got) in enumerate(zip(args.expect, typed)):
        if want != got:
            print(
                f"  first difference at index {i}: {want!r} vs {got!r}", file=sys.stderr
            )
            break
    else:
        n = min(len(args.expect), len(typed))
        kind = "truncated" if len(typed) < len(args.expect) else "extra input"
        print(f"  identical for {n} chars, then {kind}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
