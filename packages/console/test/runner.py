#!/usr/bin/env python3
"""Drive the console in headless Firefox and check the bytes it puts on the wire.

The console had no automated test at all. PR #60 described a harness of exactly
this shape -- stubbed WebSocket, fake timers, headless Firefox -- ran it by hand,
and did not commit it, so `app.js`'s comment "The headless check caught it."
referred to a check that existed nowhere. This is that harness, committed.

It serves packages/console/ over loopback, injects a stub script into
index.html's <head> at serve time (the file on disk is never modified), loads the
page once per input mode, and collects the results back over HTTP. What it pins
is the wire format in firmware/main/protocol.h: the exact bytes a key press, a
combo, a click, a mouse move and a release-all put on the socket.

What it does NOT do: it never talks to a dongle, so it proves nothing about the
firmware decoding those bytes, and nothing about a real target. See README.md.

Two things about the browser are not preferences (AGENTS.md 6.1). Firefox, not
Chromium -- Chromium headless hangs on this machine. And a fresh profile
directory every run: reusing one makes Firefox silently do nothing at all.

Run:
    mise run console:test              # the suite
    mise run console:self-test         # prove the suite can fail
    python3 packages/console/test/runner.py --timeout 120
"""

from __future__ import annotations

import argparse
import http.server
import json
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from urllib.parse import urlparse

TEST_DIR = Path(__file__).resolve().parent
CONSOLE_DIR = TEST_DIR.parent

# One page load per mode. The console picks its input mode once and never
# revisits it (AGENTS.md 4), so testing both in one load is not merely awkward,
# it would test a state the console can never be in.
MODES = ("direct", "soft")

# Injected into <head>, so the stubs are installed before any console script
# runs, and just before </body>, so the checks run after all of them. Both
# positions are structural: neither names a script file, so the planned split of
# app.js into four concatenated files changes nothing here.
HEAD_INJECT = '<script src="/__test__/harness.js"></script>'
BODY_INJECT = '<script src="/__test__/tests.js"></script>'


class Result:
    """What one page load reported, or why it reported nothing."""

    def __init__(self) -> None:
        self.queue: queue.Queue = queue.Queue()
        self.missing: list[str] = []


def make_handler(result: Result, sabotage: bool):
    class Handler(http.server.BaseHTTPRequestHandler):
        # Quiet by default: the interesting output is the checks, and a request
        # log would bury it. A 404 is still recorded -- see _send_missing.
        def log_message(self, fmt, *args):
            pass

        def _send(self, body: bytes, ctype: str, code: int = 200) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _send_missing(self, path: str) -> None:
            # A 404 fails the whole run, and that is the point rather than
            # tidiness. Several checks assert that NO frame was sent; if the
            # page half-loads because a script is missing, it sends nothing and
            # every one of those checks passes green. This is the positive
            # control that stops a broken probe reading as success (AGENTS.md 8).
            result.missing.append(path)
            self._send(b"not found\n", "text/plain", 404)

        def do_GET(self) -> None:
            path = urlparse(self.path).path
            if path == "/favicon.ico":
                # Answered rather than exempted from the 404 guard. The browser
                # asks for this on its own; index.html never references it, so a
                # 404 here says nothing about the page being intact, and
                # loosening the guard to tolerate 404s would blunt the one check
                # that stops a half-loaded page reading as success.
                self._send(b"", "image/x-icon", 204)
                return
            if path == "/settings":
                # The dongle serves this from firmware/main/settings.c and the
                # console fetches it at load, so a static tree alone 404s here.
                self._send(
                    (TEST_DIR / "settings.json").read_bytes(), "application/json"
                )
                return
            if path in ("/", "/index.html"):
                html = (CONSOLE_DIR / "index.html").read_text()
                # Anchors asserted rather than assumed: a silently-missed
                # replacement would serve the real console with no stubs, which
                # then fails as a timeout somewhere unrelated.
                assert "<head>" in html, "no <head> to inject into"
                assert "</body>" in html, "no </body> to inject before"
                html = html.replace("<head>", "<head>" + HEAD_INJECT, 1)
                inject = BODY_INJECT
                if sabotage:
                    inject = '<script src="/__test__/sabotage.js"></script>' + inject
                html = html.replace("</body>", inject + "</body>", 1)
                self._send(html.encode(), "text/html; charset=utf-8")
                return

            if path.startswith("/__test__/"):
                base = TEST_DIR
                name = path[len("/__test__/") :]
            else:
                base = CONSOLE_DIR
                name = path.lstrip("/")

            target = (base / name).resolve()
            if base not in target.parents or not target.is_file():
                self._send_missing(path)
                return

            ctypes = {
                ".js": "text/javascript",
                ".css": "text/css",
                ".html": "text/html; charset=utf-8",
            }
            ctype = ctypes.get(target.suffix, "application/octet-stream")
            self._send(target.read_bytes(), ctype)

        def do_POST(self) -> None:
            if urlparse(self.path).path != "/__result":
                self._send_missing(self.path)
                return
            length = int(self.headers.get("Content-Length", 0))
            payload = self.rfile.read(length)
            self._send(b"ok\n", "text/plain")
            result.queue.put(json.loads(payload))

    return Handler


def run_mode(
    mode: str, timeout: float, sabotage: bool
) -> tuple[list[dict], str | None]:
    """Load the console once in `mode`; return its checks, or a failure reason."""
    result = Result()
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), make_handler(result, sabotage)
    )
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]

    # A fresh profile per run, deleted afterwards. Reusing one makes Firefox
    # produce nothing with no error (AGENTS.md 6.1).
    profile = tempfile.mkdtemp(prefix="airkvm-console-test-")
    url = f"http://127.0.0.1:{port}/?mode={mode}"
    proc = subprocess.Popen(
        ["firefox", "--headless", "--no-remote", "--profile", profile, url],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        try:
            payload = result.queue.get(timeout=timeout)
        except queue.Empty:
            # A timeout is a failure, never a pass: "nothing reported" and
            # "everything passed" must not look alike.
            return [], f"no result after {timeout:.0f}s (page never reported)"
    finally:
        proc.terminate()
        proc.wait(timeout=10)
        server.shutdown()
        server.server_close()
        shutil.rmtree(profile, ignore_errors=True)

    if result.missing:
        return [], f"server returned 404 for {', '.join(sorted(set(result.missing)))}"
    if payload.get("errors"):
        return [], "page errors: " + "; ".join(payload["errors"])
    return payload.get("checks", []), None


def run_suite(timeout: float, sabotage: bool) -> int:
    """Run every mode. Returns 0 only if every check in every mode passed."""
    failures = 0
    for mode in MODES:
        print(f"--- {mode} mode ---")
        checks, reason = run_mode(mode, timeout, sabotage)
        if reason is not None:
            print(f"  FAIL [{mode}]: {reason}")
            failures += 1
            continue
        if not checks:
            print(f"  FAIL [{mode}]: reported zero checks")
            failures += 1
            continue
        for check in checks:
            if check["ok"]:
                print(f"  ok   {check['name']}")
            else:
                failures += 1
                print(f"  FAIL {check['name']}: {check['detail']}")
        print(f"  {len(checks)} checks")
    print("console: ok" if failures == 0 else f"console: {failures} failure(s)")
    return 0 if failures == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="seconds to wait for a page load to report (default: 60)",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="plant a bug and require the suite to catch it; exits 1 if it does not",
    )
    args = ap.parse_args()

    if shutil.which("firefox") is None:
        print(
            "firefox not found; this harness needs it (Chromium hangs)", file=sys.stderr
        )
        return 1

    if not args.self_test:
        return run_suite(args.timeout, sabotage=False)

    # A test that has never been seen to fail has proven nothing (AGENTS.md 8).
    # Rather than assert that once, by hand, in a commit message, this runs the
    # whole suite against a deliberately broken encoder and requires red.
    print("self-test: running the suite with a planted bug; expecting failure")
    if run_suite(args.timeout, sabotage=True) == 0:
        print("self-test: FAILED — the suite passed with a planted bug in it")
        return 1
    print("self-test: ok — the planted bug was caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
