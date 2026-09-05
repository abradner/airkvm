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
    // Releasing held keys on network loss is deliberately *not* done here: it
    // is inseparable from clearing session ownership, which lives in net.c.
    // That module registers its own handler for this event.
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

  // Start at full speed. From here the tier follows the operator — see
  // wifi_set_activity() below and AGENTS.md §9 entry 5, which this refines:
  // ESP-IDF's WIFI_PS_MIN_MODEM default measured 30ms best / 450ms average /
  // 1130ms worst round trips here, which is indeed unusable *while typing*.
  // The mistake was concluding that power save is therefore always wrong,
  // rather than that it is wrong while someone is at the keyboard.
  ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                      "set_config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
  return ESP_OK;
}

// The radio follows the operator. See wifi.h for why this is a tier rather
// than a constant.
//
// The mapping, and what each mode actually costs:
//
//   ACTIVE   WIFI_PS_NONE       receiver always on. Full speed, full current.
//   WAITING  WIFI_PS_MIN_MODEM  wakes for every DTIM beacon.
//   IDLE     WIFI_PS_MAX_MODEM  wakes every listen_interval beacons.
//
// UNMEASURED, and deliberately not asserted (AGENTS.md §4, principle 6): the
// latency of the two saving tiers depends on the access point's beacon
// interval and DTIM period, which this device does not control and cannot
// discover a useful bound for. The intent is roughly <300ms for WAITING and
// <1s for IDLE, but those are targets, not measurements, until someone runs
// ping against each tier on a real network. Do not quote them as fact.
//
// Note also that MAX_MODEM's depth is set by listen_interval in
// wifi_sta_config_t, which is fixed at association time — so the idle tier's
// sleep depth is a config decision made above, not a runtime one made here.
void wifi_set_activity(enum wifi_activity activity) {
  static enum wifi_activity current = WIFI_ACTIVITY_ACTIVE;
  static bool initialised;

  // Idempotent so callers can fire this from a timer tick without tracking
  // state. esp_wifi_set_ps() on an unchanged value is probably harmless, but
  // "probably harmless, called 33 times a second" is not a good bet to make
  // against a closed-source radio stack.
  if (initialised && activity == current) {
    return;
  }

  wifi_ps_type_t ps;
  switch (activity) {
    case WIFI_ACTIVITY_IDLE:
      ps = WIFI_PS_MAX_MODEM;
      break;
    case WIFI_ACTIVITY_WAITING:
      ps = WIFI_PS_MIN_MODEM;
      break;
    case WIFI_ACTIVITY_ACTIVE:
    default:
      ps = WIFI_PS_NONE;
      break;
  }

  const esp_err_t err = esp_wifi_set_ps(ps);
  if (err != ESP_OK) {
    // Not fatal: failing to enter a power-saving mode costs current, failing
    // to leave one costs latency, and neither is worth tearing down a working
    // session over.
    //
    // But do not re-log it every time. ui_task starts before net_start(), and
    // in the deliberately supported degraded mode where the network never
    // comes up at all (app_main keeps HID alive without it), esp_wifi_set_ps()
    // returns ESP_ERR_WIFI_NOT_INIT on every one of this task's 30ms ticks —
    // 33 log lines a second, forever. Remember the failed tier so the warning
    // is printed once per distinct failure, while still retrying whenever the
    // operator's activity actually changes tier.
    static enum wifi_activity last_failed;
    static bool have_failed;
    if (!have_failed || last_failed != activity) {
      ESP_LOGW(TAG, "could not set power save mode (%s)", esp_err_to_name(err));
      last_failed = activity;
      have_failed = true;
    }
    return;
  }

  current = activity;
  initialised = true;
  ESP_LOGI(TAG, "radio tier: %s",
           activity == WIFI_ACTIVITY_ACTIVE    ? "active"
           : activity == WIFI_ACTIVITY_WAITING ? "waiting"
                                               : "idle");
}
