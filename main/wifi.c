// Moved verbatim out of net.c; see wifi.h for why the split exists.
//
// Verified on hardware as part of net.c (ESP32-C3 bench board, 2026-08-10):
// association, the retry path, and the missing-credentials failure. This file
// changed no behavior when it was extracted.

#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "hid_out.h"
#include "nvs_flash.h"
#include "status.h"

static const char *TAG = "wifi";

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
    status_set_wifi(false, NULL, NULL);
    ESP_LOGW(TAG, "WiFi disconnected; retrying");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    status_set_wifi(true, ip, CONFIG_AIRKVM_WIFI_SSID);
#if CONFIG_AIRKVM_TLS
    // Advertised by name, not address: the certificate is issued for the mDNS
    // name, so browsing to the bare IP is a certificate error every time.
    ESP_LOGI(TAG, "console at https://%s.local/ (ip %s)",
             CONFIG_AIRKVM_MDNS_HOSTNAME, ip);
#else
    ESP_LOGI(TAG, "console at http://%s/ (or http://%s.local/)", ip,
             CONFIG_AIRKVM_MDNS_HOSTNAME);
#endif
  }
}

static esp_err_t start_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
    err = nvs_flash_init();
  }
  return err;
}

esp_err_t wifi_start(void) {
  ESP_RETURN_ON_ERROR(start_nvs(), TAG, "nvs init failed");
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
  if (CONFIG_AIRKVM_WIFI_SSID[0] == '\0') {
    ESP_LOGE(TAG, "no WiFi SSID configured — run `mise run config`");
    return ESP_ERR_INVALID_STATE;
  }

  wifi_config_t wifi_config = {0};
  strlcpy((char *)wifi_config.sta.ssid, CONFIG_AIRKVM_WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, CONFIG_AIRKVM_WIFI_PASSWORD,
          sizeof(wifi_config.sta.password));

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");

  // Power save off. ESP-IDF defaults to WIFI_PS_MIN_MODEM, which parks the
  // radio between the AP's beacons — measured here as ICMP round trips of
  // 30ms best, 450ms average, 1130ms worst, with zero packet loss. For a
  // battery sensor that is a good trade; for a keyboard it is the whole
  // product, and it presents as "the page won't load" rather than as anything
  // recognisable as power management (AGENTS.md §2, principle 6: latency is a
  // feature). This device is USB-powered by definition — it is plugged into
  // the machine it types on.
  ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                      "set_config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
  return ESP_OK;
}
