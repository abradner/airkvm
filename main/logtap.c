// Diagnostic only — see logtap.h for why this exists and why it must never
// ship enabled.
#include "logtap.h"

#if CONFIG_AIRKVM_LOG_TAP

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Big enough for a boot plus several failed handshakes. Static rather than
// heap on purpose: this exists partly to diagnose memory problems, so it must
// not be the thing that fails to allocate.
#define LOGTAP_BYTES 8192

static char s_buf[LOGTAP_BYTES];
static size_t s_head;  // next write position; the buffer wraps
static bool s_wrapped;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_previous;

static void append(const char *text, size_t len) {
  for (size_t i = 0; i < len; i++) {
    s_buf[s_head] = text[i];
    s_head = (s_head + 1) % LOGTAP_BYTES;
    if (s_head == 0) {
      s_wrapped = true;
    }
  }
}

static int logtap_vprintf(const char *fmt, va_list args) {
  // The va_list is consumed by the first vsnprintf, so the chained call needs
  // its own copy — passing a used va_list on is undefined behaviour.
  va_list copy;
  va_copy(copy, args);
  char line[256];
  const int n = vsnprintf(line, sizeof(line), fmt, args);
  if (n > 0 && s_lock != NULL &&
      xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
    const size_t len =
        (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    append(line, len);
    xSemaphoreGive(s_lock);
  }
  const int r = s_previous != NULL ? s_previous(fmt, copy) : n;
  va_end(copy);
  return r;
}

static esp_err_t log_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  if (s_lock != NULL) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
  }
  // Oldest first: once wrapped, the oldest byte is the one after the head.
  if (s_wrapped) {
    httpd_resp_send_chunk(req, &s_buf[s_head], LOGTAP_BYTES - s_head);
  }
  httpd_resp_send_chunk(req, s_buf, s_head);
  if (s_lock != NULL) {
    xSemaphoreGive(s_lock);
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

esp_err_t logtap_capture(void) {
  s_lock = xSemaphoreCreateMutex();
  if (s_lock == NULL) {
    return ESP_ERR_NO_MEM;
  }
  s_previous = esp_log_set_vprintf(logtap_vprintf);
  return ESP_OK;
}

esp_err_t logtap_serve(void) {
  // Its own server on port 80, separate from the console's on 443, so that
  // this says nothing about whether the console works — the instrument must
  // not perturb the thing under investigation.
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32001;  // must not collide with the console's
  config.lru_purge_enable = true;
  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) {
    return err;
  }
  const httpd_uri_t route = {
      .uri = "/log", .method = HTTP_GET, .handler = log_get};
  err = httpd_register_uri_handler(server, &route);
  if (err == ESP_OK) {
    ESP_LOGW(TAG_LOGTAP, "log tap on http://<ip>/log — DIAGNOSTIC BUILD ONLY");
  }
  return err;
}

#endif  // CONFIG_AIRKVM_LOG_TAP
