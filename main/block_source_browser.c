// See block_source_browser.h. Unverified on hardware: every number here
// (cache slots, timeout) is a starting point for the measurement, not a result
// of one (AGENTS.md §2 principle 6).

#include "block_source_browser.h"

#include <string.h>

#include "block_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "media_proto.h"

static const char *TAG = "media_src";

// Four chunks: the one being read, the one after it (prefetched), and room
// for the pair to slide. 64 KB in internal RAM until PSRAM is enabled; the
// allocation below prefers PSRAM when it exists, so enabling it changes only
// this number.
#define CACHE_SLOTS 4

// How long a read waits for the browser before failing the USB transfer.
// Under Linux's 30 s SCSI timeout and a BIOS's 5–10 s, over a WiFi RTT by a
// wide margin. The host sees NOT READY and retries; nothing hangs.
#define CHUNK_TIMEOUT_MS 4000

// Requests in flight at once: the awaited chunk plus one prefetch. Bounded by
// slots anyway.
#define MAX_PENDING 4

struct pending {
  bool active;
  uint32_t req_id;
  uint32_t chunk;
};

static SemaphoreHandle_t s_lock;
// Given whenever something a blocked read might be waiting for happens: a
// chunk landed, a request failed, the medium was detached. Binary: the reader
// re-checks the cache after every wake, so coalesced gives are harmless.
static SemaphoreHandle_t s_wakeup;
static browser_request_fn s_request;

static uint8_t *s_mem;
static struct block_cache_slot s_slots[CACHE_SLOTS];
static struct block_cache s_cache;

static bool s_attached;
static struct msc_geometry s_geo;
static char s_name[MEDIA_NAME_MAX + 1];
static uint32_t s_next_req;
static struct pending s_pending[MAX_PENDING];
static uint32_t s_bytes_served;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// --- helpers, all called under the lock ------------------------------------

static struct pending *pending_find_chunk(uint32_t chunk) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (s_pending[i].active && s_pending[i].chunk == chunk) {
      return &s_pending[i];
    }
  }
  return NULL;
}

static struct pending *pending_find_req(uint32_t req_id) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (s_pending[i].active && s_pending[i].req_id == req_id) {
      return &s_pending[i];
    }
  }
  return NULL;
}

static uint32_t pending_count(void) {
  uint32_t n = 0;
  for (int i = 0; i < MAX_PENDING; i++) {
    n += s_pending[i].active ? 1 : 0;
  }
  return n;
}

static void pending_clear(void) {
  memset(s_pending, 0, sizeof(s_pending));
}

static uint32_t chunk_count(void) {
  const uint64_t bytes = (uint64_t)s_geo.block_count * s_geo.block_size;
  return (uint32_t)((bytes + MEDIA_CHUNK_SIZE - 1) / MEDIA_CHUNK_SIZE);
}

// Claims a slot and records the request. Returns the req_id to send, or 0 if
// nothing needs sending (already cached or in flight) or nothing can be
// (no slot, pending table full). The actual send happens outside the lock.
static uint32_t stage_request(uint32_t chunk) {
  if (block_cache_has(&s_cache, chunk) || pending_find_chunk(chunk)) {
    return 0;
  }
  struct pending *slot = NULL;
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!s_pending[i].active) {
      slot = &s_pending[i];
      break;
    }
  }
  if (slot == NULL) {
    return 0;
  }
  if (block_cache_claim(&s_cache, chunk) == BLOCK_CACHE_NO_SLOT) {
    return 0;
  }
  s_next_req++;
  if (s_next_req == 0) {
    s_next_req = 1;  // 0 means "nothing to send" above
  }
  slot->active = true;
  slot->req_id = s_next_req;
  slot->chunk = chunk;
  return s_next_req;
}

static void drop_request(uint32_t chunk) {
  struct pending *p = pending_find_chunk(chunk);
  if (p) {
    p->active = false;
  }
  block_cache_release(&s_cache, chunk);
}

// --- the blocking read ------------------------------------------------------

// Copies one span out of the cache, fetching it first if needed. Returns
// false on timeout, detach, or a failed request.
static bool fetch_span(const struct media_span *sp, uint8_t *dst) {
  const int64_t deadline_us = esp_timer_get_time() + CHUNK_TIMEOUT_MS * 1000;
  bool requested = false;
  for (;;) {
    uint32_t send_req = 0;
    bool done = false;
    bool failed = false;
    LOCK();
    if (!s_attached) {
      failed = true;
    } else {
      const uint8_t *data = block_cache_lookup(&s_cache, sp->chunk);
      if (data) {
        memcpy(dst, data + sp->offset, sp->len);
        s_bytes_served += sp->len;
        done = true;
      } else if (!requested) {
        send_req = stage_request(sp->chunk);
        // Nothing staged and nothing in flight for it: no slot, no table
        // room. Fail now rather than wait for a chunk nobody asked for.
        if (send_req == 0 && !pending_find_chunk(sp->chunk)) {
          failed = true;
        }
        requested = true;
      } else if (!pending_find_chunk(sp->chunk) &&
                 !block_cache_has(&s_cache, sp->chunk)) {
        // Woken, and our request is gone without the chunk arriving: the
        // browser reported an error for it.
        failed = true;
      }
    }
    UNLOCK();
    if (done) {
      return true;
    }
    if (failed) {
      return false;
    }
    if (send_req && !s_request(send_req, sp->chunk)) {
      LOCK();
      drop_request(sp->chunk);
      UNLOCK();
      return false;
    }
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
      LOCK();
      drop_request(sp->chunk);
      UNLOCK();
      ESP_LOGW(TAG, "chunk %lu: no answer from the browser in %d ms",
               (unsigned long)sp->chunk, CHUNK_TIMEOUT_MS);
      return false;
    }
    (void)xSemaphoreTake(s_wakeup, pdMS_TO_TICKS(remaining_us / 1000 + 1));
  }
}

static void prefetch(uint32_t chunk) {
  uint32_t send_req = 0;
  LOCK();
  if (s_attached && chunk < chunk_count()) {
    send_req = stage_request(chunk);
  }
  UNLOCK();
  if (send_req && !s_request(send_req, chunk)) {
    LOCK();
    drop_request(chunk);
    UNLOCK();
  }
}

static int src_read(void *ctx, uint32_t lba, uint32_t offset, void *buf,
                    uint32_t len) {
  (void)ctx;
  struct media_span spans[2];
  uint16_t block_size;
  LOCK();
  block_size = s_geo.block_size;
  UNLOCK();
  const uint64_t start = (uint64_t)lba * block_size + offset;
  const size_t n = media_split(start, len, spans);
  if (n == 0) {
    return -1;
  }
  uint8_t *dst = buf;
  for (size_t i = 0; i < n; i++) {
    if (!fetch_span(&spans[i], dst)) {
      return -1;
    }
    dst += spans[i].len;
  }
  // Sequential readers — every boot loader, every dd — want the next chunk
  // next. Ask for it now so the round trip overlaps the USB transfer.
  prefetch(spans[n - 1].chunk + 1);
  return 0;
}

static bool src_present(void *ctx, struct msc_geometry *out) {
  (void)ctx;
  LOCK();
  const bool attached = s_attached;
  if (attached) {
    *out = s_geo;
  }
  UNLOCK();
  return attached;
}

static bool src_writable(void *ctx) {
  (void)ctx;
  return false;
}

static const struct msc_source s_source = {
    .name = "browser image",
    .is_present = src_present,
    .is_writable = src_writable,
    .read = src_read,
    .write = NULL,
    .flush = NULL,
    .ctx = NULL,
};

// --- public --------------------------------------------------------------

esp_err_t browser_source_init(browser_request_fn request) {
  s_request = request;
  s_lock = xSemaphoreCreateMutex();
  s_wakeup = xSemaphoreCreateBinary();
  if (s_lock == NULL || s_wakeup == NULL) {
    return ESP_ERR_NO_MEM;
  }
  const size_t bytes = (size_t)CACHE_SLOTS * MEDIA_CHUNK_SIZE;
  // PSRAM first. Not enabled in any build today (the S3 supermini's 2 MB and
  // the LCD board's 8 MB are both off), so this falls through to internal
  // RAM — and the health line in ui.c is where that cost shows up.
  s_mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_mem == NULL) {
    s_mem = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  if (s_mem == NULL) {
    ESP_LOGE(TAG, "no room for a %u byte chunk cache", (unsigned)bytes);
    return ESP_ERR_NO_MEM;
  }
  block_cache_init(&s_cache, s_mem, MEDIA_CHUNK_SIZE, s_slots, CACHE_SLOTS);
  ESP_LOGI(TAG, "chunk cache: %d x %u bytes", CACHE_SLOTS,
           (unsigned)MEDIA_CHUNK_SIZE);
  return ESP_OK;
}

const struct msc_source *browser_source(void) {
  return &s_source;
}

void browser_source_attach(uint32_t block_count, uint16_t block_size,
                           const char *name) {
  LOCK();
  block_cache_invalidate_all(&s_cache);
  pending_clear();
  s_geo.block_count = block_count;
  s_geo.block_size = block_size;
  strlcpy(s_name, name ? name : "", sizeof(s_name));
  s_bytes_served = 0;
  s_attached = true;
  UNLOCK();
  ESP_LOGI(TAG, "attached \"%s\": %lu x %u bytes", s_name,
           (unsigned long)block_count, block_size);
}

void browser_source_detach(void) {
  LOCK();
  const bool was = s_attached;
  s_attached = false;
  block_cache_invalidate_all(&s_cache);
  pending_clear();
  UNLOCK();
  // Wake a read blocked on a chunk that will now never come.
  xSemaphoreGive(s_wakeup);
  if (was) {
    ESP_LOGI(TAG, "detached");
  }
}

bool browser_source_deliver(uint32_t req_id, uint32_t chunk, uint32_t offset,
                            const uint8_t *data, uint32_t len) {
  bool ok;
  LOCK();
  struct pending *p = pending_find_req(req_id);
  ok = s_attached && p != NULL && p->chunk == chunk &&
       block_cache_fill(&s_cache, chunk, s_cache.generation, offset, data, len);
  if (ok && block_cache_lookup(&s_cache, chunk) != NULL) {
    p->active = false;  // complete; the slot is data now
  }
  UNLOCK();
  if (ok) {
    xSemaphoreGive(s_wakeup);
  }
  return ok;
}

void browser_source_fail(uint32_t req_id) {
  LOCK();
  struct pending *p = pending_find_req(req_id);
  if (p) {
    const uint32_t chunk = p->chunk;
    p->active = false;
    block_cache_release(&s_cache, chunk);
  }
  UNLOCK();
  xSemaphoreGive(s_wakeup);
}

uint32_t browser_source_bytes_served(void) {
  LOCK();
  const uint32_t n = s_bytes_served;
  UNLOCK();
  return n;
}

uint32_t browser_source_pending(void) {
  LOCK();
  const uint32_t n = pending_count();
  UNLOCK();
  return n;
}
