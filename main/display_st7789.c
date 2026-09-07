// ST7789 transport for the 172x320 IPS LCD (Waveshare ESP32-S3-LCD-1.47B).
// One of two display backends behind display.h; the other is
// display_ssd1306.c. CMake compiles exactly one, selected by the board.
//
// Verified on hardware 2026-09-06: pins (from the vendor schematic, see
// board.h), orientation, gap, and the banded flush all render correctly.
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
#include "theme.h"

static const char *TAG = "display";

// Panel bring-up steps are fatal to *this* module, not to the device. A failed
// reset or init leaves a handle that later draw calls would use anyway, so the
// only safe response is to leave s_present false and let every draw no-op —
// which is exactly what the SSD1306 backend does when its panel does not
// answer, and what display.h promises callers ("a board with no panel, or a
// mis-wired one, degrades to no display").
#define PANEL_TRY(call, what)                                              \
  do {                                                                     \
    const esp_err_t panel_err_ = (call);                                   \
    if (panel_err_ != ESP_OK) {                                            \
      ESP_LOGW(TAG, "%s failed (%s) — continuing without display", (what), \
               esp_err_to_name(panel_err_));                               \
      return panel_err_;                                                   \
    }                                                                      \
  } while (0)

#define LCD_SPI_HOST SPI2_HOST
// SPI0/1 are the flash and octal-PSRAM bus on this part; SPI2 is the first one
// a peripheral may have.

#define LCD_PCLK_HZ (40 * 1000 * 1000)

// Rows pushed per transfer. 8 matches the canvas's page height exactly, so a
// band is one page row and the dirty check below is a plain memcmp with no
// straddling.
#define BAND_ROWS 8
#define BAND_PIXELS (DISPLAY_WIDTH * BAND_ROWS)

// Spelled out arithmetically rather than as canvas_pages(DISPLAY_HEIGHT),
// because this now sizes a file-scope array and a static inline call is not a
// constant expression there — GCC rejects it as "variably modified at file
// scope". Identical value by construction, and s_bits below already spells the
// same expression out for the same reason.
#define PAGES ((DISPLAY_HEIGHT + 7) / 8)

// The panel's four colours, expanded from the theme selected at build time
// (theme.h). Indexed by canvas ink role, so expansion at flush is a lookup.
//
// Byte-swapped, and that is the fix rather than a quirk. The panel takes RGB565
// most-significant byte first; this chip is little-endian, so a uint16_t handed
// to DMA arrives with its bytes reversed. Black and white are palindromic in
// RGB565, which is why the monochrome build never noticed and why this could
// not be caught until there were real colours: the first coloured frame came
// out with base03 as vivid green and the text blue (hardware, 2026-09-07),
// which is exactly what swapping the bytes of 0x0146 predicts.
//
// Done here rather than by asking esp_lcd to do it, because the swap is a
// property of this transport and theme.h should keep saying what the colours
// ARE. If a future panel wants natural order, this is the one place to change.
#define BE16(v) ((uint16_t)((((v) & 0x00FF) << 8) | (((v) & 0xFF00) >> 8)))

// One four-entry palette per hue, filled in display_init from
// display_themes[]. Built at init rather than being a const initialiser for
// one reason worth stating: a const array cannot be indexed by another const
// array at compile time in C, and resolving that with a parallel set of
// per-theme macros would mean the table the host contrast test reads and the
// table the panel draws from are two things that can disagree. 32 bytes of BSS
// buys a single source of truth — and, incidentally, the whole of runtime
// theme selection, should that ever be wanted (theme.h).
//
// Only SECONDARY varies across hues. Background and PRIMARY are the theme's,
// unconditionally, because the background changing per row would be stripes
// and the address is the one thing whose colour must not depend on which
// section it happens to sit in. ACCENT is the theme's too: there is exactly
// one accent element on this panel and a signal bar that changed colour for
// reasons unrelated to signal would be a worse instrument, not a prettier one.
static uint16_t s_palettes[DISPLAY_HUE_COUNT][4];

// Which hue each 8-row band is drawn in. This is how the panel gets more than
// four colours without a third bit plane: the canvas stays 2bpp and 4 roles,
// and the meaning of role 2 is resolved per band on the way out.
//
// One byte per band (22 on this panel) against 6880 for a third plane, on the
// part where a full RGB565 framebuffer is exactly what does not fit.
static uint8_t s_band_hue[PAGES];

// A text row must not straddle a band, or two sections with different hues
// would fight over the bytes they share and the loser's colour would bleed
// halfway into the winner's row. With a 16px cell and 8px bands every row is
// exactly two bands; this is what makes that a fact rather than a hope, and it
// fails at compile time the moment someone changes the font.
_Static_assert(DISPLAY_CELL_H % 8 == 0,
               "the ST7789 backend resolves hue per 8-row band, so the "
               "character cell must be a whole number of bands");

static uint8_t s_hue;  // pen hue, like canvas.ink is the pen colour

static void palette_load(const struct display_theme *t) {
  for (int h = 0; h < DISPLAY_HUE_COUNT; h++) {
    s_palettes[h][CANVAS_INK_NONE] = BE16(t->ink[CANVAS_INK_NONE]);
    s_palettes[h][CANVAS_INK_PRIMARY] = BE16(t->ink[CANVAS_INK_PRIMARY]);
    s_palettes[h][CANVAS_INK_SECONDARY] = BE16(t->hue[h]);
    s_palettes[h][CANVAS_INK_ACCENT] = BE16(t->ink[CANVAS_INK_ACCENT]);
  }
}

// Records the pen hue against every band a draw touches. Called by the draw
// entry points rather than by canvas.h, deliberately: canvas.h is the shared
// 1bpp layer and knows nothing about colour, and teaching it about hues would
// put a colour concept in the one file the monochrome dongle also compiles.
static void mark_bands(int y0, int height) {
  if (s_hue == DISPLAY_HUE_NEUTRAL) {
    return;  // the value display_clear already left there
  }
  int first = y0 / 8;
  int last = (y0 + height - 1) / 8;
  if (first < 0) {
    first = 0;
  }
  if (last > PAGES - 1) {
    last = PAGES - 1;
  }
  for (int p = first; p <= last; p++) {
    s_band_hue[p] = s_hue;
  }
}

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_band_free;
static bool s_present;

static uint8_t s_bits[DISPLAY_WIDTH * ((DISPLAY_HEIGHT + 7) / 8)];
// Second ink plane. Two planes give four roles while leaving plane 0 packed
// exactly as before, so nothing about the existing pixel path changes.
static uint8_t s_bits_hi[sizeof(s_bits)];
// A checksum per page row of what the panel is currently showing, so flush
// sends only the rows that changed. Still compared rather than dirty-flagged
// on write: a flag set at every mutation site is a flag that eventually gets
// missed, and a missed one shows as a stale patch of screen that looks like a
// panel fault.
//
// A checksum rather than a full copy because a second ink plane doubled the
// canvas, and 88 bytes of CRC pays for that entirely — the whole colour
// feature costs +88 bytes of RAM against the previous shadow, on a board where
// heap is the binding constraint. The trade is a theoretical collision (two
// different page rows hashing equal, leaving one stale until it next changes)
// against 14KB, and at these odds on a status screen that is not a real risk.
static uint32_t s_shadow_crc[(DISPLAY_HEIGHT + 7) / 8];
static uint32_t page_crc(const uint8_t *lo, const uint8_t *hi, int n,
                         uint8_t hue) {
  uint32_t h = 2166136261u;  // FNV-1a
  // The hue is part of what a band LOOKS like, so it has to be part of what
  // decides the band is dirty. Without it, a section that changes colour with
  // its pixels unchanged — a row switching hue while showing the same text —
  // hashes identically, the band is skipped, and the panel keeps the old
  // colour until something else happens to disturb those bytes. That is the
  // stale-patch failure this dirty check was written to avoid, arriving by a
  // route the original had no way to see.
  h = (h ^ hue) * 16777619u;
  for (int i = 0; i < n; i++) {
    h = (h ^ lo[i]) * 16777619u;
    h = (h ^ hi[i]) * 16777619u;
  }
  return h;
}
static bool s_never_flushed = true;

static uint16_t *s_band;  // BAND_PIXELS entries, DMA-capable

static struct canvas s_canvas = {
    .bits = s_bits,
    .bits_hi = s_bits_hi,
    .w = DISPLAY_WIDTH,
    .h = DISPLAY_HEIGHT,
    .font = NULL,  // bound in display_init
    .ink = CANVAS_INK_PRIMARY,
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
  // First, and unconditionally: every early return below leaves s_present
  // false so draws no-op, but a half-initialised palette would be a table of
  // zeroes, and zero is a valid colour rather than an obviously-broken one.
  const struct display_theme *theme = &display_themes[DISPLAY_THEME_SELECTED];
  palette_load(theme);
  ESP_LOGI(TAG, "theme: %s", theme->name);

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

  PANEL_TRY(esp_lcd_panel_reset(s_panel), "panel reset");
  PANEL_TRY(esp_lcd_panel_init(s_panel), "panel init");
  // IPS panels of this family ship inverted relative to the controller's
  // default. Confirmed 2026-09-06: white-on-black renders as white on black,
  // so this is the right way round. If a future panel revision comes out
  // photographic negative, this is the flag.
  PANEL_TRY(esp_lcd_panel_invert_color(s_panel, true), "invert colour");
  PANEL_TRY(esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY), "swap_xy");
  PANEL_TRY(
      esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y),
      "mirror");
  // The 172-wide panel sits offset inside a 240-wide controller. Which axis
  // carries the offset depends on swap_xy above, and the driver applies the
  // gap to the caller's coordinates *before* CASET while swap_xy only flips a
  // MADCTL bit — so this is not decidable by reading the driver. Draw a 1px
  // border and look. See board.h.
  PANEL_TRY(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP),
            "set_gap");

  err = backlight_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "backlight gpio failed (%s)", esp_err_to_name(err));
    // Not fatal: the panel may still be driven, just unlit.
  }

  PANEL_TRY(esp_lcd_panel_disp_on_off(s_panel, true), "display on");

  // Only now is the panel known good, so only now do draw calls stop being
  // no-ops. Setting this earlier would let a half-initialised panel accept
  // draws and fail unpredictably at flush.
  s_canvas.font = &canvas_font8x16;
  s_present = true;
  display_clear();
  display_flush();
  ESP_LOGI(TAG, "ST7789 %dx%d ready (%d cols x %d rows)", DISPLAY_WIDTH,
           DISPLAY_HEIGHT, DISPLAY_COLS, DISPLAY_ROWS);
  return ESP_OK;
}

bool display_present(void) {
  return s_present;
}

void display_clear(void) {
  canvas_clear(&s_canvas);
  // Both the pen and every band go back to neutral. A frame that forgets to
  // set a hue for some section then draws it quiet, rather than inheriting
  // whatever the previous frame happened to leave in that band — which would
  // be a colour that depends on draw history, i.e. a colour nobody can reason
  // about from reading draw().
  memset(s_band_hue, DISPLAY_HUE_NEUTRAL, sizeof(s_band_hue));
  s_hue = DISPLAY_HUE_NEUTRAL;
}

void display_set_ink(enum display_ink ink) {
  canvas_set_ink(&s_canvas, (uint8_t)ink);
}

void display_set_hue(enum display_hue hue) {
  s_hue = (uint8_t)(hue < DISPLAY_HUE_COUNT ? hue : DISPLAY_HUE_NEUTRAL);
}

void display_pixel(int x, int y, bool on) {
  if (!s_present) {
    return;
  }
  // Deliberately does not mark a band. Its only caller is the screensaver,
  // which owns the whole panel and has no sections to colour.
  canvas_pixel(&s_canvas, x, y, on);
}

void display_text(int col, int row, const char *text) {
  if (!s_present) {
    return;
  }
  canvas_text(&s_canvas, col * DISPLAY_CELL_W, row * DISPLAY_CELL_H, text, 1,
              1);
  mark_bands(row * DISPLAY_CELL_H, DISPLAY_CELL_H);
}

void display_text_tall(int col, int y_px, const char *text) {
  if (!s_present) {
    return;
  }
  canvas_text(&s_canvas, col * DISPLAY_CELL_W, y_px, text, BOARD_TALL_SX,
              BOARD_TALL_SY);
  mark_bands(y_px, s_canvas.font->glyph_h * BOARD_TALL_SY);
}

void display_bar(int col, int row, int width, int filled) {
  if (!s_present) {
    return;
  }
  canvas_bar(&s_canvas, col, row, width, filled, 1, 1);
  mark_bands(row * DISPLAY_CELL_H, DISPLAY_CELL_H);
}

void display_flush(void) {
  if (!s_present) {
    return;
  }
  for (int page = 0; page < PAGES; page++) {
    const uint8_t *lo = &s_bits[page * DISPLAY_WIDTH];
    const uint8_t *hi = &s_bits_hi[page * DISPLAY_WIDTH];
    const uint32_t crc = page_crc(lo, hi, DISPLAY_WIDTH, s_band_hue[page]);
    if (!s_never_flushed && crc == s_shadow_crc[page]) {
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
    // One indirection per band, not per pixel: the inner loop is unchanged.
    const uint16_t *pal = s_palettes[s_band_hue[page]];
    for (int r = 0; r < rows; r++) {
      uint16_t *out = &s_band[r * DISPLAY_WIDTH];
      const uint8_t mask = (uint8_t)(1u << r);
      for (int x = 0; x < DISPLAY_WIDTH; x++) {
        const uint8_t ink =
            (uint8_t)(((lo[x] & mask) ? 1u : 0u) | ((hi[x] & mask) ? 2u : 0u));
        out[x] = pal[ink];
      }
    }
    if (esp_lcd_panel_draw_bitmap(s_panel, 0, y0, DISPLAY_WIDTH, y0 + rows,
                                  s_band) != ESP_OK) {
      xSemaphoreGive(s_band_free);
      return;
    }
    s_shadow_crc[page] = crc;
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
void display_set_ink(enum display_ink ink) {
  canvas_set_ink(&s_canvas, (uint8_t)ink);
}
void display_set_hue(enum display_hue hue) {
  (void)hue;
}

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
