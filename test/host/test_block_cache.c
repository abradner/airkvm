#include <string.h>

#include "block_cache.h"
#include "test_support.h"

// Small chunks so a test can fill one in a couple of calls.
#define CHUNK 64
#define NSLOTS 3

static uint8_t s_mem[CHUNK * NSLOTS];
static struct block_cache_slot s_slots[NSLOTS];
static struct block_cache s_c;

static void fresh(void) {
  memset(s_mem, 0xEE, sizeof(s_mem));
  block_cache_init(&s_c, s_mem, CHUNK, s_slots, NSLOTS);
}

// Fills `chunk` completely with bytes of value `v`, in two pieces.
static void fill_whole(uint32_t chunk, uint8_t v) {
  uint8_t piece[CHUNK / 2];
  memset(piece, v, sizeof(piece));
  CHECK(block_cache_claim(&s_c, chunk) != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_fill(&s_c, chunk, s_c.generation, 0, piece, sizeof(piece)));
  CHECK(block_cache_fill(&s_c, chunk, s_c.generation, CHUNK / 2, piece,
                         sizeof(piece)));
}

static void miss_fill_hit(void) {
  fresh();
  CHECK(block_cache_lookup(&s_c, 5) == NULL);
  CHECK(!block_cache_has(&s_c, 5));
  const int slot = block_cache_claim(&s_c, 5);
  CHECK(slot != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_has(&s_c, 5));                 // being filled counts
  CHECK(block_cache_lookup(&s_c, 5) == NULL);      // but is not data yet
  CHECK_EQ_INT(block_cache_claim(&s_c, 5), slot);  // re-claim: same slot
  fill_whole(5, 0x55);
  const uint8_t *d = block_cache_lookup(&s_c, 5);
  CHECK(d != NULL);
  CHECK_EQ_INT(d[0], 0x55);
  CHECK_EQ_INT(d[CHUNK - 1], 0x55);
  CHECK(d == s_mem + (size_t)slot * CHUNK);
}

static void partial_fill_is_never_data(void) {
  fresh();
  uint8_t piece[CHUNK - 1];
  memset(piece, 0x11, sizeof(piece));
  CHECK(block_cache_claim(&s_c, 9) != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_fill(&s_c, 9, s_c.generation, 0, piece, sizeof(piece)));
  CHECK(block_cache_lookup(&s_c, 9) == NULL);  // one byte short
  // The last byte completes it.
  CHECK(block_cache_fill(&s_c, 9, s_c.generation, CHUNK - 1, piece, 1));
  CHECK(block_cache_lookup(&s_c, 9) != NULL);
  // Filling a chunk that is already data is refused (nothing is claimed).
  CHECK(!block_cache_fill(&s_c, 9, s_c.generation, 0, piece, 1));
  // And claiming it again hands back its slot rather than a fresh one.
  const int slot = block_cache_claim(&s_c, 9);
  CHECK(slot != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_lookup(&s_c, 9) == s_mem + (size_t)slot * CHUNK);
  // release() on data, or on nothing, is a no-op.
  block_cache_release(&s_c, 9);
  block_cache_release(&s_c, 999);
  CHECK(block_cache_lookup(&s_c, 9) != NULL);
}

static void pieces_must_arrive_in_order_so_a_retransmit_cannot_fake_a_fill(
    void) {
  fresh();
  uint8_t half[CHUNK / 2];
  memset(half, 0x22, sizeof(half));
  CHECK(block_cache_claim(&s_c, 30) != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_fill(&s_c, 30, s_c.generation, 0, half, sizeof(half)));
  // The same first half again — a retransmit, or a duplicate request's
  // answer. Counted as bytes it would complete the chunk with the second
  // half never delivered, and the slot's old contents would be served as
  // data. It must be refused, and the chunk must stay incomplete.
  CHECK(!block_cache_fill(&s_c, 30, s_c.generation, 0, half, sizeof(half)));
  CHECK(block_cache_lookup(&s_c, 30) == NULL);
  // Out of order (a gap) is refused too.
  CHECK(!block_cache_fill(&s_c, 30, s_c.generation, CHUNK / 2 + 1, half, 1));
  CHECK(block_cache_lookup(&s_c, 30) == NULL);
  // Positive control: the piece that starts where the last ended completes it.
  CHECK(block_cache_fill(&s_c, 30, s_c.generation, CHUNK / 2, half,
                         sizeof(half)));
  CHECK(block_cache_lookup(&s_c, 30) != NULL);
}

static void fill_is_bounds_checked(void) {
  fresh();
  uint8_t piece[CHUNK];
  memset(piece, 0, sizeof(piece));
  CHECK(block_cache_claim(&s_c, 1) != BLOCK_CACHE_NO_SLOT);
  // Past the end of the chunk, by one byte: refused, slot untouched.
  CHECK(!block_cache_fill(&s_c, 1, s_c.generation, 1, piece, CHUNK));
  CHECK(!block_cache_fill(&s_c, 1, s_c.generation, CHUNK, piece, 1));
  CHECK(!block_cache_fill(&s_c, 1, s_c.generation, 0, piece, 0));
  CHECK(!block_cache_fill(&s_c, 1, s_c.generation, 0, NULL, 1));
  // A chunk nobody claimed.
  CHECK(!block_cache_fill(&s_c, 2, s_c.generation, 0, piece, 1));
  // Positive control: exactly the whole chunk in one piece.
  CHECK(block_cache_fill(&s_c, 1, s_c.generation, 0, piece, CHUNK));
  CHECK(block_cache_lookup(&s_c, 1) != NULL);
  // The neighbouring slot's memory was not written by the refused fills.
  CHECK_EQ_INT(s_mem[CHUNK], 0xEE);
}

static void eviction_is_lru_among_unclaimed(void) {
  fresh();
  fill_whole(10, 1);
  fill_whole(11, 2);
  fill_whole(12, 3);
  // Touch 10 so 11 is the oldest.
  CHECK(block_cache_lookup(&s_c, 10) != NULL);
  CHECK(block_cache_claim(&s_c, 13) != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_has(&s_c, 10));
  CHECK(!block_cache_has(&s_c, 11));  // evicted
  CHECK(block_cache_has(&s_c, 12));
  // Claim two more: 10 and 12 go, in age order, never the in-flight 13.
  CHECK(block_cache_claim(&s_c, 14) != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_claim(&s_c, 15) != BLOCK_CACHE_NO_SLOT);
  CHECK(block_cache_has(&s_c, 13));
  // Every slot is now mid-fill: nothing to evict.
  CHECK_EQ_INT(block_cache_claim(&s_c, 16), BLOCK_CACHE_NO_SLOT);
  // Releasing one makes room again.
  block_cache_release(&s_c, 14);
  CHECK(!block_cache_has(&s_c, 14));
  CHECK(block_cache_claim(&s_c, 16) != BLOCK_CACHE_NO_SLOT);
}

static void invalidate_drops_everything_and_stale_fills_are_refused(void) {
  fresh();
  fill_whole(20, 7);
  const uint32_t old_gen = s_c.generation;
  CHECK(block_cache_claim(&s_c, 21) != BLOCK_CACHE_NO_SLOT);  // in flight
  block_cache_invalidate_all(&s_c);
  CHECK(!block_cache_has(&s_c, 20));
  CHECK(!block_cache_has(&s_c, 21));
  CHECK(block_cache_lookup(&s_c, 20) == NULL);
  for (int i = 0; i < NSLOTS; i++) {
    CHECK(!s_slots[i].valid);
    CHECK(!s_slots[i].claimed);
  }
  // A late piece for the old image, for a chunk re-claimed under the new
  // one, must not land: same chunk number, old generation.
  uint8_t piece[CHUNK];
  memset(piece, 0xAA, sizeof(piece));
  CHECK(block_cache_claim(&s_c, 21) != BLOCK_CACHE_NO_SLOT);
  CHECK(!block_cache_fill(&s_c, 21, old_gen, 0, piece, CHUNK));
  CHECK(block_cache_lookup(&s_c, 21) == NULL);
  // Positive control: the current generation fills it.
  CHECK(block_cache_fill(&s_c, 21, s_c.generation, 0, piece, CHUNK));
  CHECK(block_cache_lookup(&s_c, 21) != NULL);
}

int main(void) {
  RUN(miss_fill_hit);
  RUN(partial_fill_is_never_data);
  RUN(pieces_must_arrive_in_order_so_a_retransmit_cannot_fake_a_fill);
  RUN(fill_is_bounds_checked);
  RUN(eviction_is_lru_among_unclaimed);
  RUN(invalidate_drops_everything_and_stale_fills_are_refused);
  return test_summary("block_cache");
}
