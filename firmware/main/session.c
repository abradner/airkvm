#include "session.h"

void session_reset(struct session *s) {
  s->owner = SESSION_NONE;
  s->last_seen_ms = 0;
}

bool session_has_owner(const struct session *s) {
  return s->owner != SESSION_NONE;
}

bool session_is_owner(const struct session *s, session_token_t tok) {
  // The SESSION_NONE guard matters: without it, a caller holding a failed fd of
  // -1 would compare equal to an unowned session and be treated as the owner.
  return tok != SESSION_NONE && s->owner == tok;
}

bool session_claim(struct session *s, session_token_t tok, uint32_t now_ms) {
  if (tok == SESSION_NONE) {
    return false;
  }
  if (s->owner != SESSION_NONE && s->owner != tok) {
    return false;
  }
  s->owner = tok;
  s->last_seen_ms = now_ms;
  return true;
}

bool session_expired(const struct session *s, uint32_t now_ms,
                     uint32_t timeout_ms) {
  if (s->owner == SESSION_NONE) {
    return false;
  }
  // Unsigned difference, so this stays correct across the counter's wrap. A
  // signed or absolute comparison would either miss the expiry for 49 days or
  // fire immediately at the wrap.
  return (uint32_t)(now_ms - s->last_seen_ms) >= timeout_ms;
}

bool session_end(struct session *s) {
  if (s->owner == SESSION_NONE) {
    return false;
  }
  s->owner = SESSION_NONE;
  return true;
}

bool session_end_if_expired(struct session *s, uint32_t now_ms,
                            uint32_t timeout_ms) {
  if (!session_expired(s, now_ms, timeout_ms)) {
    return false;
  }
  return session_end(s);
}

bool session_end_if_owner(struct session *s, session_token_t tok) {
  if (!session_is_owner(s, tok)) {
    return false;
  }
  return session_end(s);
}
