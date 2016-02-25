#include <stdio.h>
#include <string.h>
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"     /* For file operations. */

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
  // printf ("PL: %x, HF: %x\n", p.upage, e);
  lock_release (&cur->spt_lock);
  return e != NULL ? hash_entry (e, struct page_table_entry, pt_elem)
    : NULL;
}

void
extend_stack (const void *address)
{

  struct page_table_entry *pte = page_lookup (address);
  if (pte != NULL)
    {
      /* if page_lookup returns something, fetch data... (normal case)
         Set regular page table, and return */
      page_fetch_and_set (pte);
    }
  else /* If doesn't exist, then create a new frame and pte, and return */
    {
      page_create_from_vaddr (address, true);
    }
}

void
page_create_from_vaddr (const void *address, bool pinned)
{
  struct page_table_entry *pte = malloc (sizeof (struct page_table_entry));
  if (!pte)
    PANIC ("Unable to allocate page table entry!");

  struct thread *cur = thread_current ();
  struct frame_entry *fe = get_frame (PAL_USER);
  pte->kpage = pg_round_down (fe->addr);
  pte->upage = pg_round_down ((void *) address);
  pte->phys_frame = fe;
  fe->pte = pte;
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
  ASSERT (pte->page_status != PAGE_NONZEROS);

  struct thread *cur = thread_current ();
  bool success = false;
  if (status == PAGE_ZEROS)
    {
      if (pte->phys_frame != NULL)
        {
          success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
        }
      else
        {
          struct frame_entry *fe = get_frame (PAL_USER);
          pte->kpage = fe->addr;
          pte->phys_frame = fe;
          fe->pte = pte;
          memset (fe->addr, 0, PGSIZE);
          lock_acquire (&cur->spt_lock);
          hash_insert (&cur->supp_page_table, &pte->pt_elem);
          lock_release (&cur->spt_lock);

          success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
        }
    }
  else if (status == PAGE_SWAP)
    {
      struct frame_entry *fe = get_frame (PAL_USER);
      // printf("fetching swap data to %x\n", fe->addr);
      lock_acquire (&cur->spt_lock);
      pte->kpage = fe->addr;
      pte->phys_frame = fe;
      fe->pte = pte;
      pte->page_status = PAGE_NONZEROS;
      lock_release (&cur->spt_lock);
      // printf("fetching swap data1\n");
      swap_read (pte->ss, fe);
// printf("fetching swap data2\n");
      free(pte->ss);
      pte->ss = NULL;
      // printf("setting pagedir upage=%x kpage=%x\n", pte->upage, pte->kpage);
      success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
    }
  else if (status == PAGE_MMAP || status == PAGE_CODE)
    {
    //printf("fetching mmap data\n");
      struct frame_entry *fe = get_frame (PAL_USER);
      lock_acquire (&cur->spt_lock);
      pte->kpage = fe->addr;
      pte->phys_frame = fe;
      fe->pte = pte;
      lock_release (&cur->spt_lock);

      lock_acquire (&file_lock);
      //printf("after2\n");
      int rbytes = file_read_at (pte->file, pte->kpage,
        (PGSIZE - pte->num_zeros), (off_t) pte->offset);
      lock_release (&file_lock);
      if (rbytes != PGSIZE - pte->num_zeros)
        success = false;
      else
        {
          /* Set the rest of the page to be zeros. */
      // printf("before\n");
          memset (pte->kpage + (PGSIZE - pte->num_zeros), 0, pte->num_zeros);
          success = pagedir_set_page (cur->pagedir, pte->upage,
                pte->kpage, !pte->page_read_only);
      // printf("after\n");
        }
    }
  if (!success)
    {
      thread_current ()->return_status = -1;
      process_exit ();
    }
}

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
