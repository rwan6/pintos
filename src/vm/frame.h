#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <hash.h>
#include <list.h>
#include "vm/page.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* Entry into the frame table.  Holds metadata related to the frame
   table entry's address, the corresponding virtual page, and the
   thread that owns the frame. */
struct frame_entry
  {
    struct list_elem frame_elem;      /* List element for frame page. */
    void *addr;                       /* Frame's address. */
    struct page_table_entry *pte;     /* Pointer to page table entry. */
    struct thread *t;                 /* Pointer to the owner thread. */
  };

/* Prototypes for frame.c functions. */
void init_frame (void);
struct frame_entry *get_frame (enum palloc_flags);
void free_frame (struct page_table_entry *);

struct list all_frames;         /* List of all frames currently in use. */
struct list_elem *clock_handle; /* The clock handle for clock algorithm.
                                   Points to frame entry. */
struct lock frame_table_lock;   /* Global lock for frame table. */

#endif /* vm/frame.h */
