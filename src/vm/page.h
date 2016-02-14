#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "vm/frame.h"

/* Prototypes for page.c functions. */
void init_supp_page_table (void);

struct page_table_entry
  {
    struct hash_elem pt_elem;         /* Hash map page table element. */
    void *vaddr;                      /* Virtual address of page table
                                         entry. */
    struct frame_entry *phys_frame;   /* Pointer to the frame entry
                                         corresponding to this page. */
  };
  
/* Supplemental Page Table (Hash Map).  Used to hold pages mapping
   to physical frames.
   Owned by page.c and userprog/exception.c. */
struct hash supp_page_table;

#endif /* vm/page.h */
