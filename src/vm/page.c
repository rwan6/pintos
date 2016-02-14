#include "vm/page.h"

/* hash_less_func for sorting the supplemental page table according
   to the page's virtual addresses. */
bool
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
unsigned
page_hash (const struct hash_elem *page, void *aux UNUSED)
{
  const struct page_table_entry *pt_entry = hash_entry (page,
    struct page_table_entry, pt_elem);
  return hash_bytes (&pt_entry->vaddr, sizeof pt_entry->vaddr);
}
                             

/* Initialize the supplemental page table and all related data
   structures. */
void
init_supp_page_table (void)
{
  hash_init (&supp_page_table, page_hash, page_table_less, NULL);
}