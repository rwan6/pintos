#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>

/* -------- Enumeration of Page Status -------- */
/* 0 -- Zero-filled page (in Frame Table).      */
/* 1 -- Non-zero-filled page (in Frame Table).  */
/* 2 -- Code page.                              */
/* 2 -- In swap slot.                           */
/* 3 -- Memory Mapped (in disk).                */
/* -------------------------------------------- */
enum page_status
  {
    PAGE_ZEROS,
    PAGE_NONZEROS,
    PAGE_CODE,
    PAGE_SWAP,
    PAGE_MMAP
  };

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
    enum page_status page_status;     /* Gives the status of this frame
                                         entry.  See above for
                                         enumeration. */
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
