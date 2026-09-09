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
// somewhere else (`python3 -m http.server` in packages/console/) for UI-only work.
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
  keyrows: document.getElementById("keyrows"),
  macros: document.getElementById("macros"),
  release: document.getElementById("release"),
  softKbd: document.getElementById("soft-kbd"),
  softInput: document.getElementById("soft-input"),
  settings: document.getElementById("settings"),
  settingsOpen: document.getElementById("settings-open"),
  settingsStatus: document.getElementById("settings-status"),
  deviceDetails: document.getElementById("device-details"),
  deviceSettings: document.getElementById("device-settings"),
  settingsSens: document.getElementById("settings-sensitivity"),
  settingsSensOut: document.getElementById("settings-sensitivity-out"),
  settingsMode: document.getElementById("settings-mode"),
  settingsModeDirect: document.getElementById("settings-mode-direct"),
  settingsModeSoft: document.getElementById("settings-mode-soft"),
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
  cancelTyping();
  clearLatch();
  send([FRAME.RELEASE_ALL]);
}

// --- latched modifiers ------------------------------------------------------
//
// A finger cannot hold Ctrl while it taps C, so on-screen modifiers latch: tap
// one and it goes down and stays down until the next non-modifier key has been
// pressed, or until it is tapped again. A long press locks it until tapped.
//
// This is a view of what the page believes the dongle holds, and there is no
// way to read that back, so every release-all — the button, a socket close, the
// page going hidden — is authoritative and clears it without sending anything
// (docs/design/mobile-console.md). Nothing here is ever restored across a
// reconnect: that would re-press keys the dongle deliberately released.
//
// Always empty in direct mode; the calls below are no-ops there.
const LATCH_LOCK_MS = 500; // NOT measured (principle 6)
const latch = { held: [], locked: new Set() }; // held: codes in press order

function repaintLatch() {
  document.querySelectorAll("[data-mod]").forEach((b) => {
    b.classList.toggle("latched", latch.held.includes(b.dataset.mod));
    b.classList.toggle("locked", latch.locked.has(b.dataset.mod));
  });
}

// Forget, without sending: the dongle has already released, or is about to.
function clearLatch() {
  if (latch.held.length === 0 && latch.locked.size === 0) return;
  latch.held = [];
  latch.locked.clear();
  repaintLatch();
}

// After a non-modifier key: every latched-but-not-locked modifier comes up, in
// reverse order, the way a hand lets go.
function releaseLatched() {
  if (latch.held.length === 0) return;
  const keep = [];
  latch.held
    .slice()
    .reverse()
    .forEach((code) => {
      if (latch.locked.has(code)) keep.unshift(code);
      else sendKey(FRAME.KEY_UP, code);
    });
  latch.held = keep;
  repaintLatch();
}

// The latch records what was sent, not what was asked for: a tap while the
// socket is down highlights nothing, because the dongle holds nothing.
function toggleLatch(code) {
  const i = latch.held.indexOf(code);
  if (i >= 0) {
    sendKey(FRAME.KEY_UP, code);
    latch.held.splice(i, 1);
    latch.locked.delete(code);
  } else if (sendKey(FRAME.KEY_DOWN, code)) {
    latch.held.push(code);
  }
  repaintLatch();
}

function lockLatch(code) {
  if (!latch.held.includes(code)) {
    if (!sendKey(FRAME.KEY_DOWN, code)) return;
    latch.held.push(code);
  }
  latch.locked.add(code);
  repaintLatch();
}

// A combo that released a key the latch was holding (a locked Ctrl under
// Ctrl+Alt+Del) has un-held it; the latch must say so.
function forgetReleased(codes) {
  codes.forEach((code) => {
    const i = latch.held.indexOf(code);
    if (i >= 0) latch.held.splice(i, 1);
    latch.locked.delete(code);
  });
  repaintLatch();
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
  cancelTyping();
  clearLatch();
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
    if (!captured || settingsIsOpen()) return;
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
    if (!captured || settingsIsOpen()) return;
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
    forgetReleased(codes);
    releaseLatched();
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
  // A latched Shift is already down; wrapping it again would release it early.
  const wrap = shift && !latch.held.includes("ShiftLeft");
  if (wrap) sendKey(FRAME.KEY_DOWN, "ShiftLeft");
  sendKey(FRAME.KEY_DOWN, code);
  sendKey(FRAME.KEY_UP, code);
  if (wrap) sendKey(FRAME.KEY_UP, "ShiftLeft");
  releaseLatched();
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
// Strings go out one at a time, in order: a second string enqueued while one
// is still being paced waits for it, and single characters typed meanwhile
// (typeLater) queue behind both. Without this a composed word and the
// keystroke that committed it would interleave on the wire.
let typeChain = Promise.resolve();
let typing = 0;
// Bumped by cancelTyping(): a paced string checks it between characters and
// stops. Release-all means "nothing more reaches the target from what was
// queued" — a paste that carried on typing after the page went hidden, or
// after a reconnect, would be the console typing on its own (principle 4).
let typeGen = 0;

function cancelTyping() {
  typeGen += 1;
  typeChain = Promise.resolve();
  typing = 0;
}

function typeString(text) {
  const unsupported = [...new Set([...text].filter((c) => !CHAR_MAP.has(c)))];
  if (unsupported.length) {
    setStatus(`can't type: ${unsupported.join(" ")}`, "bad");
    return Promise.resolve(false);
  }
  const gen = typeGen;
  typing += 1;
  const run = typeChain.then(async () => {
    for (const ch of text) {
      if (gen !== typeGen) return false; // cancelled: report, never resume
      typeChar(ch);
      // Targets drop keystrokes that arrive faster than they poll; this is a
      // guess at a safe rate and wants measuring on real hardware.
      await new Promise((r) => setTimeout(r, 8));
    }
    return gen === typeGen;
  });
  typeChain = run.finally(() => {
    if (gen === typeGen) typing -= 1;
  });
  return run;
}

// One character or key, now if nothing is being paced, else after it.
function typeLater(fn) {
  if (typing === 0) fn();
  else typeChain = typeChain.then(fn);
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
  // has not gone out (hid_out_usb.c, issue #58): two button frames drained in one pass
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
    cancelled: false, // a finger was cancelled: this gesture can never click
    clickTimers: new Set(), // dwell releases and deferred clicks in flight
  };

  function setButtons(mask) {
    touch.buttons = mask;
    send([FRAME.MOUSE_BUTTON, mask]);
    pad.classList.toggle("dragging", mask !== 0);
  }

  // Timers here are tracked so a loss of confidence can cancel them: a
  // deferred click firing after release-all would press a button on a page
  // that has just promised it holds nothing.
  function after(ms, fn) {
    const t = setTimeout(() => {
      touch.clickTimers.delete(t);
      fn();
    }, ms);
    touch.clickTimers.add(t);
  }

  function cancelClickTimers() {
    touch.clickTimers.forEach((t) => clearTimeout(t));
    touch.clickTimers.clear();
  }

  function click(mask) {
    setButtons(mask);
    after(TOUCH.TAP_CLICK_HOLD_MS, () => {
      // A gesture that began inside the dwell owns the mask now; its own end
      // path releases it.
      if (touch.phase === "idle" && touch.buttons !== 0) setButtons(0);
    });
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
    finishGesture(allowClick);
  }

  // Lost confidence (page hidden, window blurred): whatever fingers the page
  // thinks are down, it is no longer being told about them.
  function abortGesture() {
    pointers.clear();
    cancelClickTimers();
    finishGesture(false);
  }

  function finishGesture(allowClick) {
    clearLongPress();
    const quick = performance.now() - touch.startAt < TOUCH.TAP_MAX_MS;
    let tap = 0;
    if (allowClick && quick && !touch.cancelled) {
      if (touch.phase === "pending") tap = 1;
      else if (touch.phase === "two") tap = 2;
    }
    touch.phase = "idle";
    touch.primary = null;
    touch.cancelled = false;
    touch.scrollX = touch.scrollY = 0;
    touch.pendingV = touch.pendingH = 0; // notches owed by a gesture that is over
    pad.classList.remove("scrolling");
    if (touch.buttons !== 0) {
      setButtons(0);
      // A second tap inside the first one's dwell: its down must not follow
      // that up in the same pass (issue #58), so it waits the dwell out too.
      if (tap) after(TOUCH.TAP_CLICK_HOLD_MS, () => click(tap));
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
  // A cancelled gesture is dropped, never promoted to a click — and one
  // cancelled finger taints the whole gesture, so the other finger's clean
  // lift cannot turn a broken two-finger touch into a right-click.
  const cancel = (e) => {
    if (pointers.has(e.pointerId)) touch.cancelled = true;
    endGesture(e, false);
  };
  pad.addEventListener("pointercancel", cancel);
  pad.addEventListener("lostpointercapture", cancel);
  pad.addEventListener("contextmenu", (e) => e.preventDefault());

  // touch-action: none is not enough on every iOS version: only a cancelled
  // touchmove reliably stops elastic overscroll and pinch-zoom.
  const swallow = (e) => e.preventDefault();
  pad.addEventListener("touchstart", swallow, { passive: false });
  pad.addEventListener("touchmove", swallow, { passive: false });

  // Size the page to the visible viewport, so the soft keyboard pushes the
  // trackpad and key rows up instead of covering them. Driven by resize only:
  // a first version also re-fitted on every visualViewport scroll and forced
  // the window back to the top, and on the phone (2026-09-08) that fought the
  // page's own scrolling when "More" was open — every drag, either direction,
  // scrolled up. How iOS shifts a fixed body while the keyboard is open is
  // still to be looked at on the device.
  function fitViewport() {
    const vv = window.visualViewport;
    const root = document.documentElement.style;
    root.setProperty("--vvh", `${vv ? vv.height : window.innerHeight}px`);
    root.setProperty("--vvt", `${vv ? vv.offsetTop : 0}px`);
  }
  if (window.visualViewport) {
    window.visualViewport.addEventListener("resize", fitViewport);
  }
  window.addEventListener("resize", fitViewport);
  fitViewport();

  // Open on the desktop (where the summary is hidden), folded on a phone.
  el.more.open = false;

  // Nothing on this page scrolls except the "More" sheet and the text boxes.
  // A finger dragging anywhere else must not reach Safari, whose answer to a
  // downward drag on a page that cannot scroll is pull-to-refresh — which
  // reloads the console mid-session (seen on the phone, 2026-09-08).
  document.addEventListener(
    "touchmove",
    (e) => {
      if (!e.target.closest(".more[open], textarea, dialog")) e.preventDefault();
    },
    { passive: false },
  );

  // --- the phone's keyboard -------------------------------------------------
  //
  // A soft keyboard reports no physical key position: keydown arrives with an
  // empty `code` and keyCode 229 (issue #11), so the direct path has nothing to
  // send. What it does report, reliably, is the edit it is about to make —
  // `beforeinput` says insertText "a", deleteContentBackward, insertLineBreak —
  // and that is enough: each character goes through CHAR_MAP to a physical
  // position, exactly as the paste box has always done, and the edit itself is
  // cancelled so the box never fills. Cancelling it is also what keeps the two
  // paths from double-typing: the `input` handler below only ever restores the
  // box and reports what it found; it never sends.
  //
  // The box always holds exactly one zero-width space with the caret after it.
  // Empty, Backspace produces no beforeinput at all; a space would trip the
  // double-space-to-period rewrite and type a Backspace on the target.
  const SENTINEL = "\u200b";
  const box = el.softInput;
  let composing = false; // inside an IME composition: nothing is sent until it commits
  let justComposed = false; // the input event that trails a commit is not a leak

  function resetBox() {
    box.value = SENTINEL;
    box.setSelectionRange(1, 1);
  }

  // focus() is honoured on iOS only inside a user gesture: this button is it.
  el.softKbd.addEventListener("click", () => {
    resetBox();
    box.focus();
  });
  box.addEventListener("focus", () => el.softKbd.classList.add("active"));
  box.addEventListener("blur", () => el.softKbd.classList.remove("active"));

  box.addEventListener("beforeinput", (e) => {
    if (composing) return; // let the IME work; compositionend sends the result
    if (settingsIsOpen()) return; // the dialog owns input now; nothing is sent
    e.preventDefault();
    justComposed = false;
    switch (e.inputType) {
      case "insertText": {
        // Predictive taps arrive as several characters at once; like the
        // paste box, all of them or none, never half a word.
        const text = e.data || "";
        const bad = [...new Set([...text].filter((c) => !CHAR_MAP.has(c)))];
        if (bad.length) {
          setStatus(`can't type: ${bad.join(" ")}`, "bad");
          break;
        }
        if (text.length === 1) typeLater(() => typeChar(text));
        else typeString(text); // a predictive-text commit: paced like a paste
        break;
      }
      case "insertLineBreak":
      case "insertParagraph":
        typeLater(() => typeChar("\n"));
        break;
      case "deleteContentBackward":
        typeLater(() => pressKey("Backspace", false));
        break;
      case "deleteContentForward":
        typeLater(() => pressKey("Delete", false));
        break;
      case "insertFromPaste":
      case "insertFromDrop": {
        const text =
          e.data ?? (e.dataTransfer ? e.dataTransfer.getData("text/plain") : "");
        typeString(text); // paced, and refused whole if any character is unmapped
        break;
      }
      default:
        // deleteWordBackward, insertReplacementText (an autocorrect that got
        // through), and whatever else: said out loud rather than guessed at.
        setStatus(`unsupported edit: ${e.inputType}`, "bad");
    }
  });

  // Restores the sentinel and reports anything that reached the box without
  // going through beforeinput. Never types.
  box.addEventListener("input", () => {
    if (composing) return;
    const trailing = justComposed;
    justComposed = false;
    if (box.value === SENTINEL) return;
    const leaked = box.value.split(SENTINEL).join("");
    if (leaked && !trailing) {
      setStatus(`dropped ${leaked.length} character(s) the keyboard slipped past`, "bad");
    }
    resetBox();
  });

  // Android keyboards compose whole words and commit them at once, so the
  // target sees the word when the phone does — a latency to measure, not to
  // assume. CJK commits are unmapped and refused by typeString, by name.
  box.addEventListener("compositionstart", () => {
    composing = true;
  });
  box.addEventListener("compositionend", (e) => {
    composing = false;
    justComposed = true;
    if (e.data) typeString(e.data);
    resetBox();
  });

  // Tapping an on-screen key must not move focus off the box, or the phone's
  // keyboard dismisses on every Esc. Cancelling pointerdown suppresses the
  // mousedown that would move focus; click still fires. Unverified on iOS —
  // if the keyboard still drops, the fallback is touchend + preventDefault
  // with the action invoked from there.
  for (const host of [el.keyrows, el.macros, el.release]) {
    host.addEventListener("pointerdown", (e) => {
      if (e.target.closest("button")) e.preventDefault();
    });
  }

  // Long-press timers, so losing confidence can cancel them: a finger on
  // Ctrl when the page goes hidden must not press Ctrl half a second later
  // on a page that has already sent release-all.
  const modTimers = new Map();
  function cancelModTimers() {
    modTimers.forEach((t) => clearTimeout(t));
    modTimers.clear();
  }
  el.keyrows.querySelectorAll("[data-mod]").forEach((b) => {
    const code = b.dataset.mod;
    // Modifiers run on pointer events, never on click, so cancelling
    // touchstart costs nothing here — and it is what stops iOS treating the
    // long press as a text selection when CSS alone does not.
    b.addEventListener("touchstart", (e) => e.preventDefault(), {
      passive: false,
    });
    b.addEventListener("pointerdown", () => {
      clearTimeout(modTimers.get(code));
      modTimers.set(
        code,
        setTimeout(() => {
          modTimers.delete(code);
          lockLatch(code);
        }, LATCH_LOCK_MS),
      );
    });
    b.addEventListener("pointerup", () => {
      if (!modTimers.has(code)) return; // the long press already locked it
      clearTimeout(modTimers.get(code));
      modTimers.delete(code);
      toggleLatch(code);
    });
    b.addEventListener("pointercancel", () => {
      clearTimeout(modTimers.get(code));
      modTimers.delete(code);
    });
  });

  // Lost confidence in soft mode: exitCapture() releases nothing here (nothing
  // is captured), so the release has to be explicit. The dongle would release
  // on its own three seconds later; this page does not make it wait.
  //
  // Coming back is the other half of being honest about it. A backgrounded
  // phone throttles the keepalive, so the dongle has ended the session; an
  // open socket needs one frame to claim it again (net.c re-claims on any
  // frame from the owner), and a closed one should not sit out its backoff.
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      cancelModTimers();
      abortGesture();
      sendReleaseAll();
      return;
    }
    if (socket && socket.readyState === WebSocket.OPEN) {
      send([FRAME.STATUS_REQ]);
    } else if (socket === null) {
      clearTimeout(reconnectTimer);
      reconnectDelay = 500;
      setStatus("back in view — reconnecting", "bad");
      connect();
    }
    // A socket still opening or closing runs its course; onclose reconnects.
  });
  window.addEventListener("blur", () => {
    cancelModTimers();
    abortGesture();
    sendReleaseAll();
  });
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

// --- settings -------------------------------------------------------------------
//
// The device describes itself and this renders what it is told: identity and
// features into the details list, and each setting into a control chosen by
// its declared type. A setting added in a later image needs no change here, and
// a board that advertises no themes gets no theme control — the OLED dongle is
// never offered a setting that provably does nothing.
//
// Opening the dialog RELEASES CAPTURE FIRST, in both input modes, and the key
// handlers are gated on settingsIsOpen() as well. Neither alone is enough. In
// direct mode a character typed into a field bubbles to the document handler;
// with `captured` still true it would be sent to the target while
// preventDefault() suppressed the local insertion — a field that will not
// accept text, and the operator's input landing in the target's login prompt.
// In soft mode there is no pointer lock, so `captured` is already false and the
// hazard is the hidden textarea's beforeinput path instead. Every terminal
// event releases (AGENTS.md §2, principle 4); opening a dialog is one.
//
// The gate reads the dialog's own `open` state rather than a boolean of its
// own. A first version kept one and cleared it on the `close` event — which
// the browser queues as a task, so the flag lagged the dialog by a tick. Two
// copies of one fact, and the copy was the stale one (AGENTS.md §8, the
// deletion test). The headless check caught it.

function settingsIsOpen() {
  return el.settings.open;
}

function settingsStatus(text, kind) {
  el.settingsStatus.textContent = text;
  el.settingsStatus.className = `status${kind ? ` ${kind}` : ""}`;
}

function detail(dl, term, value) {
  const dt = document.createElement("dt");
  dt.textContent = term;
  const dd = document.createElement("dd");
  dd.textContent = value;
  dl.append(dt, dd);
}

function renderDetails(doc) {
  const dl = el.deviceDetails;
  dl.replaceChildren();
  const id = doc.identity || {};
  const f = doc.features || {};
  detail(dl, "Board", id.board || "unknown");
  detail(dl, "Chip", `${id.chip || "?"}, ${id.flash_mb || "?"} MB flash`);
  detail(dl, "Firmware", `${id.fw || "?"} on ESP-IDF ${id.idf || "?"}`);
  detail(
    dl,
    "Panel",
    f.panel
      ? `${f.panel} ${f.panel_w}\u00d7${f.panel_h}${f.palette ? ", colour" : ", mono"}${f.backlight ? ", backlight" : ""}`
      : "none",
  );
  detail(dl, "LED", f.led || "none");
  detail(dl, "USB", f.usb_device ? "keyboard, mouse, disk" : "none (bench board)");
  detail(dl, "microSD", f.sdcard ? "slot" : "none");
}

// One control per declared type. The value the device holds is what is shown;
// the reply to a change is the whole document, and the panel re-renders from
// that rather than from what it asked for.
function renderSetting(s) {
  const wrap = document.createElement("div");
  const label = document.createElement("label");
  const id = `setting-${s.key}`;
  label.htmlFor = id;
  label.textContent = s.label || s.key;
  wrap.append(label);
  let input;
  if (s.type === "enum") {
    input = document.createElement("select");
    for (const v of s.values || []) {
      const opt = document.createElement("option");
      opt.value = v;
      opt.textContent = v;
      opt.selected = v === s.value;
      input.append(opt);
    }
  } else if (s.type === "range") {
    input = document.createElement("input");
    input.type = "range";
    input.min = s.min;
    input.max = s.max;
    input.step = s.step || 1;
    input.value = s.value;
    const out = document.createElement("output");
    out.textContent = `${s.value}${s.unit || ""}`;
    label.append(" ", out);
    input.addEventListener("input", () => {
      out.textContent = `${input.value}${s.unit || ""}`;
    });
  } else if (s.type === "bool") {
    input = document.createElement("input");
    input.type = "checkbox";
    input.checked = Boolean(s.value);
  } else {
    // A type this page does not know: show it, do not offer to change it.
    const dd = document.createElement("span");
    dd.className = "status";
    dd.textContent = `${s.value} (not editable here)`;
    wrap.append(dd);
    return wrap;
  }
  input.id = id;
  input.addEventListener("change", () => {
    const value = s.type === "bool" ? (input.checked ? "1" : "0") : input.value;
    postSetting(s.key, value);
  });
  wrap.append(input);
  // A setting that takes effect at the next boot says so, and offers the
  // restart only while the stored value differs from the running one — the
  // device refuses a bare restart anyway (R-010); this is the page agreeing.
  if (s.apply === "reboot") {
    const note = document.createElement("p");
    note.className = "scope-note";
    if (s.running !== undefined && s.running !== s.value) {
      note.textContent = `Running: ${s.running}. Takes effect after a restart. `;
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "danger";
      btn.textContent = "Restart now";
      btn.addEventListener("click", () => postSetting("restart", "1"));
      note.append(btn);
    } else {
      note.textContent = "Takes effect after a restart.";
    }
    wrap.append(note);
  }
  return wrap;
}

// Feature flags the page acts on outside the dialog. Fetched once at load —
// PR 1 fetched only on open, but "is there a disk interface this boot" decides
// whether the Disk image section exists at all, and a section that lets you
// attach an image the target can never see is a lie the page must not tell.
function applyFeatures(doc) {
  const f = (doc && doc.features) || {};
  const disk = document.getElementById("disk");
  if (disk) disk.hidden = f.usb_disk === false;
}

fetch("/settings", { cache: "no-store" })
  .then((r) => (r.ok ? r.json() : null))
  .then((doc) => doc && applyFeatures(doc))
  .catch(() => {});

function renderSettings(doc) {
  renderDetails(doc);
  const box = el.deviceSettings;
  box.replaceChildren();
  const list = Array.isArray(doc.settings) ? doc.settings : [];
  if (list.length === 0) {
    const p = document.createElement("p");
    p.className = "scope-note";
    p.textContent = "This board has nothing to change here yet.";
    box.append(p);
    return;
  }
  for (const s of list) box.append(renderSetting(s));
}

async function loadSettings() {
  settingsStatus("loading\u2026");
  try {
    const r = await fetch("/settings", { cache: "no-store" });
    if (!r.ok) throw new Error(`${r.status}`);
    renderSettings(await r.json());
    settingsStatus("");
  } catch (err) {
    settingsStatus(`could not read settings (${err.message})`, "bad");
  }
}

async function postSetting(key, value) {
  settingsStatus("saving\u2026");
  try {
    const r = await fetch("/settings", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: `${encodeURIComponent(key)}=${encodeURIComponent(value)}`,
      cache: "no-store",
    });
    if (r.ok) {
      const doc = await r.json();
      renderSettings(doc);
      applyFeatures(doc);
      settingsStatus(key === "restart" ? "restarting\u2026" : "saved", "ok");
      return;
    }
    // The device says why in plain text (400, 429, 500); show it and re-read
    // so the control snaps back to what the device actually holds.
    settingsStatus(`${await r.text()} (${r.status})`, "bad");
    const again = await fetch("/settings", { cache: "no-store" });
    if (again.ok) renderSettings(await again.json());
  } catch (err) {
    settingsStatus(`could not save (${err.message})`, "bad");
  }
}

function openSettings() {
  exitCapture(); // direct mode: drops pointer lock and sends release-all
  if (MODE === "soft") el.softInput.blur(); // soft mode: the keyboard's box
  el.settingsMode.textContent = MODE === "soft" ? "touch" : "pointer lock";
  el.settings.showModal();
  loadSettings();
}

el.settingsOpen.addEventListener("click", openSettings);

// The reload links keep whatever else is in the query (?ws= for UI work).
for (const [a, mode] of [
  [el.settingsModeDirect, "direct"],
  [el.settingsModeSoft, "soft"],
]) {
  const url = new URL(location.href);
  url.searchParams.set("mode", mode);
  a.href = url.toString();
}

// --- browser-scope preferences ----------------------------------------------------
//
// localStorage: per browser, per origin, and never sent anywhere — the
// console must not phone out (AGENTS.md §2, principle 5). Both sliders are
// views of one stored value; the setter is the only writer.

const SENSITIVITY_KEY = "airkvm.sensitivity";

function setSensitivity(value, save) {
  const v = Math.min(3, Math.max(0.2, parseFloat(value) || 1));
  el.sensitivity.value = v;
  el.settingsSens.value = v;
  el.settingsSensOut.textContent = `${v.toFixed(1)}\u00d7`;
  if (save) {
    try {
      localStorage.setItem(SENSITIVITY_KEY, String(v));
    } catch {
      // Private mode or storage disabled: the slider still works for this load.
    }
  }
}

try {
  setSensitivity(localStorage.getItem(SENSITIVITY_KEY) ?? "1", false);
} catch {
  setSensitivity("1", false);
}
el.sensitivity.addEventListener("input", (e) => setSensitivity(e.target.value, true));
el.settingsSens.addEventListener("input", (e) => setSensitivity(e.target.value, true));
