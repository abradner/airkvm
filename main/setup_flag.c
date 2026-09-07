#include "setup_flag.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "setup";

// Not a round number, so a stray pattern in RAM is not it.
#define SETUP_MAGIC 0x5E7A9C31u

static __NOINIT_ATTR uint32_t s_magic;

void setup_flag_request_and_restart(void) {
  ESP_LOGW(TAG, "setup mode requested — restarting");
  s_magic = SETUP_MAGIC;
  esp_restart();
}

bool setup_flag_take(void) {
  const bool requested =
      esp_reset_reason() == ESP_RST_SW && s_magic == SETUP_MAGIC;
  s_magic = 0;
  return requested;
}
