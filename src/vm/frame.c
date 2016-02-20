#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"

/* Initialize the frame table and all related structures. */
void
init_frame (void)
{
  list_init (&all_frames);
  clock_handle = NULL;
  lock_init (&frame_table_lock);
}

struct frame_entry *
get_frame (enum palloc_flags flags)
{
  lock_acquire (&frame_table_lock);
  void *frame = palloc_get_page (flags);
  if (!frame) /* All frames full, need to evict to swap */
    {
      // PANIC ("All frames full!");
      return evict_frame ();
    }

  struct frame_entry *fe = malloc (sizeof (struct frame_entry));
  if (!fe)
    {
      palloc_free_page (frame);
      lock_release (&frame_table_lock);
      PANIC ("Unable to allocate page table entry!");
    }
  list_push_back (&all_frames, &fe->frame_elem);
  if (clock_handle == NULL)
    clock_handle = list_begin (&all_frames);
  fe->addr = pg_round_down (frame);
  fe->t = thread_current ();
  lock_release (&frame_table_lock);
  return fe;
}

void
free_frame (struct page_table_entry *pte)
{
  lock_acquire (&frame_table_lock);
	palloc_free_page (pte->kpage);
  free (pte->phys_frame);
  lock_release (&frame_table_lock);
}

struct frame_entry *
evict_frame (void)
{
  bool found = false;
  struct frame_entry *fe = NULL;
  while (!found)
    {
      fe = list_entry (clock_handle,
        struct frame_entry, frame_elem);
      bool accessed = pagedir_is_accessed (fe->t->pagedir,
        fe->pte->upage);
      if (accessed)
        {
          pagedir_set_accessed (fe->t->pagedir,
            fe->pte->upage, false);
        }
      else
        {
          found = true;

          //do eviction
          if (pagedir_is_dirty (fe->t->pagedir, fe->pte->upage))
            {
              // update the pte for the evicted frame.
              // TODO: mmap case; now it only takes care of ss cases
              struct swap_slot *ss = malloc (sizeof (struct swap_slot));
              swap_write (ss, fe);
              lock_acquire (&fe->t->spt_lock);
              fe->pte->ss = ss;
              fe->pte->page_status = PAGE_SWAP;
              lock_release (&fe->t->spt_lock);

              //unlink this pte
              fe->pte = NULL;
              fe->t = thread_current ();
            }
          else
            {
              fe->pte = NULL;
              fe->t = thread_current ();
            }
        }
        // increment the clock_handle
        if (clock_handle == list_end (&all_frames))
          clock_handle = list_begin (&all_frames);
        else
          clock_handle = list_next (clock_handle);
    }
    lock_release (&frame_table_lock);
    return fe;
}

