#include <stdio.h>
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"
#include "filesys/file.h"     /* For file operations. */

/* Initialize the frame table and all related structures. */
static void
move_clock_handle (void)
{
  if (list_next (clock_handle) == list_end (&all_frames))
    clock_handle = list_begin (&all_frames);
  else
    clock_handle = list_next (clock_handle);
}

void
init_frame (void)
{
  list_init (&all_frames);
  clock_handle = NULL;
  lock_init (&frame_table_lock);
  // printf("frame table lock=%x\n", & frame_table_lock);
}

struct frame_entry *
get_frame (enum palloc_flags flags)
{
  // printf("acquireing frame lock1\n");
  lock_acquire (&frame_table_lock);
  // printf("acquireing frame lock2\n");
  void *frame = palloc_get_page (flags);

  /* If all frames are full, we need to evict. */
  if (!frame)
    return evict_frame ();

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
    // printf("frame lock released 1\n");
  lock_release (&frame_table_lock);
  return fe;
}

void
free_frame (struct page_table_entry *pte)
{
  // printf("acquireing frame lock2\n");
  lock_acquire (&frame_table_lock);
	palloc_free_page (pte->kpage);
  if (clock_handle == &pte->phys_frame->frame_elem)
    move_clock_handle ();
  list_remove (&pte->phys_frame->frame_elem);
  // printf("frame_lock=%x\n", pte->phys_frame, frame_table_lock);
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
    // printf("before %x\n", clock_handle);
      fe = list_entry (clock_handle,
        struct frame_entry, frame_elem);
      bool pinned = fe->pte->pinned;
      if (pinned)
        {
          move_clock_handle ();
          continue;
        }
        
// printf("before %x %x %x\n", fe->t, fe->pte->upage, fe->t->pagedir);
      bool accessed = pagedir_is_accessed (fe->t->pagedir,
        fe->pte->upage);

      if (accessed)
        {
          pagedir_set_accessed (fe->t->pagedir,
            fe->pte->upage, false);/*printf("after 3\n");*/
        }
      else
        {
          found = true;

          bool dirty = pagedir_is_dirty (fe->t->pagedir, fe->pte->upage);
          /* Update the pte for the evicted frame.  Account for zero
             pages that were dirtied as well. */
          if (fe->pte->page_status == PAGE_NONZEROS ||
                (dirty && (fe->pte->page_status == PAGE_ZEROS ||
                           fe->pte->page_status == PAGE_CODE)))
            {
              struct swap_slot *ss = malloc (sizeof (struct swap_slot));
      // printf("%x swapped out to swap partition\n", fe->addr);
  // lock_acquire (&file_lock);
              swap_write (ss, fe);
  // lock_release (&file_lock);
      // printf("after accessed3\n");
              lock_acquire (&fe->t->spt_lock);
              fe->pte->ss = ss;
              fe->pte->page_status = PAGE_SWAP;
              fe->pte->kpage = NULL;
              lock_release (&fe->t->spt_lock);
            }
          else if (fe->pte->page_status == PAGE_MMAP && dirty)
            {
              lock_acquire (&file_lock);
              file_write_at (fe->pte->file, fe->addr,
                             PGSIZE, (off_t) fe->pte->offset);
              lock_release (&file_lock);
            }
// printf("HERE\n");
          /* Unlink this pte and deactivate the page table.  This will
             cause a page fault when the page is next accessed. */
  // printf("evict_frame: evicted %x, now ps=%d\n", fe->pte->upage, fe->pte->page_status);
          pagedir_clear_page (fe->t->pagedir, fe->pte->upage);
          fe->pte->phys_frame = NULL;
          fe->pte = NULL;
          fe->t = thread_current ();
        }
        /* Increment the clock_handle. */
        // printf("HERE 1\n");
        move_clock_handle ();
        // printf("HERE 2\n");
    }
// printf("after 6\n");
    lock_release (&frame_table_lock);
    // printf("frame lock released 1\n");
    return fe;
}

