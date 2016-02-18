#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "vm/frame.h"

/* ------ Enumeration of Page Status ------ */
/* 0 -- Zero-filled page (in Frame Table).  */
/* 1 -- In swap slot.                       */
/* 2 -- Memory Mapped (in disk).            */
/* ---------------------------------------- */

struct page_table_entry
  {
    struct hash_elem pt_elem;         /* Hash map page table element. */
    void *vaddr;                      /* Virtual address of page table
                                         entry. */
    struct frame_entry *phys_frame;   /* Pointer to the frame entry
                                         corresponding to this page. */
    int page_status;                  /* Gives the status of this frame
                                         entry.  See above for
                                         enumeration. */
    bool page_read_only;              /* Denotes whether page is
                                         read-only. */ 
  };

/* Prototypes for page.c functions. */
void init_supp_page_table (struct hash *page_table);
struct page_table_entry *init_page_entry (void);
struct page_table_entry *page_lookup (const void *);

#endif /* vm/page.h */
