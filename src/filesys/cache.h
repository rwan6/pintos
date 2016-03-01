#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include <stdbool.h>

#define CACHE_SIZE 64 /* Size limit for the buffer cache. */
#define WRITE_BEHIND_WAIT 10000 /* Period of time (in ms) write behind
                                   thread sleeps before flushing cache
                                   to disk. */

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

/* Prototypes for cache.c functions. */
void cache_init (void);
void cache_read (block_sector_t, void *);
void cache_write (block_sector_t, void *);
void periodic_write_behind (void *);
void read_ahead (void *);

#endif /* filesys/cache.h */