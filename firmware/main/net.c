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
// network) without updating that register first. The media socket started
// from net_start() is such a widening, registered as R-008: the same caller
// can also present a disk to the target.

#include "net.h"

#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#if CONFIG_AIRKVM_TLS
#include "esp_https_server.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
// For WIFI_EVENT / WIFI_EVENT_STA_DISCONNECTED only: this module subscribes to
// network loss because that is a terminal session event (see wifi_lost).
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hid_out.h"
#include "mdns.h"
#if CONFIG_TINYUSB_MSC_ENABLED
#include "media_ws.h"
#endif
#include "protocol.h"
#include "session.h"
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

// Who is driving. All ownership decisions go through session.c; this file holds
// the only instance and the only place its transitions have effects.
static struct session s_session;

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

// Session transitions run from two tasks — the HTTP server task (the WebSocket
// handler and the close callback) and the event loop task (WiFi loss) — so each
// read-modify-write is serialised. Without it, a claim racing a disconnect can
// leave ownership set after the release has already run, which is the stale
// owner this module exists to make impossible.
//
// A mutex rather than a portMUX spinlock, deliberately: the front-panel update
// belongs inside the same critical region as the transition (see below), and
// status_set_client() takes a mutex of its own — which is illegal inside
// portENTER_CRITICAL, where the scheduler is suspended. Both callers are task
// context, never an ISR, so a mutex is the right primitive. Lock order is
// session then status, and status.c never calls back here, so it cannot invert.
static SemaphoreHandle_t s_session_lock;

// A client that vanishes without closing its TCP connection — a laptop
// suspended, a phone backgrounding its browser, a network that goes away
// mid-chord — leaves the socket open as far as this device is concerned, and
// with it every key it was holding. The console sends a PING every second and
// the session is torn down after this much silence, which is the only thing
// covering that gap (docs/repo-map.md, session lifecycle).
//
// Three missed pings rather than one: a single dropped frame on a marginal link
// is normal (this device has been measured at -85 dBm), and tearing down a live
// session costs the operator their chord for no reason.
//
// What this deadline actually measures is not the client's liveness but the
// *server task's availability*, and the two are not the same. esp_http_server
// runs one task that accepts connections, performs TLS handshakes, serves the
// static assets and dispatches WebSocket frames, serially; this timer runs on
// the independent esp_timer task and is therefore always punctual. So a PING
// that arrived on time but is queued behind other work on that one task counts
// as silence. A TLS handshake was measured at ~4.8s on this hardware at
// -85 dBm — longer than this entire deadline — and R-001 means any device on
// the LAN can trigger one, unauthenticated, at any moment.
//
// The consequence is a real false-teardown mode: an operator holding a chord
// can lose it because someone reloaded the console. It is accepted here rather
// than papered over with a longer deadline, because principle 4 settles exactly
// this tension in favour of releasing — a spurious release costs a chord, a
// missed one can leave a key held on someone's production machine. Fixing it
// properly means detecting a vanished peer somewhere that head-of-line blocking
// cannot reach (TCP keepalive on the socket, or a dedicated task); tracked as
// an issue rather than guessed at without hardware.
#define SESSION_TIMEOUT_MS 3000
#define SESSION_CHECK_INTERVAL_US (1000 * 1000)

static esp_timer_handle_t s_keepalive;

static uint32_t now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// Logging and hid_out_release_all() stay outside the lock; they are not needed
// for correctness there and one of them talks to USB.
void net_end_session(const char *why) {
  xSemaphoreTake(s_session_lock, portMAX_DELAY);
  const bool ended = session_end(&s_session);
  if (ended) {
    status_set_client(false);
  }
  xSemaphoreGive(s_session_lock);

  if (ended) {
    ESP_LOGI(TAG, "console session ended (%s) — releasing all", why);
  }
  // Released unconditionally. Principle 4 prefers a spurious release-all over a
  // plausible held key, and this is reached on paths (WiFi loss) where a racing
  // socket close may already have cleared ownership while the target still
  // holds something.
  hid_out_release_all();
}

static void end_session_if_owner(int fd, const char *why) {
  xSemaphoreTake(s_session_lock, portMAX_DELAY);
  const bool ended = session_end_if_owner(&s_session, fd);
  if (ended) {
    status_set_client(false);
  }
  xSemaphoreGive(s_session_lock);

  if (ended) {
    ESP_LOGI(TAG, "console session ended (%s) — releasing all", why);
    // Only released when this fd actually held the session: a spectator's
    // socket closing must not tear the keyboard out from under the client that
    // is driving.
    hid_out_release_all();
  }
}

// Runs on the esp_timer task — a third task touching session state, hence the
// same lock.
//
// Expiry is tested and acted on in one lock acquisition rather than checking
// here and calling net_end_session() afterwards. That version would tear down
// whichever session existed by the time it got there: a frame arriving in the
// gap refreshes liveness and the teardown still fires, costing a live chord,
// and if the old owner's socket closed in the gap it destroys the *new*
// client's session instead.
static void keepalive_tick(void *arg) {
  (void)arg;
  // Never block here. This runs on the esp_timer task, which every timer in the
  // firmware shares, so waiting on a contended lock would stall unrelated
  // timers rather than just this one. The sections under this lock are
  // microseconds long and contention is not expected — but "not expected" is a
  // poor reason to let one subsystem's stall become everyone's. Skipping a tick
  // costs at most one extra second before a vanished client is released, which
  // the next tick handles.
  if (xSemaphoreTake(s_session_lock, 0) != pdTRUE) {
    return;
  }
  const bool ended =
      session_end_if_expired(&s_session, now_ms(), SESSION_TIMEOUT_MS);
  if (ended) {
    status_set_client(false);
  }
  xSemaphoreGive(s_session_lock);

  if (ended) {
    ESP_LOGW(TAG, "no frame for %dms — assuming the console is gone, releasing",
             SESSION_TIMEOUT_MS);
    hid_out_release_all();
  }
}

// The console is gone with the network, so whatever it was holding must go too
// — a target left with a held key while WiFi retries is the exact failure
// principle 4 names. This lives here rather than in wifi.c because clearing
// ownership and releasing keys are the same action, and splitting them across
// modules is how a stale owner survives a reconnect.
static void wifi_lost(void *arg, esp_event_base_t base, int32_t id,
                      void *data) {
  (void)arg;
  (void)base;
  (void)id;
  (void)data;
  net_end_session("wifi disconnected");
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
    end_session_if_owner(fd, "recv failed");
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
    end_session_if_owner(fd, "oversized frame");
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
    end_session_if_owner(fd, "payload read failed");
    return err;
  }

  // Claim the session on the first frame that actually arrives. A second client
  // is accepted at the HTTP layer and then ignored here, so it sees a connected
  // socket that does nothing rather than fighting the first for the keyboard.
  xSemaphoreTake(s_session_lock, portMAX_DELAY);
  const bool was_unowned = !session_has_owner(&s_session);
  // Re-claiming on every frame is what keeps the session alive; a PING carries
  // no other meaning and exists solely to reach this line.
  const bool mine = session_claim(&s_session, fd, now_ms());
  // Published inside the lock, with the transition that justifies it. Setting
  // it afterwards let a concurrent WiFi-loss teardown write "no client" first
  // and this write land second, leaving the panel reporting a session that had
  // already been torn down — and nothing corrects it until the next event.
  if (mine && was_unowned) {
    status_set_client(true);
  }
  xSemaphoreGive(s_session_lock);

  if (!mine) {
    return ESP_OK;  // spectator (R-003)
  }
  if (was_unowned) {
    ESP_LOGI(TAG, "console session started (fd %d)", fd);
  }

  struct airkvm_intent intent;
  if (airkvm_decode(buf, frame.len, &intent) != AIRKVM_DECODE_OK) {
    // A frame we cannot fully parse means a client we do not understand, on a
    // path that ends in keystrokes. Drop it; do not guess.
    ESP_LOGW(TAG, "rejected malformed frame");
    return ESP_OK;
  }

  if (intent.type == AIRKVM_FRAME_STATUS_REQ) {
    // The display's facts, for a machine. Synchronous like WAKE_RESULT and as
    // small — but unlike it, periodic: the console polls every other
    // keepalive tick, so the send_wait_timeout exposure described at
    // WAKE_RESULT below is steady rather than rare. Four bytes to a peer
    // that just sent us a frame; if that blocks, the keepalive path was
    // already in trouble.
    struct status_snapshot snap;
    status_get(&snap);
    const struct airkvm_status_frame st = {.mounted = snap.usb_mounted,
                                           .suspended = snap.usb_suspended,
                                           .media = snap.media_attached};
    uint8_t reply[AIRKVM_DOWN_STATUS_LEN];
    airkvm_encode_status(reply, sizeof(reply), &st);
    httpd_ws_frame_t out;
    memset(&out, 0, sizeof(out));
    out.final = true;
    out.type = HTTPD_WS_TYPE_BINARY;
    out.payload = reply;
    out.len = sizeof(reply);
    const esp_err_t sent = httpd_ws_send_frame(req, &out);
    if (sent != ESP_OK) {
      ESP_LOGW(TAG, "STATUS reply failed: %s", esp_err_to_name(sent));
    }
    return ESP_OK;
  }

  if (intent.type == AIRKVM_FRAME_WAKE) {
    // Answered here, not queued. A suspended bus is exactly the state in which
    // the HID queue's head is parked (hid_out_usb.c, s_wake_pending), so a
    // wake routed through it would wait for the resume it is meant to cause.
    // hid_out_wake_target() only sets a flag; the transport is still touched
    // by the report task alone (AGENTS.md §4). Only the session owner reaches
    // this line, so a spectator cannot wake the target (R-003).
    //
    // The reply is the protocol's first downstream frame. Sent synchronously
    // from the handler on the server task: no stored handle, no cross-task
    // hop. It can block the way any response can — a client whose receive
    // window is full holds the send for up to send_wait_timeout (5 s), which
    // is the §9 entry 9 exposure the console's static assets already have —
    // but two bytes to a peer that just sent us a frame is as small as that
    // risk gets (STATUS above is the same size class, and periodic).
    const enum airkvm_wake_result result = hid_out_wake_target();
    uint8_t reply[2] = {AIRKVM_DOWN_WAKE_RESULT, (uint8_t)result};
    httpd_ws_frame_t out;
    memset(&out, 0, sizeof(out));
    out.final = true;
    out.type = HTTPD_WS_TYPE_BINARY;
    out.payload = reply;
    out.len = sizeof(reply);
    const esp_err_t sent = httpd_ws_send_frame(req, &out);
    ESP_LOGI(TAG, "wake requested: result %d, reply %s", (int)result,
             esp_err_to_name(sent));
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
  end_session_if_owner(sockfd, "socket closed");
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
  // Advertise what is actually being served. A hardcoded _http/80 record was
  // once simply false on a TLS build and pointed service browsers at a port
  // nothing answered on (issue #4); start_http() treats a TLS failure as fatal
  // rather than falling back to plaintext (R-007), so the console is only ever
  // on 443 there.
  //
  // A TLS build now DOES listen on 80 (http80.c, a redirect to 443), and this
  // still advertises only _https. That is deliberate rather than an oversight:
  // the record says where the CONSOLE is, and a service browser offered both
  // would list this device twice and send half its users through an extra hop
  // to reach the same page. The redirect exists for a human typing a bare
  // hostname, which is not a path service discovery takes.
  // Mirrors the same #if in start_http(); issue #4.
#if CONFIG_AIRKVM_TLS
  return mdns_service_add(NULL, "_https", "_tcp", 443, NULL, 0);
#else
  return mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
#endif
}

// How far net_start() got, so callers can tell "no TCP/IP stack at all" from
// "stack up, console down". Both are failures of net_start(), and they want
// opposite things from the port-80 server: the first cannot host one, the
// second NEEDS one, because that is the case the log tap exists to diagnose.
static bool s_netif_ready;
static bool s_console_up;

bool net_netif_ready(void) {
  return s_netif_ready;
}

bool net_console_up(void) {
  return s_console_up;
}

esp_err_t net_start(void) {
  s_session_lock = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_session_lock != NULL, ESP_ERR_NO_MEM, TAG,
                      "session lock alloc failed");
  session_reset(&s_session);
  ESP_RETURN_ON_ERROR(wifi_start(), TAG, "wifi failed");
  // esp_netif_init() happened inside wifi_start(), so from here on there is a
  // stack to bind to even if everything after this fails.
  s_netif_ready = true;
  // Registered after wifi_start() so the default event loop exists. Losing the
  // network is a terminal session event like any other.
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(
          WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_lost, NULL, NULL),
      TAG, "disconnect handler failed");
  ESP_RETURN_ON_ERROR(start_mdns(), TAG, "mdns failed");

  // Started before the server, deliberately. If it were started after, a timer
  // failure would return an error from net_start() with the console already
  // listening — and app_main treats that error as "the network never came up"
  // and carries on, leaving a reachable console whose sessions can never expire.
  // That is precisely the gap this commit closes, reopened by an error path.
  // Running with no client is free: an unowned session never expires.
  const esp_timer_create_args_t keepalive = {
      .callback = keepalive_tick,
      .name = "session_keepalive",
  };
  ESP_RETURN_ON_ERROR(esp_timer_create(&keepalive, &s_keepalive), TAG,
                      "keepalive timer failed");
  const esp_err_t timer_err =
      esp_timer_start_periodic(s_keepalive, SESSION_CHECK_INTERVAL_US);
  if (timer_err != ESP_OK) {
    esp_timer_delete(s_keepalive);
    s_keepalive = NULL;
    ESP_RETURN_ON_ERROR(timer_err, TAG, "keepalive start failed");
  }

  const esp_err_t http_err = start_http();
  s_console_up = http_err == ESP_OK;
  if (http_err != ESP_OK) {
    // The timer is started above so that it can never be missing while the
    // console is reachable. The cost of that ordering is this cleanup: on a
    // failure app_main() returns and nothing would ever stop a timer left
    // running, so a net_start() that reports failure must leave none behind.
    esp_timer_stop(s_keepalive);
    esp_timer_delete(s_keepalive);
    s_keepalive = NULL;
    ESP_RETURN_ON_ERROR(http_err, TAG, "http failed");
  }
#if CONFIG_TINYUSB_MSC_ENABLED
  // The media socket is its own server instance (media_ws.h). Not fatal:
  // a console without mass-storage media is still a console, and saying so
  // beats taking the keyboard down with it.
  const esp_err_t media_err = media_ws_start();
  if (media_err != ESP_OK) {
    ESP_LOGE(TAG, "media socket failed to start (%s) — no disk image support",
             esp_err_to_name(media_err));
  }
#endif
  return ESP_OK;
}
