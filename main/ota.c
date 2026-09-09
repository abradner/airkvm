#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "status.h"

static const char *TAG = "ota";

// The image this board pulls. If AIRKVM_OTA_TEST_URL names a file, that file
// is used as-is. If it ends in '/', it is a DIRECTORY and the board appends
// its own mDNS hostname plus ".bin" — so two dongles polling one server pull
// two different images.
//
// That is not a convenience. Both boards defaulted to the same hostname and
// therefore the same URL, and on 2026-09-09 an OLED image was found being
// served to an LCD board 700 times: the OTA path compares image hashes, sees
// a difference, and applies it, so "the wrong board is listening" is not a
// near miss, it is a flash. Deriving the name from the hostname makes the
// collision impossible rather than a thing to remember, and the hostname is
// already the name each board answers to (mDNS), so branding a board renames
// its firmware with it.
static char s_url[192];

static const char *image_url(void) {
  if (s_url[0] != '\0') {
    return s_url;
  }
  const char *base = CONFIG_AIRKVM_OTA_TEST_URL;
  const size_t n = strlen(base);
  if (n == 0) {
    return base;  // disabled; ota_task never gets here
  }
  if (base[n - 1] != '/') {
    snprintf(s_url, sizeof(s_url), "%s", base);
  } else {
    snprintf(s_url, sizeof(s_url), "%s%s.bin", base,
             CONFIG_AIRKVM_MDNS_HOSTNAME);
  }
  return s_url;
}

// How often the poll loop checks AIRKVM_OTA_TEST_URL for a different image.
// Board-initiated, same as the one-shot version — nothing here is reachable
// from the network, only reaching out to it. Fixed rather than configurable:
// this is a throwaway test knob, not a tuning parameter worth a second
// Kconfig entry.
#define OTA_POLL_MS (60 * 1000)

// If this boot followed an OTA and rollback support is enabled
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE), the bootloader will revert to the
// previous slot on the *next* reset unless something confirms this one good.
// WiFi association is used as "good enough" for this test hook — it proves
// the image at least boots and joins the network, which is already more than
// a silently-bricked update would manage. A real fleet build should gate this
// on more (the console actually serving, the tunnel reaching the
// coordinator), not just WiFi.
static void confirm_boot_if_pending(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    ESP_LOGI(TAG, "boot confirmed healthy (WiFi up) — rollback cancelled");
  }
}

// One poll: check whether AIRKVM_OTA_TEST_URL is serving a different image
// than what's running, and only download and apply it if so. Comparing the
// image's own app_elf_sha256 (part of esp_app_desc_t, filled in by the
// compiler for every build) rather than its declared version string: this
// device gets reflashed repeatedly from an uncommitted working tree all
// session, where a version string derived from `git describe --dirty` can
// read identically across two genuinely different builds. The hash cannot.
//
// esp_https_ota_get_img_desc() reads only the image header — a few hundred
// bytes — before esp_https_ota_perform() pulls the rest, so an unchanged
// image costs one small request per poll, not a ~1MB download every minute.
//
// Only ever called from ota_task()'s loop, which already exits before
// reaching it when AIRKVM_OTA_TEST_URL is unset — no second empty-URL check
// here, so there is exactly one place deciding whether this file does
// anything at all.
static void poll_once(void) {
  // Plain HTTP, deliberately: this URL is only ever a build-time value the
  // operator chose for a local test (see the Kconfig help text), never
  // attacker-reachable, so there is no trust decision here for TLS to
  // enforce. The real fleet OTA path (item 4) is a different mechanism
  // entirely and will need signed images over the mTLS tunnel — this proves
  // the esp_https_ota primitive, not that path.
  //
  // esp_https_ota_begin() below refuses an http:// URL with no cert/bundle
  // configured (ESP_ERR_INVALID_ARG) unless CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
  // is also set — deliberately, so an unverified fetch needs an explicit
  // opt-in rather than silently working. That flag is NOT set anywhere in
  // this repo's committed config (sdkconfig.defaults, sdkconfig.bench) and
  // must never be: it belongs only in a local, gitignored sdkconfig
  // fragment for whoever is running this specific test (`idf.py menuconfig`
  // → Component config → ESP HTTPS OTA → "Allow HTTP for OTA"). Without it,
  // every poll fails at esp_https_ota_begin() before touching the network,
  // logged as "could not reach ... (ESP_ERR_INVALID_ARG)" — which looks like
  // a network problem and is not one.
  esp_http_client_config_t http_config = {
      .url = image_url(),
      .timeout_ms = 15000,
      .keep_alive_enable = true,
  };
  esp_https_ota_config_t ota_config = {
      .http_config = &http_config,
  };

  esp_https_ota_handle_t handle = NULL;
  esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ota poll: could not reach %s (%s)", image_url(),
             esp_err_to_name(err));
    return;
  }

  esp_app_desc_t new_desc;
  err = esp_https_ota_get_img_desc(handle, &new_desc);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ota poll: could not read image header (%s)",
             esp_err_to_name(err));
    esp_https_ota_abort(handle);
    return;
  }

  const esp_app_desc_t *running = esp_app_get_description();
  if (memcmp(new_desc.app_elf_sha256, running->app_elf_sha256,
             sizeof(running->app_elf_sha256)) == 0) {
    // Same image already running — nothing to do, and no reason to spend a
    // full download finding that out again next poll. Logged explicitly:
    // esp_https_ota_begin() already unconditionally announced a target
    // partition above ("Starting OTA... / Writing to..."), regardless of
    // what happens next, so silence here would be indistinguishable from
    // this check never having run at all.
    ESP_LOGI(TAG, "ota poll: image unchanged, nothing to do");
    esp_https_ota_abort(handle);
    return;
  }

  ESP_LOGW(TAG, "ota poll: different image at %s — downloading", image_url());
  do {
    err = esp_https_ota_perform(handle);
  } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
    ESP_LOGE(TAG,
             "ota poll: download incomplete (%s) — staying on current image",
             esp_err_to_name(err));
    esp_https_ota_abort(handle);
    return;
  }

  err = esp_https_ota_finish(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ota poll: finish failed (%s) — staying on current image",
             esp_err_to_name(err));
    return;
  }

  ESP_LOGW(TAG, "ota poll: new image applied — rebooting into it");
  esp_restart();
}

static void ota_task(void *arg) {
  (void)arg;

  if (CONFIG_AIRKVM_OTA_TEST_URL[0] == '\0') {
    // The whole file is a throwaway test hook (see the file header and the
    // Kconfig help text) and every real build ships with this empty — which
    // must mean there is nothing here for a real build to do, confirming a
    // pending boot included. Checked here, once, at task entry rather than
    // only inside poll_once()'s fetch path: CONFIG_BOOTLOADER_APP_ROLLBACK_
    // ENABLE is on for every real build regardless of this URL, so without
    // this check confirm_boot_if_pending() below would run unconditionally
    // and become the de facto production rollback-confirmation policy for
    // whatever next calls esp_ota_set_boot_partition() — silently inheriting
    // this file's deliberately weak "WiFi is up" bar instead of that future
    // caller bringing its own, better-reasoned one. Exiting the task here
    // makes that boundary structural instead of a comment someone has to
    // keep re-reading correctly.
    //
    // No code after this is reachable, and none is needed: vTaskDelete(NULL)
    // deleting the calling task does not return to it on this port. Verified
    // against the vendored source, not assumed — FreeRTOS-Kernel-SMP/tasks.c
    // calls taskYIELD_WITHIN_API() on the task deleting itself while still
    // inside the same critical section that already evicted it from every
    // list, before the function can return. (Flagged twice by automated
    // review as needing an explicit `return` — it doesn't; this note is
    // here so a third pass doesn't re-open the same non-issue.)
    vTaskDelete(NULL);
  }

  // Polled rather than event-driven: status.c has no subscription mechanism,
  // only a snapshot getter. No bound on the wait — if WiFi never comes up
  // there is nothing better for this task to do than wait, and a task asleep
  // for most of each second costs nothing.
  struct status_snapshot s;
  do {
    vTaskDelay(pdMS_TO_TICKS(1000));
    status_get(&s);
  } while (!s.wifi_connected);

  confirm_boot_if_pending();

  for (;;) {
    poll_once();
    vTaskDelay(pdMS_TO_TICKS(OTA_POLL_MS));
  }
}

void ota_start(void) {
  if (xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL) != pdPASS) {
    ESP_LOGW(TAG, "could not start OTA task");
  }
}
