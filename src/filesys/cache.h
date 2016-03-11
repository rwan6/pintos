#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include <list.h>
#include "devices/block.h"
#include "threads/synch.h"

/* Size limit for the buffer cache. */
#define CACHE_SIZE 64

/* Period of time (in ms) write behind thread sleeps before flushing cache
   to disk. */        
#define WRITE_BEHIND_WAIT 2000

/* Size of the readahead queue. */
#define READAHEAD_SIZE (CACHE_SIZE / 2)

/* How far ahead the readahead thread should jump in its indexing so that
   it does not fall inefficiently far behind. */
#define READAHEAD_CATCHUP (READAHEAD_SIZE / 4)
                                               
/* Entry into the cache.  Holds metadata about the entry in addition to
   the data block. */
struct cache_entry
  {
    bool accessed;            /* Whether the entry was recently accessed. */
    bool dirty;               /* Whether the entry was recently modified. */
    int sector_idx;           /* Block sector index. -1 if free. */
    int next_sector_idx;      /* Next block sector if evicting. -1 if
                                 not evicting. */
    char data[BLOCK_SECTOR_SIZE]; /* Cache data block. */
    struct lock entry_lock;       /* Per-entry lock. */
  };

struct cache_entry cache_table[CACHE_SIZE]; /* Buffer cache. */
int readahead_list[READAHEAD_SIZE];         /* Readahead queue. */
int next_readahead_entry; /* Points to next readahead queue entry. */

struct lock eviction_lookup_lock; /* Lock for synchronizing eviction 
                                     and lookup. */
struct lock readahead_lock;    /* Lock associated with readahead_cond. */
struct condition readahead_cond;  /* Readahead thread wakeup condition. */

/* Prototypes for cache.c functions. */
void cache_init (void);
void cache_read (block_sector_t, void *, int, int);
void cache_write (block_sector_t, void *, int, int);
void cache_flush (void);

#endif /* filesys/cache.h */
