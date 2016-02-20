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
}

void
swap_read (struct swap_slot* ss, struct frame_entry *fe)
{
  int i = 0;
  for(; i < 8; i++)
    block_read (swap_partition, ss->sector + i,
      fe->addr + i * BLOCK_SECTOR_SIZE);
  swap_free (ss);
}

void
swap_write (struct swap_slot* ss, struct frame_entry *fe)
{
  swap_allocate (ss);
  int i = 0;
  for(; i < 8; i++)
    block_write (swap_partition, ss->sector + i,
      fe->addr + i * BLOCK_SECTOR_SIZE);
}

void
swap_allocate (struct swap_slot *ss)
{
  ASSERT (!bitmap_all (swap_bitmap, 0, block_size (swap_partition)));
  ss->sector = bitmap_scan_and_flip (swap_bitmap, 0, BLOCKS_IN_PAGE, false);
}

void
swap_free (struct swap_slot *ss)
{
  bitmap_set_multiple (swap_bitmap, ss->sector, BLOCKS_IN_PAGE, false);
}
