#include "display.h"

#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "oled_geometry.h"

#if BOARD_HAS_DISPLAY

#include "driver/i2c_master.h"

static const char *TAG = "display";

#define PAGES (BOARD_OLED_HEIGHT / 8)

#ifndef BOARD_OLED_ROTATE_180
#define BOARD_OLED_ROTATE_180 0
#endif

// The controller addresses 8 pages (64 rows) regardless of panel size — the
// multiplex ratio below is hardcoded to the controller's full height, not
// this board's. Rotating 180° reverses which physical COM lines the glass is
// bonded to, which shifts what page range is visible unless the window is
// exactly centered in that 8-page space. It generally isn't (a window flush
// against one end, as here, has zero margin on that side and all the margin
// on the other), so a correct rotation must recompute the page offset, not
// just flip the remap bits — see oled_geometry.h. Getting this wrong doesn't
// fail to build or even fail to init — it renders whatever was last left in
// the unwritten pages, which looks exactly like noise and is easy to mistake
// for a wiring problem. That is not hypothetical: it is what happened before
// this offset was compensated (2026-08-31), and the corrected result — both
// the pin fix and this rotation — was confirmed rendering correctly on the
// physical panel afterward, not just reasoned from the pre-rotation window's
// own confirmed-good geometry. The same centering argument applies to
// BOARD_OLED_X_OFFSET, left uncompensated here only because this panel's
// window happens to be centered in the 128-segment space; a future board
// without that symmetry would need the same treatment there too.
#define OLED_TOTAL_PAGES 8
// PAGES and OLED_PAGE_OFFSET are both page-granularity (÷8) arithmetic; a
// board whose HEIGHT or Y_OFFSET isn't a whole number of pages would have
// that division truncate silently rather than fail to build.
_Static_assert(BOARD_OLED_HEIGHT % 8 == 0,
               "BOARD_OLED_HEIGHT must be a multiple of 8 (page height)");
_Static_assert(BOARD_OLED_Y_OFFSET % 8 == 0,
               "BOARD_OLED_Y_OFFSET must be a multiple of 8 (page height)");
#define OLED_PAGE_OFFSET                                                 \
  OLED_PAGE_OFFSET_FOR(OLED_TOTAL_PAGES, PAGES, BOARD_OLED_Y_OFFSET / 8, \
                       BOARD_OLED_ROTATE_180)
_Static_assert(
    OLED_PAGE_OFFSET >= 0 && OLED_PAGE_OFFSET + PAGES <= OLED_TOTAL_PAGES,
    "OLED_PAGE_OFFSET out of range for this board's geometry — "
    "check BOARD_OLED_Y_OFFSET/HEIGHT against BOARD_OLED_ROTATE_180");

static i2c_master_dev_handle_t s_dev;
static bool s_present;
static uint8_t s_fb[BOARD_OLED_WIDTH * PAGES];

// 3x5 glyphs, one byte per column, bit 0 = top row. Covers space through '_'
// (ASCII 32..95); lowercase is folded to uppercase at draw time, which costs
// nothing here and halves the table.
//
// Deliberately hand-built rather than generated: at this size the difference
// between a readable 6 and an unreadable one is a single pixel, and the digits
// are what this screen exists to show.
static const uint8_t k_font[][3] = {
    {0x00, 0x00, 0x00},  // 32 space
    {0x00, 0x17, 0x00},  // 33 !
    {0x03, 0x00, 0x03},  // 34 "
    {0x1F, 0x0A, 0x1F},  // 35 #
    {0x16, 0x1F, 0x0D},  // 36 $
    {0x19, 0x04, 0x13},  // 37 %
    {0x0A, 0x15, 0x1A},  // 38 &
    {0x00, 0x03, 0x00},  // 39 '
    {0x00, 0x0E, 0x11},  // 40 (
    {0x11, 0x0E, 0x00},  // 41 )
    {0x0A, 0x04, 0x0A},  // 42 *
    {0x04, 0x0E, 0x04},  // 43 +
    {0x10, 0x08, 0x00},  // 44 ,
    {0x04, 0x04, 0x04},  // 45 -
    {0x00, 0x10, 0x00},  // 46 .
    {0x10, 0x04, 0x01},  // 47 /
    {0x1F, 0x11, 0x1F},  // 48 0
    {0x12, 0x1F, 0x10},  // 49 1
    {0x1D, 0x15, 0x17},  // 50 2
    {0x15, 0x15, 0x1F},  // 51 3
    {0x07, 0x04, 0x1F},  // 52 4
    {0x17, 0x15, 0x1D},  // 53 5
    {0x1F, 0x15, 0x1D},  // 54 6
    {0x01, 0x01, 0x1F},  // 55 7
    {0x1F, 0x15, 0x1F},  // 56 8
    {0x17, 0x15, 0x1F},  // 57 9
    {0x00, 0x0A, 0x00},  // 58 :
    {0x10, 0x0A, 0x00},  // 59 ;
    {0x04, 0x0A, 0x11},  // 60 <
    {0x0A, 0x0A, 0x0A},  // 61 =
    {0x11, 0x0A, 0x04},  // 62 >
    {0x01, 0x15, 0x03},  // 63 ?
    {0x1F, 0x15, 0x17},  // 64 @
    {0x1F, 0x05, 0x1F},  // 65 A
    {0x1F, 0x15, 0x1E},  // 66 B
    {0x1F, 0x11, 0x11},  // 67 C
    {0x1F, 0x11, 0x0E},  // 68 D
    {0x1F, 0x15, 0x15},  // 69 E
    {0x1F, 0x05, 0x05},  // 70 F
    {0x1F, 0x11, 0x1D},  // 71 G
    {0x1F, 0x04, 0x1F},  // 72 H
    {0x11, 0x1F, 0x11},  // 73 I
    {0x18, 0x10, 0x1F},  // 74 J
    {0x1F, 0x04, 0x1B},  // 75 K
    {0x1F, 0x10, 0x10},  // 76 L
    {0x1F, 0x02, 0x1F},  // 77 M
    {0x1F, 0x06, 0x1F},  // 78 N
    {0x1F, 0x11, 0x1F},  // 79 O
    {0x1F, 0x05, 0x07},  // 80 P
    {0x0F, 0x11, 0x1F},  // 81 Q
    {0x1F, 0x05, 0x17},  // 82 R
    {0x17, 0x15, 0x1D},  // 83 S
    {0x01, 0x1F, 0x01},  // 84 T
    {0x1F, 0x10, 0x1F},  // 85 U
    {0x0F, 0x10, 0x0F},  // 86 V
    {0x1F, 0x08, 0x1F},  // 87 W
    {0x1B, 0x04, 0x1B},  // 88 X
    {0x03, 0x1C, 0x03},  // 89 Y
    {0x19, 0x15, 0x13},  // 90 Z
    {0x00, 0x1F, 0x11},  // 91 [
    {0x01, 0x04, 0x10},  // 92 backslash
    {0x11, 0x1F, 0x00},  // 93 ]
    {0x02, 0x01, 0x02},  // 94 ^
    {0x10, 0x10, 0x10},  // 95 _
};

static esp_err_t send_cmd(const uint8_t *cmds, size_t n) {
  // 0x00 control byte marks the rest of the transfer as commands.
  uint8_t buf[16];
  if (n + 1 > sizeof(buf)) {
    return ESP_ERR_INVALID_SIZE;
  }
  buf[0] = 0x00;
  memcpy(buf + 1, cmds, n);
  return i2c_master_transmit(s_dev, buf, n + 1, 100);
}

void display_pixel(int x, int y, bool on) {
  if (!s_present || x < 0 || x >= BOARD_OLED_WIDTH || y < 0 ||
      y >= BOARD_OLED_HEIGHT) {
    return;
  }
  uint8_t *cell = &s_fb[(y / 8) * BOARD_OLED_WIDTH + x];
  const uint8_t bit = 1u << (y % 8);
  if (on) {
    *cell |= bit;
  } else {
    *cell &= (uint8_t)~bit;
  }
}

esp_err_t display_init(void) {
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = BOARD_I2C_SDA,
      .scl_io_num = BOARD_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus;
  esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "i2c bus init failed (%s)", esp_err_to_name(err));
    return err;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = BOARD_OLED_ADDR,
      .scl_speed_hz = 400000,
  };
  err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "i2c device add failed (%s)", esp_err_to_name(err));
    return err;
  }

  static const uint8_t init_seq[] = {
      0xAE,        // display off
      0xD5, 0x80,  // clock divide
      0xA8, 0x3F,  // multiplex 1/64 — the controller's full height, not the
                   // panel's; the visible window is carved out by the address
                   // offsets in display_flush()
      0xD3, 0x00,  // no display offset
      0x40,        // start line 0
      0x8D, 0x14,  // charge pump on
      0x20, 0x00,  // horizontal addressing mode
#if BOARD_OLED_ROTATE_180
      0xA0,  // segment remap: normal (rotated 180° from panel default)
      0xC0,  // COM scan direction: normal (rotated 180° from panel default)
#else
      0xA1,  // segment remap: remapped (panel's natural mounted orientation)
      0xC8,  // COM scan direction: remapped (panel's natural mounted orientation)
#endif
      // Both bits are always flipped together, never just one — that is what
      // makes this a rotation rather than a mirror.
      0xDA, 0x12,  // COM pins config
      0x81, 0xCF,  // contrast
      0xD9, 0xF1,  // pre-charge
      0xDB, 0x40,  // VCOM detect
      0xA4,        // resume from RAM
      0xA6,        // non-inverted
      0xAF,        // display on
  };
  // Sent in small chunks: the command buffer is deliberately stack-sized, and
  // one long transfer would need a heap allocation for no benefit.
  for (size_t i = 0; i < sizeof(init_seq);) {
    const size_t chunk =
        (sizeof(init_seq) - i) > 8 ? 8 : (sizeof(init_seq) - i);
    err = send_cmd(&init_seq[i], chunk);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "panel not responding (%s) — continuing without display",
               esp_err_to_name(err));
      return err;
    }
    i += chunk;
  }

  s_present = true;
  display_clear();
  display_flush();
  ESP_LOGI(TAG, "SSD1306 %dx%d ready", BOARD_OLED_WIDTH, BOARD_OLED_HEIGHT);
  return ESP_OK;
}

bool display_present(void) {
  return s_present;
}

void display_clear(void) {
  memset(s_fb, 0, sizeof(s_fb));
}

// scale_y of 2 draws each glyph row twice, doubling the height without
// touching the width (see display_text_tall in the header for why).
static void draw_text(int x, int y, const char *text, int scale_y) {
  if (!s_present || text == NULL) {
    return;
  }
  for (const char *p = text; *p; p++) {
    if (x + 3 > BOARD_OLED_WIDTH) {
      return;  // clip rather than wrap
    }
    char c = *p;
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if (c < 32 || c > 95) {
      c = '?';
    }
    const uint8_t *glyph = k_font[c - 32];
    for (int gx = 0; gx < 3; gx++) {
      for (int gy = 0; gy < 5; gy++) {
        if (glyph[gx] & (1u << gy)) {
          for (int rep = 0; rep < scale_y; rep++) {
            display_pixel(x + gx, y + gy * scale_y + rep, true);
          }
        }
      }
    }
    x += 4;
  }
}

void display_text(int col, int row, const char *text) {
  draw_text(col * DISPLAY_CELL_W, row * DISPLAY_CELL_H, text, 1);
}

void display_text_tall(int col, int y_px, const char *text) {
  draw_text(col * DISPLAY_CELL_W, y_px, text, 2);
}

void display_bar(int col, int row, int width, int filled) {
  if (!s_present) {
    return;
  }
  const int x0 = col * 4;
  const int y0 = row * 6;
  for (int i = 0; i < width; i++) {
    const bool on = i < filled;
    for (int gx = 0; gx < 3; gx++) {
      for (int gy = 0; gy < 5; gy++) {
        // Unfilled cells keep an outline so the bar's full length stays
        // visible; an empty gap reads as "no signal data" rather than "weak".
        const bool edge = gy == 0 || gy == 4 || gx == 0 || gx == 2;
        display_pixel(x0 + i * 4 + gx, y0 + gy, on || edge);
      }
    }
  }
}

void display_flush(void) {
  if (!s_present) {
    return;
  }
  for (int page = 0; page < PAGES; page++) {
    const uint8_t addr[] = {
        // The visible panel is a window into the controller's larger address
        // space, so every write is offset. Without these the image lands
        // off-screen and the panel looks dead.
        (uint8_t)(0xB0 + page + OLED_PAGE_OFFSET),
        (uint8_t)(0x00 | (BOARD_OLED_X_OFFSET & 0x0F)),
        (uint8_t)(0x10 | (BOARD_OLED_X_OFFSET >> 4)),
    };
    if (send_cmd(addr, sizeof(addr)) != ESP_OK) {
      return;
    }
    uint8_t row[1 + BOARD_OLED_WIDTH];
    row[0] = 0x40;  // control byte: data follows
    memcpy(row + 1, &s_fb[page * BOARD_OLED_WIDTH], BOARD_OLED_WIDTH);
    if (i2c_master_transmit(s_dev, row, sizeof(row), 100) != ESP_OK) {
      return;
    }
  }
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
