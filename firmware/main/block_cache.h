// A small cache of image chunks, filled by a transport and read by the LUN.
//
// Pure: no I/O, no allocation, no ESP-IDF headers; the caller supplies the
// memory (internal RAM today, PSRAM once that is enabled — the cache does not
// care which). Host-tested in test/host/test_block_cache.c. AGENTS.md §4.
//
// Shape: N slots of CHUNK bytes, direct-mapped by nothing — any chunk can
// live in any slot, and eviction is least-recently-used among slots that are
// not currently being filled. A slot is "claimed" while a transport fills it
// piece by piece and becomes "valid" only when every byte has arrived, so a
// reader can never see a half-filled chunk as data.
//
// Pieces of a chunk must arrive in order, each starting exactly where the
// previous one ended. That is not a convenience: the alternative, counting
// bytes received, lets a retransmitted piece complete a chunk that still has
// a hole in it, and the hole would be served to the host as data — the worst
// outcome this cache can produce. In-order is also what the browser naturally
// sends, and a piece that does not fit the sequence is simply refused.
//
// Generation: bumped by invalidate_all (a detach). Every claim records the
// generation it was made under, and a fill against an older generation is
// refused, so the tail of a download for an image that is gone can never land
// in a slot now belonging to its replacement.
//
// A claimed slot is never evicted, so a transport that neither completes nor
// releases a claim pins that slot for good, and N such claims leave nothing
// to evict. The caller owes every request a timeout that ends in release();
// nothing in here can recover a stuck claim on its own. Nothing in here
// locks either; the caller serialises (block_source_browser.c's lock).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_CACHE_NO_SLOT (-1)

struct block_cache_slot {
  uint32_t chunk;
  uint32_t generation;
  uint32_t filled;  // bytes received so far while claimed
  uint32_t last_use;
  bool valid;
  bool claimed;
};

struct block_cache {
  uint8_t *mem;
  struct block_cache_slot *slots;
  size_t nslots;
  uint32_t chunk_size;
  uint32_t generation;
  uint32_t clock;  // LRU tick
};

void block_cache_init(struct block_cache *c, uint8_t *mem, uint32_t chunk_size,
                      struct block_cache_slot *slots, size_t nslots);

// Every slot is dropped, in-flight fills included; the generation advances.
void block_cache_invalidate_all(struct block_cache *c);

// The data for `chunk` if it is fully present, else NULL. Counts as a use.
const uint8_t *block_cache_lookup(struct block_cache *c, uint32_t chunk);

// True if `chunk` is present or being filled — a caller should not request
// it again.
bool block_cache_has(const struct block_cache *c, uint32_t chunk);

// Reserve a slot for `chunk`, evicting the least recently used unclaimed slot
// if needed. Returns the slot index, the same slot if `chunk` is already
// claimed or present, or BLOCK_CACHE_NO_SLOT if every slot is mid-fill.
int block_cache_claim(struct block_cache *c, uint32_t chunk);

// Deliver `len` bytes at `offset` into the slot claimed for `chunk`. Refused
// (false) if the chunk is not claimed, the generation has moved on, the piece
// is empty or would overrun the chunk, or — the load-bearing one — `offset`
// is not exactly the number of bytes already delivered. The slot becomes
// valid when the last byte lands.
bool block_cache_fill(struct block_cache *c, uint32_t chunk,
                      uint32_t generation, uint32_t offset, const uint8_t *data,
                      uint32_t len);

// Abandon a claim (the transport gave up on it).
void block_cache_release(struct block_cache *c, uint32_t chunk);
