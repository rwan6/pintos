#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "vm/frame.h"
#include "devices/block.h"

struct swap_slot
{
  struct page_table_entry *pte;
  block_sector_t sector;
};

struct block *swap_partition;
struct bitmap *swap_bitmap;

void init_swap_partition (void);
void swap_read (struct swap_slot *, struct frame_entry *);
void swap_write (struct swap_slot *, struct frame_entry *);
void swap_allocate (struct swap_slot *);
void swap_free (struct swap_slot *);

#endif /* vm/swap.h */
