#include <string.h>
#include <stdio.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"

static int num_taken_slots; /* Number of occupied cache slots. */

/* Function prototypes. */
int cache_lookup (block_sector_t sector_idx);
int cache_fetch (block_sector_t);
void cache_writeback_if_dirty (int);
int cache_evict (void);
void cache_readahead (void);
void periodic_write_behind (void *);
void read_ahead (void *);

/* Initialize the buffer cache and all buffer cache entries.
   Also initializes the readahead list and creates the subprocesses
   that take care of periodic cache flushing (write-behind) and
   fetching future blocks (readahead). */
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

  lock_init (&clock_handle_lock);
  lock_init (&readahead_lock);
  lock_init (&io_lock);
  cond_init (&readahead_cond);

  i = 0;
  for (; i < READAHEAD_SIZE; i++)
    readahead_list[i] = -1;

  next_readahead_entry = 0;

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
  int ra_index = 0;
  int next_ra;
  int next_sector;

  /* Thread CANNOT terminate, so it is wrapped in an infinite while loop. */
  while (1)
    {
      lock_acquire (&readahead_lock);
      while (ra_index == next_readahead_entry)
        cond_wait (&readahead_cond, &readahead_lock);
      // printf ("RA: %d, NextRA: %d\n", ra_index, next_readahead_entry);
      /* If the readahead thread falls behind too far, force it to catch
         up. */
      if (ra_index + READAHEAD_SIZE < next_readahead_entry)
        ra_index = next_readahead_entry - 1;

      next_ra = ra_index % READAHEAD_SIZE;
      next_sector = readahead_list[next_ra];
      
      lock_release (&readahead_lock);
      
      /* If the block is not already in the cache, fetch it. */
      if (cache_lookup (next_sector) == -1)
        {
          int new_sector = cache_fetch (next_sector);
          cache_table[new_sector].accessed = true;          
        }
      ra_index++;
    }
}

/* Look up an entry corresponding to the inputted sector index.
   Returns the cache index of the desired block or -1 if the
   block was not found in the cache. */
int
cache_lookup (block_sector_t sector_idx)
{
  int i = 0;
  int index = -1;
  for (; i < CACHE_SIZE; i++)
    {
      lock_acquire (&cache_table[i].entry_lock);
      if (!cache_table[i].free && cache_table[i].sector_idx ==
         (int) sector_idx)
        {
          index = i;
          lock_release (&cache_table[i].entry_lock);
          break;
        }
      lock_release (&cache_table[i].entry_lock);
    }
  return index;
}

/* Read 'chunk_size' bytes of a block entry that starts at sector
  'sector_idx' with offset 'sector_ofs' into 'buffer'. If the
   entry was not found in the cache, it is fetched from disk.
   Lock down during memory copy and changing accessed bit. */
void
cache_read (block_sector_t sector_idx, void *buffer, int chunk_size,
            int sector_ofs)
{
  int index = cache_lookup (sector_idx);
  if (index == -1)
    index = cache_fetch (sector_idx);
  
  lock_acquire (&cache_table[index].entry_lock);
  memcpy (buffer, cache_table[index].data + sector_ofs, chunk_size);
  cache_table[index].accessed = true;
  lock_release (&cache_table[index].entry_lock);
}

/* Writes 'chunk_size' bytes of a block entry that starts at sector
  'sector_idx' with offset 'sector_ofs' into 'buffer'. If the
   entry was not found in the cache, it is fetched from disk.
   Lock down during memory copy and changing accessed and dirty bits. */
void
cache_write (block_sector_t sector_idx, void *buffer, int chunk_size,
             int sector_ofs)
{
  int index = cache_lookup (sector_idx);
  if (index == -1)
    index = cache_fetch (sector_idx);
  lock_acquire (&cache_table[index].entry_lock);
  memcpy (cache_table[index].data + sector_ofs, buffer, chunk_size);
  cache_table[index].accessed = true;
  cache_table[index].dirty = true;
  lock_release (&cache_table[index].entry_lock);
}

/* Fetch an entry from the disk into the cache.  If no cache entries
   are free, evict an entry before reading from disk. */
int
cache_fetch (block_sector_t sector_idx)
{
  lock_acquire (&clock_handle_lock);
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

  lock_release (&clock_handle_lock);

  ASSERT (index != -1);

  lock_acquire (&io_lock);
  block_read (fs_device, sector_idx, cache_table[index].data);
  lock_release (&io_lock);

  lock_acquire (&cache_table[index].entry_lock);
  cache_table[index].free = false;
  cache_table[index].accessed = false;
  cache_table[index].dirty = false;
  cache_table[index].sector_idx = (int) sector_idx;
  lock_release (&cache_table[index].entry_lock);
  
  return index;
}

/* Writes the cache block back to disk if the cache block is dirty. Also
   clears the dirty bit associated with that cache entry. */
void
cache_writeback_if_dirty (int index)
{
  if (cache_table[index].dirty)
    {
      lock_acquire (&io_lock);
      block_write (fs_device, cache_table[index].sector_idx,
          cache_table[index].data);
      lock_release (&io_lock);
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
  int evicted_idx = -1;
  
  // lock_acquire (&clock_handle_lock);
  while (!found)
    {
      /* Clock handle can change sporadically, so it is declared as
         a volatile int. */
      volatile int cur_clock_handle = cache_clock_handle;
      lock_acquire (&cache_table[cur_clock_handle].entry_lock);
      if (cache_table[cur_clock_handle].accessed)
        cache_table[cur_clock_handle].accessed = false;
      else
        {
          found = true;
          evicted_idx = cur_clock_handle;
          /* Release during I/O to allow other processes to evict in the
             meantime. */
          lock_release (&clock_handle_lock);          
          cache_writeback_if_dirty (cur_clock_handle);
          lock_acquire (&clock_handle_lock);
        }

      lock_release (&cache_table[cur_clock_handle].entry_lock);
      if (cache_clock_handle == CACHE_SIZE - 1)
        cache_clock_handle = 0;
      else
        cache_clock_handle++;
    }
  // lock_release (&clock_handle_lock);

  ASSERT (evicted_idx >= 0);
  return evicted_idx;
}

/* Iterate over all cache entries call cache_writeback_if_dirty if the
   entry is not unoccupied. */
void
cache_flush (void)
{
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      lock_acquire (&cache_table[i].entry_lock);
      if (!cache_table[i].free)
        cache_writeback_if_dirty (i);
      lock_release (&cache_table[i].entry_lock);
    }
}
