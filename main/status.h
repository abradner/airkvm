// Shared device status: what the display shows and the LED reacts to.
//
// Producers (net.c, the HID backends) call the setters; consumers (ui.c) take a
// snapshot. Everything goes through a mutex rather than being read field-by-
// field, so the display can never render half of one update and half of the
// next — a status line that briefly shows the previous IP next to the current
// link state is the kind of thing that gets debugged for an hour.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STATUS_IP_LEN 16
#define STATUS_SSID_LEN 33
#define STATUS_EVENT_LEN 20

struct status_snapshot {
  bool wifi_connected;
  char ip[STATUS_IP_LEN];
  char ssid[STATUS_SSID_LEN];
  int8_t rssi;  // dBm, 0 when unknown

  bool client_connected;
  bool usb_mounted;

  uint32_t events;           // input events since boot
  int64_t last_activity_us;  // esp_timer_get_time() of the last input event

  // Only populated while debug mode is on (see status_set_debug). When off,
  // nothing is recorded at all — not recorded-then-hidden, because a buffer
  // holding the last keystroke is a data leak waiting for the next bug.
  bool debug;
  char last_event[STATUS_EVENT_LEN];
};

void status_init(void);

void status_set_wifi(bool connected, const char *ip, const char *ssid);
void status_set_rssi(int8_t rssi);
void status_set_client(bool connected);
void status_set_usb_mounted(bool mounted);

// Records one input event. `describe` is only stored when debug mode is on.
void status_note_event(const char *describe);

// Physical-presence gate: toggled by the on-board button, never over the
// network (docs/repo-map.md, R-006).
bool status_toggle_debug(void);
bool status_debug_enabled(void);

void status_get(struct status_snapshot *out);
