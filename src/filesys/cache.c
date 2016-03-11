#include <string.h>
#include <stdio.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"

/* Function prototypes. */
static int cache_lookup (block_sector_t sector_idx);
static void cache_writeback_if_dirty (int);
static int cache_evict (block_sector_t);
void periodic_write_behind (void *);
void read_ahead (void *);

/* Initialize the buffer cache and all buffer cache entries.
   Also initializes the readahead list and creates the subprocesses
   that take care of periodic cache flushing (write-behind) and
   fetching future blocks (readahead). */
void
cache_init (void)
{
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      cache_table[i].accessed = false;
      cache_table[i].dirty = false;
      cache_table[i].sector_idx = -1;
      cache_table[i].next_sector_idx = -1;
      lock_init (&cache_table[i].entry_lock);
    }

  lock_init (&eviction_lookup_lock);
  lock_init (&readahead_lock);
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
  int next_sector;

  /* Thread CANNOT terminate, so it is wrapped in an infinite while loop. */
  while (1)
    {
      lock_acquire (&readahead_lock);
      ra_index = ra_index % READAHEAD_SIZE;
      while (ra_index == next_readahead_entry)
        cond_wait (&readahead_cond, &readahead_lock);
      
      /* If the readahead thread falls behind too far, force it to catch
         up. */
      if (ra_index + READAHEAD_SIZE < next_readahead_entry)
        ra_index = next_readahead_entry - READAHEAD_CATCHUP;

      next_sector = readahead_list[ra_index];
      lock_release (&readahead_lock);
      
      /* If the block is not already in the cache, fetch it. */
      int ra_index = cache_lookup ((block_sector_t) next_sector);
      ASSERT (lock_held_by_current_thread 
        (&cache_table[ra_index].entry_lock));
      lock_release (&cache_table[ra_index].entry_lock);
      ra_index++;
    }
}

/* Look up an entry corresponding to the inputted sector index.
   Returns the cache index of the desired block. */
static int
cache_lookup (block_sector_t sector_idx)
{  
  while (1)
    {
      lock_acquire (&eviction_lookup_lock);
      
      /* Look for the entry in the cache.  Set sector to be the cache index
         if we find the block in cache or if the next block is the one we
         are looking for. */
      int sector = -1;
      int i = 0;
      for (; i < CACHE_SIZE; i++)
        if (cache_table[i].sector_idx == (int) sector_idx ||
            cache_table[i].next_sector_idx == (int) sector_idx)
            sector = i;
      
      /* Could not find the block in the cache.  Proceed to eviction.
         Note that by the time cache_evict returns, the process will
         be holding the cache sector's respective lock. */
      if (sector == -1)
        {
          sector = cache_evict (sector_idx);
          block_read (fs_device, sector_idx, cache_table[sector].data);
        }
      else /* Found the block, simply acquire its respective lock and
              release the lookup/eviction lock. */
        {
          lock_release (&eviction_lookup_lock);
          lock_acquire (&cache_table[sector].entry_lock);
        }
      
      /* Confirm it is the block being looked up.  If not, repeat the
         process.  This ensures synchronization with the
         eviction process. */
      if (cache_table[sector].sector_idx == (int) sector_idx &&
          cache_table[sector].next_sector_idx == -1)
            return sector;
      else
        lock_release (&cache_table[sector].entry_lock);
    }
}

/* Evicts the appropriate cache element and returns the index of the
   evicted element */
static int
cache_evict (block_sector_t evict_sector)
{
  int evicted_idx = -1;
  static int cache_clock_handle = -1;

  while (1)
    {
      cache_clock_handle = (cache_clock_handle + 1) % CACHE_SIZE;
      
      /* If it is not currently being evicted. */
      if (cache_table[cache_clock_handle].next_sector_idx == -1)
        {
          if (cache_table[cache_clock_handle].accessed)
            cache_table[cache_clock_handle].accessed = false;
          else
            break;
        }
    }
  
  /* Alter the cache slot's metadata before releasing the eviction/lookup
     lock. */
  evicted_idx = cache_clock_handle;
  cache_table[evicted_idx].next_sector_idx = (int) evict_sector;
  lock_release (&eviction_lookup_lock);
  
  /* Acquire the respective sector's lock. */
  lock_acquire (&cache_table[evicted_idx].entry_lock);
  cache_writeback_if_dirty (evicted_idx);
  
  /* Clear remaining metadata. */
  cache_table[evicted_idx].sector_idx = (int) evict_sector;
  cache_table[evicted_idx].next_sector_idx = -1;
  cache_table[evicted_idx].accessed = false; 
  return evicted_idx;
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
  ASSERT (lock_held_by_current_thread (&cache_table[index].entry_lock));
  
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
  ASSERT (lock_held_by_current_thread (&cache_table[index].entry_lock));
  
  memcpy (cache_table[index].data + sector_ofs, buffer, chunk_size);
  cache_table[index].accessed = true;
  cache_table[index].dirty = true;
  lock_release (&cache_table[index].entry_lock);
}

/* Writes the cache block back to disk if the cache block is dirty. Also
   clears the dirty bit associated with that cache entry. */
static void
cache_writeback_if_dirty (int index)
{
  if (cache_table[index].dirty)
    {
      block_write (fs_device, cache_table[index].sector_idx,
          cache_table[index].data);
      cache_table[index].dirty = false;
    }
}

/* Iterate over all cache entries and call cache_writeback_if_dirty,
   which will check if the block is dirty and should be written back. */
void
cache_flush (void)
{
  int i = 0;
  for (; i < CACHE_SIZE; i++)
    {
      lock_acquire (&cache_table[i].entry_lock);
      cache_writeback_if_dirty (i);
      lock_release (&cache_table[i].entry_lock);
    }
}
