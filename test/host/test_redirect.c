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

  // And a newline arriving via the request target is refused too, even though
  // the server's own parser should never produce one.
  CHECK(!build("airkvm-lcd.local", "/a\r\nX: y"));
  CHECK(!build("airkvm-lcd.local", "/a\nX: y"));
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
  RUN(empty_and_missing_inputs_are_refused);
  RUN(oversized_input_fails_closed);
  RUN(a_relative_target_still_produces_an_absolute_path);
  return test_summary("redirect");
}
