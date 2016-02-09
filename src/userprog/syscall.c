#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h" /* For synching in exec and with files */
#include "threads/vaddr.h" /* For validating the user address. */
#include "devices/shutdown.h" /* For shutdown_power_off. */
#include "devices/input.h" /* For input_putc(). */
#include "filesys/file.h" /* For file operations. */
#include "filesys/filesys.h" /* For filesys operations. */

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
static bool check_pointer (const void *, unsigned);
static struct sys_fd* get_fd_item (int);

#define MAX_ARG_LENGTH 14  /* Maximum file name length. */

/* Initialize the system call interrupt. */
void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  list_init (&opened_files);
  list_init (&used_fds);
  next_avail_fd = 2; /* 0 and 1 are reserved. */
}

/* Takes the interrupt frame as an argument and traces the stack
   to determine which system call function needs to be invoked. */
static void
syscall_handler (struct intr_frame *f)
{
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
  struct list_elem *e;
  struct child_process *cp;

  /* Update each of my children's parents to NULL. */
  for (e = list_begin (&t->children);
       e != list_end (&t->children);
       e = list_next(e))
    {
      cp = list_entry (e, struct child_process,
        child_elem);
      cp->child->parent = NULL;
    }

  /* If my parent is still alive, update my status and
     notify them that I am being terminated. */
  if (t->parent != NULL)
    {
      t->my_process->terminated = true;
      t->my_process->status = status;
    }

  /* Close any open file handles.  Closing a file also reenables writes. */
  e = list_begin (&t->opened_fds);
  while (!list_empty (&t->opened_fds) && e != list_end (&t->opened_fds))
    {
      /* Since we're deleting an item, we need to save the next pointer,
         since otherwise we might page fault. */
      struct list_elem *next = list_next (e);
      int fd = list_entry (e, struct sys_fd, thread_opened_elem)->value;
      close (fd);
      e = next;
    }

  t->return_status = status;
  thread_exit ();
}

/* Runs the executable.  Returns the new process's program id.
   Must return pid = -1 if the child process failed to load. */
static pid_t
exec (const char *cmd_line)
{
  tid_t new_process_pid;
  if (!check_pointer ((const void *) cmd_line, MAX_ARG_LENGTH))
    exit (-1);
  else
    {
      new_process_pid = process_execute (cmd_line);
      return new_process_pid;
    }
}

/* Wait for a child process pid and retrieves the child's exit status. */
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
  lock_release (&file_lock);

  lock_acquire (&file_lock);
  struct file *f = filesys_open (file);
  lock_release (&file_lock);

  if (!f)
    return -1;

  struct sys_fd *fd = malloc (sizeof (struct sys_fd));
  if (!fd)
    exit (-1);
  fd->value = next_avail_fd++;
  fd->file = f;
  fd->owner_tid = (int) thread_current ()->tid;


  /* If we have not opened it before, create a new entry. */
  if (!found)
    {
      sf = malloc (sizeof (struct sys_file));
      if (!sf)
        {
          free (fd);
          exit (-1);
        }
      list_init (&sf->fd_list);
      strlcpy (sf->name, file, strlen (file) + 1);
      sf->file = f;
    }

  /* Now that sf points to something useful, add it to the opened_files
     list, add the fd to sf's list and update fd's sys_file pointer. */
  lock_acquire (&file_lock);
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
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  lock_acquire (&file_lock);
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
      struct sys_fd *fd_instance = get_fd_item (fd);

      /* If the pointer returned to fd_instance is NULL, the fd was not
         found in the file list.  Thus, we should exit immediately. */
      if (fd_instance == NULL)
        exit (-1);

      lock_acquire (&file_lock);
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
      struct sys_fd *fd_instance = get_fd_item (fd);

      /* If the pointer returned to fd_instance is NULL, the fd was not
         found in the file list.  Thus, we should exit immediately. */
      if (fd_instance == NULL)
        exit (-1);

      lock_acquire (&file_lock);
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
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  lock_acquire (&file_lock);
  file_seek (fd_instance->file, position);
  lock_release (&file_lock);

}

/* Returns the position of the next byte to be read or written.
   File system gets locked down with coarse-grain lock. */
static unsigned
tell (int fd)
{
  unsigned position;
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  lock_acquire (&file_lock);
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
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately.
     Note that this also takes care of the case when stdin or stdout
     are passed as fd (0 and 1, respectively). */
  if (fd_instance == NULL)
    exit (-1);

  /* Close the file and remove it from all lists if fd_instance
     is valid. */
  lock_acquire (&file_lock);
  file_close (fd_instance->file);
  lock_release (&file_lock);

  list_remove (&fd_instance->used_fds_elem);

  list_remove (&fd_instance->thread_opened_elem);

  list_remove (&fd_instance->sys_fd_elem);

  // if (list_empty (&fd_instance->sys_file->fd_list))
  //   {
  //     list_remove (&fd_instance->sys_file->sys_file_elem);
  //     free ((void *) fd_instance->sys_file);
  //   }
  
  free (fd_instance);
}

/* Function to check all pointers that are passed to system calls.
   Returns false if the pointer is found to be invalid, and true
   if the pointer is valid.  Note that all pointers are checked
   in the event that a buffer is passed. */
static bool
check_pointer (const void *pointer, unsigned size)
{
  struct thread *t = thread_current ();
  
  /* First validate end cases.  If these do not fail, check
     everything.  Limits the number of checks that need to
     be made. */
  if (pointer == NULL || is_kernel_vaddr (pointer) ||
    pagedir_get_page (t->pagedir, pointer) == NULL)
    return false;
  else if (pointer + (size-1) == NULL ||
    is_kernel_vaddr (pointer + (size-1)) ||
    pagedir_get_page (t->pagedir, pointer + (size-1)) == NULL)
    return false;
  
  unsigned i;
  for (i = 1; i < (size - 1); i++)
    {
      if (pointer + i == NULL || is_kernel_vaddr (pointer + i) ||
        pagedir_get_page (t->pagedir, pointer + i) == NULL)
        return false;
    }
  return true;
}

/* Function to retrieve the sys_fd struct corresponding to a particular
   fd.  Since this iterative process is used frequently, making it
   a function helps to reduce redundancies across the system call
   functions.  Returns NULL if the fd could not be located in any
   list member or if it was found but the calling process is not the
   owner. */
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
          /* Check that I am the owner of this pid. */
          if ((int) thread_current ()->tid == fd_instance->owner_tid)
            return fd_instance;
          else
            break;
        }
    }
  return NULL;
}
