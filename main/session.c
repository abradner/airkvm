#include "session.h"

void session_reset(struct session *s) {
  s->owner = SESSION_NONE;
}

bool session_has_owner(const struct session *s) {
  return s->owner != SESSION_NONE;
}

bool session_is_owner(const struct session *s, session_token_t tok) {
  // The SESSION_NONE guard matters: without it, a caller holding a failed fd of
  // -1 would compare equal to an unowned session and be treated as the owner.
  return tok != SESSION_NONE && s->owner == tok;
}

bool session_claim(struct session *s, session_token_t tok) {
  if (tok == SESSION_NONE) {
    return false;
  }
  if (s->owner == SESSION_NONE) {
    s->owner = tok;
    return true;
  }
  return s->owner == tok;
}

bool session_end(struct session *s) {
  if (s->owner == SESSION_NONE) {
    return false;
  }
  s->owner = SESSION_NONE;
  return true;
}

bool session_end_if_owner(struct session *s, session_token_t tok) {
  if (!session_is_owner(s, tok)) {
    return false;
  }
  return session_end(s);
}
