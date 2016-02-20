#include <string.h>
#include "vm/page.h"
#include "vm/frame.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"

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

  p.upage = (void *) address;
  e = hash_find (&cur->supp_page_table, &p.pt_elem);
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

  struct frame_entry *fe = get_frame (PAL_USER);
  pte->kpage = fe->addr;
  pte->upage = (void *) address;
  pte->phys_frame = fe;
  memset (fe->addr, 0, PGSIZE);
  hash_insert (&thread_current ()->supp_page_table, &pte->pt_elem);

  bool success = pagedir_set_page (thread_current ()->pagedir, pte->upage,
    pte->kpage, !pte->page_read_only);
  if (!success)
    {
      thread_current ()->return_status = -1;
      process_exit ();
    }
}

void
page_fetch_and_set (struct page_table_entry *pte)
{
  enum page_status status = pte->page_status;
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
          hash_insert (&thread_current ()->supp_page_table, &pte->pt_elem);

          success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
        }
    }
  else if (status == PAGE_SWAP)
    {
      // TODO handle swap slot case
      success = true;
    }
  else if (status == PAGE_MMAP)
    {
      // TODO mmap file case
      success = true;
    }
  if (!success)
    {
      thread_current ()->return_status = -1;
      process_exit ();
    }
}

