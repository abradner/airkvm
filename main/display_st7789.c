// ST7789 transport for the 172x320 IPS LCD (Waveshare ESP32-S3-LCD-1.47B).
// One of two display backends behind display.h; the other is
// display_ssd1306.c. CMake compiles exactly one, selected by the board.
//
// UNVERIFIED — none of this has run on hardware (R-005). Pins come from the
// vendor schematic (see board.h); orientation and gap are hypotheses that one
// flash settles.
//
// == Why there is no framebuffer ==
//
// 320x172 at RGB565 is 110KB. Usable internal heap on this part is ~190KB, and
// mbedTLS already runs with dynamic buffers (sdkconfig.defaults) specifically
// so HTTPS fits, at roughly 40KB per connection with room kept for a second
// browser. A colour framebuffer and a working console do not coexist.
//
// So the drawing surface is the same 1bpp canvas the OLED uses — 7KB — and
// pixels are expanded to RGB565 one band at a time on the way out. That keeps
// this board's heap profile identical to the 0.42" dongle's, which means every
// TLS and heap measurement already taken on that board still applies here.
//
// PSRAM would lift the constraint and is deliberately not used: it is a third
// consumer of shared bus resources on the one part where this project has
// already lost an evening to a shared-clock conflict that was invisible in
// every log (AGENTS.md §9 entry 10). If a future UI wants real colour, that is
// the upgrade path, and it is a hardware re-test rather than a review call.
#include <string.h>

#include "board.h"
#include "canvas.h"
#include "display.h"
#include "esp_log.h"

#if BOARD_HAS_DISPLAY

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

#define LCD_SPI_HOST SPI2_HOST
// SPI0/1 are the flash and octal-PSRAM bus on this part; SPI2 is the first one
// a peripheral may have.

#define LCD_PCLK_HZ (40 * 1000 * 1000)

// Rows pushed per transfer. 8 matches the canvas's page height exactly, so a
// band is one page row and the dirty check below is a plain memcmp with no
// straddling.
#define BAND_ROWS 8
#define BAND_PIXELS (DISPLAY_WIDTH * BAND_ROWS)

#define PAGES (canvas_pages(DISPLAY_HEIGHT))

// Monochrome, pass 1. Deliberately not parameterised yet: adding colour means
// either a second bit plane or a stateful ink mode on a stateless drawing API,
// and that is a decision to take with the panel in front of you rather than
// ahead of it.
//
// Convenient side effect while it lasts: 0xFFFF and 0x0000 are byte-order
// agnostic, so nothing here depends on getting RGB565 endianness right. The
// first coloured pixel is where that stops being true.
#define INK_ON 0xFFFF
#define INK_OFF 0x0000

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_band_free;
static bool s_present;

static uint8_t s_bits[DISPLAY_WIDTH * ((DISPLAY_HEIGHT + 7) / 8)];
// What the panel is currently showing. Flush sends only the page rows that
// differ, which is what keeps a 22ms full-frame repaint off a 250ms redraw
// cadence: the status screen changes a couple of rows a second.
//
// Compared rather than dirty-flagged on write, deliberately — a flag set at
// every mutation site is a flag that eventually gets missed, and a missed one
// shows as a stale patch of screen that looks like a panel fault. memcmp of
// 320 bytes cannot be wrong.
static uint8_t s_shadow[sizeof(s_bits)];
static bool s_never_flushed = true;

static uint16_t *s_band;  // BAND_PIXELS entries, DMA-capable

static struct canvas s_canvas = {
    .bits = s_bits,
    .w = DISPLAY_WIDTH,
    .h = DISPLAY_HEIGHT,
    .font = NULL,  // bound in display_init
};

static bool band_done(esp_lcd_panel_io_handle_t io,
                      esp_lcd_panel_io_event_data_t *ev, void *ctx) {
  (void)io;
  (void)ev;
  (void)ctx;
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR(s_band_free, &woke);
  return woke == pdTRUE;
}

static esp_err_t backlight_init(void) {
  const gpio_config_t bl = {
      .pin_bit_mask = 1ULL << BOARD_LCD_BL,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&bl);
  if (err != ESP_OK) {
    return err;
  }
  // Plain GPIO, not LEDC. A PWM fade would be nicer and is not worth claiming
  // LEDC_CHANNEL_0, which indicator.c already names — see board.h.
  return gpio_set_level(BOARD_LCD_BL, BOARD_LCD_BL_ON_LEVEL);
}

esp_err_t display_init(void) {
  s_band_free = xSemaphoreCreateBinary();
  if (s_band_free == NULL) {
    return ESP_ERR_NO_MEM;
  }
  xSemaphoreGive(s_band_free);

  // MALLOC_CAP_DMA is a subset of the heap, and fragmenting it is how this
  // fails at hour three rather than minute one — so one allocation at init,
  // never per frame.
  s_band = heap_caps_malloc(BAND_PIXELS * sizeof(uint16_t),
                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (s_band == NULL) {
    ESP_LOGW(TAG, "no DMA memory for band buffer — running without display");
    return ESP_ERR_NO_MEM;
  }

  const spi_bus_config_t bus = {
      .sclk_io_num = BOARD_LCD_CLK,
      .mosi_io_num = BOARD_LCD_DIN,
      .miso_io_num = -1,  // write-only panel
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = BAND_PIXELS * (int)sizeof(uint16_t),
  };
  esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "spi bus init failed (%s)", esp_err_to_name(err));
    return err;
  }

  const esp_lcd_panel_io_spi_config_t io_cfg = {
      .cs_gpio_num = BOARD_LCD_CS,
      .dc_gpio_num = BOARD_LCD_DC,
      .spi_mode = 0,
      .pclk_hz = LCD_PCLK_HZ,
      .trans_queue_depth = 2,
      .on_color_trans_done = band_done,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };
  err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                 &io_cfg, &s_io);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "panel io init failed (%s)", esp_err_to_name(err));
    return err;
  }

  const esp_lcd_panel_dev_config_t dev = {
      .reset_gpio_num = BOARD_LCD_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };
  err = esp_lcd_new_panel_st7789(s_io, &dev, &s_panel);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "st7789 init failed (%s)", esp_err_to_name(err));
    return err;
  }

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(s_panel));
  // IPS panels of this family ship inverted relative to the controller's
  // default. UNVERIFIED: if the first solid fill comes out photographic
  // negative, this is the flag.
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_invert_color(s_panel, true));
  ESP_ERROR_CHECK_WITHOUT_ABORT(
      esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY));
  ESP_ERROR_CHECK_WITHOUT_ABORT(
      esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
  // The 172-wide panel sits offset inside a 240-wide controller. Which axis
  // carries the offset depends on swap_xy above, and the driver applies the
  // gap to the caller's coordinates *before* CASET while swap_xy only flips a
  // MADCTL bit — so this is not decidable by reading the driver. Draw a 1px
  // border and look. See board.h.
  ESP_ERROR_CHECK_WITHOUT_ABORT(
      esp_lcd_panel_set_gap(s_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP));

  err = backlight_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "backlight gpio failed (%s)", esp_err_to_name(err));
    // Not fatal: the panel may still be driven, just unlit.
  }

  s_canvas.font = &canvas_font8x16;
  s_present = true;
  display_clear();
  display_flush();
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(s_panel, true));
  ESP_LOGI(TAG, "ST7789 %dx%d ready (%d cols x %d rows)", DISPLAY_WIDTH,
           DISPLAY_HEIGHT, DISPLAY_COLS, DISPLAY_ROWS);
  return ESP_OK;
}

bool display_present(void) {
  return s_present;
}

void display_clear(void) {
  canvas_clear(&s_canvas);
}

void display_pixel(int x, int y, bool on) {
  if (!s_present) {
    return;
  }
  canvas_pixel(&s_canvas, x, y, on);
}

void display_text(int col, int row, const char *text) {
  if (!s_present) {
    return;
  }
  canvas_text(&s_canvas, col * DISPLAY_CELL_W, row * DISPLAY_CELL_H, text, 1,
              1);
}

void display_text_tall(int col, int y_px, const char *text) {
  if (!s_present) {
    return;
  }
  canvas_text(&s_canvas, col * DISPLAY_CELL_W, y_px, text, BOARD_TALL_SX,
              BOARD_TALL_SY);
}

void display_bar(int col, int row, int width, int filled) {
  if (!s_present) {
    return;
  }
  canvas_bar(&s_canvas, col, row, width, filled, 1, 1);
}

void display_flush(void) {
  if (!s_present) {
    return;
  }
  for (int page = 0; page < PAGES; page++) {
    uint8_t *src = &s_bits[page * DISPLAY_WIDTH];
    uint8_t *shadow = &s_shadow[page * DISPLAY_WIDTH];
    if (!s_never_flushed && memcmp(src, shadow, DISPLAY_WIDTH) == 0) {
      continue;
    }

    // The last page is partial when the panel height is not a multiple of 8 —
    // 172 is not (21.5 pages), and this board is the first in the tree where
    // that is true. Sending the full 8 rows would run past the panel.
    const int y0 = page * 8;
    const int rows = (DISPLAY_HEIGHT - y0) < 8 ? (DISPLAY_HEIGHT - y0) : 8;

    // Wait for the previous band before overwriting the buffer it is still
    // being read from.
    xSemaphoreTake(s_band_free, portMAX_DELAY);
    for (int r = 0; r < rows; r++) {
      uint16_t *out = &s_band[r * DISPLAY_WIDTH];
      const uint8_t mask = (uint8_t)(1u << r);
      for (int x = 0; x < DISPLAY_WIDTH; x++) {
        out[x] = (src[x] & mask) ? INK_ON : INK_OFF;
      }
    }
    if (esp_lcd_panel_draw_bitmap(s_panel, 0, y0, DISPLAY_WIDTH, y0 + rows,
                                  s_band) != ESP_OK) {
      xSemaphoreGive(s_band_free);
      return;
    }
    memcpy(shadow, src, DISPLAY_WIDTH);
  }
  s_never_flushed = false;
}

#else  // !BOARD_HAS_DISPLAY

esp_err_t display_init(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
bool display_present(void) {
  return false;
}
void display_clear(void) {}
void display_pixel(int x, int y, bool on) {
  (void)x;
  (void)y;
  (void)on;
}
void display_text(int col, int row, const char *text) {
  (void)col;
  (void)row;
  (void)text;
}
void display_text_tall(int col, int y_px, const char *text) {
  (void)col;
  (void)y_px;
  (void)text;
}
void display_bar(int col, int row, int width, int filled) {
  (void)col;
  (void)row;
  (void)width;
  (void)filled;
}
void display_flush(void) {}

#endif
