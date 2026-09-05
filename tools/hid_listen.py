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
import glob
import os
import struct
import sys
import time

# struct input_event: struct timeval (2 x long), __u16 type, __u16 code,
# __s32 value. 24 bytes on 64-bit; asserted at run time rather than assumed,
# since a 32-bit host would lay it out differently.
EVENT_FORMAT = "@llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)

EV_KEY = 0x01
KEY_RELEASE, KEY_PRESS = 0, 1

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

# Non-character keys worth naming in a transcript.
NAMED = {1: "Esc", 111: "Delete", 59: "F1", 60: "F2", 61: "F3", 62: "F4"}


def find_keyboard(wait_s: float = 0.0) -> str:
    """The dongle's own keyboard node, by USB identity — never by scanning.

    Waits for it to appear, because the node legitimately goes away and comes
    back: an OTA update reboots the device, which re-enumerates it. Without
    this a test firing during an update fails as "no keyboard" rather than
    waiting the couple of seconds the device needs.
    """
    deadline = time.time() + wait_s
    while True:
        matches = sorted(glob.glob("/dev/input/by-id/*airkvm*event-kbd"))
        if matches:
            return matches[0]
        if time.time() >= deadline:
            raise SystemExit(
                "no airkvm keyboard found under /dev/input/by-id/ — is the "
                "dongle plugged into this machine and running a USB HID "
                "build? (pass --wait N to allow for an OTA reboot)"
            )
        time.sleep(0.25)


def listen(path: str, deadline: float, quiet: bool) -> str:
    """Collect typed characters until the deadline. Returns what was typed."""
    typed: list[str] = []
    # A *set* of held shift keycodes, not a boolean. With a boolean, holding
    # both shifts and releasing either one clears it, so a following KeyA is
    # transcribed as "a" while the target really typed "A" — a false pass in
    # the direction that matters, since this tool exists to be believed.
    shift_held: set[int] = set()
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except PermissionError:
        raise SystemExit(
            f"cannot read {path} — this needs membership of the 'input' group "
            "(and a fresh login to pick it up). Deliberately not asking for root."
        )
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=5.0, help="how long to listen")
    parser.add_argument(
        "--expect",
        help="text that should arrive; exit 1 with a diff if it does not",
    )
    parser.add_argument("--device", help="override the input node")
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
    args = parser.parse_args()

    if EVENT_SIZE != 24:
        print(
            f"unexpected struct input_event size {EVENT_SIZE} — this tool "
            "assumes a 64-bit kernel ABI",
            file=sys.stderr,
        )
        return 2

    path = args.device or find_keyboard(args.wait)
    if not args.quiet:
        print(f"listening on {path} for {args.seconds}s", file=sys.stderr)

    typed = listen(path, time.time() + args.seconds, args.quiet)

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
