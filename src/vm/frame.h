#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <hash.h>
#include <list.h>
#include "vm/page.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

struct frame_entry
  {
    struct list_elem frame_elem;      /* List element for frame page. */
    void *addr;                       /* Frame's address. */
    struct page_table_entry *pte;     /* Pointer to page table entry. */
    struct thread *t;                 /* Pointer to thread. */
  };

/* Prototypes for frame.c functions. */
void init_frame (void);
struct frame_entry *get_frame (enum palloc_flags);
void free_frame (struct page_table_entry *);
struct frame_entry *evict_frame (void);

/* List of all frames currently in use.
   Owned by frame.c, userprog/syscall.c, and userprog/process.c. */
struct list all_frames;

/* The clock handle for clock algorithm. Points to frame entry. */
struct list_elem *clock_handle;

/* Global lock for frame table. */
struct lock frame_table_lock;
int cnt;

#endif /* vm/frame.h */
