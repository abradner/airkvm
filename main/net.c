// The console: mDNS advertisement, static asset serving, and the WebSocket that
// carries input. WiFi station bring-up moved to wifi.c.
//
// Verified on hardware (ESP32-C3 bench board, 2026-08-10): mDNS, static asset
// serving, and the full WebSocket frame path were exercised end to end against
// tools/poke.py and a reference websockets client. What remains unverified here
// is only what the bench board cannot do — see hid_out_usb.c.
//
// Security posture, stated where the code is rather than only in docs: the
// console and the input socket are UNAUTHENTICATED. Anyone who can route to
// this device can type on the target. That is risk R-001 in docs/repo-map.md,
// and it is accepted deliberately — the security model is physical. Do not add
// a feature that widens this surface (an outbound connection, a relay, a second
// network) without updating that register first.

#include "net.h"

#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_http_server.h"
#if CONFIG_AIRKVM_TLS
#include "esp_https_server.h"
#endif
#include "esp_log.h"
#include "hid_out.h"
#include "mdns.h"
#include "protocol.h"
#include "status.h"
#include "wifi.h"

static const char *TAG = "net";

// Embedded console assets, stored gzipped (see main/CMakeLists.txt) and served
// without decompressing — the browser does that.
extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_gz_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_gz_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_gz_end");

#if CONFIG_AIRKVM_TLS
// PEM material for the TLS server, embedded at build time (main/CMakeLists.txt).
extern const uint8_t servercert_start[] asm("_binary_device_crt_start");
extern const uint8_t servercert_end[] asm("_binary_device_crt_end");
extern const uint8_t serverkey_start[] asm("_binary_device_key_start");
extern const uint8_t serverkey_end[] asm("_binary_device_key_end");
#endif

// Exactly one console session drives HID at a time. Two clients interleaving
// modifiers into one HID state is risk R-003; refusing the second is the
// conservative half of that, and it is honest about which client is in control.
static int s_active_fd = -1;

static esp_err_t send_static(httpd_req_t *req, const char *type,
                             const uint8_t *start, const uint8_t *end) {
  httpd_resp_set_type(req, type);
  // Every asset is stored gzipped; without this header the browser renders the
  // compressed bytes as text.
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  // EMBED_FILES does not NUL-terminate (EMBED_TXTFILES does), so end - start is
  // already the exact byte count — subtracting one here truncated every asset.
  return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t index_get(httpd_req_t *req) {
  return send_static(req, "text/html", index_html_start, index_html_end);
}

static esp_err_t app_js_get(httpd_req_t *req) {
  return send_static(req, "application/javascript", app_js_start, app_js_end);
}

static esp_err_t style_css_get(httpd_req_t *req) {
  return send_static(req, "text/css", style_css_start, style_css_end);
}

static void session_ended(const char *why) {
  ESP_LOGI(TAG, "console session ended (%s) — releasing all", why);
  s_active_fd = -1;
  status_set_client(false);
  // Any loss of confidence releases everything (AGENTS.md §2, principle 4).
  hid_out_release_all();
}

static esp_err_t ws_handler(httpd_req_t *req) {
  // The handler is invoked once at handshake time with no frame pending, and
  // this branch must return before attempting any read. Verified on hardware,
  // because it is not obvious and the failure is baffling: without it, the
  // first read consumes the *next* frame's opcode byte (0x82) and interprets it
  // as a length byte — 0x82 & 0x7F = 2 — so every session's first frame decoded
  // as a masked 2-byte frame full of mask bytes, and the stream desynced from
  // there. `len=2` on every single first frame is the fingerprint.
  //
  // Data frames arrive with method 0, not HTTP_GET, so this branch is genuinely
  // only the handshake. The session is still claimed lazily on the first real
  // frame below rather than here.
  if (req->method == HTTP_GET) {
    return ESP_OK;
  }

  const int fd = httpd_req_to_sockfd(req);

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.type = HTTPD_WS_TYPE_BINARY;

  // Length probe first, then read into a bounded buffer.
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    if (fd == s_active_fd) {
      session_ended("recv failed");
    }
    return err;
  }

  if (frame.len == 0) {
    // Must return before the payload read: with len 0 that call re-enters the
    // header path and blocks for an entire new frame on the single server task,
    // stalling the console and every static request behind it. Reachable in
    // normal operation — an unsolicited PONG is a legal zero-length frame.
    return ESP_OK;
  }

  // Longest valid frame is a key frame: type + length + code.
  uint8_t buf[2 + AIRKVM_MAX_CODE_LEN];
  if (frame.len > sizeof(buf)) {
    // Nothing valid is this big. The payload cannot simply be left unread: those
    // bytes stay in the socket and the next recv parses payload as a frame
    // header. Since attacker-chosen bytes can decode as well-formed key frames,
    // that turns one malformed frame into unintended keystrokes. Tearing the
    // socket down is the one response that cannot desync.
    ESP_LOGW(TAG, "oversized frame (%d bytes) — closing socket",
             (int)frame.len);
    if (fd == s_active_fd) {
      session_ended("oversized frame");
    }
    return ESP_FAIL;
  }
  frame.payload = buf;
  // max_len must be this frame's own length, not the buffer's capacity. Passing
  // sizeof(buf) makes the read pull up to 25 bytes when the frame holds one,
  // swallowing the bytes of whatever arrives next and desyncing the stream —
  // observed on hardware as the first frame of every session decoding as
  // garbage while later frames looked fine.
  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err != ESP_OK) {
    if (fd == s_active_fd) {
      session_ended("payload read failed");
    }
    return err;
  }

  // Claim the session on the first frame that actually arrives. A second client
  // is accepted at the HTTP layer and then ignored here, so it sees a connected
  // socket that does nothing rather than fighting the first for the keyboard.
  if (s_active_fd < 0) {
    s_active_fd = fd;
    status_set_client(true);
    ESP_LOGI(TAG, "console session started (fd %d)", fd);
  } else if (fd != s_active_fd) {
    return ESP_OK;  // spectator (R-003)
  }

  struct airkvm_intent intent;
  if (airkvm_decode(buf, frame.len, &intent) != AIRKVM_DECODE_OK) {
    // A frame we cannot fully parse means a client we do not understand, on a
    // path that ends in keystrokes. Drop it; do not guess.
    ESP_LOGW(TAG, "rejected malformed frame");
    return ESP_OK;
  }

  if (!hid_out_submit(&intent)) {
    // Queue full. Whether that is dangerous depends on what was dropped: a lost
    // key or button event can leave something held, but a lost movement or
    // scroll only means the pointer moves less far. Releasing on dropped motion
    // would make a fast mouse flick tear down a held chord — the safety action
    // causing the very failure it exists to prevent.
    const bool droppable = intent.type == AIRKVM_FRAME_MOUSE_MOVE ||
                           intent.type == AIRKVM_FRAME_MOUSE_SCROLL;
    if (droppable) {
      ESP_LOGD(TAG, "HID queue full — dropped motion");
    } else {
      ESP_LOGW(TAG, "HID queue full — releasing all");
      hid_out_release_all();
    }
  }
  return ESP_OK;
}

// Called by the HTTP server when a socket is torn down for any reason —
// including the ones the handler never sees, like a laptop closing its lid.
static void ws_closed(httpd_handle_t hd, int sockfd) {
  (void)hd;
  if (sockfd == s_active_fd) {
    session_ended("socket closed");
  }
  // Setting close_fn *replaces* the server's own close(), it does not add to
  // it. Without this every finished request — including each static asset —
  // leaks its socket, and the console becomes unreachable after a handful of
  // page loads. The fd may already be invalid; the return is deliberately
  // ignored.
  close(sockfd);
}

static esp_err_t start_http(void) {
  httpd_handle_t server = NULL;

#if CONFIG_AIRKVM_TLS
  // The TLS config embeds a plain httpd_config_t and yields an ordinary
  // handle, so URI handlers — including the WebSocket one — are identical
  // whether or not TLS is on. Only the scheme the browser uses changes.
  httpd_ssl_config_t tls = HTTPD_SSL_CONFIG_DEFAULT();
  tls.httpd.close_fn = ws_closed;
  tls.httpd.lru_purge_enable = true;
  // Each TLS session costs tens of KB; the default socket count would exhaust
  // heap before it exhausted sockets.
  tls.httpd.max_open_sockets = 4;
  tls.servercert = servercert_start;
  tls.servercert_len = servercert_end - servercert_start;
  tls.prvtkey_pem = serverkey_start;
  tls.prvtkey_len = serverkey_end - serverkey_start;

  const esp_err_t tls_err = httpd_ssl_start(&server, &tls);
  if (tls_err != ESP_OK) {
    // Deliberately fatal rather than falling back to plaintext. A silent
    // downgrade would leave keystrokes readable on the wire (R-007) while the
    // console still appeared to work, which is the worst of both.
    ESP_LOGE(TAG, "HTTPS failed to start (%s) — refusing to serve plaintext",
             esp_err_to_name(tls_err));
    return tls_err;
  }
  ESP_LOGI(TAG, "console served over HTTPS");
#else
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.close_fn = ws_closed;
  config.lru_purge_enable = true;
  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start failed");
  ESP_LOGW(TAG,
           "console served over plain HTTP — keystrokes are readable on "
           "the network (R-007), and browsers will refuse camera access");
#endif

  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = index_get},
      {.uri = "/app.js", .method = HTTP_GET, .handler = app_js_get},
      {.uri = "/style.css", .method = HTTP_GET, .handler = style_css_get},
      {.uri = "/ws",
       .method = HTTP_GET,
       .handler = ws_handler,
       .is_websocket = true},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG,
                        "failed to register %s", routes[i].uri);
  }
  return ESP_OK;
}

static esp_err_t start_mdns(void) {
  ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init failed");
  ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_AIRKVM_MDNS_HOSTNAME), TAG,
                      "mdns hostname failed");
  ESP_RETURN_ON_ERROR(mdns_instance_name_set("airkvm console"), TAG,
                      "mdns instance failed");
  return mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t net_start(void) {
  ESP_RETURN_ON_ERROR(wifi_start(), TAG, "wifi failed");
  ESP_RETURN_ON_ERROR(start_mdns(), TAG, "mdns failed");
  ESP_RETURN_ON_ERROR(start_http(), TAG, "http failed");
  return ESP_OK;
}
