#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "userprog/syscall.h"

void *
get_frame (enum palloc_flags flags)
{
  void *frame = palloc_get_page (flags);
  if (!frame) /* All frames full, need to evict to swap */
    ASSERT (!frame);
  
  struct frame_entry *fe = malloc (sizeof (struct frame_entry));
  if (!fe)
    {
      palloc_free_page (frame);
      return NULL;
    }
  list_push_back (&all_frames, &fe->frame_elem);
  list_init (&fe->virt_mapped_page);
  fe->frame_status = 0; /* TODO: figure out what this is used for */
  fe->addr = frame;
  return frame;
}

void
free_frame (void *kpage)
{
	palloc_free_page (kpage);
}
