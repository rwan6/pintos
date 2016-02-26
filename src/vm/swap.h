#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"

/* Swap slot entry.  Contains the page being written to the swap slot
   and the beginning of the sector at which the page will live in the
   swap slot. */
struct swap_slot
{
  struct page_table_entry *pte;
  block_sector_t sector;
};

struct block *swap_partition;   /* Memory block for the swap partition. */
struct bitmap *swap_bitmap;     /* Swap slot bitmap to track free memory. */
struct lock swap_lock;          /* Global swap slot lock. */

/* Prototypes for swap.c functions. */
void init_swap_partition (void);
void swap_read (struct swap_slot *, struct frame_entry *);
void swap_write (struct swap_slot *, struct frame_entry *);
void swap_allocate (struct swap_slot *);
void swap_free (struct swap_slot *);

#endif /* vm/swap.h */
