#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include <list.h>
#include "devices/block.h"

#define CACHE_SIZE 64 /* Size limit for the buffer cache. */
#define WRITE_BEHIND_WAIT 10000 /* Period of time (in ms) write behind
                                   thread sleeps before flushing cache
                                   to disk. */

/* Entry into the cache.  Holds metadata about the entry in addition to
   the data block. */
struct cache_entry
  {
    bool accessed;            /* Whether the entry was recently accessed. */
    bool dirty;               /* Whether the entry was recently modified. */
    block_sector_t sector_idx;  /* Block sector index. */
    bool free;                  /* Whether the cache entry is free. */
    char data[BLOCK_SECTOR_SIZE]; /* Cache data block. */
  };

/* Entry into the readahead list for the next block to be fetched. */
struct readahead_entry
  {
    block_sector_t next_sector;      /* Next sector to fetch. */
    struct list_elem readahead_elem; /* readahead_list entry. */
  };

struct cache_entry cache_table[CACHE_SIZE];

/* Prototypes for cache.c functions. */
void cache_init (void);
void cache_read (block_sector_t, void *);
void cache_write (block_sector_t, void *);
void cache_writeback_if_dirty (int);
int cache_evict (void);
void periodic_write_behind (void *);
void read_ahead (void *);

#endif /* filesys/cache.h */