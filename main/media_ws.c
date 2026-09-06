// See media_ws.h. Unverified on hardware.

#include "media_ws.h"

#include <string.h>
#include <unistd.h>

#include "block_source_browser.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#if CONFIG_AIRKVM_TLS
#include "esp_https_server.h"
#endif
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "media_proto.h"
#include "msc_out.h"
#include "status.h"

static const char *TAG = "media_ws";

// Not 443/80: the console owns those, and this instance must not race it for
// a port (the same reason AIRKVM_LOG_TAP depends on AIRKVM_TLS).
#define MEDIA_PORT_SECURE 8443
#define MEDIA_PORT_INSECURE 8080

#if CONFIG_AIRKVM_TLS
extern const uint8_t servercert_start[] asm("_binary_device_crt_start");
extern const uint8_t servercert_end[] asm("_binary_device_crt_end");
extern const uint8_t serverkey_start[] asm("_binary_device_key_start");
extern const uint8_t serverkey_end[] asm("_binary_device_key_end");
#endif

// Largest legal frame: a DATA piece plus its header.
#define RX_MAX (15 + MEDIA_DATA_MAX)

static httpd_handle_t s_server;
static SemaphoreHandle_t s_lock;  // s_owner_fd
// Who holds the medium. One client at a time — the storage twin of R-003:
// two images interleaving into one LUN is worse than refusing the second.
static int s_owner_fd = -1;
static uint8_t *s_rx;

// Outgoing REQ frames are sent asynchronously from the msc_io task via the
// server's work queue, so each needs a buffer that outlives the call. One per
// possible in-flight request; `busy` is cleared by the send callback.
#define REQ_BUFS 4
static struct {
  uint8_t buf[MEDIA_DOWN_REQ_LEN];
  volatile bool busy;
} s_req[REQ_BUFS];

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// --- medium ownership ---------------------------------------------------------

// The storage release-all: unbind the LUN first so a transfer in flight fails
// on generation, then drop the source so a blocked read wakes and fails, then
// tell the display. Total, and idempotent, because every terminal event calls
// it and several can race for the same medium (socket close and a failed read
// both fire).
static void drop_medium(const char *why) {
  LOCK();
  const bool had = s_owner_fd >= 0;
  s_owner_fd = -1;
  UNLOCK();
  if (!had) {
    return;
  }
  ESP_LOGI(TAG, "medium dropped (%s)", why);
  msc_out_unbind();
  browser_source_detach();
  status_set_media(false, NULL);
}

static void drop_medium_if_owner(int fd, const char *why) {
  LOCK();
  const bool mine = s_owner_fd == fd;
  UNLOCK();
  if (mine) {
    drop_medium(why);
  }
}

// --- sending ----------------------------------------------------------------

static void req_sent(esp_err_t err, int socket, void *arg) {
  (void)socket;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "REQ send failed: %s", esp_err_to_name(err));
  }
  // Under the lock, like the claim in request_chunk: the two run on different
  // tasks and a flag is not a synchronisation primitive.
  LOCK();
  ((volatile bool *)arg)[0] = false;
  UNLOCK();
}

// browser_stalled_fn: the client stopped answering. Called from the msc_io
// task; drop_medium is safe from any task.
static void client_stalled(void) {
  drop_medium("browser stopped answering");
}

// browser_request_fn: called from the msc_io task. Never blocks — the work
// is queued to the media server's task, and a queue that cannot take it is a
// failed request, which the source turns into a failed USB transfer.
static bool request_chunk(uint32_t req_id, uint32_t chunk) {
  int slot = -1;
  LOCK();
  const int fd = s_owner_fd;
  if (fd >= 0) {
    for (int i = 0; i < REQ_BUFS; i++) {
      if (!s_req[i].busy) {
        slot = i;
        s_req[i].busy = true;
        break;
      }
    }
  }
  UNLOCK();
  if (fd < 0 || slot < 0) {
    return false;
  }
  media_encode_req(s_req[slot].buf, sizeof(s_req[slot].buf), req_id, chunk);
  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.final = true;
  frame.type = HTTPD_WS_TYPE_BINARY;
  frame.payload = s_req[slot].buf;
  frame.len = MEDIA_DOWN_REQ_LEN;
  const esp_err_t err = httpd_ws_send_data_async(s_server, fd, &frame, req_sent,
                                                 (void *)&s_req[slot].busy);
  if (err != ESP_OK) {
    LOCK();
    s_req[slot].busy = false;
    UNLOCK();
    ESP_LOGW(TAG, "could not queue REQ: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// Synchronous, on the server task, inside the handler.
static void send_status(httpd_req_t *req, enum media_state state) {
  uint8_t buf[MEDIA_DOWN_STATUS_LEN];
  media_encode_status(buf, sizeof(buf), state, browser_source_bytes_served(),
                      browser_source_pending());
  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.final = true;
  frame.type = HTTPD_WS_TYPE_BINARY;
  frame.payload = buf;
  frame.len = sizeof(buf);
  (void)httpd_ws_send_frame(req, &frame);
}

// --- the handler --------------------------------------------------------------

static esp_err_t media_handler(httpd_req_t *req) {
  // Handshake invocation: no frame pending, must not read (AGENTS.md §9
  // entry 1, the same trap as /ws).
  if (req->method == HTTP_GET) {
    return ESP_OK;
  }
  const int fd = httpd_req_to_sockfd(req);

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.type = HTTPD_WS_TYPE_BINARY;
  // Length probe, then a bounded read of exactly that length (entry 3).
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    drop_medium_if_owner(fd, "recv failed");
    return err;
  }
  if (frame.len == 0) {
    return ESP_OK;  // unsolicited PONG; must not re-enter the header path
  }
  if (frame.len > RX_MAX) {
    // Cannot be left unread (the bytes would parse as the next header), and
    // nothing legal is this big. Tear it down.
    ESP_LOGW(TAG, "oversized media frame (%d bytes) — closing", (int)frame.len);
    drop_medium_if_owner(fd, "oversized frame");
    return ESP_FAIL;
  }
  frame.payload = s_rx;
  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err != ESP_OK) {
    drop_medium_if_owner(fd, "payload read failed");
    return err;
  }

  struct media_msg msg;
  const enum media_decode_result r = media_decode(s_rx, frame.len, &msg);
  if (r != MEDIA_DECODE_OK) {
    ESP_LOGW(TAG, "rejected media frame (%d)", (int)r);
    return ESP_OK;
  }

  LOCK();
  const int owner = s_owner_fd;
  UNLOCK();
  const bool mine = owner == fd;

  switch (msg.type) {
    case MEDIA_FRAME_ATTACH:
      if (owner >= 0 && !mine) {
        send_status(req, MEDIA_STATE_BUSY);
        return ESP_OK;
      }
      if (mine) {
        // Re-attach from the same client: a detach then an attach, so the
        // LUN raises attention and the host re-reads capacity. Through the
        // one drop path, so it is logged and total like every other drop.
        drop_medium("re-attach");
      }
      if (!browser_source_attach(msg.attach.block_count, msg.attach.block_size,
                                 msg.attach.name)) {
        // Not enough internal heap to hold the cache and still let the
        // console handshake. Media is the thing that gives way.
        send_status(req, MEDIA_STATE_NO_MEMORY);
        return ESP_OK;
      }
      LOCK();
      s_owner_fd = fd;
      UNLOCK();
      msc_out_bind(browser_source());
      status_set_media(true, msg.attach.name);
      ESP_LOGI(TAG, "fd %d attached \"%s\"", fd, msg.attach.name);
      send_status(req, MEDIA_STATE_ATTACHED);
      return ESP_OK;

    case MEDIA_FRAME_DETACH:
      if (mine) {
        drop_medium("client detached");
      }
      send_status(req, MEDIA_STATE_NONE);
      return ESP_OK;

    case MEDIA_FRAME_DATA:
      if (!mine) {
        return ESP_OK;  // a spectator's bytes are not the medium's
      }
      if (!browser_source_deliver(msg.data.req_id, msg.data.chunk,
                                  msg.data.offset, msg.data.data,
                                  msg.data.len)) {
        ESP_LOGD(TAG, "unwanted piece: req %lu chunk %lu",
                 (unsigned long)msg.data.req_id, (unsigned long)msg.data.chunk);
      }
      return ESP_OK;

    case MEDIA_FRAME_ERROR:
      if (mine) {
        ESP_LOGW(TAG, "client failed req %lu (code %u)",
                 (unsigned long)msg.error.req_id, msg.error.code);
        browser_source_fail(msg.error.req_id);
      }
      return ESP_OK;
  }
  return ESP_OK;
}

static void media_closed(httpd_handle_t hd, int sockfd) {
  (void)hd;
  drop_medium_if_owner(sockfd, "socket closed");
  // Replaces the server's own close (see ws_closed in net.c).
  close(sockfd);
}

static void wifi_lost(void *arg, esp_event_base_t base, int32_t id,
                      void *data) {
  (void)arg;
  (void)base;
  (void)id;
  (void)data;
  drop_medium("wifi disconnected");
}

// --- lifecycle --------------------------------------------------------------

static esp_err_t start_inner(void);

// Failure here is non-fatal to the console (net.c logs and carries on), and
// it is most likely under exactly the low-heap conditions where a leak would
// matter, so everything allocated is given back on the way out.
esp_err_t media_ws_start(void) {
  const esp_err_t err = start_inner();
  if (err != ESP_OK) {
    if (s_server) {
      httpd_stop(s_server);
      s_server = NULL;
    }
    free(s_rx);
    s_rx = NULL;
    if (s_lock) {
      vSemaphoreDelete(s_lock);
      s_lock = NULL;
    }
  }
  return err;
}

static esp_err_t start_inner(void) {
  s_lock = xSemaphoreCreateMutex();
  s_rx = malloc(RX_MAX);
  ESP_RETURN_ON_FALSE(s_lock && s_rx, ESP_ERR_NO_MEM, TAG, "alloc failed");
  ESP_RETURN_ON_ERROR(browser_source_init(request_chunk, client_stalled), TAG,
                      "browser source init failed");

#if CONFIG_AIRKVM_TLS
  httpd_ssl_config_t tls = HTTPD_SSL_CONFIG_DEFAULT();
  tls.port_secure = MEDIA_PORT_SECURE;
  tls.httpd.close_fn = media_closed;
  // NO LRU purge, unlike the console instance. The server's LRU clock only
  // advances on receive, and an attached owner whose target is idle sends
  // nothing — it would be the first socket purged whenever anyone else
  // connected, and the target's disk would vanish. A surplus connection
  // waits at the listen backlog instead.
  tls.httpd.lru_purge_enable = false;
  // TCP keepalive as the backstop behind the request-timeout escalation in
  // block_source_browser.c: a peer that vanished without closing is noticed
  // in ~15 s even while no request is outstanding.
  tls.httpd.keep_alive_enable = true;
  tls.httpd.keep_alive_idle = 5;
  tls.httpd.keep_alive_interval = 5;
  tls.httpd.keep_alive_count = 3;
  // One client slot, not two: a second media client would cost a second TLS
  // session (~40 KB) from the heap the console's own handshakes come out of.
  // A second client therefore cannot connect at all while one is connected
  // (it waits at the listen backlog and its console shows "reconnecting");
  // MEDIA_STATE_BUSY in the protocol is reserved for a future build that can
  // afford a spectator slot.
  tls.httpd.max_open_sockets = 1;
  // A different control port from the console instance, or the second
  // httpd_start fails with EADDRINUSE on the internal UDP socket.
  tls.httpd.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
  tls.servercert = servercert_start;
  tls.servercert_len = servercert_end - servercert_start;
  tls.prvtkey_pem = serverkey_start;
  tls.prvtkey_len = serverkey_end - serverkey_start;
  ESP_RETURN_ON_ERROR(httpd_ssl_start(&s_server, &tls), TAG,
                      "media HTTPS failed to start");
  ESP_LOGI(TAG, "media socket on wss port %d", MEDIA_PORT_SECURE);
#else
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = MEDIA_PORT_INSECURE;
  config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
  config.close_fn = media_closed;
  config.lru_purge_enable = false;  // see the TLS branch
  config.keep_alive_enable = true;
  config.keep_alive_idle = 5;
  config.keep_alive_interval = 5;
  config.keep_alive_count = 3;
  config.max_open_sockets = 1;
  ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG,
                      "media HTTP failed to start");
  ESP_LOGW(TAG, "media socket on plain ws port %d (bench build)",
           MEDIA_PORT_INSECURE);
#endif

  const httpd_uri_t route = {.uri = "/ws-media",
                             .method = HTTP_GET,
                             .handler = media_handler,
                             .is_websocket = true};
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &route), TAG,
                      "failed to register /ws-media");
  // Losing the network is a terminal event for the medium, as for a session.
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(
          WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_lost, NULL, NULL),
      TAG, "disconnect handler failed");
  return ESP_OK;
}
