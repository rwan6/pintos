#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
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
  list_init (&mmapped_files);
  lock_init (&mmap_lock);
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
   if the pointer is valid.  Note that all pointers are checked
   in the event that a buffer is passed. */
static bool
check_pointer (const void *pointer, unsigned size)
{
  // struct thread *t = thread_current ();

  /* First validate end-cases.  If these do not fail, check
     everything.  Limits the number of checks that need to
     be made. */
  if (pointer == NULL || is_kernel_vaddr (pointer) /*||
    pagedir_get_page (t->pagedir, pointer) == NULL*/)
    return false;
  else if (pointer + (size-1) == NULL ||
    is_kernel_vaddr (pointer + (size-1)) /*||
    pagedir_get_page (t->pagedir, pointer + (size-1)) == NULL*/)
    return false;

  unsigned i;
  for (i = 1; i < (size - 1); i++)
    {
      if (pointer + i == NULL || is_kernel_vaddr (pointer + i) /*||
        pagedir_get_page (t->pagedir, pointer + i) == NULL*/)
        return false;
    }
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
  while (!list_empty (&t->mmapped_mapids) && e != list_end (&t->mmapped_mapids))
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

mapid_t
mmap (int fd, void *addr)
{
  /* STDIN and STDOUT are not mappable */
  if (fd == 0 || fd == 1)
    return MAP_FAILED;

  lock_acquire (&file_lock);
  struct sys_fd *sf = get_fd_item (fd);
  lock_release (&file_lock);

  if (!sf)
    return MAP_FAILED;

  int new_fd = open (sf->sys_file->name);
  int size = filesize (new_fd);

  /* Fails if fd has a length of zero bytes */
  if (size == 0)
    return MAP_FAILED;

  /* addr needs to be page-aligned and cannot be 0 */
  if (pg_ofs (addr) != 0 || addr == 0)
    return MAP_FAILED;

  /* We need ceiling of (size / PGSIZE) pages */
  int num_pages = size / PGSIZE;
  if (size % PGSIZE != 0)
    num_pages++;

  /* Allocate sys_mmap for bookkeeping */
  lock_acquire (&mmap_lock);
  struct sys_mmap *m = malloc (sizeof (struct sys_mmap));
  if (!m)
    {
      lock_release (&mmap_lock);
      return MAP_FAILED;
    }
  m->mapid = next_avail_mapid++;
  m->fd = new_fd;
  m->owner_tid = thread_current ()->tid;
  m->start_addr = addr;
  m->size = size;
  m->num_pages = num_pages;
  list_push_back (&mmapped_files, &m->sys_mmap_elem);

  /* Add the fd to the thread's mmapped_mapids list. */
  struct thread *t = thread_current ();
  list_push_back (&t->mmapped_mapids, &m->thread_mmapped_elem);
  lock_release (&mmap_lock);

  int i;
  for (i = 0; i < num_pages; i++)
    {
      /* Address should not be in page table already */
      if (page_lookup(addr) != NULL)
        return -1;

      /* Allocate page */
      page_create_from_vaddr(addr);

      /* Copy file into the page */

      addr += PGSIZE;
    }

  return m->mapid;
}

void
munmap (mapid_t m)
{
  lock_acquire (&mmap_lock);
  struct list_elem *e;
  struct sys_mmap *mmap_instance;
  for (e = list_begin (&mmapped_files);
       e != list_end (&mmapped_files);
       e = list_next(e))
    {
      mmap_instance = list_entry (e, struct sys_mmap, sys_mmap_elem);
      if (mmap_instance->mapid == m)
        {
          if (mmap_instance->owner_tid == thread_current ()->tid)
            {
              int i;
              for (i = 0; i < mmap_instance->num_pages; i++)
                {
                  /* Evict the pages, which implicitly writes them back to
                     the file */
                }
              close (mmap_instance->fd);
              list_remove (&mmap_instance->sys_mmap_elem);
              free (mmap_instance);
            }
          else
            break;
        }
    }
  lock_release (&mmap_lock);
  exit (-1);
}
