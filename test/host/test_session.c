#include "session.h"
#include "test_support.h"

static void starts_unowned(void) {
  struct session s;
  session_reset(&s);
  CHECK(!session_has_owner(&s));
  CHECK(!session_is_owner(&s, 7));
  // Ending a session nobody holds must report that it ended nothing, or the
  // caller runs its release effects on every stray close.
  CHECK(!session_end(&s));
}

static void first_claim_wins(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7));
  CHECK(session_has_owner(&s));
  CHECK(session_is_owner(&s, 7));
}

static void second_token_is_a_spectator(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7));

  // R-003: the second client is refused rather than interleaved, and the first
  // keeps control.
  CHECK(!session_claim(&s, 9));
  CHECK(session_is_owner(&s, 7));
  CHECK(!session_is_owner(&s, 9));
}

static void owner_reclaiming_is_not_a_takeover(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7));
  // Every frame re-claims; the owner must keep driving rather than being told
  // it is a spectator on its second frame.
  CHECK(session_claim(&s, 7));
  CHECK(session_is_owner(&s, 7));
}

static void end_reports_true_exactly_once(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7));

  // This is the property the caller's release-all depends on: a socket close
  // and a failed read routinely both fire for the same session, and the
  // effects must run once, not twice.
  CHECK(session_end(&s));
  CHECK(!session_end(&s));
  CHECK(!session_has_owner(&s));
}

static void end_if_owner_ignores_other_tokens(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7));

  // A spectator disconnecting must not tear down the driver's session.
  CHECK(!session_end_if_owner(&s, 9));
  CHECK(session_is_owner(&s, 7));

  CHECK(session_end_if_owner(&s, 7));
  CHECK(!session_has_owner(&s));
}

static void a_failed_token_can_never_own(void) {
  struct session s;
  session_reset(&s);

  // httpd_req_to_sockfd() returns -1 on failure, which is also the unowned
  // sentinel. Without the guard this claim would succeed and every later
  // caller holding a failed fd would test as the owner.
  CHECK(!session_claim(&s, SESSION_NONE));
  CHECK(!session_has_owner(&s));
  CHECK(!session_is_owner(&s, SESSION_NONE));

  // And it must not be able to end a real session either.
  CHECK(session_claim(&s, 7));
  CHECK(!session_end_if_owner(&s, SESSION_NONE));
  CHECK(session_is_owner(&s, 7));
}

static void a_new_client_can_claim_after_the_owner_leaves(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7));
  CHECK(session_end(&s));

  // The point of clearing ownership on every terminal event: the next client
  // must not be locked out as a spectator of a session that no longer exists.
  CHECK(session_claim(&s, 9));
  CHECK(session_is_owner(&s, 9));
}

int main(void) {
  RUN(starts_unowned);
  RUN(first_claim_wins);
  RUN(second_token_is_a_spectator);
  RUN(owner_reclaiming_is_not_a_takeover);
  RUN(end_reports_true_exactly_once);
  RUN(end_if_owner_ignores_other_tokens);
  RUN(a_failed_token_can_never_own);
  RUN(a_new_client_can_claim_after_the_owner_leaves);
  return test_summary("session");
}
