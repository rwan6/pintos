#include "vm/frame.h"
#include "vm/swap.h"
#include <bitmap.h>

#define BLOCKS_IN_PAGE 8

void
init_swap_partition (void)
{
  swap_partition = block_get_role (BLOCK_SWAP);
  swap_bitmap = bitmap_create (block_size (swap_partition));
  bitmap_set_all (swap_bitmap, false);
  lock_init (&swap_lock);//printf("swap lock=%x\n", &swap_lock);
}

void
swap_read (struct swap_slot* ss, struct frame_entry *fe)
{
  int i = 0;
  for(; i < BLOCKS_IN_PAGE; i++)
    block_read (swap_partition, ss->sector + i,
      fe->addr + i * BLOCK_SECTOR_SIZE);
  swap_free (ss);
}

void
swap_write (struct swap_slot* ss, struct frame_entry *fe)
{
  swap_allocate (ss);
  int i = 0;
  // printf("before block write file lock\n");
  // lock_acquire (&swap_lock);
  // printf("after block write file lock\n");
  // printf("cur=%x\n", thread_current ());
  for(; i < BLOCKS_IN_PAGE; i++)
  {
    // printf("block_write number: %d %x\n", i, fe->addr+ i * BLOCK_SECTOR_SIZE);
    block_write (swap_partition, ss->sector + i,
      fe->addr + i * BLOCK_SECTOR_SIZE);
    // printf("after block_write\n");
  }
  // lock_release (&swap_lock);
  // printf("after block write\n");
    // block_write (swap_partition, ss->sector + i,
      // fe->addr + i * BLOCK_SECTOR_SIZE);
}

void
swap_allocate (struct swap_slot *ss)
{
  ASSERT (!bitmap_all (swap_bitmap, 0, block_size (swap_partition)));
  lock_acquire (&swap_lock);
  ss->sector = bitmap_scan_and_flip (swap_bitmap, 0, BLOCKS_IN_PAGE, false);
  lock_release (&swap_lock);
}

void
swap_free (struct swap_slot *ss)
{
  lock_acquire (&swap_lock);
  bitmap_set_multiple (swap_bitmap, ss->sector, BLOCKS_IN_PAGE, false);
  lock_release (&swap_lock);
}
