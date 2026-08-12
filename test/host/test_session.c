#include "session.h"
#include "test_support.h"

// Most tests do not care about the clock; claims that are not about liveness
// pass a fixed reading to keep the intent obvious.
#define T0 1000u

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
  CHECK(session_claim(&s, 7, T0));
  CHECK(session_has_owner(&s));
  CHECK(session_is_owner(&s, 7));
}

static void second_token_is_a_spectator(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, T0));

  // R-003: the second client is refused rather than interleaved, and the first
  // keeps control.
  CHECK(!session_claim(&s, 9, T0));
  CHECK(session_is_owner(&s, 7));
  CHECK(!session_is_owner(&s, 9));
}

static void owner_reclaiming_is_not_a_takeover(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, T0));
  // Every frame re-claims; the owner must keep driving rather than being told
  // it is a spectator on its second frame.
  CHECK(session_claim(&s, 7, T0));
  CHECK(session_is_owner(&s, 7));
}

static void end_reports_true_exactly_once(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, T0));

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
  CHECK(session_claim(&s, 7, T0));

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
  CHECK(!session_claim(&s, SESSION_NONE, T0));
  CHECK(!session_has_owner(&s));
  CHECK(!session_is_owner(&s, SESSION_NONE));

  // And it must not be able to end a real session either.
  CHECK(session_claim(&s, 7, T0));
  CHECK(!session_end_if_owner(&s, SESSION_NONE));
  CHECK(session_is_owner(&s, 7));
}

static void a_new_client_can_claim_after_the_owner_leaves(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, T0));
  CHECK(session_end(&s));

  // The point of clearing ownership on every terminal event: the next client
  // must not be locked out as a spectator of a session that no longer exists.
  CHECK(session_claim(&s, 9, T0));
  CHECK(session_is_owner(&s, 9));
}

// --- liveness ---------------------------------------------------------------

static void an_unowned_session_never_expires(void) {
  struct session s;
  session_reset(&s);
  // Nothing is held, so there is nothing to release — and last_seen_ms is
  // meaningless here. Reporting expiry would fire the release path forever.
  CHECK(!session_expired(&s, 0, 3000));
  CHECK(!session_expired(&s, 999999, 3000));
}

static void expires_only_after_the_full_timeout(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, 1000));

  CHECK(!session_expired(&s, 1000, 3000));  // same instant
  CHECK(!session_expired(&s, 3999, 3000));  // one millisecond short
  CHECK(session_expired(&s, 4000, 3000));   // exactly the deadline
  CHECK(session_expired(&s, 9000, 3000));
}

static void activity_postpones_expiry(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, 1000));
  CHECK(session_expired(&s, 4000, 3000));

  // A frame arriving late still resets the clock: the client is demonstrably
  // alive, whatever the previous silence looked like.
  CHECK(session_claim(&s, 7, 4000));
  CHECK(!session_expired(&s, 4000, 3000));
  CHECK(!session_expired(&s, 6999, 3000));
  CHECK(session_expired(&s, 7000, 3000));
}

static void a_spectators_frames_do_not_postpone_expiry(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, 1000));

  // A second client hammering the socket must not keep a dead owner's session
  // alive — that would defeat the deadline exactly when someone else wants the
  // keyboard.
  CHECK(!session_claim(&s, 9, 3500));
  CHECK(session_expired(&s, 4000, 3000));
}

static void expiry_survives_the_millisecond_counter_wrapping(void) {
  struct session s;
  session_reset(&s);

  // A uint32 millisecond counter wraps every ~49.7 days. Claim just before the
  // wrap and evaluate just after: the unsigned difference is still 2000ms, so
  // the session must not be expired at a 3000ms timeout.
  const uint32_t before_wrap = 0xFFFFFFFFu - 1000u;
  CHECK(session_claim(&s, 7, before_wrap));

  const uint32_t after_wrap = 1000u;  // 2000ms later in real time
  CHECK(!session_expired(&s, after_wrap, 3000));

  // And it must still expire on the far side of the wrap rather than becoming
  // immortal, which is what a signed or absolute comparison would do here.
  CHECK(session_expired(&s, 2000u, 3000));
}

static void end_if_expired_only_ends_an_expired_session(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, 1000));

  CHECK(!session_end_if_expired(&s, 3999, 3000));
  CHECK(session_is_owner(&s, 7));

  CHECK(session_end_if_expired(&s, 4000, 3000));
  CHECK(!session_has_owner(&s));

  // And it reports nothing to do on an already-empty session, so the caller's
  // release effects do not run once per timer tick forever.
  CHECK(!session_end_if_expired(&s, 9000, 3000));
}

static void a_frame_arriving_before_the_check_saves_the_session(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, 1000));

  // The race this exists to close: the timer observes an expired session, but
  // a frame lands before the teardown. Testing expiry and ending as one
  // operation means the refreshed session survives; doing it in two steps
  // tears down a client that had just proved it was alive.
  CHECK(session_expired(&s, 4000, 3000));
  CHECK(session_claim(&s, 7, 4000));
  CHECK(!session_end_if_expired(&s, 4000, 3000));
  CHECK(session_is_owner(&s, 7));
}

static void a_new_owner_is_not_torn_down_by_the_old_ones_deadline(void) {
  struct session s;
  session_reset(&s);
  CHECK(session_claim(&s, 7, 1000));
  CHECK(session_expired(&s, 4000, 3000));

  // The old owner's socket closes and a different client claims, all before
  // the teardown runs. The new session is live and must survive.
  CHECK(session_end(&s));
  CHECK(session_claim(&s, 9, 4000));

  CHECK(!session_end_if_expired(&s, 4000, 3000));
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
  RUN(an_unowned_session_never_expires);
  RUN(expires_only_after_the_full_timeout);
  RUN(activity_postpones_expiry);
  RUN(a_spectators_frames_do_not_postpone_expiry);
  RUN(expiry_survives_the_millisecond_counter_wrapping);
  RUN(end_if_expired_only_ends_an_expired_session);
  RUN(a_frame_arriving_before_the_check_saves_the_session);
  RUN(a_new_owner_is_not_torn_down_by_the_old_ones_deadline);
  return test_summary("session");
}
