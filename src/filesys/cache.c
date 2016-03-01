#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"

static int num_slots;

struct condition readahead_cond;  /* Readahead thread wakeup condition. */
struct lock readahead_lock;       /* Lock associated with readahead_cond. */

/* Function prototypes. */
int cache_lookup (block_sector_t sector_idx);
int cache_fetch (block_sector_t);
int cache_evict (void);
void cache_flush (void);
void cache_readahead (void);
void periodic_write_behind (void *);
void read_ahead (void *);

void
cache_init (void)
{
  num_slots = 0;
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      cache_table[i].accessed = false;
      cache_table[i].dirty = false;
      cache_table[i].sector_idx = 0;
      cache_table[i].free = true;
    }
    
  lock_init (&readahead_lock);
  cond_init (&readahead_cond);
  
  /* Spawn threads that will write back to cache periodically and will
     manage readahead in the background. */
  thread_create ("write-behind", PRI_DEFAULT, periodic_write_behind, NULL);
  thread_create ("read-ahead", PRI_DEFAULT, read_ahead, NULL);
}

/* One thread is in charge of periodically being awoken and flushing
   the cache back to disk.  This process is repeated for the duration
   of the program. */
void
periodic_write_behind (void *aux UNUSED)
{
  /* Thread CANNOT terminate, so it is wrapped in an infinite while loop. */
  while (1)
    {
      timer_msleep (WRITE_BEHIND_WAIT);
      cache_flush();
    }
}

/* One thread is in charge of reading ahead by one block from the disk
   after the intended block was returned.  The thread will wait to
   be awoken by the original file reader so that it does not busy
   wait in the background. */
void
read_ahead (void *aux UNUSED)
{
  /* Thread CANNOT terminate, so it is wrapped in an infinite while loop. */
  while (1)
    {
      /* TODO: Implement read_ahead */
      cond_wait (&readahead_cond, &readahead_lock);
    }
}

int
cache_lookup (block_sector_t sector_idx)
{
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      if (!cache_table[i].free && cache_table[i].sector_idx == sector_idx)
        return i;
    }
  return -1;
}

void
cache_read (block_sector_t sector_idx, void *buffer)
{
  int index = cache_lookup (sector_idx);
  if (index >= 0 && index < CACHE_SIZE)
    memcpy (buffer, cache + index, BLOCK_SECTOR_SIZE);
  else
    {
      index = cache_fetch (sector_idx);
      memcpy (buffer, cache + index, BLOCK_SECTOR_SIZE);
      cache_table[index].accessed = true;
    }
}

void
cache_write (block_sector_t sector_idx, void *buffer)
{
  int index = cache_lookup (sector_idx);
  if (index >= 0 && index < CACHE_SIZE)
    memcpy (cache + index, buffer, BLOCK_SECTOR_SIZE);
  else
    {
      index = cache_fetch (sector_idx);
      memcpy (cache + index, buffer, BLOCK_SECTOR_SIZE);
      cache_table[index].accessed = true;
      cache_table[index].dirty = true;
      
      /* Signal readahead thread to fetch next block in the background. */
      cond_signal (&readahead_cond, &readahead_lock);
    }
}

int
cache_fetch (block_sector_t sector_idx)
{
  int index = -1;
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      if (cache_table[i].free)
        index = i;
    }
  if (index == -1)
    {
      index = cache_evict ();
    }
  else
      num_slots++;
  block_read (fs_device, sector_idx, cache + index);
  cache_table[index].free = false;
  cache_table[index].sector_idx = sector_idx;
  return index;
}

int
cache_evict (void)
{
  return 0;
}

void
cache_flush (void)
{
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      if (!cache_table[i].free && cache_table[i].dirty)
        block_write (fs_device, cache_table[i].sector_idx, cache + i);
    }
}
