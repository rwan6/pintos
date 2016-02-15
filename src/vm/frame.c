#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/syscall.h"

/* Initialize the frame table and all related structures. */
void
init_frame (void)
{
  list_init (&all_frames);
}

void *
get_frame (enum palloc_flags flags)
{
  void *frame = palloc_get_page (flags);
  if (!frame) /* All frames full, need to evict to swap */
    PANIC ("All frames full!");
  
  struct frame_entry *fe = malloc (sizeof (struct frame_entry));
  if (!fe)
    {
      palloc_free_page (frame);
      PANIC ("Unable to allocate page table entry!");
    }
  list_push_back (&all_frames, &fe->frame_elem);
  fe->frame_status = 0; /* TODO: figure out what this is used for */
  fe->addr = frame;
  return frame;
}

void
free_frame (void *kpage)
{
	palloc_free_page (kpage);
}
