#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <hash.h>
#include <list.h>
#include "threads/palloc.h"
#include "threads/thread.h"

/* Prototypes for frame.c functions. */
void init_frame (void);

struct frame_entry
  {
    struct list_elem frame_elem;      /* List element for frame page. */
    int frame_status;                 /* Frame's status. */
    void *addr;                       /* Frame's address. */
  };

void *get_frame (enum palloc_flags);
void free_frame (void *);

/* List of all frames currently in use.
   Owned by frame.c, userprog/syscall.c, and userprog/process.c. */
struct list all_frames;

#endif /* vm/frame.h */
