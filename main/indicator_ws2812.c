// RGB status light, for boards with a single addressable LED.
//
// This is the *primary* status channel on the LCD board, and the screen is the
// secondary one. That inversion is deliberate: a light is readable from any
// angle, at any distance, without reading — so it can answer "is this thing
// healthy" instantly, which frees the panel from having to alarm and lets it
// stay a calm, legible page of information.
//
// One of two indicator backends behind indicator.h; the other is indicator.c
// (single-channel LEDC PWM). CMake compiles exactly one. What state we are in
// is decided by the shared indicator_state_for() so the two cannot disagree —
// only how it is *rendered* differs.
//
// UNVERIFIED — no RGB LED has been driven on hardware yet (R-005). Pin from the
// vendor schematic (board.h). WS2812 timing below is from the datasheet.
#include "board.h"
#include "indicator.h"

#if BOARD_HAS_LED && defined(BOARD_LED_RGB_GPIO)

#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "palette.h"

static const char *TAG = "indicator";

// 0.1us per tick. WS2812B wants T0H 0.35us, T1H 0.7us, and a ~1.25us bit
// period; these are the middle of the datasheet's tolerance bands.
#define RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define T0H 3
#define T0L 9
#define T1H 7
#define T1L 6

// Brightness is the whole design constraint here, not an afterthought.
// indicator.h puts it plainly: this sits on a desk next to a machine you are
// working on, and a status light at full brightness in a dark room is worse
// than no status light. A WS2812 at 255 is a torch. These are percentages of
// full scale, and they are low on purpose.
#define BRIGHT_IDLE 8
#define BRIGHT_BOUND 20
#define BRIGHT_WARN 28
#define BRIGHT_FAULT 35
#define BLIP_BOOST_PCT 18
#define BLIP_MS 120

// Debug mode (R-006) pulses the light between whatever it was showing and a
// colour used for nothing else, so you cannot leave keystroke display on
// without the device saying so — the same argument as a camera's recording
// light. The risk it addresses is the operator forgetting, which the 5-minute
// auto-off already bounds; this makes the window visible rather than silent.
//
// It is a *pulse between two colours* rather than a brighter or extra light on
// purpose: it has to be unmistakable to someone at the device, without turning
// into a beacon that tells a passer-by there is something worth reading on the
// screen. That is the same tension that keeps the debug row rendered at the
// smallest cell and uncoloured (see ui.c).
#define DEBUG_PULSE_PERIOD_MS 2000

// A slow breath, so "alive" is distinguishable from "stuck" at a glance
// without being a distraction.
#define BREATHE_PERIOD_MS 4000

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static bool s_ready;
static uint32_t s_last_grb =
    0xFFFFFFFF;  // impossible, so the first write sends

static void emit(uint8_t r, uint8_t g, uint8_t b) {
  if (!s_ready) {
    return;
  }
  // WS2812 takes green first.
  const uint8_t grb[3] = {g, r, b};
  const uint32_t packed = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
  if (packed == s_last_grb) {
    return;  // nothing changed; do not spend a transmission on it
  }
  const rmt_transmit_config_t cfg = {.loop_count = 0};
  if (rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &cfg) == ESP_OK) {
    s_last_grb = packed;
  }
  // No explicit reset symbol: the latch is >50us of idle line, and the gap
  // until the next tick is three orders of magnitude longer than that.
}

static void scale(uint8_t *r, uint8_t *g, uint8_t *b, int percent) {
  *r = (uint8_t)((*r * percent) / 100);
  *g = (uint8_t)((*g * percent) / 100);
  *b = (uint8_t)((*b * percent) / 100);
}

void indicator_init(void) {
  const rmt_tx_channel_config_t chan_cfg = {
      .gpio_num = BOARD_LED_RGB_GPIO,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = RMT_RESOLUTION_HZ,
      .mem_block_symbols = 64,
      .trans_queue_depth = 4,
  };
  if (rmt_new_tx_channel(&chan_cfg, &s_chan) != ESP_OK) {
    ESP_LOGW(TAG, "rmt channel init failed — running without the status LED");
    return;
  }

  const rmt_bytes_encoder_config_t enc_cfg = {
      .bit0 = {.level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L},
      .bit1 = {.level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L},
      .flags.msb_first = 1,
  };
  if (rmt_new_bytes_encoder(&enc_cfg, &s_encoder) != ESP_OK) {
    ESP_LOGW(TAG, "rmt encoder init failed — running without the status LED");
    return;
  }
  if (rmt_enable(s_chan) != ESP_OK) {
    ESP_LOGW(TAG, "rmt enable failed — running without the status LED");
    return;
  }

  s_ready = true;
  emit(0, 0, 0);
  ESP_LOGI(TAG, "WS2812 status LED on GPIO%d", BOARD_LED_RGB_GPIO);
}

void indicator_tick(void) {
  if (!s_ready) {
    return;
  }
  struct status_snapshot s;
  status_get(&s);

  uint8_t r, g, b;
  int ceiling;
  switch (indicator_state_for(&s)) {
    case INDICATOR_FAULT: {
      const uint8_t c[] = {SOL_RED};
      r = c[0], g = c[1], b = c[2];
      ceiling = BRIGHT_FAULT;
      break;
    }
    case INDICATOR_SUSPENDED: {
      // Cyan: distinct from every other state, and reads as dormant rather
      // than alarming — nothing is wrong, the target is just asleep.
      const uint8_t c[] = {SOL_CYAN};
      r = c[0], g = c[1], b = c[2];
      ceiling = BRIGHT_WARN;
      break;
    }
    case INDICATOR_WARN: {
      const uint8_t c[] = {SOL_YELLOW};
      r = c[0], g = c[1], b = c[2];
      ceiling = BRIGHT_WARN;
      break;
    }
    case INDICATOR_BOUND: {
      const uint8_t c[] = {SOL_BLUE};
      r = c[0], g = c[1], b = c[2];
      ceiling = BRIGHT_BOUND;
      break;
    }
    default: {
      const uint8_t c[] = {SOL_GREEN};
      r = c[0], g = c[1], b = c[2];
      ceiling = BRIGHT_IDLE;
      break;
    }
  }

  // Triangle-wave breath. A fault does NOT breathe: something being wrong
  // should read as steady and insistent, not as a pulse you stop noticing.
  int percent = ceiling;
  if (indicator_state_for(&s) != INDICATOR_FAULT) {
    const int64_t phase = (esp_timer_get_time() / 1000) % BREATHE_PERIOD_MS;
    const int half = BREATHE_PERIOD_MS / 2;
    const int up = phase < half ? (int)phase : BREATHE_PERIOD_MS - (int)phase;
    // Never all the way to zero: an LED that fully extinguishes reads as
    // "off/dead" rather than "idle and well".
    percent = (ceiling / 3) + ((ceiling * 2 / 3) * up) / half;
  }

  // A blip on input, so "my keystrokes are arriving" is answerable without
  // looking at the screen at all.
  const int64_t since_ms = (esp_timer_get_time() - s.last_activity_us) / 1000;
  if (since_ms >= 0 && since_ms < BLIP_MS) {
    percent += BLIP_BOOST_PCT;
  }
  if (percent > 100) {
    percent = 100;
  }

  // Debug pulse. Applied to the hue before brightness so it reads as a colour
  // change rather than a flash — a brightness flash would be indistinguishable
  // from the activity blip below it.
  if (s.debug) {
    const uint8_t dbg[] = {SOL_MAGENTA};
    const int64_t phase = (esp_timer_get_time() / 1000) % DEBUG_PULSE_PERIOD_MS;
    const int half = DEBUG_PULSE_PERIOD_MS / 2;
    const int up =
        phase < half ? (int)phase : DEBUG_PULSE_PERIOD_MS - (int)phase;
    const int mix = (up * 100) / half;  // 0..100, triangle
    r = (uint8_t)((r * (100 - mix) + dbg[0] * mix) / 100);
    g = (uint8_t)((g * (100 - mix) + dbg[1] * mix) / 100);
    b = (uint8_t)((b * (100 - mix) + dbg[2] * mix) / 100);
  }

  scale(&r, &g, &b, percent);
  emit(r, g, b);
}

#else

void indicator_init(void) {}
void indicator_tick(void) {}

#endif
