// Who is currently allowed to drive the target.
//
// Exactly one console session drives HID at a time. Two clients interleaving
// modifiers into one HID state is risk R-003 (docs/repo-map.md); refusing the
// second is the conservative half of that.
//
// This is a pure state machine with no I/O, in the same shape as hid_state.c:
// the caller owns the struct, every function is total, and it compiles with no
// ESP-IDF headers so the host tests can exercise it (AGENTS.md §4). The
// *effects* of a transition — releasing every held key, clearing the front
// panel — deliberately live with the caller, in exactly one function, so that
// "the session ended" and "everything was released" cannot drift apart.
//
// The transport token is opaque on purpose. Standalone mode passes the
// WebSocket socket fd; fleet mode will pass the coordinator-assigned session id
// (docs/design/fleet.md) without this file changing.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Socket fds and coordinator session ids are both non-negative, so -1 is
// unambiguous as "nobody". Guarded against in session_claim(): a caller that
// passes a failed fd must not be able to take ownership.
#define SESSION_NONE (-1)

typedef int session_token_t;

struct session {
  session_token_t owner;
  // Millisecond clock reading at the last frame from the owner. Only meaningful
  // while owned. Deliberately uint32_t and compared by unsigned difference, so
  // the ~49.7-day wrap of a millisecond counter is a non-event rather than a
  // session that can never time out again.
  uint32_t last_seen_ms;
};

// Unowned. Safe to call at any time; does not run the caller's release effects,
// so use session_end() for a session that may currently be live.
void session_reset(struct session *s);

bool session_has_owner(const struct session *s);

bool session_is_owner(const struct session *s, session_token_t tok);

// Claims the session for `tok` if it is unowned. Returns true when `tok` owns
// the session on return — whether it just claimed it or already held it — and
// false when someone else does (the spectator case, R-003) or when `tok` is
// SESSION_NONE.
//
// Records `now_ms` as the owner's liveness whenever it returns true. Taking the
// clock here rather than in a separate call is deliberate: every frame already
// re-claims, so liveness cannot be forgotten at a call site that handles some
// new frame type later.
bool session_claim(struct session *s, session_token_t tok, uint32_t now_ms);

// True when a session is owned and nothing has arrived from its owner for at
// least `timeout_ms`. False when unowned: there is nothing to time out, and a
// caller must not read a stale liveness value as an expiry.
//
// This is the only thing standing between a client that vanishes without
// closing its TCP connection and a target left holding whatever it was holding
// (docs/repo-map.md, session lifecycle).
bool session_expired(const struct session *s, uint32_t now_ms,
                     uint32_t timeout_ms);

// session_end(), but only when the session is owned and has expired. Returns
// true if it ended one.
//
// Check and end are one operation on purpose. A caller that tests expiry, and
// then ends the session as a separate step, tears down whatever session exists
// by the time it gets there — which may be a session that proved its liveness
// in between, or an entirely different one belonging to a client that claimed
// after the old owner left. Both cost the operator a session that was alive.
bool session_end_if_expired(struct session *s, uint32_t now_ms,
                            uint32_t timeout_ms);

// Releases ownership. Returns true only if a session was actually live, so the
// caller's release-all and status effects run exactly once no matter how many
// terminal events race for the same session (socket close and a failed read
// both fire routinely).
bool session_end(struct session *s);

// session_end(), but only when `tok` is the current owner. Returns true if it
// ended one.
bool session_end_if_owner(struct session *s, session_token_t tok);
