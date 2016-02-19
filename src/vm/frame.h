#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "vm/page.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include <hash.h>
#include <list.h>

struct frame_entry
  {
    struct list_elem frame_elem;      /* List element for frame page. */
    void *addr;                       /* Frame's address. */
    uint32_t offset;                  /* Offset of read_bytes. */
    uint32_t num_bytes;               /* Number of read_bytes for
                                         this frame. */
  };

/* Prototypes for frame.c functions. */
void init_frame (void);
struct frame_entry *get_frame (enum palloc_flags);
void free_frame (struct page_table_entry *);

/* List of all frames currently in use.
   Owned by frame.c, userprog/syscall.c, and userprog/process.c. */
struct list all_frames;

#endif /* vm/frame.h */
