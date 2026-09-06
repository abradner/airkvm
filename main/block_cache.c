#include "block_cache.h"

#include <string.h>

static struct block_cache_slot *find(const struct block_cache *c,
                                     uint32_t chunk) {
  for (size_t i = 0; i < c->nslots; i++) {
    struct block_cache_slot *s = &c->slots[i];
    if ((s->valid || s->claimed) && s->chunk == chunk) {
      return s;
    }
  }
  return NULL;
}

static void reset_slot(struct block_cache_slot *s) {
  memset(s, 0, sizeof(*s));
}

void block_cache_init(struct block_cache *c, uint8_t *mem, uint32_t chunk_size,
                      struct block_cache_slot *slots, size_t nslots) {
  c->mem = mem;
  c->slots = slots;
  c->nslots = nslots;
  c->chunk_size = chunk_size;
  c->generation = 1;
  c->clock = 0;
  for (size_t i = 0; i < nslots; i++) {
    reset_slot(&slots[i]);
  }
}

void block_cache_invalidate_all(struct block_cache *c) {
  for (size_t i = 0; i < c->nslots; i++) {
    reset_slot(&c->slots[i]);
  }
  c->generation++;
}

const uint8_t *block_cache_lookup(struct block_cache *c, uint32_t chunk) {
  struct block_cache_slot *s = find(c, chunk);
  if (s == NULL || !s->valid) {
    return NULL;
  }
  s->last_use = ++c->clock;
  return c->mem + (size_t)(s - c->slots) * c->chunk_size;
}

bool block_cache_has(const struct block_cache *c, uint32_t chunk) {
  return find(c, chunk) != NULL;
}

int block_cache_claim(struct block_cache *c, uint32_t chunk) {
  struct block_cache_slot *s = find(c, chunk);
  if (s != NULL) {
    return (int)(s - c->slots);
  }
  // An empty slot first; otherwise the least recently used unclaimed one.
  struct block_cache_slot *victim = NULL;
  for (size_t i = 0; i < c->nslots; i++) {
    struct block_cache_slot *cand = &c->slots[i];
    if (cand->claimed) {
      continue;
    }
    if (!cand->valid) {
      victim = cand;
      break;
    }
    // Signed difference, so the comparison survives the tick counter wrapping.
    if (victim == NULL || (int32_t)(cand->last_use - victim->last_use) < 0) {
      victim = cand;
    }
  }
  if (victim == NULL) {
    return BLOCK_CACHE_NO_SLOT;
  }
  reset_slot(victim);
  victim->chunk = chunk;
  victim->generation = c->generation;
  victim->claimed = true;
  victim->last_use = ++c->clock;
  return (int)(victim - c->slots);
}

bool block_cache_fill(struct block_cache *c, uint32_t chunk,
                      uint32_t generation, uint32_t offset, const uint8_t *data,
                      uint32_t len) {
  struct block_cache_slot *s = find(c, chunk);
  // Slot generation alone suffices: invalidate_all resets every slot, so a
  // slot that is still claimed was claimed under the current generation.
  if (s == NULL || !s->claimed || s->generation != generation) {
    return false;
  }
  // In order, contiguous, inside the chunk. See the header for why a byte
  // count would not do.
  if (len == 0 || offset != s->filled || len > c->chunk_size - offset) {
    return false;
  }
  memcpy(c->mem + (size_t)(s - c->slots) * c->chunk_size + offset, data, len);
  s->filled += len;
  if (s->filled == c->chunk_size) {
    s->claimed = false;
    s->valid = true;
    s->last_use = ++c->clock;
  }
  return true;
}

void block_cache_release(struct block_cache *c, uint32_t chunk) {
  struct block_cache_slot *s = find(c, chunk);
  if (s != NULL && s->claimed) {
    reset_slot(s);
  }
}
