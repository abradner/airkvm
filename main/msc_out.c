// See msc_out.h. Verified on nothing yet: the descriptor, the no-medium LUN
// and the async I/O path below are hypotheses until a host enumerates them
// (AGENTS.md §4, last rule).

#include "msc_out.h"

#include <string.h>

#include "class/msc/msc_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "msc_lun.h"

static const char *TAG = "usb_msc";

// One LUN today: a removable disk. A second, fixed-type CD-ROM LUN is issue
// #34 — fixed rather than a LUN that flips INQUIRY type at runtime, because
// hosts cache INQUIRY and re-reading it needs a replug, which drops the
// keyboard.
#define LUN_DISK 0
#define LUN_COUNT 1

static struct msc_lun s_lun;
// Guards s_lun. Taken briefly from TinyUSB's callbacks and from whichever task
// binds a medium; never held across a source call.
static SemaphoreHandle_t s_lock;

// One transfer at a time — bulk-only transport is strictly sequential, so a
// depth-1 mailbox is the design, not a shortcut. The worker owns the entry
// while it is out of the queue.
//
// Two ids guard a completion, because two different things can go away
// underneath a blocked source call:
//   generation  the medium (msc_lun): a detach means the bytes are not the
//               host's to receive.
//   xfer_id     the transfer: a bus reset clears TinyUSB's pending state
//               (mscd_reset) and the host re-issues the READ, so a late
//               completion from before the reset would otherwise be taken as
//               the answer to the new command and hand it the old LBA's data.
//               Bumped on mount and unmount, and per submit.
// The worker also never touches TinyUSB's endpoint buffer while a call is in
// flight: it reads into its own bounce buffer and copies across only once the
// transfer is confirmed still current, and writes are copied *out* of the
// endpoint buffer at submit time. Between a reset and our copy there is still
// a window of a few instructions; the BOT class-reset request has no app
// hook at all. Residual, recorded.
struct msc_io {
  enum msc_op op;
  uint32_t lba;
  uint32_t offset;
  uint32_t len;
  void *buf;  // TinyUSB's endpoint buffer, for the final copy on READ
  const struct msc_source *src;
  uint32_t generation;
  uint32_t xfer_id;
};
static QueueHandle_t s_io_q;
static uint32_t s_xfer_id;  // under s_lock
// 4-byte aligned for the SDMMC driver's DMA path: an unaligned buffer makes
// it allocate a bounce of its own and read one block at a time.
static uint8_t s_bounce[CONFIG_TINYUSB_MSC_BUFSIZE] __attribute__((aligned(4)));
// A flush is owed after a write burst. Done by the worker when the mailbox
// goes quiet, never on the USB task (AGENTS.md §4).
static volatile bool s_flush_wanted;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void apply_sense(uint8_t lun, const struct msc_sense *s) {
  tud_msc_set_sense(lun, s->key, s->asc, s->ascq);
}

// --- worker -----------------------------------------------------------------

// Off the TinyUSB task on purpose. tud_msc_read10_cb must return
// TUD_MSC_RET_ASYNC and let this task finish the job: returning 0 there is
// not "wait", it is BUSY, which TinyUSB answers by faking a completion and
// re-invoking the callback at once — a hot spin on the USB task, the shape of
// AGENTS.md §9 entry 12. And a blocking read inside the callback would hold
// the task that services the keyboard endpoint for the read's duration.
#define FLUSH_IDLE_MS 50

static void io_task(void *arg) {
  (void)arg;
  struct msc_io io;
  for (;;) {
    if (xQueueReceive(s_io_q, &io, pdMS_TO_TICKS(FLUSH_IDLE_MS)) != pdTRUE) {
      if (s_flush_wanted) {
        s_flush_wanted = false;
        LOCK();
        const struct msc_source *src = s_lun.src;
        UNLOCK();
        if (src && src->flush) {
          (void)src->flush(src->ctx);
        }
      }
      continue;
    }
    int rc;
    if (io.op == MSC_OP_READ) {
      rc = io.src->read(io.src->ctx, io.lba, io.offset, s_bounce, io.len);
    } else {
      rc = io.src->write(io.src->ctx, io.lba, io.offset, s_bounce, io.len);
    }

    // Still the same medium and the same transfer? A detach means the bytes
    // are not the host's to receive; a bus reset means TinyUSB has forgotten
    // this transfer and any completion would land on the next one.
    LOCK();
    const bool stale_medium = io.generation != s_lun.generation;
    const bool stale_xfer = io.xfer_id != s_xfer_id;
    UNLOCK();

    if (stale_xfer) {
      ESP_LOGW(
          TAG,
          "dropping completion for a transfer that the bus reset cancelled");
      continue;  // no async_io_done: nothing is waiting for it
    }
    if (stale_medium || rc < 0) {
      // TinyUSB replaces the sense with NOT READY / MEDIUM NOT PRESENT on any
      // error completion (proc_read_io_data), and tud_msc_set_sense is not
      // safe from this task anyway, so the distinction between "gone" and
      // "medium error" lives in the log only.
      if (!stale_medium) {
        ESP_LOGW(TAG, "%s lba %lu failed (%d)",
                 io.op == MSC_OP_READ ? "read" : "write", (unsigned long)io.lba,
                 rc);
      }
      tud_msc_async_io_done(TUD_MSC_RET_ERROR, false);
      continue;
    }
    if (io.op == MSC_OP_READ) {
      memcpy(io.buf, s_bounce, io.len);
    }
    tud_msc_async_io_done((int32_t)io.len, false);
  }
}

// Shared tail of read10/write10: validate under the lock, then hand off.
static int32_t submit(enum msc_op op, uint8_t lun, uint32_t lba,
                      uint32_t offset, void *buf, uint32_t len) {
  struct msc_sense sense;
  struct msc_io io = {
      .op = op, .lba = lba, .offset = offset, .len = len, .buf = buf};
  if (len > sizeof(s_bounce)) {
    // Cannot happen: TinyUSB never asks for more than its own buffer, which
    // is the same size. Refuse rather than overrun if that ever changes.
    const struct msc_sense s = {MSC_SENSE_KEY_ILLEGAL_REQUEST, 0x24, 0};
    apply_sense(lun, &s);
    return TUD_MSC_RET_ERROR;
  }
  LOCK();
  const bool ok = msc_lun_check_transfer(&s_lun, op, lba, offset, len, &sense);
  if (ok) {
    io.src = s_lun.src;
    io.generation = s_lun.generation;
    io.xfer_id = ++s_xfer_id;
  }
  UNLOCK();
  if (!ok) {
    apply_sense(lun, &sense);
    return TUD_MSC_RET_ERROR;
  }
  if (op == MSC_OP_WRITE) {
    // Out of the endpoint buffer now, while it is still this transfer's: after
    // a bus reset it becomes the next CBW's.
    memcpy(s_bounce, buf, len);
  }
  // Never block here: this is the USB task. The mailbox is only full if the
  // worker is still on a previous transfer's blocking call after the host
  // reset the bus and re-issued the command (its completion will be dropped
  // by xfer_id). Fail this one; the host retries.
  if (xQueueSend(s_io_q, &io, 0) != pdTRUE) {
    ESP_LOGW(
        TAG,
        "I/O worker still busy with a transfer that the bus reset cancelled");
    const struct msc_sense s = {MSC_SENSE_KEY_NOT_READY,
                                MSC_ASC_MEDIUM_NOT_PRESENT, 0};
    apply_sense(lun, &s);
    return TUD_MSC_RET_ERROR;
  }
  return TUD_MSC_RET_ASYNC;
}

// --- TinyUSB callbacks ------------------------------------------------------
//
// Every one of these is also defined, strongly, by esp_tinyusb's
// tusb_msc_storage.c, which is compiled whenever CONFIG_TINYUSB_MSC_ENABLED is
// set. Ours win only because main is linked WHOLE_ARCHIVE (main/CMakeLists.txt)
// so no reference is left for the linker to satisfy from that object, and the
// post-link check in the root CMakeLists.txt proves it on every build. The
// same object also defines tud_mount_cb/tud_umount_cb — see hid_out_usb.c for
// why losing those would be a safety bug, not a cosmetic one.

uint8_t tud_msc_get_maxlun_cb(void) {
  return LUN_COUNT;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  // Padded with spaces, not NULs: SCSI strings are fixed-width ASCII.
  memcpy(vendor_id, "airkvm  ", 8);
  memcpy(product_id, "Removable Disk  ", 16);
  memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  struct msc_sense sense;
  LOCK();
  const bool ready = msc_lun_test_unit_ready(&s_lun, &sense);
  UNLOCK();
  if (!ready) {
    apply_sense(lun, &sense);
  }
  return ready;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
  (void)lun;
  struct msc_geometry geo;
  LOCK();
  (void)msc_lun_capacity(&s_lun, &geo);
  UNLOCK();
  // A zero count is how TinyUSB learns there is no medium: it answers READ
  // CAPACITY with NOT READY / MEDIUM NOT PRESENT itself.
  *block_count = geo.block_count;
  *block_size = geo.block_size;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  LOCK();
  const bool w = msc_lun_writable(&s_lun);
  UNLOCK();
  return w;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
  (void)lun;
  (void)power_condition;
  // The host ejecting is a request to detach. Honoured once a medium can be
  // attached at all; today it can only be logged.
  if (load_eject) {
    ESP_LOGI(TAG, "host asked to %s the medium", start ? "load" : "eject");
  }
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
  return submit(MSC_OP_READ, lun, lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
  return submit(MSC_OP_WRITE, lun, lba, offset, buffer, bufsize);
}

void tud_msc_write10_complete_cb(uint8_t lun) {
  (void)lun;
  // Not flushed here: this is the USB task. The worker flushes once the
  // mailbox has been quiet for FLUSH_IDLE_MS, which also coalesces a burst of
  // writes into one flush.
  s_flush_wanted = true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
  (void)buffer;
  (void)bufsize;
  // Anything TinyUSB did not handle itself. A removable disk needs nothing
  // beyond the core set; the CD-ROM LUN (issue #34) will answer READ TOC,
  // GET CONFIGURATION and friends here.
  ESP_LOGD(TAG, "unsupported SCSI command 0x%02x", scsi_cmd[0]);
  const struct msc_sense s = {MSC_SENSE_KEY_ILLEGAL_REQUEST, 0x20, 0};
  apply_sense(lun, &s);
  return TUD_MSC_RET_ERROR;
}

// --- lifecycle --------------------------------------------------------------

static void start_cleanup(void) {
  if (s_io_q) {
    vQueueDelete(s_io_q);
    s_io_q = NULL;
  }
  if (s_lock) {
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
  }
}

esp_err_t msc_out_start(void) {
  s_lock = xSemaphoreCreateMutex();
  s_io_q = xQueueCreate(1, sizeof(struct msc_io));
  if (s_lock == NULL || s_io_q == NULL) {
    start_cleanup();
    return ESP_ERR_NO_MEM;
  }
  msc_lun_init(&s_lun);
  // Below the HID report task (5) so a slow medium never delays a keystroke
  // by priority; pinned to the core TinyUSB is not on (it defaults to CPU1),
  // so a blocking source call never shares a core with the endpoint service.
  if (xTaskCreatePinnedToCore(io_task, "msc_io", 4096, NULL, 4, NULL, 0) !=
      pdPASS) {
    start_cleanup();
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "USB MSC ready (1 LUN, no medium)");
  return ESP_OK;
}

void msc_out_bus_reset(void) {
  LOCK();
  s_xfer_id++;
  UNLOCK();
}

bool msc_out_bind_if_free(const struct msc_source *src) {
  LOCK();
  const bool bound = msc_lun_try_bind(&s_lun, src);
  UNLOCK();
  if (bound) {
    ESP_LOGI(TAG, "medium attached: %s", src->name ? src->name : "?");
  }
  return bound;
}

bool msc_out_unbind_if(const struct msc_source *src) {
  LOCK();
  const bool released = msc_lun_unbind_if(&s_lun, src);
  UNLOCK();
  if (released) {
    ESP_LOGI(TAG, "medium detached: %s", src->name ? src->name : "?");
  }
  return released;
}

const struct msc_source *msc_out_current(void) {
  LOCK();
  const struct msc_source *src = s_lun.src;
  UNLOCK();
  return src;
}

// (plain bind/unbind deliberately not offered: see msc_out.h)
