// Tests for redirect.h — building the Location value for HTTP->HTTPS.
//
// This is a string routine over attacker-controlled input whose output lands in
// a response header, so most of what is below is about refusing input rather
// than formatting it. The happy path is three cases; the rest is the reason
// this file exists.
#include "redirect.h"
#include "test_support.h"

static char g_out[160];

static bool build(const char *host, const char *uri) {
  memset(g_out, 0xAA, sizeof(g_out));  // poison: catch a partial write
  return redirect_location(host, uri, g_out, sizeof(g_out));
}

// A refused call must not have written anything. The poison above was being
// laid down and never checked, so a version that assembled half a URL and then
// bailed would have passed every test in this file.
static bool refused_and_wrote_nothing(const char *host, const char *uri) {
  if (build(host, uri)) {
    return false;
  }
  for (size_t i = 0; i < sizeof(g_out); i++) {
    if ((unsigned char)g_out[i] != 0xAA) {
      return false;
    }
  }
  return true;
}

static void the_ordinary_case(void) {
  CHECK(build("airkvm-lcd.local", "/"));
  CHECK_EQ_STR(g_out, "https://airkvm-lcd.local/");

  // The path and its query survive: a bookmark deeper than "/" has to land
  // where it was pointing, or the redirect is a worse experience than none.
  CHECK(build("airkvm-lcd.local", "/app.js?v=2"));
  CHECK_EQ_STR(g_out, "https://airkvm-lcd.local/app.js?v=2");

  // A bare IP is how this device is reached when mDNS does not resolve, which
  // on some networks is always.
  CHECK(build("10.10.40.240", "/"));
  CHECK_EQ_STR(g_out, "https://10.10.40.240/");
}

// The port has to go. Echoing ":80" into an https:// URL would redirect the
// client to a port that does not speak TLS — a loop the user cannot see the
// cause of.
static void the_port_is_stripped(void) {
  CHECK(build("airkvm-lcd.local:80", "/"));
  CHECK_EQ_STR(g_out, "https://airkvm-lcd.local/");

  CHECK(build("10.10.40.240:80", "/style.css"));
  CHECK_EQ_STR(g_out, "https://10.10.40.240/style.css");
}

// An IPv6 literal is full of colons, so "strip everything after the last colon"
// and "strip everything after the first colon" are both wrong. The port is only
// the colon that follows the closing bracket.
static void ipv6_literals_keep_their_address(void) {
  CHECK(build("[fe80::1]", "/"));
  CHECK_EQ_STR(g_out, "https://[fe80::1]/");

  CHECK(build("[fe80::1]:80", "/"));
  CHECK_EQ_STR(g_out, "https://[fe80::1]/");

  // Unterminated bracket is malformed: refuse rather than emit a URL whose
  // authority section never closes.
  CHECK(!build("[fe80::1", "/"));
}

// The one that matters. A Host containing CR or LF would end the Location
// header early and let the client dictate the rest of the response.
static void header_injection_is_refused(void) {
  CHECK(!build("evil\r\nX-Injected: yes", "/"));
  CHECK(!build("evil\nX-Injected: yes", "/"));
  CHECK(!build("evil\rX", "/"));
  // LF on its own, with nothing after it that would be rejected anyway. The
  // two cases above both contain a space and a colon, so they would still be
  // refused with the newline check removed — this one would not, and mutating
  // the allowlist to permit \n survived until it existed.
  CHECK(!build("evil\nX", "/"));
  // Not merely CR/LF: the allowlist refuses anything it does not recognise, so
  // the next header-splitting trick does not need a new rule here.
  //
  // No embedded-NUL case here, deliberately. "host\0hidden" as a C literal ends
  // at the NUL, so this function would receive a valid "host" and pass — the
  // assertion was written, failed, and was removed rather than the code
  // weakened to satisfy it. Nothing past a NUL is reachable through a
  // `const char *`, and esp_http_server hands us a NUL-terminated value, so
  // there is no smuggling route to close.
  CHECK(!build("host with space", "/"));
  CHECK(!build("host\twith\ttab", "/"));
  CHECK(!build("h\x7fost", "/"));
  CHECK(!build("h\x80ost", "/"));
  CHECK(!build("evil/../../etc", "/"));
  CHECK(!build("evil?query", "/"));
  CHECK(!build("user@evil", "/"));

  // Bytes AFTER the port colon are validated too, even though they are
  // discarded rather than echoed. Stopping validation at the colon accepted
  // these, which made "we refuse anything we do not recognise" untrue.
  CHECK(!build("safe.example:bad suffix", "/"));
  CHECK(!build("safe.example:\r\nX: y", "/"));
  CHECK(!build("safe.example:80junk\n", "/"));
  // ...and a well-formed port still passes, so the check is not just refusing
  // everything with a colon in it.
  CHECK(build("safe.example:8443", "/"));
  CHECK_EQ_STR(g_out, "https://safe.example/");

  // And a newline arriving via the request target is refused too, even though
  // the server's own parser should never produce one.
  CHECK(!build("airkvm-lcd.local", "/a\r\nX: y"));
  CHECK(!build("airkvm-lcd.local", "/a\nX: y"));
}

// The poison check, applied to one refusal of each kind.
static void a_refused_build_leaves_the_output_untouched(void) {
  CHECK(refused_and_wrote_nothing("evil\r\nX: y", "/"));
  CHECK(refused_and_wrote_nothing("", "/"));
  CHECK(refused_and_wrote_nothing("[fe80::1", "/"));
  CHECK(refused_and_wrote_nothing("airkvm-lcd.local", "/a\nX: y"));
}

static void empty_and_missing_inputs_are_refused(void) {
  CHECK(!build("", "/"));
  CHECK(!redirect_location(NULL, "/", g_out, sizeof(g_out)));
  CHECK(!redirect_location("airkvm-lcd.local", NULL, g_out, sizeof(g_out)));
  CHECK(!redirect_location("airkvm-lcd.local", "/", NULL, sizeof(g_out)));
  CHECK(!redirect_location("airkvm-lcd.local", "/", g_out, 0));
  // A host that is nothing but a port is not a host.
  CHECK(!build(":80", "/"));
}

// Length limits, from both ends: a host longer than the cap, and an output
// buffer too small for a legitimate host. Both must fail closed rather than
// truncate — a truncated authority is a different host.
static void oversized_input_fails_closed(void) {
  char big[REDIRECT_MAX_HOST + 32];
  memset(big, 'a', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  CHECK(!build(big, "/"));

  // Exactly one over the cap. Without this, relaxing the bound from >= to >
  // passes every other length case — the cap and cap+31 are both still
  // rejected — so this is the only assertion that pins the boundary itself.
  char over_by_one[REDIRECT_MAX_HOST + 2];
  memset(over_by_one, 'a', REDIRECT_MAX_HOST + 1);
  over_by_one[REDIRECT_MAX_HOST + 1] = '\0';
  CHECK(!build(over_by_one, "/"));

  // Exactly at the cap is still fine, so the check is a cap and not an
  // off-by-one that rejects the longest legal name.
  char at_cap[REDIRECT_MAX_HOST + 1];
  memset(at_cap, 'a', REDIRECT_MAX_HOST);
  at_cap[REDIRECT_MAX_HOST] = '\0';
  CHECK(build(at_cap, "/"));

  // "https://" + "abc" + "/" + NUL is 13 bytes; 12 must refuse.
  char small[12];
  CHECK(!redirect_location("abc", "/", small, sizeof(small)));
  char just_enough[13];
  CHECK(redirect_location("abc", "/", just_enough, sizeof(just_enough)));
  CHECK_EQ_STR(just_enough, "https://abc/");
}

// A request target that does not start with "/" must not be pasted straight
// onto the host, or "app.js" becomes "https://hostapp.js" — a different host
// entirely, which is the one failure here that silently points somewhere real.
static void a_relative_target_still_produces_an_absolute_path(void) {
  CHECK(build("airkvm-lcd.local", "app.js"));
  CHECK_EQ_STR(g_out, "https://airkvm-lcd.local/app.js");

  CHECK(build("airkvm-lcd.local", ""));
  CHECK_EQ_STR(g_out, "https://airkvm-lcd.local/");

  // ...and the inserted slash is counted by the bounds check. "https://ab" +
  // "/" + "c" + NUL is 13, so 12 must refuse rather than overrun.
  char small[12];
  CHECK(!redirect_location("ab", "c", small, sizeof(small)));
}

int main(void) {
  RUN(the_ordinary_case);
  RUN(the_port_is_stripped);
  RUN(ipv6_literals_keep_their_address);
  RUN(header_injection_is_refused);
  RUN(a_refused_build_leaves_the_output_untouched);
  RUN(empty_and_missing_inputs_are_refused);
  RUN(oversized_input_fails_closed);
  RUN(a_relative_target_still_produces_an_absolute_path);
  return test_summary("redirect");
}
