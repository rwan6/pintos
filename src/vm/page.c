#include "vm/page.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"

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
   A.8.5 in the Pintos documentation. */
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
extend_stack(const void *address)
{
  struct page_table_entry *pte = page_lookup (address);
  if (pte != NULL)//page_lookup, if exists, fetch data.. (normal case)
    {
      //set regular page table, and return
      page_fetch_and_set (pte);
    }
  else //if doesn't exist, then create a new frame and pte, and return
    {
      pte = malloc (sizeof (struct page_table_entry));
      struct frame_entry *fe = get_frame (PAL_USER);
      pte->kpage = fe->addr;
      pte->phys_frame = fe;
      memset (fe->addr, 0, PGSIZE);
      bool success = pagedir_set_page (thread_current ()->pagedir, pte->upage,
        pte->kpage, !pte->page_read_only);
      if (!success)
        PANIC ("Set page failed.");
    }
}

void
page_fetch_and_set (struct page_table_entry *pte)
{
  int status = pte->page_status;
  struct thread *cur = thread_current ();

  bool success;
  if (status == 0)
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
          success = pagedir_set_page (cur->pagedir, pte->upage,
            pte->kpage, !pte->page_read_only);
        }
    }
  else if (status == 1)
    {
      // TODO handle swap slot case
      success = true;
    }
  else if (status == 2)
    {
      // TODO mmap file case
      success = true;
    }
  if (!success)
    PANIC ("Set page failed in page table");
}
