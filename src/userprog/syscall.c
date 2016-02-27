#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include <hash.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"    /* For synching in exec and with files */
#include "threads/vaddr.h"    /* For validating the user address. */
#include "devices/shutdown.h" /* For shutdown_power_off. */
#include "devices/input.h"    /* For input_putc(). */
#include "filesys/file.h"     /* For file operations. */
#include "filesys/filesys.h"  /* For filesys operations. */
#include "vm/frame.h"         /* For frame page. */
#include "vm/page.h"          /* For page tables. */
#include "vm/swap.h"          /* For swap slots. */

/* Prototypes for system call functions and helper functions. */
static void syscall_handler (struct intr_frame *);
static void halt (void);
static void exit (int);
static pid_t exec (const char *);
static int wait (pid_t);
static bool create (const char *, unsigned);
static bool remove (const char *);
static int open (const char *);
static int filesize (int);
static int read (int, void *, unsigned);
static int write (int, const void *, unsigned);
static void seek (int, unsigned);
static unsigned tell (int);
static void close (int);
static mapid_t mmap (int, void *);
static void munmap (mapid_t);
static bool check_pointer (const void *, unsigned);
static struct sys_fd* get_fd_item (int);
static void prefetch_user_memory (void *, size_t);
static void unpin_user_memory (void *, size_t);

static int next_avail_fd;       /* Tracks the next available fd. */
static mapid_t next_avail_mapid; /* Tracks the next available mapid */

/* Initialize the system call interrupt, as well as the next available
   file descriptor and the file lists. */
void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  list_init (&opened_files);
  list_init (&used_fds);

  next_avail_fd = 2; /* 0 and 1 are reserved. */
  next_avail_mapid = 0;

  /* Initialize frame table items. */
  init_frame ();
}

/* Takes the interrupt frame as an argument and traces the stack
   to determine which system call function needs to be invoked.
   Also performs stack pointer, argument, and buffer/filename
   address checks. */
static void
syscall_handler (struct intr_frame *f)
{
  thread_current ()->esp = f->esp;
  /* If esp is a bad address, kill the process immediately. */
  if (!check_pointer ((const void *) (f->esp), 1))
    exit (-1);

  int *sp = (int *) (f->esp);
  /* Check if the argument pointers are valid. */
  if (!check_pointer (sp, 1) ||
      !check_pointer ((sp + 1), 1) ||
      !check_pointer ((sp + 2), 1) ||
      !check_pointer ((sp + 3), 1))
    exit (-1);

  int syscall_num = *sp;
  int arg1 = *(sp + 1);
  int arg2 = *(sp + 2);
  int arg3 = *(sp + 3);

  /* Check that arg1 (file name for exec, create, remove, and open)
     are valid separate from the above check.  In the second check,
     arg2 is evaluated for the read and write functions. */
  if (syscall_num == SYS_EXEC || syscall_num == SYS_CREATE ||
      syscall_num == SYS_REMOVE || syscall_num == SYS_OPEN)
    {
      if (!check_pointer ((const void *) arg1, 1))
        exit (-1);
    }
  else if (syscall_num == SYS_READ || syscall_num == SYS_WRITE)
    {
      if (!check_pointer ((const void *) arg2, 1))
        exit (-1);
    }

  switch (syscall_num)
    {
      case SYS_HALT :
        halt();
        break;
      case SYS_EXIT :
        exit (arg1);
        break;
      case SYS_EXEC :
        f->eax = exec ((const char *) arg1);
        break;
      case SYS_WAIT :
        f->eax = wait (arg1);
        break;
      case SYS_CREATE :
        f->eax = create ((char *) arg1, arg2);
        break;
      case SYS_REMOVE :
        f->eax = remove ((char *) arg1);
        break;
      case SYS_OPEN :
        f->eax = open ((char *) arg1);
        break;
      case SYS_FILESIZE :
        f->eax = filesize (arg1);
        break;
      case SYS_READ :
        f->eax = read (arg1, (void *) arg2, arg3);
        break;
      case SYS_WRITE :
        f->eax = write (arg1, (void *) arg2, arg3);
        break;
      case SYS_SEEK :
        seek (arg1, arg2);
        break;
      case SYS_TELL :
        f->eax = tell (arg1);
        break;
      case SYS_CLOSE :
        close (arg1);
        break;
      case SYS_MMAP :
        f->eax = mmap (arg1, (void *) arg2);
        break;
      case SYS_MUNMAP :
        munmap (arg1);
        break;
      default :
        exit (-1);
        break;
    }
}

/* Terminates Pintos. */
static void
halt (void)
{
  shutdown_power_off();
}

/* Terminates the current user program.
   Status = 0 indicates success.
   Status != 0 indicates errors. */
static void
exit (int status)
{
  struct thread *t = thread_current ();

  /* If my parent is still alive, update my status so it can
     be checked after I am terminated. */
  if (t->parent != NULL)
    t->my_process->status = status;

  t->return_status = status;
  thread_exit ();
}

/* Runs the executable.  Returns the new process's program id.
   Returns pid = -1 if the child process failed to load. */
static pid_t
exec (const char *cmd_line)
{
  tid_t new_process_pid;
  if (!check_pointer ((const void *) cmd_line, MAX_FNAME_LENGTH))
    exit (-1);
  else
    {
      new_process_pid = process_execute (cmd_line);
      return new_process_pid;
    }
}

/* Waits for a child process pid and retrieves the child's exit
   status. */
static int
wait (pid_t pid)
{
  return process_wait ((tid_t) pid);
}

/* Creates a new file.  Returns true if successful, false otherwise.
   File system gets locked down with a coarse-grain lock. */
static bool
create (const char *file, unsigned initial_size)
{
  bool success;
  lock_acquire (&file_lock);
  success = filesys_create (file, initial_size);
  lock_release (&file_lock);
  return success;
}

/* Deletes the file.  Returns true if successful, false otherwise.
   File can be removed while it is open or closed.  File system
   gets locked down with a coarse-grain lock. */
static bool
remove (const char *file)
{
  bool success;
  lock_acquire (&file_lock);
  success = filesys_remove (file);
  lock_release (&file_lock);
  return success;
}

/* Opens the file and returns a file descriptor.  If open fails,
   -1 is returned.  File system gets locked down with a coarse-
   grain lock. */
static int
open (const char *file)
{
  bool found = false;
  struct list_elem *e;
  struct sys_file* sf = NULL;

  /* Figure out if we have opened this file before. */
  lock_acquire (&file_lock);
  for (e = list_begin (&opened_files);
       e != list_end (&opened_files);
       e = list_next(e))
    {
      sf = list_entry (e, struct sys_file, sys_file_elem);
      if (!sf->name && !strcmp(file, sf->name))
        {
          found = true;
          break;
        }
    }

  struct file *f = filesys_open (file);

  if (!f)
    {
      lock_release (&file_lock);
      return -1;
    }

  struct sys_fd *fd = malloc (sizeof (struct sys_fd));
  if (!fd)
    {
      lock_release (&file_lock);
      exit (-1);
    }
  fd->value = next_avail_fd++;
  fd->file = f;
  fd->owner_tid = thread_current ()->tid;

  /* If we have not opened it before, create a new entry. */
  if (!found)
    {
      sf = malloc (sizeof (struct sys_file));
      if (!sf)
        {
          free (fd);
          lock_release (&file_lock);
          exit (-1);
        }
      list_init (&sf->fd_list);
      strlcpy (sf->name, file, strlen (file) + 1);
    }

  /* Now that sf points to something useful, add it to the opened_files
     list, add the fd to sf's list and update fd's sys_file pointer. */
  list_push_back (&opened_files, &sf->sys_file_elem);
  list_push_back (&sf->fd_list, &fd->sys_fd_elem);
  fd->sys_file = sf;

  /* Add to used_fds list. */
  list_push_back (&used_fds, &fd->used_fds_elem);

  /* Add the fd to the thread's opened_fds list. */
  struct thread *t = thread_current ();
  list_push_back (&t->opened_fds, &fd->thread_opened_elem);

  lock_release (&file_lock);

  return fd->value;
}

/* Returns the file size (in bytes) of the file open.  File
   system gets locked down with a coarse-grain lock. */
static int
filesize (int fd)
{
  int file_size;
  lock_acquire (&file_lock);
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    {
      lock_release (&file_lock);
      exit (-1);
    }

  file_size = file_length (fd_instance->file);
  lock_release (&file_lock);
  return file_size;
}

/* Returns the number of bytes actually read, or -1 if the file could
   not be read.  If fd = 0, function reads from the keyboard.
   File system gets locked down with a coarse-grain lock. */
static int
read (int fd, void *buffer, unsigned size)
{
  int num_read;
  /* If SDIN_FILENO. */
  if (fd == 0)
    {
      /* Read from the console. */
      input_getc();
      return size;
    }
  else
    {
      prefetch_user_memory (buffer, size);
      lock_acquire (&file_lock);
      struct sys_fd *fd_instance = get_fd_item (fd);

      /* If the pointer returned to fd_instance is NULL, the fd was not
         found in the file list.  Thus, we should exit immediately. */
      if (fd_instance == NULL)
        {
          lock_release (&file_lock);
          exit (-1);
        }

      num_read = file_read (fd_instance->file, buffer, size);
      unpin_user_memory (buffer, size);
      lock_release (&file_lock);
      return num_read;
    }
}

/* Writes 'size' bytes from 'buffer' to file 'fd'.  Returns number
   of bytes actually written, or -1 if there was an error in writing
   to the file. If fd = 1, function write to the console.  File
   system gets locked down with a coarse-grain lock. */
static int
write (int fd, const void *buffer, unsigned size)
{
  int num_written;
  /* If SDOUT_FILENO. */
  if (fd == 1)
    {
      /* Write to console. */
      putbuf (buffer, size);
      return size;
    }
  else
    {
      prefetch_user_memory ( (void *) buffer, size);
      lock_acquire (&file_lock);
      struct sys_fd *fd_instance = get_fd_item (fd);

      /* If the pointer returned to fd_instance is NULL, the fd was not
         found in the file list.  Thus, we should exit immediately. */
      if (fd_instance == NULL)
        {
          lock_release (&file_lock);
          exit (-1);
        }

      num_written = file_write (fd_instance->file, buffer, size);
      unpin_user_memory ((void *) buffer, size);
      lock_release (&file_lock);
      return num_written;
    }
}

/* Changes the next byte to be read or written in an open file to
   'position'.  Seeking past the end of a file is not an error.
   File system gets locked down with coarse-grain lock. */
static void
seek (int fd, unsigned position)
{
  lock_acquire (&file_lock);
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    {
      lock_release (&file_lock);
      exit (-1);
    }

  file_seek (fd_instance->file, position);
  lock_release (&file_lock);

}

/* Returns the position of the next byte to be read or written.
   File system gets locked down with coarse-grain lock. */
static unsigned
tell (int fd)
{
  lock_acquire (&file_lock);
  unsigned position;
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    {
      lock_release (&file_lock);
      exit (-1);
    }

  position = (unsigned) file_tell (fd_instance->file);
  lock_release (&file_lock);
  return position;
}

/* Closes file descriptor 'fd'.  Exiting or terminating a process
   will close all open file descriptors.  File system gets locked
   down with a coarse-grain lock. */
static void
close (int fd)
{
  lock_acquire (&file_lock);
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately.
     Note that this also takes care of the case when stdin or stdout
     are passed as fd (0 and 1, respectively). */
  if (fd_instance == NULL)
    {
      lock_release (&file_lock);
      exit (-1);
    }

  /* Close the file and remove it from all lists if fd_instance
     is valid. */
  file_close (fd_instance->file);

  list_remove (&fd_instance->used_fds_elem);
  list_remove (&fd_instance->thread_opened_elem);
  list_remove (&fd_instance->sys_fd_elem);

  if (list_empty (&fd_instance->sys_file->fd_list))
    {
      list_remove (&fd_instance->sys_file->sys_file_elem);
      free (fd_instance->sys_file);
    }
  free (fd_instance);
  lock_release (&file_lock);
}

/* Function to check all pointers that are passed to system calls.
   Returns false if the pointer is found to be invalid, and true
   if the pointer is valid.  Bound cases are checked. */
static bool
check_pointer (const void *pointer, unsigned size)
{
  if (pointer == NULL || is_kernel_vaddr (pointer))
    return false;
  else if (pointer + (size-1) == NULL ||
    is_kernel_vaddr (pointer + (size-1)))
    return false;
  return true;
}

/* Function to retrieve the sys_fd struct corresponding to a particular
   fd. Returns NULL if the fd could not be located in any list member or
   if it was found but the calling process is not the owner. */
static struct sys_fd *
get_fd_item (int fd)
{
  struct list_elem *e;
  struct sys_fd *fd_instance;
  for (e = list_begin (&used_fds);
       e != list_end (&used_fds);
       e = list_next(e))
    {
      fd_instance = list_entry (e, struct sys_fd, used_fds_elem);
      if (fd == fd_instance->value)
        {
          /* Check that I am the owner of this fd. */
          if (thread_current ()->tid == fd_instance->owner_tid)
            return fd_instance;
          else
            break;
        }
    }
  return NULL;
}

/* Helper function to close all outstanding file descriptors.  This
   function can be called whether or not a thread dies "gracefully"
   (i.e. via exit) or abruptly (directly to process_exit). */
void
close_fd (struct thread *t)
{
  struct list_elem *e;
  struct list_elem *next;
  e = list_begin (&t->opened_fds);
  while (!list_empty (&t->opened_fds) && e != list_end (&t->opened_fds))
    {
      /* Since we're deleting an item, we need to save the next pointer,
         since otherwise we might page fault. */
      next = list_next (e);
      int fd = list_entry (e, struct sys_fd, thread_opened_elem)->value;
      close (fd);
      e = next;
    }
}

/* Helper function to close all memory mapped files.  This
   function can be called whether or not a thread dies "gracefully"
   (i.e. via exit) or abruptly (directly to process_exit). */
void
munmap_all (struct thread *t)
{
  struct list_elem *e;
  struct list_elem *next;
  e = list_begin (&t->mmapped_mapids);
  while (!list_empty (&t->mmapped_mapids) &&
         e != list_end (&t->mmapped_mapids))
    {
      /* Since we're deleting an item, we need to save the next pointer,
         since otherwise we might page fault. */
      next = list_next (e);
      struct sys_mmap *m = list_entry (e, struct sys_mmap, thread_mmapped_elem);
      mapid_t mapid = m->mapid;
      munmap (mapid);
      e = next;
    }
}

/* Maps the file pointed to by fd to the address pointed to by addr. Returns
   the mapid of the mapping. Creates a new file descriptor for the memory
   mapping. Returns -1 if mmap fails */
static mapid_t
mmap (int fd, void *addr)
{
  /* STDIN and STDOUT are not mappable. */
  if (fd == 0 || fd == 1)
    return MAP_FAILED;

  lock_acquire (&file_lock);
  struct sys_fd *sf = get_fd_item (fd);
  lock_release (&file_lock);

  if (!sf)
    return MAP_FAILED;

  int new_fd = open (sf->sys_file->name);
  struct file *new_file = file_reopen (sf->file);
  int size = filesize (new_fd);

  /* Fails if fd has a length of zero bytes. */
  if (size == 0)
    return MAP_FAILED;

  /* addr needs to be page-aligned and cannot be 0. */
  if (pg_ofs (addr) != 0 || addr == 0)
    return MAP_FAILED;

  /* The number of pages is the ceiling of (size / PGSIZE). If the file size
     does not evenly divide into PGSIZE, the number of useful bytes in the
     last page is size % PGSIZE. The rest of the page is zero bytes. */
  int num_pages = size / PGSIZE;
  int num_zeros = 0;
  if (size % PGSIZE != 0)
    {
      num_zeros = PGSIZE - (size % PGSIZE);
      num_pages++;
    }

  /* None of the pages should already exist in the page table */
  int i;
  void *addr_copy = addr;
  for (i = 0; i < num_pages; i++)
    {
      if (page_lookup (addr_copy) != NULL)
        return MAP_FAILED;

      addr_copy += PGSIZE;
    }

  /* Allocate sys_mmap for bookkeeping. */
  struct sys_mmap *m = malloc (sizeof (struct sys_mmap));
  if (!m)
    return MAP_FAILED;

  list_init (&m->file_mmap_list);
  m->mapid = next_avail_mapid++;
  m->fd = new_fd;
  m->owner_tid = thread_current ()->tid;
  m->start_addr = addr;
  m->size = size;

  /* Add the fd to the thread's mmapped_mapids list. */
  struct thread *t = thread_current ();
  list_push_back (&t->mmapped_mapids, &m->thread_mmapped_elem);

  struct page_table_entry *pte_mmap;
  for (i = 0; i < num_pages; i++)
    {
      /* Allocate page and complete last page with zeros. */
      if (i == (num_pages - 1))
        pte_mmap = page_create_mmap (addr, new_file, i * PGSIZE, num_zeros);
      else
        pte_mmap = page_create_mmap (addr, new_file, i * PGSIZE, 0);

      list_push_back (&m->file_mmap_list, &pte_mmap->mmap_elem);
      addr += PGSIZE;
    }
  return m->mapid;
}

/* Unmaps the file and pages corresponding to the mapid */
static void
munmap (mapid_t m)
{
  struct thread *cur = thread_current ();
  struct list_elem *e_mmap;
  struct list_elem *next_mmap;
  struct list_elem *e_pte;
  struct list_elem *next_pte;
  struct sys_mmap *mmap_instance;
  struct page_table_entry *pte_instance;

  /* Iterate over all of the current thread's memory mapped file IDs
     to find the matching mapid_t m. */
  for (e_mmap = list_begin (&cur->mmapped_mapids);
       e_mmap != list_end (&cur->mmapped_mapids);)
    {
      next_mmap = list_next(e_mmap);
      mmap_instance = list_entry (e_mmap, struct sys_mmap,
                                  thread_mmapped_elem);
      if (mmap_instance->mapid == m &&
          mmap_instance->owner_tid == thread_current ()->tid)
        {
          /* Once located, iterate over the correspond mapped instance's
             list of pages corresponding to it to properly unmap
             and deallocate it. */
          for (e_pte = list_begin (&mmap_instance->file_mmap_list);
               e_pte != list_end (&mmap_instance->file_mmap_list);)
            {
              next_pte = list_next(e_pte);
              pte_instance = list_entry (e_pte, struct page_table_entry,
                                          mmap_elem);
              /* First check if the file is not already in the disk.
                 If it is, we do not need to perform any write-back. */
              if (pte_instance->phys_frame != NULL &&
                  pagedir_is_dirty (cur->pagedir, pte_instance->upage))
                {
                  lock_acquire (&file_lock);
                  file_write_at (pte_instance->file,
                                 pte_instance->kpage,
                                 PGSIZE,
                                 (off_t) pte_instance->offset);
                  lock_release (&file_lock);
                  lock_acquire (&frame_table_lock);
                  list_remove (&pte_instance->phys_frame->frame_elem);
                  free (pte_instance->phys_frame);
                  lock_release (&frame_table_lock);
                }
              /* Remove the page and clear the page table entry. */
              lock_acquire (&thread_current ()->spt_lock);
              hash_delete (&thread_current ()->supp_page_table,
                &pte_instance->pt_elem);
              lock_release (&thread_current ()->spt_lock);
              pagedir_clear_page (thread_current ()->pagedir,
                pte_instance->upage);
              list_remove (&pte_instance->mmap_elem);
              free (pte_instance);
              e_pte = next_pte;
            }
          /* Close the corresponding file descriptor. */
          close (mmap_instance->fd);
          list_remove (&mmap_instance->thread_mmapped_elem);
          free (mmap_instance);
          break;
        }
      e_mmap = next_mmap;
    }
}

/* Ensure that all pages needed for a file read or write are located
   in the frame table upon the file system call by paging it in and
   pinning the frame to the frame table. */
static void
prefetch_user_memory (void *pointer, size_t size)
{
  size_t i;
  void *sp = thread_current ()->esp;
  size_t len = (size / PGSIZE) + 1;

  for (i = 0; i < len; i++)
    {
      void *fa = pointer + i * PGSIZE;
      struct page_table_entry *pte = page_lookup (fa);

      if (pte == NULL)
        {
          /* Perform the same stack growth checks as the page fault
             handler in order to determine if the read or write system
             call requires stack growth. */
          if (sp < PHYS_BASE - STACK_SIZE_LIMIT)
            exit (-1);
          else if (fa >= sp - 32)
            extend_stack (fa);
          else
            exit (-1);
        }
      else if (pte->page_status != PAGE_NONZEROS && pte->phys_frame == NULL)
        {
          page_fetch_and_set (pte);
          pte->pinned = true;
        }
      else
        pte->pinned = true;
    }
}

/* Unpin the user memory from the frame table that was used in read or
   write.  All pages that are passed to this function were originally
   pinned and, as a result, need to be unpinned.  The case when a page
   table entry is not found should never occur, but it is checked
   anyways. */
static void
unpin_user_memory (void *pointer, size_t size)
{
  size_t i;
  size_t len = (size / PGSIZE) + 1;
  for (i = 0; i < len; i++)
    {
      struct page_table_entry *pte = page_lookup (pointer + i * PGSIZE);
      if (pte == NULL)
        exit (-1);
      else
        pte->pinned = false;
    }
}