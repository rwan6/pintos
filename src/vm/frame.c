#include "vm/frame.h"
#include "threads/palloc.h"
#include "userprog/syscall.h"

void *
get_frame (enum palloc_flags flags)
{
  void *frame = palloc_get_page (flags);
  if (!frame)
    ASSERT (!frame);
  return frame;
}

void
free_frame (void *kpage)
{
	palloc_free_page (kpage);
}
