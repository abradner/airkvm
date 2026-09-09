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
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_IDLE);
}

static void a_client_makes_it_bound(void) {
  struct status_snapshot s = healthy();
  s.client_connected = true;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_BOUND);
}

static void either_half_being_down_is_a_fault(void) {
  // Both halves matter and neither is more of a fault than the other: no WiFi
  // means you cannot reach it, no HID means your keystrokes go nowhere.
  struct status_snapshot s = healthy();
  s.wifi_connected = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_FAULT);
  s = healthy();
  s.usb_mounted = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_FAULT);
}

static void a_fault_outranks_being_bound(void) {
  struct status_snapshot s = healthy();
  s.client_connected = true;
  s.usb_mounted = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_FAULT);
}

static void weak_signal_warns_but_only_once_measured(void) {
  struct status_snapshot s = healthy();
  s.rssi = -80;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_WARN);
  // Exactly at the threshold counts as weak, matching ui.c's bar.
  s.rssi = INDICATOR_WEAK_RSSI_DBM;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_WARN);
  s.rssi = INDICATOR_WEAK_RSSI_DBM + 1;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_IDLE);
  // 0 is "no measurement yet" and must not read as a dead link, or the light
  // shows amber for the first seconds of every boot.
  s.rssi = 0;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_IDLE);
}

// usb_mounted stays true across a suspend, so "mounted" alone is not health.
static void a_suspended_target_is_its_own_state(void) {
  struct status_snapshot s = healthy();
  s.usb_suspended = true;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SUSPENDED);
  // Still suspended even with a console attached — the console being there is
  // exactly when you most need to know the target will not receive anything.
  s.client_connected = true;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SUSPENDED);
  // A weak link matters less than a target that is asleep.
  s.rssi = -85;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SUSPENDED);
  // But a real fault still outranks it.
  s.usb_mounted = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_FAULT);
}

static void warn_outranks_bound_but_not_fault(void) {
  struct status_snapshot s = healthy();
  s.client_connected = true;
  s.rssi = -85;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_WARN);
  s.wifi_connected = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_FAULT);
}

// Hysteresis: RSSI is a noisy single sample, so entering and leaving the
// warning must not share a threshold — otherwise one unlucky reading flashes
// the light amber, which is what was seen on hardware.
static void weak_signal_warning_has_hysteresis(void) {
  struct status_snapshot s = healthy();

  // A dip past the weak level starts warning.
  s.rssi = INDICATOR_WEAK_RSSI_DBM;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_WARN);

  // Recovering to just above weak does NOT stop the warning — this is the case
  // a bare threshold gets wrong, and the one that produced the flicker.
  s.rssi = INDICATOR_WEAK_RSSI_DBM + 1;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_WARN), INDICATOR_WARN);

  // It only clears once the signal is genuinely back.
  s.rssi = INDICATOR_STRONG_RSSI_DBM;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_WARN), INDICATOR_IDLE);

  // And a healthy link never enters the warning just because it was asked
  // while already warning.
  s.rssi = -50;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_WARN), INDICATOR_IDLE);

  // The band between the two thresholds is sticky in BOTH directions: coming
  // from idle, a reading inside it must not start a warning.
  s.rssi = INDICATOR_STRONG_RSSI_DBM - 1;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_IDLE);
}

// Setup mode outranks every other state, and the reason is the NCM device:
// TinyUSB enumerates in setup mode, so usb_mounted goes TRUE while there is no
// keyboard behind it. Ranked the other way round the light reports health
// about a network adapter.
static void setup_mode_outranks_everything(void) {
  struct status_snapshot s = healthy();
  s.setup_active = true;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SETUP);

  // The case that matters: mounted, because NCM enumerated.
  s.usb_mounted = true;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SETUP);

  // And it must beat every other state, not just the healthy one.
  s.wifi_connected = false;  // would be FAULT
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SETUP);
  s = healthy();
  s.setup_active = true;
  s.usb_mounted = false;  // would be FAULT
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SETUP);
  s = healthy();
  s.setup_active = true;
  s.usb_suspended = true;  // would be SUSPENDED
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SETUP);
  s = healthy();
  s.setup_active = true;
  s.rssi = -85;  // would be WARN
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SETUP);
  s = healthy();
  s.setup_active = true;
  s.client_connected = true;  // would be BOUND
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_SETUP);
}

// No credentials is WARN, not FAULT: nothing is broken, and the remedy is
// printed on the panel.
static void no_credentials_warns_rather_than_faults(void) {
  // The production combination, and the only one that occurs: wifi_start()
  // sets no_credentials and returns before esp_wifi_start(), so there is never
  // a link, while HID starts regardless (airkvm_main.c, verified on the C3
  // 2026-08-10). An earlier version of this test set exactly this state,
  // asserted FAULT, and called the reachable case uninteresting — which made
  // the WARN branch dead code that the suite then endorsed.
  struct status_snapshot s = healthy();
  s.no_credentials = true;
  s.wifi_connected = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_WARN);

  // It must not mask an absent target, which is why the USB half is checked
  // ahead of it rather than beside the link.
  s.usb_mounted = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_FAULT);

  // A link that is down for any other reason is still a fault.
  s = healthy();
  s.wifi_connected = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_FAULT);

  // Stable across ticks: it is set once at boot, so it must not interact with
  // the RSSI hysteresis band.
  s = healthy();
  s.no_credentials = true;
  s.wifi_connected = false;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_WARN), INDICATOR_WARN);
}

// media_attached is deliberately NOT a state: it is not a health signal, and
// the panel already carries it. Assert the absence so nobody adds it back
// without arguing for it.
static void media_attached_is_not_a_health_state(void) {
  struct status_snapshot s = healthy();
  s.media_attached = true;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_IDLE);
  s.client_connected = true;
  CHECK_EQ_INT(indicator_state_for(&s, INDICATOR_IDLE), INDICATOR_BOUND);
}

int main(void) {
  RUN(setup_mode_outranks_everything);
  RUN(no_credentials_warns_rather_than_faults);
  RUN(media_attached_is_not_a_health_state);
  RUN(healthy_and_unbound_is_idle);
  RUN(a_client_makes_it_bound);
  RUN(either_half_being_down_is_a_fault);
  RUN(a_fault_outranks_being_bound);
  RUN(weak_signal_warns_but_only_once_measured);
  RUN(weak_signal_warning_has_hysteresis);
  RUN(a_suspended_target_is_its_own_state);
  RUN(warn_outranks_bound_but_not_fault);
  return test_summary("indicator");
}
