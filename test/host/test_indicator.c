// The LED's state policy.
//
// Pure by construction so it can be checked without a board, and shared so the
// PWM and RGB backends cannot disagree about what a given status means.
#include "indicator.h"
#include "test_support.h"

static struct status_snapshot healthy(void) {
  struct status_snapshot s = {0};
  s.wifi_connected = true;
  s.usb_mounted = true;
  s.rssi = -50;
  return s;
}

static void healthy_and_unbound_is_idle(void) {
  struct status_snapshot s = healthy();
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_IDLE);
}

static void a_client_makes_it_bound(void) {
  struct status_snapshot s = healthy();
  s.client_connected = true;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_BOUND);
}

static void either_half_being_down_is_a_fault(void) {
  // Both halves matter and neither is more of a fault than the other: no WiFi
  // means you cannot reach it, no HID means your keystrokes go nowhere.
  struct status_snapshot s = healthy();
  s.wifi_connected = false;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_FAULT);
  s = healthy();
  s.usb_mounted = false;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_FAULT);
}

static void a_fault_outranks_being_bound(void) {
  struct status_snapshot s = healthy();
  s.client_connected = true;
  s.usb_mounted = false;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_FAULT);
}

static void weak_signal_warns_but_only_once_measured(void) {
  struct status_snapshot s = healthy();
  s.rssi = -80;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_WARN);
  // Exactly at the threshold counts as weak, matching ui.c's bar.
  s.rssi = INDICATOR_WEAK_RSSI_DBM;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_WARN);
  s.rssi = INDICATOR_WEAK_RSSI_DBM + 1;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_IDLE);
  // 0 is "no measurement yet" and must not read as a dead link, or the light
  // shows amber for the first seconds of every boot.
  s.rssi = 0;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_IDLE);
}

static void warn_outranks_bound_but_not_fault(void) {
  struct status_snapshot s = healthy();
  s.client_connected = true;
  s.rssi = -85;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_WARN);
  s.wifi_connected = false;
  CHECK_EQ_INT(indicator_state_for(&s), INDICATOR_FAULT);
}

int main(void) {
  RUN(healthy_and_unbound_is_idle);
  RUN(a_client_makes_it_bound);
  RUN(either_half_being_down_is_a_fault);
  RUN(a_fault_outranks_being_bound);
  RUN(weak_signal_warns_but_only_once_measured);
  RUN(warn_outranks_bound_but_not_fault);
  return test_summary("indicator");
}
