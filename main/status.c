#include "status.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static struct status_snapshot s_state;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

void status_init(void) {
  s_lock = xSemaphoreCreateMutex();
  memset(&s_state, 0, sizeof(s_state));
}

void status_set_wifi(bool connected, const char *ip, const char *ssid) {
  LOCK();
  s_state.wifi_connected = connected;
  if (ip) {
    strlcpy(s_state.ip, ip, sizeof(s_state.ip));
  } else {
    s_state.ip[0] = '\0';
  }
  if (ssid) {
    strlcpy(s_state.ssid, ssid, sizeof(s_state.ssid));
  }
  UNLOCK();
}

void status_set_rssi(int8_t rssi) {
  LOCK();
  s_state.rssi = rssi;
  UNLOCK();
}

void status_set_client(bool connected) {
  LOCK();
  s_state.client_connected = connected;
  UNLOCK();
}

void status_set_usb_mounted(bool mounted) {
  LOCK();
  s_state.usb_mounted = mounted;
  UNLOCK();
}

void status_note_event(const char *describe) {
  LOCK();
  s_state.events++;
  s_state.last_activity_us = esp_timer_get_time();
  // Only retained while debug mode is on. The alternative — always record,
  // display conditionally — leaves the last keystroke sitting in RAM for any
  // future logging change to expose.
  if (s_state.debug && describe) {
    strlcpy(s_state.last_event, describe, sizeof(s_state.last_event));
  }
  UNLOCK();
}

bool status_toggle_debug(void) {
  LOCK();
  s_state.debug = !s_state.debug;
  const bool now = s_state.debug;
  // Leaving debug mode wipes what was captured, so turning it off is an
  // effective erase rather than merely hiding the text.
  if (!now) {
    memset(s_state.last_event, 0, sizeof(s_state.last_event));
  }
  UNLOCK();
  return now;
}

bool status_debug_enabled(void) {
  LOCK();
  const bool on = s_state.debug;
  UNLOCK();
  return on;
}

void status_get(struct status_snapshot *out) {
  LOCK();
  *out = s_state;
  UNLOCK();
}
