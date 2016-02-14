#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"
#include "threads/thread.h"

struct frame_entry
  {
    struct list_elem frame_elem;      /* List element for frame page. */
    struct list virt_mapped_page;     /* List of virtual pages that map
                                         to this frame entry. */
    struct hash_elem frame_hash_elem; /* Hash elment for frame page */
    int frame_status;                 /* Frame's status. */
    void *addr;                       /* Frame's address. */
  };

void *get_frame (enum palloc_flags);
void free_frame (void *);

/* List of all frames currently in use.
   Owned by frame.c, userprog/syscall.c, and userprog/process.c. */
struct list all_frames;

#endif /* vm/frame.h */
