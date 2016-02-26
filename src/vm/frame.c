#include <stdio.h>
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"
#include "filesys/file.h"

struct frame_entry *evict_frame (void);
static void evict_to_swap (struct frame_entry *);
static void evict_to_file (struct frame_entry *, bool);
static void unlink_page_table_entry (struct frame_entry *);

/* Move the clock handle to the next frame entry.  If the clock
   handle is already pointing to the last entry, reset it to the
   front of the frame table. */
static void
move_clock_handle (void)
{
  if (list_next (clock_handle) == list_end (&all_frames))
    clock_handle = list_begin (&all_frames);
  else
    clock_handle = list_next (clock_handle);
}

/* Initialize the frame table and all related structures. */
void
init_frame (void)
{
  list_init (&all_frames);
  clock_handle = NULL;
  lock_init (&frame_table_lock);
}

/* Allocate a physical frame and update the frame table.
   If frame table is full, then evict a frame. */
struct frame_entry *
get_frame (enum palloc_flags flags)
{
  lock_acquire (&frame_table_lock);
  void *frame = palloc_get_page (flags);

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

  /* If this is the first frame table entry, set up the
     clock handle to point to the first entry. */
  if (clock_handle == NULL)
    clock_handle = list_begin (&all_frames);

  fe->addr = pg_round_down (frame);
  fe->t = thread_current ();
  lock_release (&frame_table_lock);
  return fe;
}

/* Free a frame from the physical memory. Deallocate corresponding
   frame entry from the bookkeeping frame table. */
void
free_frame (struct page_table_entry *pte)
{
  lock_acquire (&frame_table_lock);
  palloc_free_page (pte->kpage);
  list_remove (&pte->phys_frame->frame_elem);
  free (pte->phys_frame);
  lock_release (&frame_table_lock);
}

/* Evict a frame from physical memory. The evicted frame will be
   stored to a swap slot or a file in disk if it is dirty. Otherwise,
   evicted frame will be simply dropped, since it is recoverable. */
struct frame_entry *
evict_frame (void)
{
  bool found = false;
  struct frame_entry *fe = NULL;
  while (!found)
    {
      fe = list_entry (clock_handle, struct frame_entry, frame_elem);

      /* Skip over if the frame is pinned. */
      if (fe->pte->pinned)
        {
          move_clock_handle ();
          continue;
        }

      bool accessed = pagedir_is_accessed (fe->t->pagedir, fe->pte->upage);
      if (accessed)
        pagedir_set_accessed (fe->t->pagedir, fe->pte->upage, false);
      else
        {
          found = true;
          lock_acquire (&fe->t->spt_lock);
          pagedir_clear_page (fe->t->pagedir, fe->pte->upage);
          bool dirty = pagedir_is_dirty (fe->t->pagedir, fe->pte->upage);
          enum page_status status = fe->pte->page_status;

          /* Update the pte for the evicted frame.  Account for zero
             pages that were dirtied as well. */
          if (fe->pte->page_status == PAGE_NONZEROS ||
                (dirty && (status == PAGE_ZEROS || status == PAGE_CODE)))
              evict_to_swap (fe);
          else if (status == PAGE_MMAP)
              evict_to_file (fe, dirty);

          unlink_page_table_entry (fe);
          lock_release (&fe->t->spt_lock);
          fe->t = thread_current ();
        }

        /* Increment the clock_handle. */
        move_clock_handle ();
    }

    lock_release (&frame_table_lock);
    return fe;
}

/* Evict a frame to the swap partition. */
static void
evict_to_swap (struct frame_entry *fe)
{
  struct swap_slot *ss = malloc (sizeof (struct swap_slot));
  if (!ss)
    PANIC ("Unable to allocate swap slot entry!");

  swap_write (ss, fe);

  fe->pte->ss = ss;
  fe->pte->page_status = PAGE_SWAP;
  fe->pte->kpage = NULL;
}

/* Evict a frame to the file system.  First, we lock down the
   file system and proceed to check whether we need to write
   to the disk.  This prevents a process from evicting a file
   that is currently being read in via page_fetch_and_set. */
static void
evict_to_file (struct frame_entry *fe, bool dirty)
{
  /* Release the frame table lock while writing to the file and reacquire
     after complete. */
  lock_release (&frame_table_lock);
  lock_acquire (&file_lock);
  if (dirty)
    file_write_at (fe->pte->file, fe->addr,
                   PGSIZE, (off_t) fe->pte->offset);
  lock_release (&file_lock);
  lock_acquire (&frame_table_lock);
}

/* Unlink a frame entry's supplemental page table entry. */
static void
unlink_page_table_entry (struct frame_entry *fe)
{
  /* Unlink this pte and deactivate the page table.  This will cause a
     page fault when the page is next accessed. */
  fe->pte->phys_frame = NULL;
  fe->pte = NULL;
}
