#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"

struct frame_entry
  {

  };

void *get_frame (enum palloc_flags);
void free_frame (void *);

#endif /* vm/frame.h */
