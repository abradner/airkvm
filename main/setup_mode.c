// See setup_mode.h. Unverified on hardware.

#include "setup_mode.h"

#include <string.h>
#include <sys/socket.h>

#include "captive_dns.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "provision.h"
#include "status.h"
#if CONFIG_TINYUSB_NET_MODE_NCM
#include "usb_net.h"
#endif
#include "wifi.h"

static const char *TAG = "setup";

// How long the AP stays up with nobody finishing the form. Bounded exposure;
// a second 5 s hold exits early (ui.c).
#define SETUP_TIMEOUT_S 300
// Enough for a phone to hold the portal open plus a laptop poking at it.
#define SETUP_HTTP_SOCKETS 6
// A form with a 32-byte SSID and a 63-byte passphrase, percent-encoded three
// bytes per character in the worst case, is well under this.
#define SETUP_BODY_MAX 512

extern const uint8_t setup_html_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_gz_end");

static esp_netif_t *s_ap;
static char s_ip[16];
// DHCP option 114's value. esp_netif_dhcps_option() keeps the POINTER, not a
// copy (dhcpserver.c stores it and strlen()s it on every OFFER/ACK), so this
// must outlive start_ap() — a stack buffer here was a use-after-return that
// a reviewer caught. The IDF example mallocs and never frees for the same
// reason.
static char s_captive_uri[32];
// Set by a successful save; the main loop restarts shortly after so the
// reply reaches the browser first.
static volatile bool s_saved;

// --- HTTP --------------------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char *)setup_html_start,
                         setup_html_end - setup_html_start);
}

static esp_err_t send_plain(httpd_req_t *req, const char *status,
                            const char *text) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len > SETUP_BODY_MAX) {
    return send_plain(req, "400 Bad Request",
                      "<p>Form too large or empty.</p>");
  }
  char body[SETUP_BODY_MAX + 1];
  size_t got = 0;
  while (got < req->content_len) {
    const int n = httpd_req_recv(req, body + got, req->content_len - got);
    if (n <= 0) {
      return ESP_FAIL;  // socket gone; nothing to say to nobody
    }
    got += (size_t)n;
  }
  body[got] = '\0';

  struct provision_form form;
  if (!provision_parse_form(body, got, &form)) {
    // The page's own checks refuse these first; this is the server saying it
    // too (AGENTS.md §8: the enforcement is here, the page is politeness).
    return send_plain(req, "400 Bad Request",
                      "<p>Network name must be 1&ndash;32 bytes (UTF-8) and "
                      "the password empty or 8&ndash;63 bytes.</p>"
                      "<p><a href=\"/\">Back</a></p>");
  }

  wifi_config_t sta = {0};
  strlcpy((char *)sta.sta.ssid, form.ssid, sizeof(sta.sta.ssid));
  strlcpy((char *)sta.sta.password, form.psk, sizeof(sta.sta.password));
  // Persists to NVS (CONFIG_ESP_WIFI_NVS_ENABLED); wifi.c reads it back on
  // the next boot. The STA interface exists because the radio is in APSTA
  // mode for exactly this call.
  const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
    return send_plain(req, "500 Internal Server Error",
                      "<p>Could not store the credentials.</p>");
  }
  // The name, never the passphrase, in the log.
  ESP_LOGI(TAG, "saved network \"%s\" — restarting", form.ssid);
  s_saved = true;
  return send_plain(req, "200 OK",
                    "<p>Saved. The dongle is restarting and will join "
                    "<b>your network</b>; this access point goes away in a "
                    "moment.</p>");
}

// The captive trick: every URL a phone probes to detect a portal
// (captive.apple.com, connectivitycheck.gstatic.com, ...) lands here and is
// redirected to the form. iOS needs a body, not just the 302.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err) {
  (void)err;
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, "<a href=\"/\">airkvm setup</a>",
                         HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = SETUP_HTTP_SOCKETS;
  config.lru_purge_enable = true;
  // save_post keeps the body, the parsed form and a wifi_config_t on this
  // stack and then calls into NVS; the 4 KB default is probably enough and
  // "probably" is not a margin.
  config.stack_size = 6144;
  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start failed");
  const httpd_uri_t root = {
      .uri = "/", .method = HTTP_GET, .handler = root_get};
  const httpd_uri_t save = {
      .uri = "/save", .method = HTTP_POST, .handler = save_post};
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "route");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save), TAG, "route");
  ESP_RETURN_ON_ERROR(
      httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found), TAG,
      "404 handler");
  // Every probe URL is a 404 first, and the server logs each at WARN.
  esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
  return ESP_OK;
}

// --- captive DNS, AP link only -----------------------------------------------

static void dns_task(void *arg) {
  const uint32_t ip_be = (uint32_t)(uintptr_t)arg;
  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "dns socket: errno %d", errno);
    vTaskDelete(NULL);
  }
  // Bound to the AP's address, not INADDR_ANY: this resolver must never be
  // reachable from any other link the dongle may have (captive_dns.h).
  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = ip_be};
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "dns bind: errno %d", errno);
    close(sock);
    vTaskDelete(NULL);
  }
  uint8_t req[512];
  uint8_t resp[512];
  for (;;) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(sock, req, sizeof(req), 0, (struct sockaddr *)&from,
                           &from_len);
    if (n <= 0) {
      vTaskDelay(1);  // a persistent error must not become a hot loop
      continue;
    }
    const size_t out =
        captive_dns_answer(req, (size_t)n, resp, sizeof(resp), ip_be);
    if (out) {
      (void)sendto(sock, resp, out, 0, (struct sockaddr *)&from, from_len);
    }
  }
}

// --- the mode ---------------------------------------------------------------------

static esp_err_t start_ap(char *ssid, char *psk) {
  ESP_RETURN_ON_ERROR(airkvm_nvs_init(), TAG, "nvs");
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
  s_ap = esp_netif_create_default_wifi_ap();
  // The STA netif exists so esp_wifi_set_config(WIFI_IF_STA) is valid; it
  // never connects (nothing calls esp_wifi_connect here).
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");

  uint8_t mac[6];
  ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "mac");
  provision_ap_ssid(mac, ssid);
  // esp_random() is true random once the radio is initialised, which it is.
  provision_psk_hex(esp_random, psk);

  wifi_config_t ap = {0};
  strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
  ap.ap.ssid_len = (uint8_t)strlen(ssid);
  strlcpy((char *)ap.ap.password, psk, sizeof(ap.ap.password));
  ap.ap.channel = 1;
  // WPA3-SAE only, not WPA2/WPA3 mixed: SAE is a PAKE, so a handshake
  // captured from the air cannot be attacked offline — which is the whole
  // point, because the operator's home network password is what crosses
  // this link. Mixed mode would hand an attacker a WPA2 downgrade and the
  // 32-bit passphrase to crack at leisure. The cost is that clients older
  // than iOS 13 / Android 10 cannot join; accepted (Alex, 2026-09-07).
  // WPA3 mandates protected management frames, so PMF is required
  // explicitly rather than left to the driver to infer.
  ap.ap.authmode = WIFI_AUTH_WPA3_PSK;
  ap.ap.pmf_cfg.capable = true;
  ap.ap.pmf_cfg.required = true;
  ap.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  ap.ap.max_connection = 2;
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

  esp_netif_ip_info_t ip_info;
  ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_ap, &ip_info), TAG, "ap ip");
  snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip_info.ip));

  // DHCP option 114, the captive-portal URI, for clients that honour it.
  // Best effort: the DNS catch-all and the 404 redirect are the real
  // mechanism, and this is a hint on top.
  snprintf(s_captive_uri, sizeof(s_captive_uri), "http://%s", s_ip);
  (void)esp_netif_dhcps_stop(s_ap);
  const esp_err_t opt = esp_netif_dhcps_option(
      s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_captive_uri,
      strlen(s_captive_uri));
  if (opt != ESP_OK) {
    ESP_LOGW(TAG, "captive-portal DHCP option not set (%s)",
             esp_err_to_name(opt));
  }
  // The DNS option names this address — explicitly. It used to arrive by
  // accident: with CONFIG_LWIP_DHCPS_ADD_DNS the server offered itself
  // whenever the flag was off, which is off in sdkconfig.defaults now
  // because the USB link must offer nothing (usb_net.h). Not best effort:
  // the captive pop-up on a phone depends on this, and a silent miss would
  // present as "the portal works if you type the address", which is
  // exactly the report nobody files.
  esp_netif_dns_info_t dns = {.ip = {.type = ESP_IPADDR_TYPE_V4}};
  dns.ip.u_addr.ip4 = ip_info.ip;
  ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns),
                      TAG, "ap dns address");
  uint8_t on = 1;
  ESP_RETURN_ON_ERROR(
      esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET,
                             ESP_NETIF_DOMAIN_NAME_SERVER, &on, sizeof(on)),
      TAG, "ap dns option");
  (void)esp_netif_dhcps_start(s_ap);

  BaseType_t ok = xTaskCreate(dns_task, "captive_dns", 3072,
                              (void *)(uintptr_t)ip_info.ip.addr, 4, NULL);
  return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void setup_mode_run(void) {
  char ssid[PROVISION_AP_SSID_LEN + 1];
  char psk[PROVISION_PSK_LEN + 1];
  ESP_LOGW(TAG, "SETUP MODE: no keyboard, no disk, no console for up to %d s",
           SETUP_TIMEOUT_S);

  esp_err_t err = start_ap(ssid, psk);
  if (err == ESP_OK) {
    err = start_http();
  }
  if (err != ESP_OK) {
    // Nothing to fall back to: a half-working setup mode is worse than a
    // restart into a mode that at least types.
    ESP_LOGE(TAG, "setup mode failed to start (%s) — restarting normally",
             esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
  }

  status_set_setup(ssid, psk, s_ip);
  // The passphrase goes to the panel and nowhere else. On a board without a
  // USB device controller (the C3 bench) this mode starts no TinyUSB, so
  // the dongle enumerates on whatever it is plugged into as a USB-Serial/
  // JTAG console carrying this log — a channel the panel-only claim would be
  // false through. On the S3 the NCM device below takes the port instead,
  // and the rule holds for both.
  ESP_LOGI(TAG,
           "join WiFi \"%s\" (passphrase on the panel), then open http://%s/",
           ssid, s_ip);

#if CONFIG_TINYUSB_NET_MODE_NCM
  // After esp_wifi_start() (gotcha 10: the radio owns the BBPLL first).
  // Best effort: the softAP is the primary path and works without this.
  const esp_err_t usb = usb_net_start();
  if (usb == ESP_OK) {
    status_set_setup_usb_ip(USB_NET_IP_STR);
    ESP_LOGI(TAG, "or over USB: http://%s/", USB_NET_IP_STR);
  } else {
    ESP_LOGW(TAG, "USB network device not started (%s); softAP only",
             esp_err_to_name(usb));
  }
#endif

  for (int remaining = SETUP_TIMEOUT_S; remaining > 0; remaining--) {
    status_set_setup_remaining((uint16_t)remaining);
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (s_saved) {
      // One more second so the "Saved" reply is on the wire before the AP
      // vanishes under the browser.
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;
    }
  }
  ESP_LOGW(TAG, "%s — restarting into normal mode",
           s_saved ? "credentials saved" : "setup timed out");
  esp_restart();
}
