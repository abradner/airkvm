#!/usr/bin/env python3
"""Drive an airhid dongle from the command line, without a browser.

Speaks the wire format in main/protocol.h directly, which makes it the tool for
answering "is the firmware wrong or is the console wrong?" — and for typing at a
target from a machine with no GUI at all.

Standard library only, including a minimal WebSocket client: adding a dependency
to a debugging aid would mean a virtualenv between you and a stuck key.

    ./tools/poke.py airhid.local type "hello world"
    ./tools/poke.py airhid.local key ControlLeft+AltLeft+Delete
    ./tools/poke.py airhid.local release
"""

from __future__ import annotations

import argparse
import base64
import os
import secrets
import socket
import struct
import sys
import time

KEY_DOWN = 0x01
KEY_UP = 0x02
MOUSE_MOVE = 0x03
MOUSE_BUTTON = 0x04
MOUSE_SCROLL = 0x05
RELEASE_ALL = 0x06
PING = 0x07

MAX_CODE_LEN = 23  # must match AIRHID_MAX_CODE_LEN in main/protocol.h


class WebSocket:
    """Just enough client to send binary frames to the dongle."""

    def __init__(self, host: str, port: int = 80, path: str = "/ws") -> None:
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        response = self._read_headers()
        if b"101" not in response.split(b"\r\n", 1)[0]:
            raise RuntimeError(f"handshake failed: {response.decode(errors='replace')}")

    def _read_headers(self) -> bytes:
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise RuntimeError("connection closed during handshake")
            buf += chunk
        return buf

    def send_binary(self, payload: bytes) -> None:
        # Client frames must be masked (RFC 6455 §5.3); the server unmasks.
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        header = bytes([0x82])  # FIN + binary opcode
        length = len(payload)
        if length < 126:
            header += bytes([0x80 | length])
        elif length < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", length)
        else:
            raise ValueError("frame too large for this tool")
        self.sock.sendall(header + mask + masked)

    def close(self) -> None:
        try:
            self.sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
        finally:
            self.sock.close()


def key_frame(frame_type: int, code: str) -> bytes:
    encoded = code.encode("ascii")
    if len(encoded) > MAX_CODE_LEN:
        raise ValueError(f"key code too long: {code}")
    return bytes([frame_type, len(encoded)]) + encoded


# Mirrors the console's CHAR_MAP (web/app.js). Two copies of this table is a
# known duplication: this one exists so the tool has no dependency on a browser,
# and it is small enough that divergence shows up immediately as a wrong
# character. If it grows, move it into firmware and send text as text.
_SHIFTED = {
    "!": "Digit1",
    "@": "Digit2",
    "#": "Digit3",
    "$": "Digit4",
    "%": "Digit5",
    "^": "Digit6",
    "&": "Digit7",
    "*": "Digit8",
    "(": "Digit9",
    ")": "Digit0",
    "_": "Minus",
    "+": "Equal",
    "{": "BracketLeft",
    "}": "BracketRight",
    "|": "Backslash",
    ":": "Semicolon",
    '"': "Quote",
    "~": "Backquote",
    "<": "Comma",
    ">": "Period",
    "?": "Slash",
}
_PLAIN = {
    " ": "Space",
    "\n": "Enter",
    "\t": "Tab",
    "-": "Minus",
    "=": "Equal",
    "[": "BracketLeft",
    "]": "BracketRight",
    "\\": "Backslash",
    ";": "Semicolon",
    "'": "Quote",
    "`": "Backquote",
    ",": "Comma",
    ".": "Period",
    "/": "Slash",
}


def char_to_key(ch: str) -> tuple[str, bool] | None:
    if ch.isalpha() and ch.isascii():
        return f"Key{ch.upper()}", ch.isupper()
    if ch.isdigit() and ch.isascii():
        return f"Digit{ch}", False
    if ch in _PLAIN:
        return _PLAIN[ch], False
    if ch in _SHIFTED:
        return _SHIFTED[ch], True
    return None


def cmd_type(ws: WebSocket, text: str, delay: float) -> int:
    unsupported = sorted({c for c in text if char_to_key(c) is None})
    if unsupported:
        # Refuse the whole string rather than type part of it: half a password
        # reaching the target is worse than none.
        print(f"can't type: {' '.join(repr(c) for c in unsupported)}", file=sys.stderr)
        return 1
    for ch in text:
        code, shift = char_to_key(ch)
        if shift:
            ws.send_binary(key_frame(KEY_DOWN, "ShiftLeft"))
        ws.send_binary(key_frame(KEY_DOWN, code))
        ws.send_binary(key_frame(KEY_UP, code))
        if shift:
            ws.send_binary(key_frame(KEY_UP, "ShiftLeft"))
        time.sleep(delay)
    return 0


def cmd_key(ws: WebSocket, combo: str) -> int:
    codes = combo.split("+")
    for code in codes:
        ws.send_binary(key_frame(KEY_DOWN, code))
    for code in reversed(codes):
        ws.send_binary(key_frame(KEY_UP, code))
    return 0


def cmd_move(ws: WebSocket, dx: int, dy: int) -> int:
    ws.send_binary(bytes([MOUSE_MOVE]) + struct.pack("<hh", dx, dy))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="dongle hostname or IP")
    parser.add_argument("--port", type=int, default=80)
    sub = parser.add_subparsers(dest="command", required=True)

    p_type = sub.add_parser("type", help="type a string")
    p_type.add_argument("text")
    # On the subparser, not the top level: argparse only accepts top-level
    # options *before* the subcommand, so `poke.py host type x --delay 0.02`
    # would fail with "unrecognized arguments" — which is exactly how anyone
    # would write it.
    p_type.add_argument(
        "--delay",
        type=float,
        default=0.008,
        help="seconds between keystrokes (default: 0.008)",
    )

    p_key = sub.add_parser("key", help="press a chord, e.g. ControlLeft+AltLeft+Delete")
    p_key.add_argument("combo")

    p_move = sub.add_parser("move", help="move the pointer")
    p_move.add_argument("dx", type=int)
    p_move.add_argument("dy", type=int)

    sub.add_parser("release", help="release every held key and button")
    sub.add_parser("ping", help="check the socket answers")

    args = parser.parse_args()

    try:
        ws = WebSocket(args.host, args.port)
    except (OSError, RuntimeError) as err:
        # A traceback here says nothing useful: the answer is almost always
        # "wrong host" or "the dongle isn't up".
        print(f"can't reach {args.host}:{args.port} — {err}", file=sys.stderr)
        return 1

    try:
        if args.command == "type":
            return cmd_type(ws, args.text, args.delay)
        if args.command == "key":
            return cmd_key(ws, args.combo)
        if args.command == "move":
            return cmd_move(ws, args.dx, args.dy)
        if args.command == "release":
            ws.send_binary(bytes([RELEASE_ALL]))
            return 0
        if args.command == "ping":
            ws.send_binary(bytes([PING]))
            return 0
        return 2
    finally:
        # Always release before leaving: a tool that exits mid-chord and leaves
        # Ctrl held on someone's machine is the exact failure this project's
        # principle 4 names.
        try:
            ws.send_binary(bytes([RELEASE_ALL]))
        except OSError:
            pass
        ws.close()


if __name__ == "__main__":
    sys.exit(main())
