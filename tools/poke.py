#!/usr/bin/env python3
"""Drive an airkvm dongle from the command line, without a browser.

Speaks the wire format in main/protocol.h directly, which makes it the tool for
answering "is the firmware wrong or is the console wrong?" — and for typing at a
target from a machine with no GUI at all.

Standard library only, including a minimal WebSocket client: adding a dependency
to a debugging aid would mean a virtualenv between you and a stuck key.

    ./tools/poke.py airkvm.local type "hello world"
    ./tools/poke.py airkvm.local key ControlLeft+AltLeft+Delete
    ./tools/poke.py airkvm.local release
    ./tools/poke.py airkvm.local wake
    ./tools/poke.py airkvm.local hold ShiftLeft --seconds 20   # keep pressed
    ./tools/poke.py --tls airkvm.local media debian.iso        # serve an image

The real firmware serves over TLS by default (CONFIG_AIRKVM_TLS) — add --tls
to reach it; --port defaults to 443 in that case. Verified against the
operator's own CA (certs/ca.crt, from tools/make_certs.sh) unless --insecure
is given:

    ./tools/poke.py --tls airkvm.local type "hello world"
"""

from __future__ import annotations

import argparse
import base64
import os
import secrets
import socket
import ssl
import struct
import sys
import time
from pathlib import Path

# The operator-run CA tools/make_certs.sh generates, trusted by default the
# same way a browser that's imported it would be — this tool exists to answer
# "is the firmware wrong or the console wrong", and a client that silently
# skips verification can't tell "wrong cert" apart from "no console at all".
DEFAULT_CA = Path(__file__).resolve().parent.parent / "certs" / "ca.crt"

KEY_DOWN = 0x01
KEY_UP = 0x02
MOUSE_MOVE = 0x03
MOUSE_BUTTON = 0x04
MOUSE_SCROLL = 0x05
RELEASE_ALL = 0x06
PING = 0x07
WAKE = 0x08

# Dongle → tool. Bit 7 set, see main/protocol.h.
DOWN_WAKE_RESULT = 0x80

# The media socket (main/media_proto.h), a separate WebSocket on its own port.
MEDIA_ATTACH = 0x10
MEDIA_DETACH = 0x11
MEDIA_DATA = 0x12
MEDIA_ERROR = 0x13
MEDIA_REQ = 0x90
MEDIA_STATUS = 0x91
MEDIA_CHUNK = 16384  # MEDIA_CHUNK_SIZE
MEDIA_BLOCK = 512
MEDIA_PORT_SECURE = 8443
MEDIA_PORT_INSECURE = 8080
MEDIA_STATES = {0: "no medium", 1: "attached", 2: "busy (another client)"}
WAKE_RESULTS = {
    0: "requested — pulse sent, watch the target",
    1: "no USB host enumerated",
    2: "bus not suspended (target already awake?)",
    3: "host did not arm remote wakeup before suspending",
}

MAX_CODE_LEN = 23  # must match AIRKVM_MAX_CODE_LEN in main/protocol.h

# The device retires roughly one HID report per 20ms: the interrupt endpoint's
# bInterval is 10ms (main/hid_out_usb.c) and the report task applies at most one
# keyboard state change per report, with measurement on hardware landing nearer
# 20ms than 10. Every wait in this tool is derived from this number, and all of
# them count *reports*, not frames or characters — those are not the same thing
# and conflating them is what made an earlier version of this tool truncate
# input: a shifted character costs four reports rather than two, and one
# `move 32767 0` frame costs 259, because take_delta() emits at most 127 units
# per report and carries the remainder.
REPORT_INTERVAL_S = 0.02

# Floor for any settling wait, so even a single chord gets a window.
SETTLE_FLOOR_S = 0.3


def mouse_reports(*deltas: int) -> int:
    """How many HID reports the device needs to express these deltas."""
    return max(1, max((abs(d) + 126) // 127 for d in deltas))


class WebSocket:
    """Just enough client to send binary frames to the dongle."""

    def __init__(
        self,
        host: str,
        port: int,
        path: str = "/ws",
        tls: bool = False,
        ca: Path | None = None,
        insecure: bool = False,
    ) -> None:
        self.reports = 0
        raw = socket.create_connection((host, port), timeout=5)
        if tls:
            if insecure:
                ctx = ssl._create_unverified_context()
            else:
                # Verify against the operator's own CA, not the system trust
                # store: the cert this device presents is issued by a CA
                # nothing else on the machine has any reason to trust, and
                # falling back to "no CA given, so accept anything" would
                # make --insecure the silent default instead of an opt-in.
                if ca is None or not ca.is_file():
                    raise RuntimeError(
                        f"no CA cert at {ca} — pass --ca <path> or --insecure "
                        "to skip verification (see tools/make_certs.sh)"
                    )
                ctx = ssl.create_default_context(cafile=str(ca))
            self.sock = ctx.wrap_socket(raw, server_hostname=host)
        else:
            self.sock = raw
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

    def send_binary(self, payload: bytes, reports: int = 1) -> None:
        # Callers declare what this frame costs the device in HID reports, so
        # settle() knows how much work is still outstanding. Defaults to 1
        # because every key frame is exactly one keyboard state change; only
        # mouse motion expands (see mouse_reports).
        self.reports += reports
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

    def recv_binary(self, timeout: float | None = 2.0) -> bytes | None:
        """Read one unmasked server frame, or None if nothing arrives in time.

        The dongle only ever replies to WAKE today, so this is deliberately
        minimal: no fragmentation, no extended lengths, no control-frame
        handling beyond ignoring them. Server frames are unmasked (RFC 6455
        §5.1), which is why there is no unmasking here.
        """
        self.sock.settimeout(timeout)
        try:
            header = self._recv_exact(2)
            if header is None:
                return None
            opcode = header[0] & 0x0F
            length = header[1] & 0x7F
            if length == 126:
                ext = self._recv_exact(2)
                if ext is None:
                    return None
                length = struct.unpack(">H", ext)[0]
            elif length == 127:
                raise RuntimeError("server frame longer than this tool expects")
            payload = self._recv_exact(length) if length else b""
            if opcode != 0x2:
                return None  # ping/pong/close: not a reply
            return payload
        except (TimeoutError, socket.timeout):
            return None
        finally:
            self.sock.settimeout(5)

    def _recv_exact(self, n: int) -> bytes | None:
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def settle(self) -> None:
        """Wait for the device to work through what we sent, before closing.

        Closing the socket is *destructive to queued input*, and not via the
        release-all frame this tool sends on the way out — that one is queued
        and applied in order, so it cannot overtake anything. The close is the
        problem: the firmware's close_fn ends the session, which calls
        hid_out_release_all(), which sets the release flag directly rather
        than queueing it, and the report task then discards the whole queue
        with xQueueReset. Anything not yet applied is simply gone.

        That is correct behaviour for a real session ending (principle 4:
        never leave a key held), and it makes the sender responsible for not
        hanging up mid-sentence. Measured on hardware: without this, a
        seventeen-character string lost its last five characters while every
        frame arrived — the device's own event counter read 24 of 34 sent.

        There is no ack to wait on, so this waits by the clock, scaled by the
        number of HID *reports* the device still owes — not frames, which
        undercounts badly: one large `move` frame can cost hundreds of
        reports.
        """
        time.sleep(max(SETTLE_FLOOR_S, REPORT_INTERVAL_S * self.reports))

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
        # Pace by what this character actually costs the device, which is four
        # reports when shifted and two when not. A single per-character delay
        # is wrong for mixed-case text: tuned for two reports it overruns every
        # shifted character, and the overrun is not benign — a full queue makes
        # net.c reject the key frame and respond with release-all, wiping the
        # backlog and silently truncating the string.
        reports = 4 if shift else 2
        time.sleep(delay if delay is not None else REPORT_INTERVAL_S * reports)
    return 0


def cmd_key(ws: WebSocket, combo: str) -> int:
    codes = combo.split("+")
    for code in codes:
        ws.send_binary(key_frame(KEY_DOWN, code))
    for code in reversed(codes):
        ws.send_binary(key_frame(KEY_UP, code))
    return 0


def cmd_hold(ws: WebSocket, combo: str, seconds: float | None) -> int:
    """Press a chord and keep the socket open, pinging, until told to stop.

    Exists for one hardware test that nothing else can run: is a key still
    released when the USB cable is pulled with the socket alive? `key` and
    `type` press and release, and closing the socket releases everything on
    its own (net_end_session), so any test built on them exercises the close
    path and passes for the wrong reason. This one holds the key down with
    the session live, so the *only* thing that can release it is the USB
    unmount path — and after the port comes back, no key event must arrive
    at the target that nobody typed.
    """
    codes = combo.split("+")
    for code in codes:
        ws.send_binary(key_frame(KEY_DOWN, code))
    print(f"holding {combo} — Ctrl-C to release", file=sys.stderr)
    deadline = None if seconds is None else time.monotonic() + seconds
    try:
        while deadline is None or time.monotonic() < deadline:
            # One PING per second keeps the dongle's 3s keepalive fed; without
            # it the session expires and release-all runs for the wrong reason.
            ws.send_binary(bytes([PING]), reports=0)
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        # Best effort on every exit path. If the socket itself is what broke,
        # this cannot land — and then the dongle's own close path has already
        # released everything, which is the point of that path.
        for code in reversed(codes):
            try:
                ws.send_binary(key_frame(KEY_UP, code))
            except OSError:
                break
    return 0


def cmd_media(ws: WebSocket, path: Path) -> int:
    """Serve a disk image to the target from this machine's filesystem.

    The dongle presents a removable disk; this attaches `path` as its medium
    and answers chunk requests until Ctrl-C. It is the throughput instrument
    for the media path — `dd` on the target against a clock here — and the
    way to boot a target from an image without a browser at all.
    """
    size = path.stat().st_size
    blocks = (size + MEDIA_BLOCK - 1) // MEDIA_BLOCK
    name = path.name.encode("utf-8")[:31]
    attach = (
        bytes([MEDIA_ATTACH])
        + struct.pack("<HQB", MEDIA_BLOCK, blocks, 0x01)
        + bytes([len(name)])
        + name
    )
    ws.send_binary(attach, reports=0)
    served = 0
    started = time.monotonic()
    print(f"attaching {path.name}: {size} bytes, {blocks} blocks", file=sys.stderr)
    try:
        with path.open("rb") as f:
            while True:
                reply = ws.recv_binary(timeout=None)
                if reply is None:
                    continue
                if reply[0] == MEDIA_STATUS and len(reply) == 10:
                    state = reply[1]
                    print(f"status: {MEDIA_STATES.get(state, state)}", file=sys.stderr)
                    if state == 2:
                        return 1
                elif reply[0] == MEDIA_REQ and len(reply) == 9:
                    req_id, chunk = struct.unpack("<II", reply[1:9])
                    f.seek(chunk * MEDIA_CHUNK)
                    data = f.read(MEDIA_CHUNK)
                    if not data and chunk * MEDIA_CHUNK >= blocks * MEDIA_BLOCK:
                        ws.send_binary(
                            bytes([MEDIA_ERROR]) + struct.pack("<IB", req_id, 1),
                            reports=0,
                        )
                        continue
                    # Always a whole chunk; the tail past the image is zeros.
                    data = data.ljust(MEDIA_CHUNK, b"\0")
                    frame = (
                        bytes([MEDIA_DATA])
                        + struct.pack("<IIIH", req_id, chunk, 0, MEDIA_CHUNK)
                        + data
                    )
                    ws.send_binary(frame, reports=0)
                    served += MEDIA_CHUNK
                    elapsed = time.monotonic() - started
                    if served % (64 * MEDIA_CHUNK) == 0 and elapsed > 0:
                        print(
                            f"served {served >> 20} MiB, {served / elapsed / 1048576:.2f} MiB/s",
                            file=sys.stderr,
                        )
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ws.send_binary(bytes([MEDIA_DETACH]), reports=0)
        except OSError:
            pass
    elapsed = time.monotonic() - started
    if elapsed > 0:
        print(
            f"detached: {served} bytes in {elapsed:.1f}s, {served / elapsed / 1048576:.2f} MiB/s",
            file=sys.stderr,
        )
    return 0


def cmd_wake(ws: WebSocket) -> int:
    ws.send_binary(bytes([WAKE]), reports=0)
    reply = ws.recv_binary()
    if reply is None or len(reply) != 2 or reply[0] != DOWN_WAKE_RESULT:
        print("no wake reply from the dongle", file=sys.stderr)
        return 1
    print(f"wake: {WAKE_RESULTS.get(reply[1], f'unknown result {reply[1]}')}")
    return 0 if reply[1] == 0 else 1


def cmd_move(ws: WebSocket, dx: int, dy: int) -> int:
    ws.send_binary(
        bytes([MOUSE_MOVE]) + struct.pack("<hh", dx, dy),
        reports=mouse_reports(dx, dy),
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="dongle hostname or IP")
    # None, not a literal default: whether the port was explicitly given
    # changes which implicit default is right (80 for plain, 443 for --tls),
    # and argparse has no clean way to ask "did the user pass this" other
    # than checking for the sentinel after parsing.
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument(
        "--tls",
        action="store_true",
        help="use WSS — required for CONFIG_AIRKVM_TLS builds, which is the "
        "real firmware's default. Implies --port 443 unless --port is given.",
    )
    parser.add_argument(
        "--ca",
        type=Path,
        default=DEFAULT_CA,
        help=f"CA cert to verify the dongle's TLS cert against (default: {DEFAULT_CA})",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="skip TLS certificate verification — for debugging only, never "
        "the default, and this tool will say so every time it's used",
    )
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
        default=None,
        help="fixed seconds between keystrokes; the default paces each "
        "character by the number of HID reports it costs the device "
        "(two, or four when shifted), which is what keeps a mixed-case "
        "string from overrunning the endpoint",
    )
    # The pacing rationale lives in REPORT_INTERVAL_S at the top of this file;
    # the short version is that overrunning the endpoint is not merely slow.
    # A full queue makes net.c reject the key frame and respond with
    # release-all, and a dropped key-*up* leaves a key held on the target —
    # the failure AGENTS.md §2 principle 4 exists to prevent.

    p_key = sub.add_parser("key", help="press a chord, e.g. ControlLeft+AltLeft+Delete")
    p_key.add_argument("combo")

    p_move = sub.add_parser("move", help="move the pointer")
    p_move.add_argument("dx", type=int)
    p_move.add_argument("dy", type=int)

    p_media = sub.add_parser(
        "media",
        help="attach a disk image and serve its chunks until Ctrl-C (talks to "
        "the media socket, not the input one)",
    )
    p_media.add_argument("image", type=Path)

    p_hold = sub.add_parser(
        "hold",
        help="press a chord and keep it held with the socket open, for the "
        "unplug-releases-keys hardware test (see cmd_hold)",
    )
    p_hold.add_argument("combo")
    p_hold.add_argument(
        "--seconds",
        type=float,
        default=None,
        help="release after this long (default: hold until Ctrl-C)",
    )

    sub.add_parser("release", help="release every held key and button")
    sub.add_parser("ping", help="check the socket answers")
    sub.add_parser(
        "wake",
        help="signal USB remote wakeup to a suspended target and print the "
        "dongle's reply (exit 0 only if the pulse was actually sent)",
    )

    args = parser.parse_args()
    port = args.port if args.port is not None else (443 if args.tls else 80)
    if args.insecure and not args.tls:
        # Refuse rather than warn about a TLS session that is not happening:
        # printing "certificate NOT verified" over a plaintext connection
        # describes a risk that isn't the real one, and hides the one that is.
        print("--insecure has no meaning without --tls", file=sys.stderr)
        return 2
    if args.insecure:
        print("--insecure: TLS certificate NOT verified", file=sys.stderr)

    # The media subcommand speaks to a different socket on a different port.
    path = "/ws"
    if args.command == "media":
        path = "/ws-media"
        if args.port is None:
            port = MEDIA_PORT_SECURE if args.tls else MEDIA_PORT_INSECURE

    try:
        ws = WebSocket(
            args.host, port, path=path, tls=args.tls, ca=args.ca, insecure=args.insecure
        )
    except (OSError, RuntimeError, ssl.SSLError) as err:
        # A traceback here says nothing useful: the answer is almost always
        # "wrong host" or "the dongle isn't up".
        print(f"can't reach {args.host}:{port} — {err}", file=sys.stderr)
        return 1

    try:
        if args.command == "type":
            return cmd_type(ws, args.text, args.delay)
        if args.command == "key":
            return cmd_key(ws, args.combo)
        if args.command == "move":
            return cmd_move(ws, args.dx, args.dy)
        if args.command == "wake":
            return cmd_wake(ws)
        if args.command == "hold":
            return cmd_hold(ws, args.combo, args.seconds)
        if args.command == "media":
            return cmd_media(ws, args.image)
        if args.command == "release":
            ws.send_binary(bytes([RELEASE_ALL]))
            return 0
        if args.command == "ping":
            ws.send_binary(bytes([PING]))
            return 0
        return 2
    finally:
        # Let the device finish what it was given before tearing the socket
        # down — closing discards anything still queued (see settle()). This
        # covers every subcommand, chords included: a Ctrl-Alt-Del is six
        # frames that now drain one endpoint interval apart.
        try:
            ws.settle()
        except OSError:
            pass
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
