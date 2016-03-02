#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"

static int num_taken_slots;

struct condition readahead_cond;  /* Readahead thread wakeup condition. */
struct lock readahead_lock;       /* Lock associated with readahead_cond. */

static struct list readahead_list; /* Readahead queue. */

/* Function prototypes. */
int cache_lookup (block_sector_t sector_idx);
int cache_fetch (block_sector_t);
void cache_writeback_if_dirty (int);
int cache_evict (void);
void cache_flush (void);
void cache_readahead (void);
void periodic_write_behind (void *);
void read_ahead (void *);

void
cache_init (void)
{
  num_taken_slots = 0;
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      cache_table[i].accessed = false;
      cache_table[i].dirty = false;
      cache_table[i].sector_idx = 0;
      cache_table[i].free = true;
      cache_table[i].sector_idx = -1;
      lock_init (&cache_table[i].entry_lock);
    }

  // list_init (&readahead_list);
  // lock_init (&readahead_lock);
  // cond_init (&readahead_cond);

  /* Spawn threads that will write back to cache periodically and will
     manage readahead in the background. */
  // thread_create ("write-behind", PRI_DEFAULT, periodic_write_behind, NULL);
  // thread_create ("read-ahead", PRI_DEFAULT, read_ahead, NULL);
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
  struct list_elem *next_elem;

  /* Thread CANNOT terminate, so it is wrapped in an infinite while loop. */
  while (1)
    {
      lock_acquire (&readahead_lock);
      if (list_empty (&readahead_list))
        cond_wait (&readahead_cond, &readahead_lock);

      next_elem = list_pop_front (&readahead_list);
      struct readahead_entry *next_readahead = list_entry (next_elem,
        struct readahead_entry, readahead_elem);
      if (cache_lookup (next_readahead->next_sector) == -1)
        cache_fetch (next_readahead->next_sector);
      lock_release (&readahead_lock);
    }
}

int
cache_lookup (block_sector_t sector_idx)
{
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      lock_acquire (&cache_table[i].entry_lock);
      if (!cache_table[i].free && cache_table[i].sector_idx == sector_idx)
        return i;
      lock_release (&cache_table[i].entry_lock);
    }
  return -1;
}

void
cache_read (block_sector_t sector_idx, void *buffer, int chunk_size, int sector_ofs)
{
  // printf("reading sector %d\n", sector_idx);
  int index = cache_lookup (sector_idx);
  if (index != -1)
    {
      // index = cache_fetch (sector_idx);
      // printf("in cache.. index = %d\n", index);
    memcpy (buffer, cache_table[index].data + sector_ofs, chunk_size);
    }
  else
    {
      index = cache_fetch (sector_idx);
      // printf("index = %d, chunk_size=%d, sector_ofs=%d\n", index, chunk_size, sector_ofs);
      memcpy (buffer, cache_table[index].data + sector_ofs, chunk_size);
      cache_table[index].accessed = true;
    }

          // block_read (fs_device, sector_idx, cache_table[11].data);
          // memcpy (buffer, cache_table[11].data + sector_ofs, chunk_size);
}

void
cache_write (block_sector_t sector_idx, void *buffer, int chunk_size)
{
  int index = cache_lookup (sector_idx);
  if (index != -1)
    memcpy (cache_table[index].data, buffer, chunk_size);
  else
    {
      index = cache_fetch (sector_idx);
      memcpy (cache_table[index].data, buffer, chunk_size);
      cache_table[index].accessed = true;
      cache_table[index].dirty = true;
    }
}

int
cache_fetch (block_sector_t sector_idx)
{
  int index = -1;

  /* If cache is already full, immediately call evict. */
  if (num_taken_slots == CACHE_SIZE)
    index = cache_evict ();
  else
    {
      int i = 0;
      for (; i < CACHE_SIZE; i++)
        {
          if (cache_table[i].free)
            {
              index = i;
              break;
            }
        }
      num_taken_slots++;
    }

  ASSERT (index != -1);

  block_read (fs_device, sector_idx, cache_table[index].data);

  cache_table[index].free = false;
  cache_table[index].accessed = false;
  cache_table[index].dirty = false;
  cache_table[index].sector_idx = sector_idx;
  return index;
}

/* Writes the cache block back to disk if the cache block is dirty. Also
   clears the dirty bit associated with that cache entry. */
void
cache_writeback_if_dirty (int index)
{
  if (cache_table[index].dirty)
    {
      block_write (fs_device, cache_table[index].sector_idx,
          cache_table[index].data);
      cache_table[index].dirty = false;
    }
}

/* Evicts the appropriate cache element and returns the index of the
   evicted element */
int
cache_evict (void)
{
  static int cache_clock_handle = 0;
  bool found = false;
  int evicted_idx;
  while (!found)
    {
      lock_acquire (&cache_table[cache_clock_handle].entry_lock);
      if (cache_table[cache_clock_handle].accessed)
        cache_table[cache_clock_handle].accessed = false;
      else
        {
          found = true;
          cache_writeback_if_dirty (cache_clock_handle);
          evicted_idx = cache_clock_handle;
        }

      lock_release (&cache_table[cache_clock_handle].entry_lock);
      if (cache_clock_handle == CACHE_SIZE - 1)
        cache_clock_handle = 0;
      else
        cache_clock_handle++;
    }
  return evicted_idx;
}

void
cache_flush (void)
{
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      if (!cache_table[i].free)
        {
          lock_acquire (&cache_table[i].entry_lock);
          cache_writeback_if_dirty (i);
          lock_release (&cache_table[i].entry_lock);
        }
    }
}
