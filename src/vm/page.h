#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "vm/frame.h"

/* Prototypes for page.c functions. */
void init_supp_page_table (struct hash *page_table);
void init_page_entry (struct frame_entry *);

struct page_table_entry
  {
    struct hash_elem pt_elem;         /* Hash map page table element. */
    void *vaddr;                      /* Virtual address of page table
                                         entry. */
    struct frame_entry *phys_frame;   /* Pointer to the frame entry
                                         corresponding to this page. */
  };

#endif /* vm/page.h */
