#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include <list.h>
#include "devices/block.h"
#include "threads/synch.h"

#define CACHE_SIZE 64 /* Size limit for the buffer cache. */
#define WRITE_BEHIND_WAIT 200 /* Period of time (in ms) write behind
                                   thread sleeps before flushing cache
                                   to disk. */
#define READAHEAD_SIZE CACHE_SIZE / 4 /* Size of the readahead queue. */

/* Entry into the cache.  Holds metadata about the entry in addition to
   the data block. */
struct cache_entry
  {
    bool accessed;            /* Whether the entry was recently accessed. */
    bool dirty;               /* Whether the entry was recently modified. */
    int sector_idx;           /* Block sector index. */
    bool free;                /* Whether the cache entry is free. */
    char data[BLOCK_SECTOR_SIZE]; /* Cache data block. */
    struct lock entry_lock;       /* Per-entry lock. */
  };

struct cache_entry cache_table[CACHE_SIZE]; /* Buffer cache. */
int readahead_list[READAHEAD_SIZE];         /* Readahead queue. */
int next_readahead_entry; /* Points to next readahead queue entry. */

struct lock clock_handle_lock;    /* Lock for the eviction clock handle. */
struct lock readahead_lock;       /* Lock associated with readahead_cond. */
struct condition readahead_cond;  /* Readahead thread wakeup condition. */

/* Prototypes for cache.c functions. */
void cache_init (void);
void cache_read (block_sector_t, void *, int, int);
void cache_write (block_sector_t, void *, int, int);

#endif /* filesys/cache.h */
