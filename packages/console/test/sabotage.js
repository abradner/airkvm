// Loaded ONLY by `runner.py --self-test`, never by the suite itself.
//
// A test that has never been seen to fail has proven nothing (AGENTS.md §8).
// Rather than assert that once by hand and write it in a commit message, the
// self-test plants this and requires the suite to go red — so "these checks can
// fail" is something CI re-establishes on every run rather than a claim.
//
// It runs after the console's scripts and before tests.js. `send` is a
// top-level function declaration, so reassigning it here rebinds what the
// console's own handlers resolve.
"use strict";

(() => {
  const real = window.send;
  window.send = (bytes) => real([bytes[0] + 1, ...bytes.slice(1)]);
})();
