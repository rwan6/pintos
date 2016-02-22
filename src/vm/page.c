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
  lock_release (&cur->spt_lock);
  return e != NULL ? hash_entry (e, struct page_table_entry, pt_elem)
    : NULL;
}

void
extend_stack (const void *address)
{
  struct page_table_entry *pte = page_lookup (address);
  if (pte != NULL)//page_lookup, if exists, fetch data.. (normal case)
    {
      //set regular page table, and return
      page_fetch_and_set (pte);
    }
  else //if doesn't exist, then create a new frame and pte, and return
    {
      page_create_from_vaddr (address);
    }
}

void
page_create_from_vaddr (const void *address)
{
  struct page_table_entry *pte = malloc (sizeof (struct page_table_entry));
  if (!pte)
    PANIC ("Unable to allocate page table entry!");

  struct thread *cur = thread_current ();
  struct frame_entry *fe = get_frame (PAL_USER);
  pte->kpage = pg_round_down (fe->addr);
  pte->upage = pg_round_down ((void *) address);
  pte->phys_frame = fe;
  pte->page_status = PAGE_ZEROS;
  pte->num_zeros = PGSIZE;
  pte->offset = 0; /* Not used for non-file related pages. */
  pte->file = NULL; /* Not used for non-file related pages. */
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
  lock_acquire (&cur->spt_lock);
  hash_insert (&cur->supp_page_table, &pte->pt_elem);
  lock_release (&cur->spt_lock);
  return pte;
}

void
page_fetch_and_set (struct page_table_entry *pte)
{
  enum page_status status = pte->page_status;
  
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
          memset (fe->addr, 0, PGSIZE);
          lock_acquire (&cur->spt_lock);
          hash_insert (&cur->supp_page_table, &pte->pt_elem);
          lock_release (&cur->spt_lock);

          success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
        }
    }
  else if (status == PAGE_SWAP)
    {//printf("2 %x\n", thread_current ());
      struct frame_entry *fe = get_frame (PAL_USER);

      lock_acquire (&cur->spt_lock);
      pte->kpage = fe->addr;
      pte->phys_frame = fe;
      pte->page_status = PAGE_NONZEROS;
      lock_release (&cur->spt_lock);

      swap_read (pte->ss, fe);
      pte->ss = NULL;

      success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
    }
  else if (status == PAGE_MMAP || status == PAGE_CODE)
    { 
      struct frame_entry *fe = get_frame (PAL_USER);
      
      lock_acquire (&cur->spt_lock);
      pte->kpage = fe->addr;
      pte->phys_frame = fe;
      lock_release (&cur->spt_lock);
      
      lock_acquire (&file_lock);
      file_read_at (pte->file, pte->kpage, (PGSIZE - pte->num_zeros),
                    (off_t) pte->offset);
      lock_release (&file_lock);
          
      /* Set the rest of the page to be zeros. */
      memset (pte->kpage + (PGSIZE - pte->num_zeros), 0, pte->num_zeros);
      success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
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
  struct page_table_entry *pte = hash_entry (e,
    struct page_table_entry, pt_elem);
  /* Determine page's status and deallocate respective resources. */
  enum page_status ps = pte->page_status;
  if (ps == PAGE_ZEROS || ps == PAGE_NONZEROS)
    {
      if (pte->phys_frame != NULL)
        {
          free_frame (pte);
          pagedir_clear_page (thread_current ()->pagedir, pte->upage);
        }
    }
  else if (ps == PAGE_SWAP)
    {
      swap_free (pte->ss);
      free (pte->ss);
    }
  if (pte != NULL)
    free (pte);
}