// Injected just before </body>, so it runs after every console script. The
// expected bytes below are firmware/main/protocol.h's layout, written out
// literally rather than derived from app.js's own FRAME table — a test that
// computes its expectation from the code under test agrees with that code by
// construction, including when both are wrong.
"use strict";

(() => {
  const KEY_DOWN = 0x01;
  const KEY_UP = 0x02;
  const MOUSE_MOVE = 0x03;
  const MOUSE_BUTTON = 0x04;
  const RELEASE_ALL = 0x06;
  const PING = 0x07;
  const STATUS_REQ = 0x09;

  const MODE = new URLSearchParams(location.search).get("mode");

  // "KeyA" as UTF-8. Spelled out so the check does not lean on TextEncoder
  // producing what app.js's encoder produced.
  const KEY_A = [0x4b, 0x65, 0x79, 0x41];
  const CTRL = [0x43, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x4c, 0x65, 0x66, 0x74];
  const ALT = [0x41, 0x6c, 0x74, 0x4c, 0x65, 0x66, 0x74];
  const DEL = [0x44, 0x65, 0x6c, 0x65, 0x74, 0x65];

  const key = (type, code) => [type, code.length, ...code];

  const pointer = (type, id, x, y) =>
    new PointerEvent(type, {
      pointerId: id,
      pointerType: "touch",
      isPrimary: true,
      clientX: x,
      clientY: y,
      bubbles: true,
      cancelable: true,
    });

  async function direct() {
    __check("mode is direct", el.mode.textContent === "pointer lock", el.mode.textContent);

    // Pointer lock needs a real user gesture, which a synthetic click is not,
    // so the lock can never be acquired headlessly and every direct-mode
    // handler would early-return. `captured` is a top-level `let`, so it is
    // reachable and assignable from this script.
    captured = true;
    // Every mousemove multiplies by this; left at its default the expected
    // counts would depend on an HTML attribute rather than on the encoder.
    el.sensitivity.value = "1";
    __drain();

    document.dispatchEvent(new KeyboardEvent("keydown", { code: "KeyA" }));
    __expectFrames("key down sends 01 <len> KeyA", [key(KEY_DOWN, KEY_A)]);

    document.dispatchEvent(new KeyboardEvent("keyup", { code: "KeyA" }));
    __expectFrames("key up sends 02 <len> KeyA", [key(KEY_UP, KEY_A)]);

    document.getElementById("release-all").click();
    __expectFrames("release-all is a single 06", [[RELEASE_ALL]]);

    // Downs in listed order, ups in reverse: releasing Ctrl before Delete would
    // leave the target seeing a bare Delete.
    document
      .querySelector('[data-combo="ControlLeft+AltLeft+Delete"]')
      .click();
    __expectFrames("combo presses in order and releases in reverse", [
      key(KEY_DOWN, CTRL),
      key(KEY_DOWN, ALT),
      key(KEY_DOWN, DEL),
      key(KEY_UP, DEL),
      key(KEY_UP, ALT),
      key(KEY_UP, CTRL),
    ]);

    // The mask is absolute state read from MouseEvent.buttons, so a release is
    // 04 00 rather than a distinct opcode.
    document.dispatchEvent(new MouseEvent("mousedown", { buttons: 1 }));
    __expectFrames("mouse down sends the button mask", [[MOUSE_BUTTON, 0x01]]);
    document.dispatchEvent(new MouseEvent("mouseup", { buttons: 0 }));
    __expectFrames("mouse up sends an empty mask", [[MOUSE_BUTTON, 0x00]]);

    // The one worth pinning: motion is clamped to ±127 per frame (the HID
    // report limit) but encoded as int16 little-endian, so a 300px flick is
    // three frames and NOT one, and each is five bytes wide rather than three.
    document.dispatchEvent(
      new MouseEvent("mousemove", { movementX: 300, movementY: 0 }),
    );
    await __advance(32); // let the rAF-scheduled flush run
    __expectFrames("a 300px move splits into ±127 frames, int16 LE", [
      [MOUSE_MOVE, 0x7f, 0x00, 0x00, 0x00],
      [MOUSE_MOVE, 0x7f, 0x00, 0x00, 0x00],
      [MOUSE_MOVE, 0x2e, 0x00, 0x00, 0x00],
    ]);

    // Negative counts are where a byte-wise encoder would silently disagree
    // with an int16 one: -1 is ff ff, not ff.
    document.dispatchEvent(
      new MouseEvent("mousemove", { movementX: -1, movementY: 2 }),
    );
    await __advance(32);
    __expectFrames("negative motion is sign-extended, not truncated", [
      [MOUSE_MOVE, 0xff, 0xff, 0x02, 0x00],
    ]);

    // The mode exclusion, asserted as an absence — its positive control is the
    // identical gesture in soft mode, which must produce frames.
    const pad = document.getElementById("touchpad");
    pad.dispatchEvent(pointer("pointerdown", 1, 50, 50));
    pad.dispatchEvent(pointer("pointerup", 1, 50, 50));
    await __advance(64);
    __expectFrames("direct mode ignores the touchpad entirely", []);

    // Last, because advancing a second of fake time fires anything else due.
    // Odd keepalives are PING, even ones are a status poll.
    await __advance(1000);
    __expectFrames("first keepalive is a PING", [[PING]]);
    await __advance(1000);
    __expectFrames("second keepalive is a status request", [[STATUS_REQ]]);
  }

  async function soft() {
    __check("mode is soft", el.mode.textContent === "touch", el.mode.textContent);
    __check(
      "body carries the soft class",
      document.body.classList.contains("soft"),
      document.body.className,
    );
    __drain();

    // A tap: down and up with no travel. The click is held briefly and then
    // released on a timer, which is why the clock has to be advanced for the
    // second frame — and why the release is a frame at all rather than being
    // coalesced away.
    const pad = document.getElementById("touchpad");
    pad.dispatchEvent(pointer("pointerdown", 1, 50, 50));
    pad.dispatchEvent(pointer("pointerup", 1, 50, 50));
    __expectFrames("a tap presses the left button", [[MOUSE_BUTTON, 0x01]]);
    await __advance(64);
    __expectFrames("the tap releases it on its own", [[MOUSE_BUTTON, 0x00]]);

    // Mode-independent controls stay wired in soft mode.
    document.getElementById("release-all").click();
    __expectFrames("release-all works in soft mode too", [[RELEASE_ALL]]);

    // The other half of the exclusion: no document-level key listener exists
    // here, so a keystroke that direct mode forwards must go nowhere.
    document.dispatchEvent(new KeyboardEvent("keydown", { code: "KeyA" }));
    document.dispatchEvent(new KeyboardEvent("keyup", { code: "KeyA" }));
    __expectFrames("soft mode ignores document key events", []);
  }

  window.addEventListener("load", async () => {
    try {
      if (MODE === "soft") await soft();
      else await direct();
    } catch (e) {
      __check("suite ran to completion", false, `threw: ${e && e.message}`);
    }
    await __report({ checks: __checks, errors: __errors });
  });
})();
