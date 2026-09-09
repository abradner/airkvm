// One-off hardware identification tool, not part of the firmware.
//
// The bench board has two LEDs. Only the blue one's pin is known (GPIO8, from a
// working ESPHome config); this walks the plausible candidates so a human can
// watch the board and say which number the red one lit on.
//
// Each candidate is driven for 2 seconds, announced on serial before and after,
// and then released. Pins that carry the flash/serial interface are excluded by
// name below rather than by a range check, so it is obvious what was skipped and
// why — driving those would brick the session mid-scan.

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led_scan";

// GPIO0-5 are broken out and safe. 6/7 are the OLED's I2C on this board (SDA
// GPIO5 / SCL GPIO6 per the ESPHome config) but driving them briefly is
// harmless with the display idle. 8 is the known blue LED, included as the
// positive control: if 8 does not visibly light, the scan itself is broken and
// nothing else it reports can be trusted. 9 is the BOOT button. 12-17 are the
// SPI flash lines on the C3 and are deliberately absent. 18/19 are USB D-/D+.
static const int k_candidates[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 20, 21};

static void drive(int pin, int level) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << pin,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  gpio_set_level(pin, level);
}

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(2000));  // let the serial monitor attach
  ESP_LOGW(TAG, "==============================================");
  ESP_LOGW(TAG, " LED scan: watch the board, note which GPIO");
  ESP_LOGW(TAG, " numbers light the RED led.");
  ESP_LOGW(TAG, " GPIO8 is the known BLUE led (positive control)");
  ESP_LOGW(TAG, "==============================================");

  for (;;) {
    for (size_t i = 0; i < sizeof(k_candidates) / sizeof(k_candidates[0]); i++) {
      const int pin = k_candidates[i];
      // Both polarities: this board's blue LED is active-low (the ESPHome
      // config marks GPIO8 inverted), so a scan that only drove high would
      // light nothing and read as "no LED here".
      ESP_LOGW(TAG, ">>> GPIO %d : driving LOW (active-low LEDs light now)", pin);
      drive(pin, 0);
      vTaskDelay(pdMS_TO_TICKS(2000));

      ESP_LOGW(TAG, ">>> GPIO %d : driving HIGH (active-high LEDs light now)",
               pin);
      drive(pin, 1);
      vTaskDelay(pdMS_TO_TICKS(2000));

      // Back to input so a pin that drives something other than an LED is not
      // left asserted while the scan moves on.
      gpio_reset_pin(pin);
      vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGW(TAG, "--- scan complete, repeating ---");
  }
}
