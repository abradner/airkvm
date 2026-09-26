// The drain's coalescing rule (issue #58, AGENTS.md §9 entries 11 and 19).
//
// The bug this pins is not a crash and not a legal-value problem: applying a
// button-down and its button-up in one drain pass leaves the mask exactly as
// it started, so the single report that follows says "nothing pressed" and the
// click never happened. Everything builds, links and passes either way, which
// is why the classification is a function with a test rather than a condition
// at a call site.
#include "report_drain.h"
#include "test_support.h"

// Every frame type the protocol defines. Listed by hand rather than derived,
// so adding one to protocol.h fails HERE as well as at the -Werror=switch in
// the header — the test is what says the new type was *considered*, not merely
// compiled.
static const enum airkvm_frame_type ALL_TYPES[] = {
    AIRKVM_FRAME_KEY_DOWN,     AIRKVM_FRAME_KEY_UP,
    AIRKVM_FRAME_MOUSE_MOVE,   AIRKVM_FRAME_MOUSE_BUTTON,
    AIRKVM_FRAME_MOUSE_SCROLL, AIRKVM_FRAME_RELEASE_ALL,
    AIRKVM_FRAME_PING,         AIRKVM_FRAME_WAKE,
    AIRKVM_FRAME_STATUS_REQ,
};
#define N_TYPES (sizeof(ALL_TYPES) / sizeof(ALL_TYPES[0]))

static void absolute_state_intents_name_their_channel(void) {
  // These overwrite, so an unreported intermediate is a lost event.
  CHECK_EQ_INT(report_drain_channel(AIRKVM_FRAME_KEY_DOWN),
               REPORT_CHANNEL_KEYBOARD);
  CHECK_EQ_INT(report_drain_channel(AIRKVM_FRAME_KEY_UP),
               REPORT_CHANNEL_KEYBOARD);
  CHECK_EQ_INT(report_drain_channel(AIRKVM_FRAME_MOUSE_BUTTON),
               REPORT_CHANNEL_MOUSE_BUTTONS);
}

static void accumulating_intents_are_free_to_coalesce(void) {
  // These add, so ten collapsed into one report lose nothing. Coalescing them
  // is the whole point of the drain and must not be given up by this fix.
  CHECK_EQ_INT(report_drain_channel(AIRKVM_FRAME_MOUSE_MOVE),
               REPORT_CHANNEL_NONE);
  CHECK_EQ_INT(report_drain_channel(AIRKVM_FRAME_MOUSE_SCROLL),
               REPORT_CHANNEL_NONE);
}

static void intents_that_carry_no_report_state_never_block(void) {
  // RELEASE_ALL writes both channels but is serviced by resetting the queue at
  // the top of the loop, so it is never held behind an unreported state — and
  // holding it would be the worse failure (principle 4: prefer a spurious
  // release to a plausible held key).
  CHECK_EQ_INT(report_drain_channel(AIRKVM_FRAME_RELEASE_ALL),
               REPORT_CHANNEL_NONE);
  CHECK(!report_drain_blocked(AIRKVM_FRAME_RELEASE_ALL, true, true));
  for (size_t i = 0; i < N_TYPES; i++) {
    const enum airkvm_frame_type t = ALL_TYPES[i];
    if (t == AIRKVM_FRAME_PING || t == AIRKVM_FRAME_WAKE ||
        t == AIRKVM_FRAME_STATUS_REQ) {
      CHECK_EQ_INT(report_drain_channel(t), REPORT_CHANNEL_NONE);
      CHECK(!report_drain_blocked(t, true, true));
    }
  }
}

static void a_channel_blocks_only_on_its_own_owed_report(void) {
  // The bug: before the fix, a button intent was blocked by NOTHING, so a
  // second one overwrote the first inside a single pass.
  CHECK(report_drain_blocked(AIRKVM_FRAME_MOUSE_BUTTON, false, true));
  CHECK(!report_drain_blocked(AIRKVM_FRAME_MOUSE_BUTTON, false, false));
  // And it must not block on the OTHER channel's owed report, or a click
  // would wait behind a keystroke for no reason (and vice versa) — which is
  // issue #27's head-of-line problem made worse rather than better.
  CHECK(!report_drain_blocked(AIRKVM_FRAME_MOUSE_BUTTON, true, false));
  CHECK(report_drain_blocked(AIRKVM_FRAME_KEY_DOWN, true, false));
  CHECK(report_drain_blocked(AIRKVM_FRAME_KEY_UP, true, false));
  CHECK(!report_drain_blocked(AIRKVM_FRAME_KEY_DOWN, false, true));
  // Motion never blocks, whatever is owed.
  CHECK(!report_drain_blocked(AIRKVM_FRAME_MOUSE_MOVE, true, true));
  CHECK(!report_drain_blocked(AIRKVM_FRAME_MOUSE_SCROLL, true, true));
}

static void nothing_blocks_when_no_report_is_owed(void) {
  // Positive control on the whole predicate: with both channels clean, every
  // type is free. Without this, "blocked" returning false everywhere would
  // pass the assertions above that expect false.
  for (size_t i = 0; i < N_TYPES; i++) {
    CHECK(!report_drain_blocked(ALL_TYPES[i], false, false));
  }
}

static void every_type_is_classified(void) {
  // Total by construction (no `default:` in the header, -Werror=switch), and
  // asserted here too so a type added without thought fails a test rather
  // than inheriting a neighbour's arm.
  for (size_t i = 0; i < N_TYPES; i++) {
    const enum report_channel c = report_drain_channel(ALL_TYPES[i]);
    CHECK(c == REPORT_CHANNEL_NONE || c == REPORT_CHANNEL_KEYBOARD ||
          c == REPORT_CHANNEL_MOUSE_BUTTONS);
  }
  CHECK_EQ_INT((long)N_TYPES, 9);  // fails when protocol.h grows a frame type
}

int main(void) {
  RUN(absolute_state_intents_name_their_channel);
  RUN(accumulating_intents_are_free_to_coalesce);
  RUN(intents_that_carry_no_report_state_never_block);
  RUN(a_channel_blocks_only_on_its_own_owed_report);
  RUN(nothing_blocks_when_no_report_is_owed);
  RUN(every_type_is_classified);
  return test_summary("report_drain");
}
