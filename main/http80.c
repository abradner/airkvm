#include "http80.h"

#if CONFIG_AIRKVM_TLS

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "logtap.h"
#include "redirect.h"

static const char *TAG = "http80";

// Verified on hardware 2026-09-07 (ESP32-S3-LCD-1.47B, over OTA). What was
// actually checked, because "it built" says nothing about a URI matcher:
//
//   http://airkvm-lcd.local/          -> 302, Location https://airkvm-lcd.local/
//   http://airkvm-lcd.local/app.js?v=2 -> path AND query preserved
//   http://10.10.40.240/style.css     -> an IP literal Host echoes back intact
//   http://10.10.40.240/log           -> 200 and the log, NOT the redirect
//   following the redirect            -> the real console, 200 over TLS
//   Host: "evil host with spaces"     -> 400, no Location emitted
//   Host: "evil/../etc"               -> 400
//   HTTP/1.0 with no Host at all      -> 302 to the configured mDNS name
//
// The fourth line is the one worth having run: /log winning over the catch-all
// depends on esp_http_server returning the FIRST matching handler in
// registration order, which is a framework behaviour this file assumes and
// AGENTS.md §9 entry 2 says not to assume (a well-argued, source-cited review
// claim about this same server was wrong once already).

// 302 and not 301, and this is the one decision in the file worth arguing.
//
// A 301 is cached by browsers indefinitely and is not revisited — that is the
// point of it. This project supports building with AIRKVM_TLS=n for bench work
// (see the Kconfig help), and on such a build the console IS the thing on port
// 80. A browser holding a cached 301 from an earlier TLS image would silently
// rewrite http:// to https:// and fail to connect, on a device that is working
// perfectly, and the only fix is clearing browser state — which nobody will
// think to do, because the device looks broken rather than the browser.
//
// 302 costs one extra round trip per visit on a LAN. That is the whole price,
// and it buys a build flag that keeps working in both positions.
//
// Note this deliberately is NOT HSTS. HSTS would pin the upgrade far harder and
// has the same trap with none of the escape: a header that outlives the
// firmware that sent it.
#define REDIRECT_STATUS "302 Found"

// Enough for "https://" + a 96-byte host + a request target. The target is
// short on this device (the longest route is "/style.css"), but a client may
// ask for anything and the builder refuses rather than truncates.
#define LOCATION_MAX 256

static esp_err_t redirect_get(httpd_req_t *req) {
  char host[REDIRECT_MAX_HOST + 1];
  char location[LOCATION_MAX];

  // A Host header is mandatory in HTTP/1.1 and absent in some HTTP/1.0
  // clients. Falling back to the configured mDNS name rather than failing
  // keeps `curl -0` and the odd embedded client working, and that name is
  // ours rather than the caller's — it cannot carry an injection.
  size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
  if (host_len == 0 || host_len > REDIRECT_MAX_HOST) {
    snprintf(host, sizeof(host), "%s.local", CONFIG_AIRKVM_MDNS_HOSTNAME);
  } else if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) !=
             ESP_OK) {
    snprintf(host, sizeof(host), "%s.local", CONFIG_AIRKVM_MDNS_HOSTNAME);
  }

  if (!redirect_location(host, req->uri, location, sizeof(location))) {
    // The host did not validate. Say so plainly rather than redirecting
    // somewhere invented: a Location built from input we could not check is
    // worse than no Location at all (redirect.h).
    ESP_LOGW(TAG, "refusing to redirect: unusable Host header");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "bad Host header\n");
  }

  httpd_resp_set_status(req, REDIRECT_STATUS);
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_set_type(req, "text/html");
  // A body, because a client that does not follow redirects — curl without
  // -L, a service browser, someone telnetting the port to see what is there —
  // should still be told where the console is rather than shown a blank page.
  char body[LOCATION_MAX + 96];
  const int n = snprintf(body, sizeof(body),
                         "<!doctype html><meta charset=utf-8>"
                         "<title>airkvm</title>"
                         "<p>The console is at <a href=\"%s\">%s</a>",
                         location, location);
  if (n < 0 || (size_t)n >= sizeof(body)) {
    return httpd_resp_sendstr(req, "");
  }
  return httpd_resp_send(req, body, n);
}

esp_err_t http80_start(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  // Must not collide with the console's control socket. This value came from
  // logtap.c, which used to own this server; keeping the number means a build
  // that had the log tap enabled does not change which ports it opens.
  config.ctrl_port = 32001;
  config.lru_purge_enable = true;
  // The catch-all below is a wildcard, which esp_http_server only honours with
  // this matcher. Without it "/*" is a literal path and nothing matches it —
  // the redirect would appear to be registered and would never fire.
  config.uri_match_fn = httpd_uri_match_wildcard;
  // Two routes today. The default is larger, but stating it keeps a future
  // third route from silently failing to register.
  config.max_uri_handlers = 4;

  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "port 80 unavailable (%s) — no redirect, no log tap",
             esp_err_to_name(err));
    return err;
  }

  // ORDER IS PRECEDENCE. esp_http_server returns the first registered handler
  // whose pattern matches, and the redirect matches everything, so anything
  // with a real path must be registered before it. That is why both live here
  // instead of each module registering its own.
#if CONFIG_AIRKVM_LOG_TAP
  const httpd_uri_t log_route = {
      .uri = "/log", .method = HTTP_GET, .handler = logtap_http_get};
  err = httpd_register_uri_handler(server, &log_route);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "log tap route failed (%s)", esp_err_to_name(err));
  } else {
    ESP_LOGW(TAG, "log tap on http://<ip>/log — DIAGNOSTIC BUILD ONLY");
  }
#endif

  const httpd_uri_t redirect_route = {
      .uri = "/*", .method = HTTP_GET, .handler = redirect_get};
  err = httpd_register_uri_handler(server, &redirect_route);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "redirect route failed (%s)", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "port 80 redirects to https://%s.local/",
           CONFIG_AIRKVM_MDNS_HOSTNAME);
  return ESP_OK;
}

#endif  // CONFIG_AIRKVM_TLS
