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
  // The target has suspended the bus. Usually "the target is asleep", but
  // not only that: hosts may suspend an idle device (selective suspend), and
  // with no VBUS sense a cable pull can look the same. The display says SUSP,
  // the bus fact, rather than guessing which.
  bool usb_suspended;

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
void status_set_usb_suspended(bool suspended);

// Records one input event. `describe` is only stored when debug mode is on.
void status_note_event(const char *describe);

// How long debug mode stays on before turning itself off.
//
// R-006's argument for showing keystrokes at all is physical presence: anyone
// who can press the button could already unplug the device. That is a fact
// about the moment of the press, not about the following hour — without an
// expiry the mitigation outlives the presence it rests on, and a row showing
// what is being typed is left up for whoever walks past next.
//
// It binds harder on the LCD board, where the smallest renderable glyph is
// ~2.5x the OLED's linear size and the row is therefore legible from across a
// room rather than only at arm's reach. Applied to both boards deliberately:
// gating it per board would put enforcement in two shapes, which is what
// AGENTS.md §8's deletion test exists to prevent, and the shipping dongle
// benefits from it too.
//
// Five minutes matches the screensaver's idle threshold. Same judgement in
// both places — "the operator has walked away" — but a separate constant,
// because tying a security timeout to a cosmetic one would let a future
// screensaver tweak silently extend how long keystrokes stay on screen.
#define STATUS_DEBUG_TIMEOUT_MS (5 * 60 * 1000)

// Split out as a pure predicate so it is host-testable without FreeRTOS or
// esp_timer, for the same reason oled_geometry.h exists. Times are
// esp_timer_get_time() microseconds: monotonic since boot, 64-bit, so there is
// no wrap to defend against. A negative delta (impossible from a monotonic
// clock, but cheap to be right about) reads as "not expired" rather than
// "expired long ago".
static inline bool status_debug_is_expired(int64_t enabled_at_us,
                                           int64_t now_us) {
  return (now_us - enabled_at_us) >= (int64_t)STATUS_DEBUG_TIMEOUT_MS * 1000;
}

// Physical-presence gate: toggled by the on-board button, never over the
// network (docs/repo-map.md, R-006). Also turns itself off after
// STATUS_DEBUG_TIMEOUT_MS.
bool status_toggle_debug(void);
bool status_debug_enabled(void);

void status_get(struct status_snapshot *out);
