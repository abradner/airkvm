#include "msc_lun.h"

#include <string.h>

static void set_sense(struct msc_sense *sense, uint8_t key, uint8_t asc) {
  if (sense) {
    sense->key = key;
    sense->asc = asc;
    sense->ascq = 0;
  }
}

static bool present(const struct msc_lun *lun, struct msc_geometry *geo) {
  struct msc_geometry scratch;
  if (lun->src == NULL || lun->src->is_present == NULL) {
    return false;
  }
  if (!lun->src->is_present(lun->src->ctx, geo ? geo : &scratch)) {
    return false;
  }
  return true;
}

void msc_lun_init(struct msc_lun *lun) {
  memset(lun, 0, sizeof(*lun));
}

void msc_lun_bind(struct msc_lun *lun, const struct msc_source *src) {
  lun->src = src;
  lun->attention = src != NULL;
  lun->generation++;
}

void msc_lun_unbind(struct msc_lun *lun) {
  lun->src = NULL;
  // No attention here: NOT READY after READY is how removal is reported, and
  // the next bind raises its own attention for the medium that replaces it.
  lun->attention = false;
  lun->generation++;
}

bool msc_lun_test_unit_ready(struct msc_lun *lun, struct msc_sense *sense) {
  if (!present(lun, NULL)) {
    set_sense(sense, MSC_SENSE_KEY_NOT_READY, MSC_ASC_MEDIUM_NOT_PRESENT);
    return false;
  }
  if (lun->attention) {
    lun->attention = false;
    set_sense(sense, MSC_SENSE_KEY_UNIT_ATTENTION,
              MSC_ASC_NOT_READY_TO_READY_CHANGE);
    return false;
  }
  return true;
}

bool msc_lun_capacity(const struct msc_lun *lun, struct msc_geometry *out) {
  struct msc_geometry geo = {0, 512};
  const bool ok = present(lun, &geo);
  if (!ok || geo.block_size == 0) {
    out->block_count = 0;
    out->block_size = geo.block_size ? geo.block_size : 512;
    return false;
  }
  *out = geo;
  return true;
}

bool msc_lun_writable(const struct msc_lun *lun) {
  if (!present(lun, NULL) || lun->src->write == NULL) {
    return false;
  }
  return lun->src->is_writable ? lun->src->is_writable(lun->src->ctx) : true;
}

bool msc_lun_check_transfer(const struct msc_lun *lun, enum msc_op op,
                            uint32_t lba, uint32_t offset, uint32_t len,
                            struct msc_sense *sense) {
  struct msc_geometry geo;
  if (!msc_lun_capacity(lun, &geo)) {
    set_sense(sense, MSC_SENSE_KEY_NOT_READY, MSC_ASC_MEDIUM_NOT_PRESENT);
    return false;
  }
  if (op == MSC_OP_WRITE && !msc_lun_writable(lun)) {
    set_sense(sense, MSC_SENSE_KEY_DATA_PROTECT, MSC_ASC_WRITE_PROTECTED);
    return false;
  }
  // 64-bit throughout: a 32 GB card has 2^26 blocks, and lba * 512 alone
  // overflows uint32 past 4 GB.
  const uint64_t start = (uint64_t)lba * geo.block_size + offset;
  const uint64_t end = start + len;
  const uint64_t size = (uint64_t)geo.block_count * geo.block_size;
  if (end > size || end < start) {
    set_sense(sense, MSC_SENSE_KEY_ILLEGAL_REQUEST, MSC_ASC_LBA_OUT_OF_RANGE);
    return false;
  }
  return true;
}
