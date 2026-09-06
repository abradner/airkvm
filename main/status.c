#include "status.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static struct status_snapshot s_state;
// When the button last turned debug mode on, for the R-006 expiry.
static int64_t s_debug_enabled_at_us;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

void status_init(void) {
  s_lock = xSemaphoreCreateMutex();
  memset(&s_state, 0, sizeof(s_state));
}

// The one place debug mode is turned off and the captured text erased. Both
// the button and the timeout come through here, so there is exactly one erase
// path rather than two that can drift.
static void debug_off_locked(void) {
  s_state.debug = false;
  // Leaving debug mode wipes what was captured, so turning it off is an
  // effective erase rather than merely hiding the text.
  memset(s_state.last_event, 0, sizeof(s_state.last_event));
}

// Called by every reader rather than driven by a timer: the expiry only has to
// be true before anyone can observe or add to the captured text, and a timer
// would be a second thing to keep alive for no gain.
static void expire_debug_locked(void) {
  if (!s_state.debug) {
    return;
  }
  if (!status_debug_is_expired(s_debug_enabled_at_us, esp_timer_get_time())) {
    return;
  }
  debug_off_locked();
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
  expire_debug_locked();
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
  // Expire first, so a press after the timeout reads as "turn it on again"
  // rather than "turn off something already logically off" — otherwise the
  // operator has to press twice to get a row they believe is already gone.
  expire_debug_locked();
  if (s_state.debug) {
    debug_off_locked();
  } else {
    s_state.debug = true;
    s_debug_enabled_at_us = esp_timer_get_time();
  }
  const bool now = s_state.debug;
  UNLOCK();
  return now;
}

bool status_debug_enabled(void) {
  LOCK();
  expire_debug_locked();
  const bool on = s_state.debug;
  UNLOCK();
  return on;
}

void status_get(struct status_snapshot *out) {
  LOCK();
  expire_debug_locked();
  *out = s_state;
  UNLOCK();
}
