#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>

/* ------------ Page Status Enumeration ------------ */
/* 0 -- Zero-filled clean page.                      */
/* 1 -- Non-zero-filled clean page in frame table.   */
/* 2 -- Clean Code/Data page.                        */
/* 3 -- Dirty page that is either in the frame       */
/*      table and needs to be written to the         */
/*      swap partition or lives in the swap          */
/*      partition.                                   */
/* 4 -- Memory Mapped Page.                          */
/* ------------------------------------------------- */
enum page_status
  {
    PAGE_ZEROS,
    PAGE_NONZEROS,
    PAGE_CODE,
    PAGE_SWAP,
    PAGE_MMAP
  };

/* Entry into the supplemental page table.  Holds metadata about the
   page table entry and its corresponding frame entry, swap slot (if
   applicable), and/or mapped file (if applicable). */
struct page_table_entry
  {
    struct hash_elem pt_elem;         /* Hash map page table element. */
    struct list_elem mmap_elem;       /* Mmap list element. */
    void *kpage;                      /* Kernel page address of page table
                                         entry. */
    void *upage;                      /* User page address of page table
                                         entry. */
    struct frame_entry *phys_frame;   /* Pointer to the frame entry
                                         corresponding to this page. */
    enum page_status page_status;     /* Status of this frame entry. */
    bool pinned;                      /* Whether this page cannot be chosen
                                         for eviction */
    bool page_read_only;              /* Denotes whether page is
                                         read-only. */
    struct swap_slot *ss;             /* Swap slot for this page. */
    int num_zeros;                    /* Number of zeros to fill page. */
    uint32_t offset;                  /* Offset of read_bytes. */
    struct file *file;                /* File pointer for MMAP files
                                         (NULL otherwise). */
  };

/* Prototypes for page.c functions. */
void init_supp_page_table (struct hash *page_table);
struct page_table_entry *init_page_entry (void);
struct page_table_entry *page_lookup (const void *);
void extend_stack (const void *);
void page_fetch_and_set (struct page_table_entry *);
void page_create_from_vaddr (const void *, bool);
struct page_table_entry * page_create_mmap (const void *, struct file *,
                                            uint32_t, int);
void page_deallocate (struct hash_elem *, void *UNUSED);

#endif /* vm/page.h */
