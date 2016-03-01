#include "devices/block.h"
#include <stdbool.h>

#define CACHE_SIZE 64

struct cache_entry
  {
    bool accessed;
    bool dirty;
    block_sector_t sector_idx;
    bool free;
  };

struct cache_slot
  {
    char data[BLOCK_SECTOR_SIZE];
  };

static struct cache_entry cache_table[CACHE_SIZE];
static struct cache_slot cache[CACHE_SIZE];

void cache_init (void);
void cache_read (block_sector_t, void *);
void cache_write (block_sector_t, void *);