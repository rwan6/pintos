#include <stdio.h>
#include <string.h>
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"

bool create_zero_page (struct page_table_entry *, struct thread *);
bool fetch_from_swap (struct page_table_entry *, struct thread *);
bool fetch_from_file (struct page_table_entry *, struct thread *);

/* hash_less_func for sorting the supplemental page table according
   to the page's virtual addresses. */
static bool
page_table_less (const struct hash_elem *a,
                 const struct hash_elem *b,
                 void *aux UNUSED)
{
  const struct page_table_entry *pt_a = hash_entry (a,
    struct page_table_entry, pt_elem);
  const struct page_table_entry *pt_b = hash_entry (b,
    struct page_table_entry, pt_elem);

  return pt_a->upage < pt_b->upage;
}

/* hash_hash_func for obtaining a hash value for a given page. */
static unsigned
page_hash (const struct hash_elem *page, void *aux UNUSED)
{
  const struct page_table_entry *pt_entry = hash_entry (page,
    struct page_table_entry, pt_elem);
  return hash_bytes (&pt_entry->upage, sizeof pt_entry->upage);
}

/* Initialize the supplemental page table and all related data
   structures. */
void
init_supp_page_table (struct hash *page_table)
{
  hash_init (page_table, page_hash, page_table_less, NULL);
}

/* Set up a page table entry each time a new frame is allocated. */
struct page_table_entry *
init_page_entry (void)
{
  struct page_table_entry *pte = malloc (sizeof
    (struct page_table_entry));
  if (!pte)
    PANIC ("Unable to allocate page table entry!");
  return pte;
}

/* Returns the page containing the given virtual address,
   or a null pointer if no such page exists.  Derived from
   section A.8.5 in the Pintos documentation. */
struct page_table_entry *
page_lookup (const void *address)
{
  struct page_table_entry p;
  struct hash_elem *e;
  struct thread *cur = thread_current ();

  lock_acquire (&cur->spt_lock);
  p.upage = pg_round_down ((void *) address);
  e = hash_find (&cur->supp_page_table, &p.pt_elem);
  lock_release (&cur->spt_lock);
  return e != NULL ? hash_entry (e, struct page_table_entry, pt_elem)
    : NULL;
}

/* Extends the stack. Stack growth can occur either due to the page
   fault handler or a read/write system call buffer handler that
   determined valid stack growth was necessary. */
void
extend_stack (const void *address)
{
  struct page_table_entry *pte = page_lookup (address);

  /* If page_lookup returns a valid entry, fetch the data and set up
     the frame entry.  If the page does not exists, create a new frame
     and page table entry. */
  if (pte != NULL)
    page_fetch_and_set (pte);
  else
    page_create_from_vaddr (address, true);
}

/* Create and set up a zero-filled page, for extending stack.  The page
   immediately gets pinned to the frame table. */
void
page_create_from_vaddr (const void *address, bool pinned)
{
  struct page_table_entry *pte = malloc (sizeof (struct page_table_entry));
  if (!pte)
    PANIC ("Unable to allocate page table entry!");

  struct thread *cur = thread_current ();
  struct frame_entry *fe = get_frame (PAL_USER);
  fe->pte = pte;

  pte->kpage = pg_round_down (fe->addr);
  pte->upage = pg_round_down ((void *) address);
  pte->phys_frame = fe;
  pte->page_read_only = false;
  pte->page_status = PAGE_ZEROS;
  pte->num_zeros = PGSIZE;
  pte->offset = 0; /* Not used for non-file related pages. */
  pte->file = NULL; /* Not used for non-file related pages. */
  pte->page_read_only = false;
  pte->pinned = pinned;
  memset (fe->addr, 0, PGSIZE);
  lock_acquire (&cur->spt_lock);
  hash_insert (&cur->supp_page_table, &pte->pt_elem);
  lock_release (&cur->spt_lock);

  bool success = pagedir_set_page (cur->pagedir, pte->upage,
    pte->kpage, !pte->page_read_only);

  if (!success)
    {
      cur->return_status = -1;
      process_exit ();
    }
}

/* Create and set up a page for a memory-mapped file, and add it to the
   process's supplemental page table.  For memory-mapped files, we do
   not initially allocate a frame for the file. */
struct page_table_entry *
page_create_mmap (const void *address, struct file *file,
                  uint32_t offset, int num_zeros)
{
  struct page_table_entry *pte = malloc (sizeof (struct page_table_entry));
  if (!pte)
    PANIC ("Unable to allocate page table entry!");

  struct thread *cur = thread_current ();

  pte->kpage = NULL;  /* Doesn't exist in the frame table. */
  pte->upage = pg_round_down ((void *) address);
  pte->phys_frame = NULL;
  pte->page_status = PAGE_MMAP;
  pte->num_zeros = num_zeros;
  pte->offset = offset;
  pte->file = file;
  pte->page_read_only = false;
  /* Needs to be pinned when brought into the frame table for read/write */
  pte->pinned = true;
  lock_acquire (&cur->spt_lock);
  hash_insert (&cur->supp_page_table, &pte->pt_elem);
  lock_release (&cur->spt_lock);

  return pte;
}

void
page_fetch_and_set (struct page_table_entry *pte)
{
  enum page_status status = pte->page_status;

  /* Should not be attempting to fetch a page that already lives in the
     frame table. */
  ASSERT (status != PAGE_NONZEROS);

  struct thread *cur = thread_current ();
  bool success = false;
  if (status == PAGE_ZEROS)
    {
      if (pte->phys_frame != NULL)
        success = pagedir_set_page (cur->pagedir, pte->upage,
          pte->kpage, !pte->page_read_only);
      else
        success = create_zero_page (pte, cur);
    }
  else if (status == PAGE_SWAP)
    success = fetch_from_swap (pte, cur);
  else if (status == PAGE_MMAP || status == PAGE_CODE)
    success = fetch_from_file (pte, cur);

  if (!success)
    {
      thread_current ()->return_status = -1;
      process_exit ();
    }
}

/* Upon process termination, deallocate a page from a process's
   supplemental page table, remove it from the frame table (if applicable),
   and free the allocated swap metadata element.  Note that we are not
   freeing a swap slot here, only the swap slot metadata. */
void
page_deallocate (struct hash_elem *e, void *aux UNUSED)
{
  lock_acquire (&thread_current ()->spt_lock);
  struct page_table_entry *pte = hash_entry (e,
    struct page_table_entry, pt_elem);
  lock_release (&thread_current ()->spt_lock);

  /* Determine page's status and deallocate respective resources. */
  enum page_status ps = pte->page_status;
  if (ps != PAGE_SWAP)
    {
      if (pte->phys_frame != NULL)
        {
          free_frame (pte);
          pagedir_clear_page (thread_current ()->pagedir, pte->upage);
        }
    }
  else
    {
      swap_free (pte->ss);
      free (pte->ss);
    }
  if (pte != NULL)
    free (pte);
}

/* Create a all-zeroed page and link to the frame table. */
bool
create_zero_page (struct page_table_entry *pte, struct thread *cur)
{
  struct frame_entry *fe = get_frame (PAL_USER);
  lock_acquire (&cur->spt_lock);
  pte->kpage = fe->addr;
  pte->phys_frame = fe;
  fe->pte = pte;
  memset (fe->addr, 0, PGSIZE);
  hash_insert (&cur->supp_page_table, &pte->pt_elem);
  lock_release (&cur->spt_lock);

  bool success = pagedir_set_page (cur->pagedir, pte->upage,
    pte->kpage, !pte->page_read_only);
  return success;
}

/* Fetch data from the swap partition and link to the frame table. */
bool
fetch_from_swap (struct page_table_entry *pte, struct thread *cur)
{
  struct frame_entry *fe = get_frame (PAL_USER);
  lock_acquire (&cur->spt_lock);
  pte->kpage = fe->addr;
  pte->phys_frame = fe;
  fe->pte = pte;
  pte->page_status = PAGE_NONZEROS;

  swap_read (pte->ss, fe);

  free(pte->ss);
  pte->ss = NULL;
  lock_release (&cur->spt_lock);
  bool success = pagedir_set_page (cur->pagedir, pte->upage,
        pte->kpage, !pte->page_read_only);
  return success;
}

/* Fetch data from the file and link to the frame table. */
bool
fetch_from_file (struct page_table_entry *pte, struct thread *cur)
{
  bool success = false;
  struct frame_entry *fe = get_frame (PAL_USER);
  lock_acquire (&cur->spt_lock);
  pte->kpage = fe->addr;
  pte->phys_frame = fe;
  fe->pte = pte;
  lock_release (&cur->spt_lock);

  lock_acquire (&file_lock);
  int rbytes = file_read_at (pte->file, pte->kpage,
    (PGSIZE - pte->num_zeros), (off_t) pte->offset);
  lock_release (&file_lock);
  if (rbytes != PGSIZE - pte->num_zeros)
    success = false;
  else
    {
      /* Set the rest of the page to be zeros. */
      memset (pte->kpage + (PGSIZE - pte->num_zeros), 0, pte->num_zeros);
      success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
    }
  return success;
}
