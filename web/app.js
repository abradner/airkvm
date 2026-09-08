// airkvm console.
//
// No build step, no framework, no CDN: the dongle serves everything, because a
// machine you walked up to may be on a network with no internet (AGENTS.md §4).
//
// Verified against real hardware 2026-09-04 (keystrokes typed on a macOS
// target). The wake path below is newer and has not been.

"use strict";

const FRAME = {
  KEY_DOWN: 0x01,
  KEY_UP: 0x02,
  MOUSE_MOVE: 0x03,
  MOUSE_BUTTON: 0x04,
  MOUSE_SCROLL: 0x05,
  RELEASE_ALL: 0x06,
  PING: 0x07,
  WAKE: 0x08,
  STATUS_REQ: 0x09,
};

// Dongle → console frames: same shape, bit 7 set (protocol.h).
const DOWN = {
  WAKE_RESULT: 0x80,
  STATUS: 0x81,
};

// Result byte of WAKE_RESULT, keyed by the values in protocol.h's
// airkvm_wake_result so a reordered enum cannot silently relabel them.
// "signalled" is not "woke": the dongle can only report that it sent the pulse.
const WAKE_RESULT = {
  0: ["wake signalled — watch the target", "ok"],
  1: ["can't wake: no USB host enumerated", "bad"],
  2: ["target's USB bus is not suspended — already awake?", ""],
  3: ["target didn't arm USB wakeup before sleeping — check its OS/firmware wake settings", "bad"],
};

// A wake outcome is a moment, not a state: the status line goes back to
// describing the connection after this long, and a request nobody answered
// says so instead of sitting there as "requested…" forever.
const WAKE_STATUS_MS = 5000;
// How long to wait for the dongle's WAKE_RESULT before saying none came.
const WAKE_REPLY_MS = 2000;
let wakeStatusTimer = null;
let wakeReplyTimer = null;

function setWakeStatus(text, kind) {
  setStatus(text, kind);
  clearTimeout(wakeStatusTimer);
  wakeStatusTimer = setTimeout(() => {
    if (socket && socket.readyState === WebSocket.OPEN) {
      setStatus("connected", "ok");
    }
  }, WAKE_STATUS_MS);
}

// `?ws=host:port` points the console at a dongle while serving this page from
// somewhere else (`python3 -m http.server` in web/) for UI-only work.
const wsOverride = new URLSearchParams(location.search).get("ws");
// Scheme follows the page: a wss page may not open a ws socket, and browsers
// block that as mixed content with an error that says nothing about the cause.
const wsScheme = location.protocol === "https:" ? "wss" : "ws";
const wsUrl = wsOverride
  ? `${wsScheme}://${wsOverride}/ws`
  : `${wsScheme}://${location.host}/ws`;

const el = {
  status: document.getElementById("status"),
  stage: document.getElementById("stage"),
  overlay: document.getElementById("overlay"),
  video: document.getElementById("video"),
  camera: document.getElementById("camera"),
  startVideo: document.getElementById("start-video"),
  pasteText: document.getElementById("paste-text"),
  sendText: document.getElementById("send-text"),
  sensitivity: document.getElementById("sensitivity"),
  releaseAll: document.getElementById("release-all"),
  wakeTarget: document.getElementById("wake-target"),
  target: document.getElementById("target-state"),
  mode: document.getElementById("mode"),
  touchpad: document.getElementById("touchpad"),
  more: document.getElementById("more"),
};

// --- input mode -------------------------------------------------------------
//
// One mode per page load, chosen here and never changed. `direct` is pointer
// lock plus KeyboardEvent.code — the path verified on hardware. `soft` is a
// touch surface plus characters through CHAR_MAP (docs/design/mobile-console.md).
// The inactive mode's listeners are never registered, so the two cannot both
// forward the same keystroke; that exclusion is structural, not a flag.
//
// Feature detection first: iOS and iPadOS Safari have no Pointer Lock at all,
// so `direct` could never capture there. The coarse-pointer clause catches
// Android Chrome, which exposes requestPointerLock on a touchscreen. A
// touchscreen laptop with a mouse primary stays `direct`. `?mode=` overrides
// for testing either UI on the other kind of device.
const MODE = (() => {
  const forced = new URLSearchParams(location.search).get("mode");
  if (forced === "direct" || forced === "soft") return forced;
  const hasLock = "requestPointerLock" in Element.prototype;
  const coarse = matchMedia("(pointer: coarse)").matches;
  return hasLock && !coarse ? "direct" : "soft";
})();
document.body.classList.toggle("soft", MODE === "soft");
el.mode.textContent = MODE === "soft" ? "touch" : "pointer lock";

let socket = null;
let reconnectDelay = 500;
let reconnectTimer = null;

function setStatus(text, kind) {
  el.status.textContent = text;
  el.status.className = `status ${kind || ""}`;
}

// The dongle tears the session down, releasing every held key, after roughly
// three seconds without a frame. Sending one PING per second is what tells it
// this console is still here during an idle moment — a page with nothing to say
// is indistinguishable from a laptop that was suspended mid-chord, and the
// dongle resolves that ambiguity in the safe direction.
const PING_INTERVAL_MS = 1000;
let pingTimer = null;

// Every other keepalive is a status poll instead of a bare PING: same
// liveness effect on the dongle, and the reply keeps the target row honest.
let pingCount = 0;
function startPinging() {
  stopPinging();
  pingTimer = setInterval(() => {
    pingCount += 1;
    send([pingCount % 2 ? FRAME.PING : FRAME.STATUS_REQ]);
  }, PING_INTERVAL_MS);
}

function stopPinging() {
  if (pingTimer !== null) {
    clearInterval(pingTimer);
    pingTimer = null;
  }
}

function connect() {
  socket = new WebSocket(wsUrl);
  socket.binaryType = "arraybuffer";

  socket.onopen = () => {
    reconnectDelay = 500;
    setStatus("connected", "ok");
    // Unknown until the first STATUS reply; a spectator session never gets
    // one, and must not inherit the previous owner's facts.
    el.target.textContent = "target: unknown";
    startPinging();
  };

  // Every terminal event releases: a stuck modifier on someone's machine is the
  // failure that matters, and a spurious release costs nothing (AGENTS.md §2).
  socket.onclose = () => {
    // A wake outcome pending from before the drop must not overwrite this:
    // the connection state is the one that matters now.
    clearTimeout(wakeReplyTimer);
    clearTimeout(wakeStatusTimer);
    el.target.textContent = "target: unknown";
    setStatus("disconnected — reconnecting…", "bad");
    stopPinging();
    exitCapture();
    socket = null;
    reconnectTimer = setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 10000);
  };

  socket.onerror = () => setStatus("connection error", "bad");

  socket.onmessage = (e) => {
    if (!(e.data instanceof ArrayBuffer)) return;
    const bytes = new Uint8Array(e.data);
    if (bytes.length === 4 && bytes[0] === DOWN.STATUS) {
      const [, mounted, suspended, media] = bytes;
      el.target.textContent = `target: ${
        !mounted
          ? "no USB host"
          : suspended
            ? "USB host suspended"
            : media
              ? "USB host awake, disk attached"
              : "USB host awake"
      }`;
      return;
    }
    if (bytes.length === 2 && bytes[0] === DOWN.WAKE_RESULT) {
      clearTimeout(wakeReplyTimer);
      const [text, kind] = WAKE_RESULT[bytes[1]] || [
        `wake: unknown result ${bytes[1]}`,
        "bad",
      ];
      setWakeStatus(text, kind);
    }
    // Anything else is a frame from a newer dongle than this page; ignored.
  };
}

function send(bytes) {
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(new Uint8Array(bytes));
    return true;
  }
  return false;
}

function sendKey(type, code) {
  const encoded = new TextEncoder().encode(code);
  if (encoded.length > 23) return false; // matches AIRKVM_MAX_CODE_LEN
  return send([type, encoded.length, ...encoded]);
}

function sendReleaseAll() {
  send([FRAME.RELEASE_ALL]);
}

// --- capture ---------------------------------------------------------------

let captured = false;

function enterCapture() {
  // Chrome 111+ returns a promise, older browsers return undefined. The usual
  // rejection is the re-lock cooldown right after an Esc exit — without this the
  // click just silently does nothing.
  const pending = el.stage.requestPointerLock();
  if (pending && typeof pending.catch === "function") {
    pending.catch(() => setStatus("capture refused — click again", "bad"));
  }
}

function exitCapture() {
  if (document.pointerLockElement) document.exitPointerLock();
  if (captured) {
    captured = false;
    el.overlay.hidden = false;
    sendReleaseAll();
  }
}

// Leaving the tab with keys held would leave them held on the target, because
// the browser stops delivering keyup once the page is hidden.
document.addEventListener("visibilitychange", () => {
  if (document.hidden) exitCapture();
});
window.addEventListener("blur", exitCapture);
window.addEventListener("pagehide", sendReleaseAll);

// Motion is accumulated and flushed once per animation frame rather than sent
// per event. Un-coalesced, a fast flick fills the dongle's HID queue, and a full
// queue used to trigger release-all — mouse movement tearing down a held chord.
// Firefox and Safari deliver mousemove at device rate, so this is not academic.
let pendingDx = 0;
let pendingDy = 0;
let flushScheduled = false;

function flushMotion() {
  flushScheduled = false;
  // Only whole units can be sent; the sub-pixel remainder stays in the
  // accumulator so slow movement adds up instead of rounding away to nothing.
  let ix = Math.trunc(pendingDx);
  let iy = Math.trunc(pendingDy);
  pendingDx -= ix;
  pendingDy -= iy;

  // A HID report carries one signed byte per axis. Anything larger goes as
  // successive frames rather than being clamped, so a fast flick travels its
  // full distance instead of silently under-shooting.
  while (ix !== 0 || iy !== 0) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      pendingDx = pendingDy = 0;
      return;
    }
    const dx = Math.max(-127, Math.min(127, ix));
    const dy = Math.max(-127, Math.min(127, iy));
    ix -= dx;
    iy -= dy;
    const buf = new ArrayBuffer(5);
    const view = new DataView(buf);
    view.setUint8(0, FRAME.MOUSE_MOVE);
    view.setInt16(1, dx, true);
    view.setInt16(3, dy, true);
    socket.send(buf);
  }
}

// Everything below registers only in direct mode (see MODE). Nothing here is
// reachable on a device without Pointer Lock, and in soft mode the same
// gestures arrive from the touch surface instead.
function installDirectInput() {
  document.addEventListener("pointerlockchange", () => {
    captured = document.pointerLockElement === el.stage;
    el.overlay.hidden = captured;
    if (!captured) sendReleaseAll();
  });

  el.stage.addEventListener("click", () => {
    if (!captured) enterCapture();
  });

  document.addEventListener("keydown", (e) => {
    if (!captured) return;
    e.preventDefault();
    // Esc is the release gesture and is not forwarded; the sidebar button sends it
    // to the target when that is what you actually want.
    if (e.code === "Escape") {
      exitCapture();
      return;
    }
    // `code` is the physical key position, which is what HID usage codes mean —
    // `key` would be layout- and shift-dependent and send the wrong key.
    sendKey(FRAME.KEY_DOWN, e.code);
  });

  document.addEventListener("keyup", (e) => {
    if (!captured) return;
    e.preventDefault();
    sendKey(FRAME.KEY_UP, e.code);
  });

  // --- mouse -----------------------------------------------------------------

  document.addEventListener("mousemove", (e) => {
    if (!captured) return;
    const scale = parseFloat(el.sensitivity.value);
    pendingDx += e.movementX * scale;
    pendingDy += e.movementY * scale;
    if (!flushScheduled) {
      flushScheduled = true;
      requestAnimationFrame(flushMotion);
    }
  });

  // Buttons go as an absolute mask, so a dropped frame self-corrects instead of
  // leaving a button held (protocol.h).
  function sendButtons(e) {
    if (!captured) return;
    e.preventDefault();
    const mask =
      (e.buttons & 1 ? 1 : 0) | (e.buttons & 2 ? 2 : 0) | (e.buttons & 4 ? 4 : 0);
    send([FRAME.MOUSE_BUTTON, mask]);
  }
  document.addEventListener("mousedown", sendButtons);
  document.addEventListener("mouseup", sendButtons);
  document.addEventListener("contextmenu", (e) => {
    if (captured) e.preventDefault();
  });

  document.addEventListener(
    "wheel",
    (e) => {
      if (!captured) return;
      e.preventDefault();
      const v = Math.max(-127, Math.min(127, -Math.sign(e.deltaY)));
      const h = Math.max(-127, Math.min(127, Math.sign(e.deltaX)));
      send([FRAME.MOUSE_SCROLL, v & 0xff, h & 0xff]);
    },
    { passive: false },
  );
}

// --- sidebar ---------------------------------------------------------------

document.querySelectorAll("[data-combo]").forEach((button) => {
  button.addEventListener("click", () => {
    const codes = button.dataset.combo.split("+");
    codes.forEach((code) => sendKey(FRAME.KEY_DOWN, code));
    // Released in reverse order, the way a human's hand does it: modifiers last.
    codes
      .slice()
      .reverse()
      .forEach((code) => sendKey(FRAME.KEY_UP, code));
  });
});

el.releaseAll.addEventListener("click", sendReleaseAll);

el.wakeTarget.addEventListener("click", () => {
  if (send([FRAME.WAKE])) {
    setStatus("wake requested…");
    // A spectator session (R-003) gets no answer at all, and "requested…"
    // must not sit there looking pending forever.
    clearTimeout(wakeReplyTimer);
    wakeReplyTimer = setTimeout(
      () => setWakeStatus("no reply to wake — not the active session?", "bad"),
      WAKE_REPLY_MS,
    );
  } else {
    setStatus("not connected", "bad");
  }
});

// Typing text is a convenience for URLs and long commands. Only the characters
// in this table are sent; anything else is reported rather than silently
// dropped, because "half my password went in" is worse than "nothing went in".
const SHIFT = 1;
const CHAR_MAP = (() => {
  const map = new Map();
  const add = (ch, code, shift) => map.set(ch, { code, shift: !!shift });
  "abcdefghijklmnopqrstuvwxyz".split("").forEach((c) => {
    add(c, `Key${c.toUpperCase()}`, false);
    add(c.toUpperCase(), `Key${c.toUpperCase()}`, SHIFT);
  });
  "0123456789".split("").forEach((d) => add(d, `Digit${d}`, false));
  const pairs = [
    [" ", "Space"], ["\n", "Enter"], ["\t", "Tab"],
    ["-", "Minus"], ["=", "Equal"], ["[", "BracketLeft"], ["]", "BracketRight"],
    ["\\", "Backslash"], [";", "Semicolon"], ["'", "Quote"], ["`", "Backquote"],
    [",", "Comma"], [".", "Period"], ["/", "Slash"],
  ];
  pairs.forEach(([ch, code]) => add(ch, code, false));
  const shifted = [
    ["!", "Digit1"], ["@", "Digit2"], ["#", "Digit3"], ["$", "Digit4"],
    ["%", "Digit5"], ["^", "Digit6"], ["&", "Digit7"], ["*", "Digit8"],
    ["(", "Digit9"], [")", "Digit0"], ["_", "Minus"], ["+", "Equal"],
    ["{", "BracketLeft"], ["}", "BracketRight"], ["|", "Backslash"],
    [":", "Semicolon"], ['"', "Quote"], ["~", "Backquote"], ["<", "Comma"],
    [">", "Period"], ["?", "Slash"],
  ];
  shifted.forEach(([ch, code]) => add(ch, code, SHIFT));
  return map;
})();

// Press one physical key, wrapped in Shift when asked. This is the one place
// a character becomes keystrokes, for the paste box and the soft keyboard
// alike; the wire sees exactly what the paste box always sent.
function pressKey(code, shift) {
  if (shift) sendKey(FRAME.KEY_DOWN, "ShiftLeft");
  sendKey(FRAME.KEY_DOWN, code);
  sendKey(FRAME.KEY_UP, code);
  if (shift) sendKey(FRAME.KEY_UP, "ShiftLeft");
}

// One character through CHAR_MAP. Unmapped is reported, never dropped.
function typeChar(ch) {
  const m = CHAR_MAP.get(ch);
  if (!m) {
    setStatus(`can't type: ${JSON.stringify(ch)}`, "bad");
    return false;
  }
  pressKey(m.code, m.shift);
  return true;
}

// The shifted characters above assume a US layout on the *target*: the target
// decides what a physical key position produces. Non-US targets will get the
// wrong punctuation, which is a known limitation, not a bug to fix here.
async function typeString(text) {
  const unsupported = [...new Set([...text].filter((c) => !CHAR_MAP.has(c)))];
  if (unsupported.length) {
    setStatus(`can't type: ${unsupported.join(" ")}`, "bad");
    return false;
  }
  for (const ch of text) {
    typeChar(ch);
    // Targets drop keystrokes that arrive faster than they poll; this is a guess
    // at a safe rate and wants measuring on real hardware.
    await new Promise((r) => setTimeout(r, 8));
  }
  return true;
}

el.sendText.addEventListener("click", async () => {
  const text = el.pasteText.value;
  if (await typeString(text)) {
    setStatus(`sent ${text.length} characters`, "ok");
  }
});

// --- local capture video ----------------------------------------------------

async function listCameras() {
  const devices = await navigator.mediaDevices.enumerateDevices();
  el.camera.innerHTML = "";
  devices
    .filter((d) => d.kind === "videoinput")
    .forEach((d) => {
      const option = document.createElement("option");
      option.value = d.deviceId;
      option.textContent = d.label || "capture device";
      el.camera.append(option);
    });
}

el.startVideo.addEventListener("click", async () => {
  try {
    const deviceId = el.camera.value;
    el.video.srcObject = await navigator.mediaDevices.getUserMedia({
      video: deviceId ? { deviceId: { exact: deviceId } } : true,
      audio: false,
    });
    // Labels are only populated after permission is granted, so refresh.
    await listCameras();
  } catch (err) {
    setStatus(`no video: ${err.message}`, "bad");
  }
});

// getUserMedia needs a secure context. The real firmware serves the console
// over HTTPS/WSS and does not serve plain HTTP at all (R-007), so this guard
// normally does not fire — but a CONFIG_AIRKVM_TLS=n bench build is plain HTTP
// on a LAN IP, which browsers do not treat as secure the way they do
// localhost. Saying so beats a silently dead button.
//
// (This comment used to claim the page was always served over plain HTTP,
// which stopped being true when TLS landed — principle 1.)
if (!navigator.mediaDevices) {
  el.startVideo.disabled = true;
  el.startVideo.title =
    "Video needs a secure context — this looks like a plain-HTTP bench build.";
} else {
  listCameras().catch(() => {});
}

// --- soft input: touch trackpad and soft keyboard --------------------------
//
// Registered only in soft mode (see MODE). There is no pointer lock anywhere
// in here: a finger on the surface is its own capture, which is why a phone
// dodges the Safari keyboard.lock() limitation rather than inheriting it.

// Starting values, NOT measured (AGENTS.md §2, principle 6). Tune against a
// real phone and a real target, then record what was measured in
// docs/design/mobile-console.md.
const TOUCH = {
  GAIN: 2, // finger px → mouse counts, times the sensitivity slider
  TAP_MAX_MS: 250, // down→up faster than this with little travel is a tap
  TAP_SLOP_PX: 10, // travel allowed inside a tap
  LONG_PRESS_MS: 400, // one still finger this long → left button held (drag)
  // Dwell between BUTTON 1 and BUTTON 0 of a synthesized click. The dongle's
  // report task drains every queued intent before emitting, and only keyboard
  // intents are guarded against being applied on top of a state whose report
  // has not gone out (hid_out_usb.c): two button frames drained in one pass
  // collapse into one report with nothing pressed, and the click is lost. A
  // human click outlasts a pass by orders of magnitude, which is why the
  // desktop path never sees this.
  TAP_CLICK_HOLD_MS: 40,
  SCROLL_PX_PER_NOTCH: 20, // finger travel per wheel notch
  SCROLL_MAX_PER_FLUSH: 3, // notches per animation frame; the rest carries
  SCROLL_NATURAL: true, // finger down → content down, as the phone itself scrolls
};

function installSoftInput() {
  const pad = el.touchpad;
  const pointers = new Map(); // pointerId → last seen position
  const touch = {
    // idle | pending (one finger, might be a tap) | move | drag (button held)
    // | two (two fingers, might be a right-click) | scroll
    phase: "idle",
    primary: null, // the first finger down; its travel is the scroll's
    startAt: 0,
    startX: 0,
    startY: 0,
    buttons: 0, // what this page believes the dongle holds
    longPress: null,
    scrollX: 0, // finger px accumulated toward the next notch
    scrollY: 0,
    pendingV: 0, // notches owed to the dongle
    pendingH: 0,
    scrollScheduled: false,
  };

  function setButtons(mask) {
    touch.buttons = mask;
    send([FRAME.MOUSE_BUTTON, mask]);
    pad.classList.toggle("dragging", mask !== 0);
  }

  function click(mask) {
    setButtons(mask);
    setTimeout(() => {
      // A gesture that began inside the dwell owns the mask now; its own end
      // path releases it.
      if (touch.phase === "idle" && touch.buttons !== 0) setButtons(0);
    }, TOUCH.TAP_CLICK_HOLD_MS);
  }

  function clearLongPress() {
    if (touch.longPress !== null) {
      clearTimeout(touch.longPress);
      touch.longPress = null;
    }
  }

  function startDrag() {
    touch.longPress = null;
    if (touch.phase !== "pending") return;
    touch.phase = "drag";
    setButtons(1);
  }

  function scheduleMotion() {
    if (!flushScheduled) {
      flushScheduled = true;
      requestAnimationFrame(flushMotion);
    }
  }

  // Scroll goes out at most once per animation frame, a few notches at a
  // time, with the remainder carried — never one frame per pointermove. The
  // dongle drops motion when its queue is full, but a full queue meeting a
  // key frame is a release-all, and two fingers must not be able to cause
  // that (net.c).
  function flushScroll() {
    touch.scrollScheduled = false;
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      touch.pendingV = touch.pendingH = 0;
      return;
    }
    const cap = TOUCH.SCROLL_MAX_PER_FLUSH;
    const clamp = (n) => Math.max(-cap, Math.min(cap, n));
    const v = clamp(touch.pendingV);
    const h = clamp(touch.pendingH);
    touch.pendingV -= v;
    touch.pendingH -= h;
    if (v || h) send([FRAME.MOUSE_SCROLL, v & 0xff, h & 0xff]);
    if (touch.pendingV || touch.pendingH) scheduleScroll();
  }

  function scheduleScroll() {
    if (!touch.scrollScheduled) {
      touch.scrollScheduled = true;
      requestAnimationFrame(flushScroll);
    }
  }

  // Every way a gesture can end comes through here, and every one of them
  // leaves the button mask at zero — a cancelled drag must not stay a drag.
  function endGesture(e, allowClick) {
    if (!pointers.delete(e.pointerId)) return; // already ended (pointerup then lostpointercapture)
    if (pointers.size > 0) return; // wait for the last finger
    clearLongPress();
    const quick = performance.now() - touch.startAt < TOUCH.TAP_MAX_MS;
    let tap = 0;
    if (allowClick && quick) {
      if (touch.phase === "pending") tap = 1;
      else if (touch.phase === "two") tap = 2;
    }
    touch.phase = "idle";
    touch.primary = null;
    touch.scrollX = touch.scrollY = 0;
    touch.pendingV = touch.pendingH = 0; // notches owed by a gesture that is over
    pad.classList.remove("scrolling");
    if (touch.buttons !== 0) {
      setButtons(0);
      // A second tap inside the first one's dwell: its down must not follow
      // that up in the same pass (issue #58), so it waits the dwell out too.
      if (tap) setTimeout(() => click(tap), TOUCH.TAP_CLICK_HOLD_MS);
      return;
    }
    if (tap) click(tap);
  }

  pad.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    // Keeps pointerup coming to the pad when the finger slides off it.
    // Throws NotFoundError for a pointer the browser no longer counts as
    // active; a gesture that cannot be captured is still tracked, and a
    // thrown handler would leave it untracked with nothing to release it.
    try {
      pad.setPointerCapture(e.pointerId);
    } catch (_) {
      /* tracked anyway */
    }
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (pointers.size === 1) {
      touch.phase = "pending";
      touch.primary = e.pointerId;
      touch.startAt = performance.now();
      touch.startX = e.clientX;
      touch.startY = e.clientY;
      touch.longPress = setTimeout(startDrag, TOUCH.LONG_PRESS_MS);
    } else if (
      pointers.size === 2 &&
      (touch.phase === "pending" || touch.phase === "move")
    ) {
      clearLongPress();
      touch.phase = "two";
      touch.scrollX = touch.scrollY = 0;
    }
    // A second finger during a drag, or a third finger: ignored, the gesture
    // in progress keeps going and ends when the last finger lifts.
  });

  pad.addEventListener("pointermove", (e) => {
    const p = pointers.get(e.pointerId);
    if (!p) return;
    // movementX/Y is always 0 on iOS Safari; take the difference ourselves.
    const dx = e.clientX - p.x;
    const dy = e.clientY - p.y;
    p.x = e.clientX;
    p.y = e.clientY;
    if (pointers.size === 1) {
      if (touch.phase === "pending") {
        const travel = Math.hypot(
          e.clientX - touch.startX,
          e.clientY - touch.startY,
        );
        if (travel < TOUCH.TAP_SLOP_PX) return; // still a candidate tap
        clearLongPress();
        touch.phase = "move";
      }
      if (touch.phase === "move" || touch.phase === "drag") {
        const scale = TOUCH.GAIN * parseFloat(el.sensitivity.value);
        pendingDx += dx * scale;
        pendingDy += dy * scale;
        scheduleMotion();
      }
      return;
    }
    if (touch.phase !== "two" && touch.phase !== "scroll") return;
    if (e.pointerId !== touch.primary) return; // one finger's travel, not both
    touch.scrollX += dx;
    touch.scrollY += dy;
    const nx = Math.trunc(touch.scrollX / TOUCH.SCROLL_PX_PER_NOTCH);
    const ny = Math.trunc(touch.scrollY / TOUCH.SCROLL_PX_PER_NOTCH);
    if (nx === 0 && ny === 0) return;
    touch.scrollX -= nx * TOUCH.SCROLL_PX_PER_NOTCH;
    touch.scrollY -= ny * TOUCH.SCROLL_PX_PER_NOTCH;
    touch.phase = "scroll";
    pad.classList.add("scrolling");
    // Wheel +1 is "content down" on the wire (the desktop path sends
    // -sign(deltaY)); a finger moving down drags the content down with it.
    touch.pendingV += TOUCH.SCROLL_NATURAL ? ny : -ny;
    touch.pendingH += TOUCH.SCROLL_NATURAL ? -nx : nx;
    scheduleScroll();
  });

  pad.addEventListener("pointerup", (e) => endGesture(e, true));
  // A cancelled gesture is dropped, never promoted to a click.
  pad.addEventListener("pointercancel", (e) => endGesture(e, false));
  pad.addEventListener("lostpointercapture", (e) => endGesture(e, false));
  pad.addEventListener("contextmenu", (e) => e.preventDefault());

  // touch-action: none is not enough on every iOS version: only a cancelled
  // touchmove reliably stops elastic overscroll and pinch-zoom.
  const swallow = (e) => e.preventDefault();
  pad.addEventListener("touchstart", swallow, { passive: false });
  pad.addEventListener("touchmove", swallow, { passive: false });

  // Size the page to the visible viewport, so the soft keyboard pushes the
  // trackpad and key rows up instead of covering them. Unverified on a real
  // phone: how iOS shifts a fixed body when the keyboard opens is the thing
  // to look at first.
  function fitViewport() {
    const vv = window.visualViewport;
    const root = document.documentElement.style;
    root.setProperty("--vvh", `${vv ? vv.height : window.innerHeight}px`);
    root.setProperty("--vvt", `${vv ? vv.offsetTop : 0}px`);
    window.scrollTo(0, 0);
  }
  if (window.visualViewport) {
    window.visualViewport.addEventListener("resize", fitViewport);
    window.visualViewport.addEventListener("scroll", fitViewport);
  }
  window.addEventListener("resize", fitViewport);
  fitViewport();

  // Open on the desktop (where the summary is hidden), folded on a phone.
  el.more.open = false;
}

if (MODE === "direct") {
  installDirectInput();
} else {
  installSoftInput();
}
connect();

// --- disk image over /ws-media --------------------------------------------
//
// The image never leaves this browser. The dongle presents an empty
// removable disk to the target; attaching a file here tells it the geometry,
// and from then on it asks for 16 KB chunks (MEDIA_CHUNK_SIZE in
// main/media_proto.h — this constant ships in the same firmware image as that
// one, so they cannot drift) as the target reads them. Slices are read
// lazily, so a multi-gigabyte ISO costs nothing until it is read.
//
// Its own socket on its own port, so a chunk in flight never sits in front of
// a keystroke on the input socket's task (main/media_ws.h).

const MEDIA = {
  ATTACH: 0x10,
  DETACH: 0x11,
  DATA: 0x12,
  ERROR: 0x13,
  REQ: 0x90,
  STATUS: 0x91,
  CHUNK: 16384,
  BLOCK: 512,
  PORT_SECURE: 8443,
  PORT_INSECURE: 8080,
  STATE: [
    "no image attached",
    "attached",
    "another client holds the medium",
    "refused: not enough memory on the dongle to hold the cache and keep the console",
  ],
  ERR_OUT_OF_RANGE: 1,
  ERR_READ_FAILED: 2,
};

const mediaEl = {
  file: document.getElementById("media-file"),
  attach: document.getElementById("media-attach"),
  eject: document.getElementById("media-eject"),
  status: document.getElementById("media-status"),
};

// Host follows the console's host (or the ?ws= override); only the port
// differs, and the scheme follows the page like the input socket does.
const mediaHost = (wsOverride || location.host).replace(/:\d+$/, "");
const mediaUrl = `${wsScheme}://${mediaHost}:${
  wsScheme === "wss" ? MEDIA.PORT_SECURE : MEDIA.PORT_INSECURE
}/ws-media`;

let mediaSocket = null;
let mediaReconnectDelay = 1000;
let mediaFile = null; // what the file input holds
let attachedFile = null; // what the dongle was told about — served from this
let mediaServed = 0;

function setMediaStatus(text, kind) {
  mediaEl.status.textContent = text;
  mediaEl.status.className = `status ${kind || ""}`;
}

function mediaSend(bytes) {
  if (mediaSocket && mediaSocket.readyState === WebSocket.OPEN) {
    mediaSocket.send(bytes);
    return true;
  }
  return false;
}

function encodeAttach(file) {
  const name = new TextEncoder().encode(file.name).slice(0, 31);
  const buf = new ArrayBuffer(13 + name.length);
  const v = new DataView(buf);
  v.setUint8(0, MEDIA.ATTACH);
  v.setUint16(1, MEDIA.BLOCK, true);
  // The target sees whole blocks; the tail of the last one is zero-padded on
  // the way out (see serveChunk).
  v.setBigUint64(3, BigInt(Math.ceil(file.size / MEDIA.BLOCK)), true);
  v.setUint8(11, 0x01); // read-only
  v.setUint8(12, name.length);
  new Uint8Array(buf, 13).set(name);
  return buf;
}

async function serveChunk(reqId, chunk) {
  // Always the file the dongle was told about, never whatever the input
  // holds now: changing the selection must not serve a different image under
  // the old geometry.
  const file = attachedFile;
  if (!file) return;
  const start = chunk * MEDIA.CHUNK;
  if (start >= Math.ceil(file.size / MEDIA.BLOCK) * MEDIA.BLOCK) {
    mediaSend(encodeError(reqId, MEDIA.ERR_OUT_OF_RANGE));
    return;
  }
  let bytes;
  try {
    // Blob.arrayBuffer: Safari 14+, Chrome 76+, Firefox 69+. Lazy: only this
    // slice is read.
    bytes = new Uint8Array(
      await file.slice(start, start + MEDIA.CHUNK).arrayBuffer(),
    );
  } catch (err) {
    // The file went away (unplugged drive, revoked handle).
    mediaSend(encodeError(reqId, MEDIA.ERR_READ_FAILED));
    setMediaStatus(`read failed: ${err.message}`, "bad");
    return;
  }
  // Always a full chunk: the dongle's cache completes on the last byte, and
  // bytes past the image are zeros, which is what the padded last block is.
  const out = new ArrayBuffer(15 + MEDIA.CHUNK);
  const v = new DataView(out);
  v.setUint8(0, MEDIA.DATA);
  v.setUint32(1, reqId, true);
  v.setUint32(5, chunk, true);
  v.setUint32(9, 0, true);
  v.setUint16(13, MEDIA.CHUNK, true);
  new Uint8Array(out, 15).set(bytes);
  mediaSend(out);
  mediaServed += bytes.length;
}

function encodeError(reqId, code) {
  const buf = new ArrayBuffer(6);
  const v = new DataView(buf);
  v.setUint8(0, MEDIA.ERROR);
  v.setUint32(1, reqId, true);
  v.setUint8(5, code);
  return buf;
}

// Connected on demand, never at page load: every open console tab holding a
// media socket would be one more TLS session on the dongle's heap for a
// feature the tab may never use.
function mediaConnect() {
  if (mediaSocket) return;
  mediaSocket = new WebSocket(mediaUrl);
  mediaSocket.binaryType = "arraybuffer";
  mediaSocket.onopen = () => {
    mediaReconnectDelay = 1000;
    mediaEl.attach.disabled = !mediaFile;
    if (mediaFile) setMediaStatus("ready to attach");
  };
  mediaSocket.onclose = () => {
    // The dongle drops the medium when this socket closes (its release-all
    // for storage), so the page must not pretend otherwise — and it must not
    // re-attach on its own: the operator chose to attach, and the dongle may
    // have dropped the medium for a reason.
    attachedFile = null;
    mediaEl.attach.disabled = true;
    mediaEl.eject.disabled = true;
    mediaEl.file.disabled = false;
    mediaSocket = null;
    // Reconnect only while there is a file to serve, and back off: the
    // dongle has one media slot, and a refused connect is a TLS handshake it
    // paid for. Nothing to serve means nothing to reconnect for.
    if (mediaFile) {
      setMediaStatus(
        `media socket closed — retrying in ${mediaReconnectDelay / 1000}s`,
        "bad",
      );
      setTimeout(mediaConnect, mediaReconnectDelay);
      mediaReconnectDelay = Math.min(mediaReconnectDelay * 2, 30000);
    } else {
      setMediaStatus("no image");
    }
  };
  mediaSocket.onmessage = (e) => {
    if (!(e.data instanceof ArrayBuffer)) return;
    const v = new DataView(e.data);
    const type = v.getUint8(0);
    if (type === MEDIA.REQ && e.data.byteLength === 9) {
      serveChunk(v.getUint32(1, true), v.getUint32(5, true));
    } else if (type === MEDIA.STATUS && e.data.byteLength === 10) {
      const state = v.getUint8(1);
      const served = v.getUint32(2, true);
      const attached = state === 1;
      if (!attached) attachedFile = null;
      mediaEl.eject.disabled = !attached;
      mediaEl.attach.disabled = attached || !mediaFile;
      // While attached the selection is frozen: see serveChunk.
      mediaEl.file.disabled = attached;
      setMediaStatus(
        attached
          ? `attached: ${attachedFile ? attachedFile.name : "?"} — ${(served / 1048576).toFixed(1)} MB served`
          : MEDIA.STATE[state] || `state ${state}`,
        attached ? "ok" : state >= 2 ? "bad" : "",
      );
    }
  };
}

mediaEl.file.addEventListener("change", () => {
  mediaFile = mediaEl.file.files[0] || null;
  mediaServed = 0;
  if (mediaFile) mediaConnect();
  mediaEl.attach.disabled =
    !mediaFile || !mediaSocket || mediaSocket.readyState !== WebSocket.OPEN;
  setMediaStatus(
    mediaFile
      ? `${mediaFile.name}: ${(mediaFile.size / 1048576).toFixed(1)} MB, not attached`
      : "no image",
  );
});

mediaEl.attach.addEventListener("click", () => {
  if (!mediaFile) return;
  // Snapshot now; the STATUS reply confirms or clears it.
  attachedFile = mediaFile;
  if (mediaSend(encodeAttach(mediaFile))) setMediaStatus("attaching…");
});

mediaEl.eject.addEventListener("click", () => {
  mediaSend(new Uint8Array([MEDIA.DETACH]));
});

// DataView.setBigUint64 (encodeAttach) is Safari 15+. Older Safari gets a
// clear message instead of a silent no-op.
if (typeof DataView.prototype.setBigUint64 !== "function") {
  mediaEl.file.disabled = true;
  setMediaStatus("disk images need a newer browser (BigInt DataView)", "bad");
}
