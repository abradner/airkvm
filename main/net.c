// Verified on hardware (ESP32-C3 bench board, 2026-08-10): WiFi association,
// mDNS, static asset serving, and the full WebSocket frame path were exercised
// end to end against tools/poke.py and a reference websockets client. What
// remains unverified here is only what the bench board cannot do — see
// hid_out_usb.c.
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
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "hid_out.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "protocol.h"

static const char *TAG = "net";

// Embedded console assets (see main/CMakeLists.txt EMBED_FILES).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");

// Exactly one console session drives HID at a time. Two clients interleaving
// modifiers into one HID state is risk R-003; refusing the second is the
// conservative half of that, and it is honest about which client is in control.
static int s_active_fd = -1;

static esp_err_t send_static(httpd_req_t *req, const char *type,
                             const uint8_t *start, const uint8_t *end) {
  httpd_resp_set_type(req, type);
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
  uint8_t buf[2 + AIRHID_MAX_CODE_LEN];
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
    ESP_LOGI(TAG, "console session started (fd %d)", fd);
  } else if (fd != s_active_fd) {
    return ESP_OK;  // spectator (R-003)
  }

  struct airhid_intent intent;
  if (airhid_decode(buf, frame.len, &intent) != AIRHID_DECODE_OK) {
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
    const bool droppable = intent.type == AIRHID_FRAME_MOUSE_MOVE ||
                           intent.type == AIRHID_FRAME_MOUSE_SCROLL;
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
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.close_fn = ws_closed;
  config.lru_purge_enable = true;

  httpd_handle_t server = NULL;
  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start failed");

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

static void wifi_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)arg;
  (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    // The console is gone with the network, so whatever it was holding must go
    // too — a target left with a held key while we retry is the exact failure
    // principle 4 names.
    hid_out_release_all();
    ESP_LOGW(TAG, "WiFi disconnected; retrying");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "console at http://" IPSTR "/ (or http://%s.local/)",
             IP2STR(&event->ip_info.ip), CONFIG_AIRHID_MDNS_HOSTNAME);
  }
}

static esp_err_t start_wifi(void) {
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                      "event loop failed");
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                          WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL),
                      TAG, "wifi handler failed");
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          wifi_event, NULL, NULL),
      TAG, "ip handler failed");

  // Required config, no fallback default (AGENTS.md §8, fail fast on boot).
  // Checked at runtime rather than with _Static_assert: the firmware must still
  // compile with no SSID configured, because CI builds a fresh checkout that has
  // no sdkconfig and never will have credentials in it.
  if (CONFIG_AIRHID_WIFI_SSID[0] == '\0') {
    ESP_LOGE(TAG, "no WiFi SSID configured — run `mise run config`");
    return ESP_ERR_INVALID_STATE;
  }

  wifi_config_t wifi_config = {0};
  strlcpy((char *)wifi_config.sta.ssid, CONFIG_AIRHID_WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, CONFIG_AIRHID_WIFI_PASSWORD,
          sizeof(wifi_config.sta.password));

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                      "set_config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
  return ESP_OK;
}

static esp_err_t start_mdns(void) {
  ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init failed");
  ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_AIRHID_MDNS_HOSTNAME), TAG,
                      "mdns hostname failed");
  ESP_RETURN_ON_ERROR(mdns_instance_name_set("airhid console"), TAG,
                      "mdns instance failed");
  return mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t net_start(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
    err = nvs_flash_init();
  }
  ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

  ESP_RETURN_ON_ERROR(start_wifi(), TAG, "wifi failed");
  ESP_RETURN_ON_ERROR(start_mdns(), TAG, "mdns failed");
  ESP_RETURN_ON_ERROR(start_http(), TAG, "http failed");
  return ESP_OK;
}
