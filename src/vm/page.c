#include "vm/page.h"
#include "threads/malloc.h"

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
  
  return pt_a->vaddr < pt_b->vaddr;
}

/* hash_hash_func for obtaining a hash value for a given page. */
static unsigned
page_hash (const struct hash_elem *page, void *aux UNUSED)
{
  const struct page_table_entry *pt_entry = hash_entry (page,
    struct page_table_entry, pt_elem);
  return hash_bytes (&pt_entry->vaddr, sizeof pt_entry->vaddr);
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

  p.vaddr = (void *) address;
  e = hash_find (&cur->supp_page_table, &p.pt_elem);
  return e != NULL ? hash_entry (e, struct page_table_entry, pt_elem) 
    : NULL;
}


