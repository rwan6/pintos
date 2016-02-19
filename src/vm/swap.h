#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"
#include "vm/swap.h"

struct swap_slot
{
  struct page_table_entry *pte;
  block_sector_t sector;
  struct listelem *swap_elem;
};

struct bitmap *bitmap;

#endif /* vm/swap.h */
