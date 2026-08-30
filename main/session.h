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

// Socket fds and coordinator session ids are both non-negative, so -1 is
// unambiguous as "nobody". Guarded against in session_claim(): a caller that
// passes a failed fd must not be able to take ownership.
#define SESSION_NONE (-1)

typedef int session_token_t;

struct session {
  session_token_t owner;
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
bool session_claim(struct session *s, session_token_t tok);

// Releases ownership. Returns true only if a session was actually live, so the
// caller's release-all and status effects run exactly once no matter how many
// terminal events race for the same session (socket close and a failed read
// both fire routinely).
bool session_end(struct session *s);

// session_end(), but only when `tok` is the current owner. Returns true if it
// ended one.
bool session_end_if_owner(struct session *s, session_token_t tok);
