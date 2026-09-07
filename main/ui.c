#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hold_button.h"
#include "indicator.h"
#include "setup_flag.h"
#include "status.h"
#include "wifi.h"

static const char *TAG = "ui";

// Safe area. The 1.47" LCD's corners are physically rounded and have no pixels
// there (confirmed on hardware 2026-09-06). That is not a driver offset error
// — nothing is displaced — so it is fixed by keeping content out of the
// corners rather than by shifting the image, which would hide a few dead
// pixels by moving everything else off-centre. One cell in from the left and
// one row down is enough, and costs nothing at 40 columns. The 72x40 OLED is
// square-cornered and takes no inset at all.
#if BOARD_DISPLAY_ST7789
#define UI_SAFE_INSET 8
#define UI_X_COL 1
#else
#define UI_SAFE_INSET 0
#define UI_X_COL 0
#endif

// The LED breathes, so it needs to be updated far more often than the screen
// needs redrawing. One task, two rates.
#define TICK_MS 30
#define DISPLAY_EVERY_TICKS (250 / TICK_MS)
#define RSSI_EVERY_TICKS (2000 / TICK_MS)

// Periodic health line. Exists so "is it leaking?" has an answer that is not
// somebody's opinion: free heap trending down across minutes is a leak, a flat
// line is not, and the low-water mark catches a transient the average hides.
#define HEALTH_EVERY_TICKS (30000 / TICK_MS)

// Long enough to reject contact bounce, short enough that a deliberate press
// always registers.
#define DEBOUNCE_MS 40

// The long-hold gesture (hold_button.h). While feedback is showing the panel
// is the progress bar and nothing else; a release cancels, 5 s reboots into
// setup mode.
static struct hold_button s_hold;
static bool s_hold_showing;
static bool s_hold_armed;
static uint8_t s_hold_fraction;

// The screensaver's motion is a passive "still alive" signal for the 0.42"
// dongle, which has no LED at all (BOARD_HAS_LED 0). On boards that do drive
// one — the C3 bench board and now the LCD board's RGB LED — the light already
// says that better, and this still runs harmlessly: blanking a static screen
// after the operator has walked away is worth doing on its own merits.
#define SCREENSAVER_IDLE_MS (5 * 60 * 1000)
#if BOARD_CELL_H >= 16
// 3 pixels on the LCD is 0.3mm and reads as a dead pixel, not a sign of life.
#define SCREENSAVER_SIZE 8
#else
#define SCREENSAVER_SIZE 3
#endif

// How long after the last input the radio stays at full speed. Short, because
// this is the tier that costs current: the point is to be at full speed while
// someone is actually typing and to stop paying for it a few seconds later.
// Anything the operator does resets it, so an active session never drops out
// of it mid-keystroke.
#define RADIO_ACTIVE_MS 5000

// Tracks a button-triggered wake separately from status.c's last_activity_us,
// which is specifically "an input event arrived from the client" — the thing
// the on-screen event counter and the "is my input reaching the target"
// signal both rely on. Folding a physical button press into that field would
// make a local touch indistinguishable from a client keystroke in both
// places. The idle clock below reads whichever of the two is more recent.
static int64_t s_local_wake_us;

static int s_ss_x = UI_SAFE_INSET + 3, s_ss_y = UI_SAFE_INSET + 3, s_ss_dx = 1,
           s_ss_dy = 1;

static void draw_screensaver(void) {
  display_clear();
  s_ss_x += s_ss_dx;
  s_ss_y += s_ss_dy;
  if (s_ss_x <= UI_SAFE_INSET ||
      s_ss_x >= DISPLAY_WIDTH - UI_SAFE_INSET - SCREENSAVER_SIZE) {
    s_ss_dx = -s_ss_dx;
    s_ss_x += s_ss_dx;
  }
  if (s_ss_y <= UI_SAFE_INSET ||
      s_ss_y >= DISPLAY_HEIGHT - UI_SAFE_INSET - SCREENSAVER_SIZE) {
    s_ss_dy = -s_ss_dy;
    s_ss_y += s_ss_dy;
  }
  for (int dy = 0; dy < SCREENSAVER_SIZE; dy++) {
    for (int dx = 0; dx < SCREENSAVER_SIZE; dx++) {
      display_pixel(s_ss_x + dx, s_ss_y + dy, true);
    }
  }
  display_flush();
}

// Signal bars from RSSI. Thresholds are the conventional WiFi ones; the point
// is relative ("is this the reason it feels slow"), not absolute accuracy.
static int rssi_bars(int8_t rssi) {
  if (rssi == 0) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -66) return 3;
  if (rssi >= -77) return 2;
  return 1;
}

// Laid out in pixels rather than rows because the address is drawn enlarged
// and everything else is not. Per board, because the two panels are different
// shapes rather than one scaled: the OLED spends 40 pixels with nothing to
// spare, the LCD has 172 and can afford air between rows.
//
// Every value except the address must be a whole multiple of the cell height:
// display_text takes a row, so a value that is not a multiple is silently
// snapped down and lands somewhere this comment does not say.
#if BOARD_DISPLAY_ST7789
//   y 16..31   header     identity + who holds the keyboard     (row 1)
//   y 40..71   ADDRESS    2x, the one thing you read            (32px)
//   y 80..95   mDNS name  what you actually type                (row 5)
//   y 96..111  network    SSID + signal bars                    (row 6)
//   y 112..127 target     USB state and the event counter       (row 7)
//   y 144..159 debug      only when physically enabled (R-006)  (row 9)
//
// The debug row is deliberately set apart from the block above it rather than
// packed against it: it is the one row that is not always present, and the one
// that shows keystrokes. A reader should be able to see at a glance that it is
// on, not have to notice a fifth line in a paragraph.
//
// The name sits directly under the address because they answer the same
// question — where do I point a browser — and it is deliberately the smaller
// of the two: mDNS does not resolve on every network, the IP always works, so
// the fallback is the one that gets the emphasis.
#define UI_Y_HEADER 16
#define UI_Y_ADDRESS 40
#define UI_Y_MDNS 80
#define UI_Y_NETWORK 96
#define UI_Y_TARGET 112
#define UI_Y_DEBUG 144
#else
#define UI_Y_HEADER 0
//   y 0..5    header      identity + who holds the keyboard      (6px)
//   y 7..16   ADDRESS     double height, the one thing you read  (10px)
//   y 18..23  network     SSID + signal bars                     (6px)
//   y 24..29  target      USB state and the event counter        (6px)
//   y 30..35  debug       only when physically enabled (R-006)   (6px)
#define UI_Y_ADDRESS 7
#define UI_Y_NETWORK 18
#define UI_Y_TARGET 24
#define UI_Y_DEBUG 30
#endif
#ifdef UI_Y_MDNS
_Static_assert(UI_Y_MDNS % DISPLAY_CELL_H == 0,
               "UI_Y_MDNS must be a whole multiple of the cell height");
#endif
_Static_assert(UI_Y_NETWORK % DISPLAY_CELL_H == 0 &&
                   UI_Y_TARGET % DISPLAY_CELL_H == 0 &&
                   UI_Y_DEBUG % DISPLAY_CELL_H == 0,
               "UI_Y_* rows must be whole multiples of the cell height, or "
               "display_text silently snaps them somewhere else");
_Static_assert(UI_Y_DEBUG + DISPLAY_CELL_H <= DISPLAY_HEIGHT,
               "layout runs off the bottom of the panel");

static void draw(const struct status_snapshot *s) {
  display_clear();

  // Ink is set per element rather than once, because the point of having it is
  // hierarchy: one thing on this screen is read at a glance and the rest is
  // reference. On a single-colour panel every one of these is a no-op, so this
  // stays one draw() for both boards.
  display_set_ink(DISPLAY_INK_SECONDARY);

  // Identity, and who holds the keyboard — the thing you check first when the
  // console feels unresponsive, so it sits top-right.
  display_text(UI_X_COL, UI_Y_HEADER / DISPLAY_CELL_H, "AIRKVM");
  display_text(DISPLAY_COLS - 7, UI_Y_HEADER / DISPLAY_CELL_H,
               s->client_connected ? "BOUND" : "IDLE");

  // The address, at double height: this is what the screen is *for*, and at
  // single height it was technically legible rather than actually readable.
  // Stretched vertically only so a 15-character IP still fits on one line
  // (see display_text_tall).
  // The address is the only PRIMARY thing on the screen. Everything else is
  // context for it, and dimming the context is what makes it stand out —
  // cheaper and calmer than making the address itself louder.
  display_set_ink(DISPLAY_INK_PRIMARY);
  display_text_tall(UI_X_COL, UI_Y_ADDRESS,
                    s->wifi_connected ? s->ip : "NO WIFI");
  display_set_ink(DISPLAY_INK_SECONDARY);

#ifdef UI_Y_MDNS
  // The name the certificate is issued for, so this also says which name will
  // validate in the browser. Only meaningful once there is a network to
  // resolve it on. The 0.42" panel has no row to spare for it.
  if (s->wifi_connected) {
    char host[DISPLAY_COLS + 1];
    snprintf(host, sizeof(host), "%s.local", CONFIG_AIRKVM_MDNS_HOSTNAME);
    display_text(UI_X_COL, UI_Y_MDNS / DISPLAY_CELL_H, host);
  }
#endif

  if (!s->wifi_connected && s->no_credentials) {
    // Nothing to join and no way to say so otherwise: the one line the panel
    // has for "what do I do now". 18 columns exactly on the 0.42" panel.
    display_text(UI_X_COL, UI_Y_NETWORK / DISPLAY_CELL_H, "HOLD BOOT 5S:SETUP");
  }
  if (s->wifi_connected) {
    display_text(UI_X_COL, UI_Y_NETWORK / DISPLAY_CELL_H, s->ssid);
    // The only non-text element, and the only accent on the panel. Note it is
    // NOT a warning colour: a weak signal shows amber on the LED, which is
    // visible from across a room. Saying it twice would make neither say it.
    display_set_ink(DISPLAY_INK_ACCENT);
    display_bar(DISPLAY_COLS - 4, UI_Y_NETWORK / DISPLAY_CELL_H, 4,
                rssi_bars(s->rssi));
    display_set_ink(DISPLAY_INK_SECONDARY);
  }

  // Whether a target is actually listening, and proof that input is arriving —
  // together these separate "my input isn't getting there" from "the target is
  // ignoring me", which is the question this device gets used to answer.
  char line[DISPLAY_COLS + 1];
  if (s->usb_mounted && s->usb_suspended) {
    // Enumerated, but the target has parked the bus: nothing typed now will
    // land until it resumes. "SUSP" is the bus state, not a claim the target
    // is asleep (see status.h).
    snprintf(line, sizeof(line), "USB SUSP %lu", (unsigned long)s->events);
  } else if (s->usb_mounted) {
    // "+DSK": a medium is attached to the mass-storage LUN, so the target
    // sees a disk as well as a keyboard.
    snprintf(line, sizeof(line), "%s %lu",
             s->media_attached ? "USB+DSK" : "USB OK ",
             (unsigned long)s->events);
  } else {
    snprintf(line, sizeof(line), "NO HID  %lu", (unsigned long)s->events);
  }
  display_text(UI_X_COL, UI_Y_TARGET / DISPLAY_CELL_H, line);

  // Debug row. Off by default and only reachable by physically pressing the
  // button on the device (R-006) — this shows what is being typed, so it must
  // never be enable-able from the network.
  //
  // Rendered at the smallest cell the panel has, never enlarged, and that is a
  // security parameter rather than a layout one: R-006's argument is that
  // anyone who can press the button could already unplug the device, which
  // silently assumed the row is only legible from arm's reach. On the LCD the
  // smallest renderable glyph is already 2.5x the OLED's linear size, so the
  // assumption is weaker here and must not be weakened further. Do not "fix"
  // this to match the other rows.
  if (s->debug) {
    display_text(UI_X_COL, UI_Y_DEBUG / DISPLAY_CELL_H,
                 s->last_event[0] ? s->last_event : "DEBUG ON");
  }

  display_flush();
}

// The hold-progress page. Rows 1/3/5 exist on both panels (6 rows on the
// OLED, 10 on the LCD); the bar is the width of the panel less the safe inset.
// Once armed the bar is full and the instruction flips: the restart happens
// on release, never while the strap pin is still held (hold_button.h).
static void draw_hold(uint8_t fraction, bool armed) {
  display_clear();
  display_text(UI_X_COL, 1, "HOLD FOR SETUP");
  const int width = DISPLAY_COLS - 2 * UI_X_COL;
  const int filled = armed ? width : (fraction * width + 254) / 255;
  display_set_ink(DISPLAY_INK_ACCENT);
  display_bar(UI_X_COL, 3, width, filled);
  display_set_ink(DISPLAY_INK_SECONDARY);
  display_text(UI_X_COL, 5, armed ? "RELEASE TO RESTART" : "RELEASE = CANCEL");
  display_flush();
}

// The setup-mode page. Six rows fit the OLED exactly; the LCD gets the same
// six at its larger cell. The passphrase is deliberately on screen: reading
// the panel is the presence the security model asks for (R-009).
static void draw_setup(const struct status_snapshot *s) {
  char line[DISPLAY_COLS + 1];
  display_clear();
  snprintf(line, sizeof(line), "SETUP  %u:%02u", s->setup_remaining_s / 60,
           s->setup_remaining_s % 60);
  display_text(UI_X_COL, 0, line);
  snprintf(line, sizeof(line), "AP %s", s->setup_ssid);
  display_text(UI_X_COL, 1, line);
  snprintf(line, sizeof(line), "PW %s", s->setup_psk);
  display_set_ink(DISPLAY_INK_ACCENT);
  display_text(UI_X_COL, 2, line);
  display_set_ink(DISPLAY_INK_SECONDARY);
  display_text(UI_X_COL, 3, s->setup_ip);
  display_text(UI_X_COL, 5, "HOLD 5S: EXIT");
  display_flush();
}

static void poll_button(bool screensaver_active, bool in_setup) {
#if defined(BOARD_BUTTON)
  static bool was_pressed;
  static int64_t last_change_us;
  // Latched at the press edge, not re-read at release: a real press spans
  // several ticks, and status.last_activity_us can change mid-hold if a
  // client HID event lands while the button is down (e.g. the screensaver
  // was active when pressed but a keystroke arrives before release). Without
  // latching, that race picks which of the two actions below fires — turning
  // "just wake the panel" into a debug-mode toggle by accident, exactly the
  // outcome this dismiss/toggle split exists to prevent.
  static bool screensaver_active_at_press;

  const int level = gpio_get_level(BOARD_BUTTON);
#if BOARD_BUTTON_ACTIVE_LOW
  const bool pressed = level == 0;
#else
  const bool pressed = level != 0;
#endif

  // Debounce: a level change within DEBOUNCE_MS of the last accepted change
  // is bounce and the accepted level stands.
  const int64_t now = esp_timer_get_time();
  if (pressed != was_pressed && (now - last_change_us) / 1000 >= DEBOUNCE_MS) {
    last_change_us = now;
    was_pressed = pressed;
    if (pressed) {
      screensaver_active_at_press = screensaver_active;
    }
  }

  // The debounced level goes to the hold machine every tick, not only on
  // edges: the 1 s and 5 s thresholds are crossed while nothing changes.
  uint8_t fraction = 0;
  switch (hold_button_step(&s_hold, was_pressed, (uint32_t)(now / 1000),
                           &fraction)) {
    case HOLD_BUTTON_NONE:
      break;

    case HOLD_BUTTON_SHORT_RELEASE:
      // Act on release, not press: it makes an accidental brush less likely
      // to toggle a mode that puts keystrokes on a screen.
      //
      // Any physical interaction with the button counts as recent activity,
      // not just a dismiss — otherwise an operator standing right there who
      // just toggled debug mode (screensaver wasn't active at press) could
      // see it kick in moments later because this press never refreshed the
      // idle clock.
      s_local_wake_us = now;
      if (screensaver_active_at_press) {
        // Dismiss only. Toggling debug mode here too would mean walking up
        // to wake the screen silently turns on keystroke display (R-006) as
        // a side effect of a press that was never meant as a debug toggle.
        ESP_LOGI(TAG, "screensaver dismissed (button)");
      } else {
        const bool on = status_toggle_debug();
        ESP_LOGW(TAG, "debug display %s (physical button)", on ? "ON" : "OFF");
      }
      break;

    case HOLD_BUTTON_FEEDBACK:
      s_hold_showing = true;
      s_hold_armed = false;
      s_hold_fraction = fraction;
      s_local_wake_us = now;
      break;

    case HOLD_BUTTON_ARMED:
      s_hold_showing = true;
      s_hold_armed = true;
      s_local_wake_us = now;
      break;

    case HOLD_BUTTON_CANCEL:
      // Released between 1 s and 5 s: not a press, not a hold. Nothing
      // toggles — the person changed their mind, and that must cost nothing.
      s_hold_showing = false;
      s_hold_armed = false;
      s_local_wake_us = now;
      ESP_LOGI(TAG, "setup hold cancelled");
      break;

    case HOLD_BUTTON_FIRE:
      // On the release edge, so BOOT is high when the ROM samples it. From
      // setup mode the same gesture is the way out: a plain restart, no flag.
      s_hold_showing = false;
      display_clear();
      display_text(UI_X_COL, 3,
                   in_setup ? "LEAVING SETUP" : "SETUP: RESTARTING");
      display_flush();
      if (in_setup) {
        esp_restart();
      }
      setup_flag_request_and_restart();  // does not return
      break;
  }
#endif
}

static void ui_task(void *arg) {
  (void)arg;
  int tick = 0;
  struct status_snapshot snapshot;

  for (;;) {
    status_get(&snapshot);
    const int64_t last_activity_us = snapshot.last_activity_us > s_local_wake_us
                                         ? snapshot.last_activity_us
                                         : s_local_wake_us;
    const int64_t idle_ms = (esp_timer_get_time() - last_activity_us) / 1000;
    const bool screensaver_active = idle_ms >= SCREENSAVER_IDLE_MS;

    poll_button(screensaver_active, snapshot.setup_active);
    indicator_tick();

    // Let the radio follow the operator. This task already knows how long it
    // has been since input arrived, which is exactly the signal, so the tier
    // is decided here — but what it *means* for the radio lives in wifi.c.
    // The screensaver threshold is reused deliberately rather than given its
    // own constant: "the screen has given up on you" and "the radio can doze"
    // are the same judgement, and two constants would drift apart.
    wifi_set_activity(screensaver_active           ? WIFI_ACTIVITY_IDLE
                      : idle_ms >= RADIO_ACTIVE_MS ? WIFI_ACTIVITY_WAITING
                                                   : WIFI_ACTIVITY_ACTIVE);

    if (tick % HEALTH_EVERY_TICKS == 0) {
      struct status_snapshot health;
      status_get(&health);
      ESP_LOGI(TAG, "health: heap %u free / %u low-water, rssi %d, events %lu",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
               (int)health.rssi, (unsigned long)health.events);
    }

    if (tick % RSSI_EVERY_TICKS == 0) {
      wifi_ap_record_t ap;
      if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        status_set_rssi((int8_t)ap.rssi);
      }
    }

    if (tick % DISPLAY_EVERY_TICKS == 0 && display_present()) {
      if (s_hold_showing) {
        draw_hold(s_hold_fraction, s_hold_armed);
      } else if (snapshot.setup_active) {
        draw_setup(&snapshot);
      } else if (screensaver_active) {
        draw_screensaver();
      } else {
        draw(&snapshot);
      }
    }

    tick++;
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
  }
}

void ui_start(void) {
#if defined(BOARD_BUTTON)
  const gpio_config_t btn = {
      .pin_bit_mask = 1ULL << BOARD_BUTTON,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en =
          BOARD_BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&btn);
#endif

  indicator_init();
  display_init();  // failure is logged and non-fatal; the device runs headless

  if (xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL) != pdPASS) {
    ESP_LOGW(TAG, "could not start UI task; running without front panel");
  }
}
