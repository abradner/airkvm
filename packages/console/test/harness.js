// Installed into <head>, so everything below is in place before any console
// script runs. Nothing here is loaded by the real console; the runner injects
// it at serve time and the files on disk are untouched.
"use strict";

(() => {
  // Errors are invisible in headless Firefox by default (AGENTS.md §6.1), and
  // an uncaught throw in app.js would otherwise present as a timeout with no
  // hint of where. Captured synchronously, and reported instead of the checks.
  const errors = [];
  window.addEventListener("error", (e) => {
    errors.push(`${e.message} @ ${e.filename}:${e.lineno}`);
  });
  window.addEventListener("unhandledrejection", (e) => {
    errors.push(`unhandled rejection: ${e.reason}`);
  });

  // --- fake clock ------------------------------------------------------------
  //
  // The console schedules real work on timers: keepalive on a 1000ms interval,
  // motion flushes on requestAnimationFrame, paced typing on ~8ms timeouts.
  // Driving those by sleeping makes a test that is slow when it passes and
  // flaky when it fails, so time is made explicit instead: nothing fires until
  // a check asks it to.
  let now = 0;
  let seq = 0;
  const pending = new Map();

  window.setTimeout = (fn, ms, ...args) => {
    const id = ++seq;
    pending.set(id, { at: now + (ms || 0), fn, args, every: null });
    return id;
  };
  window.setInterval = (fn, ms, ...args) => {
    const id = ++seq;
    pending.set(id, { at: now + (ms || 0), fn, args, every: ms || 0 });
    return id;
  };
  window.clearTimeout = (id) => pending.delete(id);
  window.clearInterval = (id) => pending.delete(id);
  // rAF is a timer here too. 16ms is arbitrary; only the ordering matters.
  window.requestAnimationFrame = (fn) => window.setTimeout(() => fn(now), 16);
  window.cancelAnimationFrame = (id) => pending.delete(id);

  // Advances to `now + ms`, firing everything due in timestamp order and
  // draining microtasks between callbacks so promise chains (the paced paste
  // awaits one per character) actually progress.
  window.__advance = async (ms) => {
    const until = now + ms;
    for (;;) {
      let next = null;
      for (const [id, t] of pending) {
        if (t.at <= until && (next === null || t.at < next[1].at)) next = [id, t];
      }
      if (next === null) break;
      const [id, t] = next;
      now = t.at;
      if (t.every === null) pending.delete(id);
      else t.at = now + t.every;
      t.fn(...t.args);
      await Promise.resolve();
      await Promise.resolve();
    }
    now = until;
    await Promise.resolve();
  };

  // --- stub socket -----------------------------------------------------------
  //
  // Records what the console sends rather than connecting anywhere. Every frame
  // is kept as a plain byte array so a check can assert exact bytes, which is
  // the whole point: the wire format is firmware/main/protocol.h's, and a
  // console that sends plausible-but-wrong bytes is the failure worth catching.
  const sent = [];
  window.__sent = sent;

  class StubSocket {
    constructor(url) {
      this.url = url;
      this.readyState = 0; // CONNECTING
      this.binaryType = "blob";
      this.onopen = this.onclose = this.onerror = this.onmessage = null;
      // A microtask, not a timer: app.js assigns socket.onopen on the line
      // after `new WebSocket(...)`, so opening must be later than "now" but
      // must not depend on the fake clock being advanced.
      queueMicrotask(() => {
        this.readyState = 1; // OPEN
        if (this.onopen) this.onopen({});
      });
    }
    send(data) {
      const bytes =
        data instanceof ArrayBuffer
          ? new Uint8Array(data)
          : new Uint8Array(data.buffer || data);
      sent.push(Array.from(bytes));
    }
    close() {
      this.readyState = 3; // CLOSED
      if (this.onclose) this.onclose({});
    }
    addEventListener() {}
    removeEventListener() {}
  }
  StubSocket.CONNECTING = 0;
  StubSocket.OPEN = 1;
  StubSocket.CLOSING = 2;
  StubSocket.CLOSED = 3;
  window.WebSocket = StubSocket;

  // --- check bookkeeping -----------------------------------------------------
  const checks = [];
  window.__checks = checks;
  window.__errors = errors;

  window.__check = (name, ok, detail) => {
    checks.push({ name, ok: !!ok, detail: detail || "" });
  };

  const hex = (frame) =>
    frame.map((b) => b.toString(16).padStart(2, "0")).join(" ");

  // Asserts the exact frames sent since the last drain, then clears them. Taking
  // "since the last drain" rather than "the last N" means a check fails if the
  // console sent something extra, not just something wrong.
  window.__expectFrames = (name, expected) => {
    const actual = sent.splice(0, sent.length);
    const a = actual.map(hex).join(" | ");
    const e = expected.map(hex).join(" | ");
    window.__check(name, a === e, a === e ? "" : `sent [${a}], expected [${e}]`);
  };

  window.__drain = () => sent.splice(0, sent.length);

  window.__report = async (payload) => {
    await fetch("/__result", { method: "POST", body: JSON.stringify(payload) });
  };
})();
