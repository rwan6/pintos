#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include <string.h>

static int num_slots;

int cache_lookup (block_sector_t sector_idx);
int cache_fetch (block_sector_t);
int cache_evict (void);
void cache_flush (void);
void cache_readahead (void);

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
    }
}

int cache_fetch (block_sector_t sector_idx)
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
