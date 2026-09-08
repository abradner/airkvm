#include "indicator.h"

#include "board.h"
#include "esp_timer.h"
#include "status.h"

#if BOARD_HAS_LED

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "indicator";

#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_RES LEDC_TIMER_10_BIT
#define DUTY_MAX ((1 << 10) - 1)

// Ceilings as fractions of full brightness (AGENTS-visible rationale in the
// header): breathing tops out at half, and an activity blip adds a quarter on
// top, so the brightest this ever gets is 75%.
#define BREATHE_CEILING_PCT 50
#define BLIP_BOOST_PCT 25

// How long a blip stays visible. Below about 80ms a single keystroke is not
// perceptible; much above 150ms and continuous typing smears into a constant
// glow that says nothing.
#define BLIP_MS 120

// One breath cycle. Slow enough to read as "alive and waiting" rather than
// "something is wrong".
#define BREATHE_PERIOD_MS 4000

static bool s_ready;

static void set_percent(int percent) {
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }
  uint32_t duty = (uint32_t)((DUTY_MAX * percent) / 100);
#if BOARD_LED_PRIMARY_INVERTED
  // Active-low: the pin sinks the LED, so duty is inverted. Getting this
  // backwards shows as a light that is on when idle and off when busy, which
  // reads as working-but-wrong.
  duty = DUTY_MAX - duty;
#endif
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

void indicator_init(void) {
  ledc_timer_config_t timer = {
      .speed_mode = LEDC_MODE,
      .timer_num = LEDC_TIMER,
      .duty_resolution = LEDC_RES,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  if (ledc_timer_config(&timer) != ESP_OK) {
    ESP_LOGW(TAG, "LEDC timer config failed; LED disabled");
    return;
  }
  ledc_channel_config_t channel = {
      .gpio_num = BOARD_LED_PRIMARY,
      .speed_mode = LEDC_MODE,
      .channel = LEDC_CHANNEL,
      .timer_sel = LEDC_TIMER,
      .duty = 0,
      .hpoint = 0,
  };
  if (ledc_channel_config(&channel) != ESP_OK) {
    ESP_LOGW(TAG, "LEDC channel config failed; LED disabled");
    return;
  }
  s_ready = true;
  set_percent(0);
}

// Triangle wave rather than a sine: at these sizes the eye cannot tell the
// difference, and this avoids pulling in floating-point math for a blinkenlight.
static int breathe_percent(int64_t now_us) {
  const int64_t phase = (now_us / 1000) % BREATHE_PERIOD_MS;
  const int64_t half = BREATHE_PERIOD_MS / 2;
  const int64_t up = phase < half ? phase : (BREATHE_PERIOD_MS - phase);
  return (int)((up * BREATHE_CEILING_PCT) / half);
}

enum indicator_state indicator_tick(const struct status_snapshot *snap) {
  const struct status_snapshot s = *snap;

  const int64_t now = esp_timer_get_time();

  // State comes from the shared policy in indicator.h, not from re-reading the
  // snapshot here — a single-colour LED renders it differently from the RGB
  // one, but they must not disagree about what is being rendered.
  static enum indicator_state prev = INDICATOR_IDLE;
  const enum indicator_state state = indicator_state_for(&s, prev);
  prev = state;

  // The POLICY runs whether or not there is a working LED, and the readiness
  // check sits AFTER it. The panel colours its signal bar from this return
  // value, so bailing out early meant a board whose LED failed to initialise
  // reported every link as healthy on a screen that was working fine — a dead
  // LED silently disabling an unrelated indicator. Only the emission below
  // depends on the hardware, and the latch has to keep advancing regardless or
  // it drifts from the panel's for the same reason a second copy would.
  if (!s_ready) {
    return state;
  }

  // With one channel the only vocabulary is brightness, so the mapping is
  // coarser than the RGB backend's: a fault sits steady at full, and anything
  // healthy-but-unbound stays dark. Dark is the honest signal that nobody is
  // holding the keyboard, and it is the state the device spends most of its
  // life in.
  if (state == INDICATOR_FAULT) {
    set_percent(100);
    return state;
  }
  if (state == INDICATOR_IDLE) {
    set_percent(0);
    return state;
  }
  // One channel cannot say "suspended" in a colour, so it says it in a rhythm:
  // a fast blink, distinguishable from the slow breath of a bound session.
  if (state == INDICATOR_SUSPENDED) {
    set_percent(((now / 250000) % 2) ? 60 : 0);
    return state;
  }

  int percent = breathe_percent(now);

  const int64_t since_activity_ms = (now - s.last_activity_us) / 1000;
  if (s.last_activity_us != 0 && since_activity_ms < BLIP_MS) {
    percent += BLIP_BOOST_PCT;
  }
  set_percent(percent);
  return state;
}

#else  // !BOARD_HAS_LED

void indicator_init(void) {}
enum indicator_state indicator_tick(const struct status_snapshot *snap) {
  // No LED on this board, but what this answers is not about the LED: the
  // status panel colours its signal bar from it. Run the policy anyway.
  static enum indicator_state prev = INDICATOR_IDLE;
  prev = indicator_state_for(snap, prev);
  return prev;
}

#endif
