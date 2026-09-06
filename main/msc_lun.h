// One logical unit's state, and the SCSI-facing decisions about it.
//
// Pure: no I/O, no allocation, no ESP-IDF headers (AGENTS.md §4). This is the
// storage analogue of hid_state.c — the single owner of "is there a medium,
// how big, may it be written, has it changed since the host last looked" —
// and msc_out.c is the only caller, from TinyUSB's callbacks. Host-tested
// against a fake source in test/host/test_msc_lun.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "msc_source.h"

// SCSI sense as the host will see it: key / additional sense code / qualifier.
struct msc_sense {
  uint8_t key;
  uint8_t asc;
  uint8_t ascq;
};

// The SPC-3 values this file speaks. Kept here rather than pulled from
// TinyUSB's headers so the LUN compiles on the host.
#define MSC_SENSE_KEY_NOT_READY 0x02
#define MSC_SENSE_KEY_MEDIUM_ERROR 0x03
#define MSC_SENSE_KEY_ILLEGAL_REQUEST 0x05
#define MSC_SENSE_KEY_UNIT_ATTENTION 0x06
#define MSC_SENSE_KEY_DATA_PROTECT 0x07
#define MSC_ASC_WRITE_ERROR 0x0C
#define MSC_ASC_UNRECOVERED_READ_ERROR 0x11
#define MSC_ASC_LBA_OUT_OF_RANGE 0x21
#define MSC_ASC_WRITE_PROTECTED 0x27
#define MSC_ASC_NOT_READY_TO_READY_CHANGE 0x28
#define MSC_ASC_MEDIUM_NOT_PRESENT 0x3A

struct msc_lun {
  const struct msc_source *src;  // NULL: no medium
  // A medium appeared since the host last asked. Reported once, as UNIT
  // ATTENTION / NOT READY TO READY CHANGE, so the host re-reads capacity
  // instead of trusting the geometry of whatever was there before.
  bool attention;
  // Bumped on every bind and unbind. A transfer carries the generation it was
  // issued under, and a result for an older generation belongs to a medium
  // that is gone — the worker must fail it, not deliver it.
  uint32_t generation;
};

void msc_lun_init(struct msc_lun *lun);
void msc_lun_bind(struct msc_lun *lun, const struct msc_source *src);
void msc_lun_unbind(struct msc_lun *lun);

// TEST UNIT READY. False fills `sense` with why. Clears the attention flag
// when it reports it — the host gets told exactly once.
bool msc_lun_test_unit_ready(struct msc_lun *lun, struct msc_sense *sense);

// READ CAPACITY. False (and a zero block count) when no medium is present;
// block_size is still filled so callers never divide by zero.
bool msc_lun_capacity(const struct msc_lun *lun, struct msc_geometry *out);

bool msc_lun_writable(const struct msc_lun *lun);

enum msc_op {
  MSC_OP_READ,
  MSC_OP_WRITE,
};

// Validates a READ(10)/WRITE(10) fragment against the current medium:
// presence, write protection, and that [lba*block_size+offset, +len) lies
// inside it. This is the one place range is enforced (AGENTS.md §8, layered
// checks) — the transports that feed a source check only frame shape.
bool msc_lun_check_transfer(const struct msc_lun *lun, enum msc_op op,
                            uint32_t lba, uint32_t offset, uint32_t len,
                            struct msc_sense *sense);
