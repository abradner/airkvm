#include "msc_lun.h"
#include "test_support.h"

// A fake source the tests can flip between states, standing in for a card or
// a streamed image.
struct fake {
  bool present;
  bool writable;
  struct msc_geometry geo;
};

static bool fake_present(void *ctx, struct msc_geometry *out) {
  const struct fake *f = ctx;
  if (f->present) {
    *out = f->geo;
  }
  return f->present;
}
static bool fake_writable(void *ctx) {
  return ((struct fake *)ctx)->writable;
}
static int fake_read(void *ctx, uint32_t lba, uint32_t offset, void *buf,
                     uint32_t len) {
  (void)ctx;
  (void)lba;
  (void)offset;
  (void)buf;
  (void)len;
  return 0;
}
static int fake_write(void *ctx, uint32_t lba, uint32_t offset, const void *buf,
                      uint32_t len) {
  (void)ctx;
  (void)lba;
  (void)offset;
  (void)buf;
  (void)len;
  return 0;
}

static struct fake s_fake;
static const struct msc_source s_rw = {
    .name = "fake",
    .is_present = fake_present,
    .is_writable = fake_writable,
    .read = fake_read,
    .write = fake_write,
    .ctx = &s_fake,
};
static const struct msc_source s_ro = {
    .name = "fake-ro",
    .is_present = fake_present,
    .is_writable = fake_writable,
    .read = fake_read,
    .write = NULL,
    .ctx = &s_fake,
};

static void reset_fake(void) {
  s_fake.present = true;
  s_fake.writable = true;
  s_fake.geo.block_count = 1000;
  s_fake.geo.block_size = 512;
}

static void no_medium_is_not_ready(void) {
  struct msc_lun lun;
  struct msc_sense sense = {0, 0, 0};
  struct msc_geometry geo;
  msc_lun_init(&lun);
  CHECK(!msc_lun_test_unit_ready(&lun, &sense));
  CHECK_EQ_INT(sense.key, MSC_SENSE_KEY_NOT_READY);
  CHECK_EQ_INT(sense.asc, MSC_ASC_MEDIUM_NOT_PRESENT);
  CHECK(!msc_lun_capacity(&lun, &geo));
  CHECK_EQ_INT(geo.block_count, 0);
  CHECK_EQ_INT(geo.block_size, 512);  // never zero: nobody divides by it
  CHECK(!msc_lun_writable(&lun));
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_READ, 0, 0, 512, &sense));
  CHECK_EQ_INT(sense.asc, MSC_ASC_MEDIUM_NOT_PRESENT);
}

static void bind_reports_attention_exactly_once(void) {
  struct msc_lun lun;
  struct msc_sense sense = {0, 0, 0};
  struct msc_geometry geo;
  reset_fake();
  msc_lun_init(&lun);
  msc_lun_bind(&lun, &s_rw);
  // First ask: "something changed", so the host re-reads capacity.
  CHECK(!msc_lun_test_unit_ready(&lun, &sense));
  CHECK_EQ_INT(sense.key, MSC_SENSE_KEY_UNIT_ATTENTION);
  CHECK_EQ_INT(sense.asc, MSC_ASC_NOT_READY_TO_READY_CHANGE);
  // Then ready, and it stays ready.
  CHECK(msc_lun_test_unit_ready(&lun, &sense));
  CHECK(msc_lun_test_unit_ready(&lun, &sense));
  CHECK(msc_lun_capacity(&lun, &geo));
  CHECK_EQ_INT(geo.block_count, 1000);
  CHECK(msc_lun_writable(&lun));
}

static void medium_vanishing_under_a_bound_source(void) {
  struct msc_lun lun;
  struct msc_sense sense = {0, 0, 0};
  reset_fake();
  msc_lun_init(&lun);
  msc_lun_bind(&lun, &s_rw);
  (void)msc_lun_test_unit_ready(&lun, &sense);  // consume the attention
  s_fake.present = false;                       // card pulled
  CHECK(!msc_lun_test_unit_ready(&lun, &sense));
  CHECK_EQ_INT(sense.asc, MSC_ASC_MEDIUM_NOT_PRESENT);
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_READ, 0, 0, 512, &sense));
  // Re-inserted: the attention was already consumed, so it is simply ready
  // again — the source, not the LUN, is what changed here.
  s_fake.present = true;
  CHECK(msc_lun_test_unit_ready(&lun, &sense));
}

static void unbind_and_rebind_bumps_generation(void) {
  struct msc_lun lun;
  struct msc_sense sense = {0, 0, 0};
  reset_fake();
  msc_lun_init(&lun);
  const uint32_t g0 = lun.generation;
  msc_lun_bind(&lun, &s_rw);
  CHECK(lun.generation != g0);
  const uint32_t g1 = lun.generation;
  msc_lun_unbind(&lun);
  CHECK(lun.generation != g1);
  CHECK(!msc_lun_test_unit_ready(&lun, &sense));
  CHECK_EQ_INT(sense.asc, MSC_ASC_MEDIUM_NOT_PRESENT);
  // A new medium raises its own attention.
  msc_lun_bind(&lun, &s_ro);
  CHECK(!msc_lun_test_unit_ready(&lun, &sense));
  CHECK_EQ_INT(sense.key, MSC_SENSE_KEY_UNIT_ATTENTION);
}

static void read_only_refuses_writes_with_data_protect(void) {
  struct msc_lun lun;
  struct msc_sense sense = {0, 0, 0};
  reset_fake();
  msc_lun_init(&lun);
  msc_lun_bind(&lun, &s_ro);
  CHECK(!msc_lun_writable(&lun));
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_WRITE, 0, 0, 512, &sense));
  CHECK_EQ_INT(sense.key, MSC_SENSE_KEY_DATA_PROTECT);
  CHECK_EQ_INT(sense.asc, MSC_ASC_WRITE_PROTECTED);
  // Positive control: reads of the same range are fine.
  CHECK(msc_lun_check_transfer(&lun, MSC_OP_READ, 0, 0, 512, &sense));
  // And a source that *has* a write hook but says not-writable right now.
  msc_lun_bind(&lun, &s_rw);
  s_fake.writable = false;
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_WRITE, 0, 0, 512, &sense));
  CHECK_EQ_INT(sense.key, MSC_SENSE_KEY_DATA_PROTECT);
}

static void range_is_enforced_in_bytes_without_overflow(void) {
  struct msc_lun lun;
  struct msc_sense sense = {0, 0, 0};
  reset_fake();
  msc_lun_init(&lun);
  msc_lun_bind(&lun, &s_rw);
  // Last block, whole block: fine.
  CHECK(msc_lun_check_transfer(&lun, MSC_OP_READ, 999, 0, 512, &sense));
  // One byte past the end: refused, even though the LBA itself is valid.
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_READ, 999, 0, 513, &sense));
  CHECK_EQ_INT(sense.key, MSC_SENSE_KEY_ILLEGAL_REQUEST);
  CHECK_EQ_INT(sense.asc, MSC_ASC_LBA_OUT_OF_RANGE);
  // Sub-block fragments (USB buffer smaller than a block) add up correctly.
  s_fake.geo.block_size = 2048;
  s_fake.geo.block_count = 4;
  CHECK(msc_lun_check_transfer(&lun, MSC_OP_READ, 3, 1536, 512, &sense));
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_READ, 3, 1536, 513, &sense));
  // A 32 GB card: lba * 512 overflows uint32 past 4 GB. The math must not.
  s_fake.geo.block_size = 512;
  s_fake.geo.block_count = 62500000u;  // 32 GB
  CHECK(msc_lun_check_transfer(&lun, MSC_OP_READ, 62499999u, 0, 512, &sense));
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_READ, 62500000u, 0, 512, &sense));
  // The old 32-bit wrap: lba 8388608 * 512 == 2^32, which a uint32 product
  // turns into 0 and would accept anywhere. Must be judged by real position.
  s_fake.geo.block_count = 8388608u;  // exactly 4 GB
  CHECK(msc_lun_check_transfer(&lun, MSC_OP_READ, 8388607u, 0, 512, &sense));
  CHECK(!msc_lun_check_transfer(&lun, MSC_OP_READ, 8388608u, 0, 512, &sense));
}

static bool present_no_geometry(void *ctx, struct msc_geometry *out) {
  (void)ctx;
  out->block_count = 10;
  out->block_size = 0;  // a source that forgot; must not become a divisor
  return true;
}
static int read_only_stub(void *ctx, uint32_t lba, uint32_t offset, void *buf,
                          uint32_t len) {
  (void)ctx;
  (void)lba;
  (void)offset;
  (void)buf;
  (void)len;
  return 0;
}

static void degenerate_sources_are_handled(void) {
  struct msc_lun lun;
  struct msc_geometry geo;
  msc_lun_init(&lun);
  // bind(NULL) is a detach, not a crash, and sense may be NULL.
  msc_lun_bind(&lun, NULL);
  CHECK(!lun.attention);
  CHECK(!msc_lun_test_unit_ready(&lun, NULL));
  // A source with no is_present is never present.
  const struct msc_source no_present = {.name = "x", .read = read_only_stub};
  msc_lun_bind(&lun, &no_present);
  CHECK(!msc_lun_test_unit_ready(&lun, NULL));
  CHECK(!msc_lun_capacity(&lun, &geo));
  CHECK_EQ_INT(geo.block_size, 512);
  // A source reporting block_size 0: treated as absent, size defaults.
  const struct msc_source zero_bs = {
      .name = "z", .is_present = present_no_geometry, .read = read_only_stub};
  msc_lun_bind(&lun, &zero_bs);
  CHECK(!msc_lun_capacity(&lun, &geo));
  CHECK_EQ_INT(geo.block_count, 0);
  CHECK_EQ_INT(geo.block_size, 512);
  // No is_writable hook but a write hook: writable.
  reset_fake();
  const struct msc_source no_hook = {.name = "w",
                                     .is_present = fake_present,
                                     .read = fake_read,
                                     .write = fake_write,
                                     .ctx = &s_fake};
  msc_lun_bind(&lun, &no_hook);
  CHECK(msc_lun_writable(&lun));
}

int main(void) {
  RUN(degenerate_sources_are_handled);
  RUN(no_medium_is_not_ready);
  RUN(bind_reports_attention_exactly_once);
  RUN(medium_vanishing_under_a_bound_source);
  RUN(unbind_and_rebind_bumps_generation);
  RUN(read_only_refuses_writes_with_data_protect);
  RUN(range_is_enforced_in_bytes_without_overflow);
  return test_summary("msc_lun");
}
