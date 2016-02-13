#include "vm/frame.h"
#include "threads/palloc.h"
#include "userprog/syscall.h"

void *
get_frame (enum palloc_flags flags)
{
  void *frame = palloc_get_page (flags);
  if (!frame)
    {
      exit (-1);
    }
  return frame;
}
