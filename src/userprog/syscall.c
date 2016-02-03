#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h" /* For validating the user address. */
#include "devices/shutdown.h" /* For shutdown_power_off. */
#include "devices/input.h" /* For input_putc(). */
#include "filesys/file.h" /* For file operations. */
#include "filesys/filesys.h" /* For filesys operations. */

static void syscall_handler (struct intr_frame *);
static void halt (void);
static void exit (int);
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

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  list_init (&opened_files);
  list_init (&used_fds);
  next_avail_fd = 2; /* 0 and 1 are reserved */
}

/* Takes the interrupt frame as an argument and traces the stack
   to determine which system call function needs to be invoked. */
static void
syscall_handler (struct intr_frame *f UNUSED)
{
  int syscall_num = *((int *) (f->esp));
  f->esp = (void *) ((int *) f->esp + 1);
  printf ("syscall_num: %d\n", syscall_num);

  char *name;
  int fd, status;
  void *buffer;
  unsigned size;
  /* The possible system calls start at 0. */
  switch (syscall_num)
    {
      case SYS_HALT :
        break;
      case SYS_EXIT :
        status = *((int *) (f->esp)++);
        exit (status);
        break;
      case SYS_EXEC :
        break;
      case SYS_WAIT :
        break;
      case SYS_CREATE :
        break;
      case SYS_REMOVE :
        break;
      case SYS_OPEN :
        name = *((char **) (f->esp)++);
        open (name);
        break;
      case SYS_FILESIZE :
        break;
      case SYS_READ :
        break;
      case SYS_WRITE :
        fd = *((int *) (f->esp));
        f->esp = (void *) ((int *) f->esp + 1);
        buffer = *((void **) (f->esp));
        f->esp = (void *) ((void **) f->esp + 1);
        size = *((unsigned *) (f->esp));
        f->esp = (void *) ((unsigned *) f->esp + 1);
        write (fd, buffer, size);
        break;
      case SYS_SEEK :
        break;
      case SYS_TELL :
        break;
      case SYS_CLOSE :
        break;
      default:
        break;
    }

  //Destined for removal
  printf ("system call!\n");
  thread_exit ();
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

  /* Close any open file handles */
  struct list_elem *e;
  for (e = list_begin (&t->opened_fds);
       e != list_end (&t->opened_fds);
       e = list_next(e))
    {
      int fd = list_entry (e, struct sys_fd, thread_opened_elem);
      close (fd);
    }

  printf ("%s: exit(%d)\n", t->name, status);
  thread_exit ();
}

/* Runs the executable.  Returns the new process's program id.
   Must return pid = -1. */
static pid_t
exec (const char *cmd_line UNUSED)
{
  return -1;
}

/* Wait for a child process pid and retrieves the child's exit status. */
static int
wait (pid_t pid UNUSED)
{
  return 0;
}

/* Creates a new file.  Returns true if successful, false otherwise. */
static bool
create (const char *file, unsigned initial_size)
{
  if (!check_pointer ((const void *) file, strlen (file)))
    return false;
  else
    return filesys_create (file, initial_size);
}

/* Deletes the file.  Returns true if successful, false otherwise.
   File can be removed while it is open or closed. */
static bool
remove (const char *file)
{
  struct list_elem *e;
  struct sys_file* sf;
  for (e = list_begin (&opened_files);
       e != list_end (&opened_files);
       e = list_next(e))
    {
      sf = list_entry (e, struct sys_file, sys_file_elem);
      if (!strcmp(file, sf->name))
        {
          sf->to_be_removed = true;
          break;
        }
    }
  bool success = filesys_remove(file);
  return success;
}

/* Opens the file and returns a file descriptor.  If open fails,
   -1 is returned. */
static int
open (const char *file)
{
  bool found = false;
  struct list_elem *e;
  struct sys_file* sf;
  /* Figure out if we have opened this file before */
  for (e = list_begin (&opened_files);
       e != list_end (&opened_files);
       e = list_next(e))
    {
      sf = list_entry (e, struct sys_file, sys_file_elem);
      if (!strcmp(file, sf->name))
        {
          /* If the file was already removed, but other processes
             are still reading from it, it cannot be opened. */
          if (sf->to_be_removed)
            return -1;

          found = true;
          break;
        }
    }

  struct file *f = filesys_open (file);
  if (!f)
    return -1;

  struct sys_fd *fd = malloc (sizeof (struct sys_fd));
  fd->value = next_avail_fd++;
  fd->file = f;


  /* If we have not opened it before, create a new entry */
  if (!found)
    {
      sf = malloc (sizeof (struct sys_file));
      strlcpy (sf->name, file, strlen (file) + 1);
    }

  /* Now that sf points to something useful, add it to the opened_files
     list, add the fd to sf's list and update fd's sys_file pointer */
  list_push_back (&opened_files, &sf->sys_file_elem);
  list_push_back (&sf->fd_list, &fd->sys_file_elem);
  fd->sys_file = sf;
  sf->to_be_removed = false;

  /* Add to used_fds list */
  list_push_back (&used_fds, &fd->used_fds_elem);

  /* Add the fd to the thread's opened_fds list */
  struct thread *t = thread_current ();
  list_push_back (&t->opened_fds, &fd->thread_opened_elem);

  return fd->value;
}

/* Returns the file size (in bytes) of the file open. */
static int
filesize (int fd)
{

  struct list_elem *e;
  struct sys_fd* fd_instance;
  for (e = list_begin (&used_fds);
       e != list_end (&used_fds);
       e = list_next(e))
    {
      fd_instance = list_entry (e, struct sys_fd, used_fds_elem);
      if (fd == fd_instance->value)
        {
          return file_length (fd_instance->file);
        }
    }

  return -1;
}

/* Returns the number of bytes actually read, or -1 if the file could
   not be read.  If fd = 0, function reads from the keyboard. */
static int
read (int fd, void *buffer, unsigned size)
{
  /* If SDIN_FILENO. */
  if (fd == 0)
    {
      /* Read from the console. */
      input_getc();
      return 1;
    }
  else
    {
      bool success = check_pointer(buffer, size);
      if (!success)
        return -1;
      else
        {
          struct sys_fd *sf = get_fd_item (fd);
          if (sf != NULL)
            return file_read (sf->file, buffer, size);
          else
            return -1;
        }
    }
}

/* Writes 'size' bytes from 'buffer' to file 'fd'.  Returns number
   of bytes actually written, or -1 if there was an error in writing
   to the file. If fd = 1, function write to the console. */
static int
write (int fd, const void *buffer, unsigned size)
{
  /* If SDOUT_FILENO. */
  if (fd == 1)
    {
      /* Write to console. */
      putbuf (buffer, size);
      return 1;
    }
  else
    {
      bool success = check_pointer(buffer, size);
      if (!success)
        return -1;
      else
        {
          struct sys_fd *sf = get_fd_item (fd);
          if (sf != NULL)
            return file_write (sf->file, buffer, size);
          else
            return -1;
        }
    }
}

/* Changes the next byte to be read or written in an open file to
   'position'.  Seeking past the end of a file is not an error. */
static void
seek (int fd, unsigned position)
{

}

/* Returns the position of the next byte to be read or written. */
static unsigned
tell (int fd)
{
  return 1;
}

/* Closes file descriptor 'fd'.  Exiting or terminating a process
   will close all open file descriptors. */
static void
close (int fd)
{
  const char *filename;

  struct list_elem *e;
  struct sys_fd *fd_instance;
  for (e = list_begin (&used_fds);
       e != list_end (&used_fds);
       e = list_next(e))
    {
      fd_instance = list_entry (e, struct sys_fd, used_fds_elem);
      if (fd == fd_instance->value)
        {
          list_remove (e);
          break;
        }
    }

  list_remove (&fd_instance->thread_opened_elem);

  list_remove (&fd_instance->sys_file_elem);

  if (list_empty (&fd_instance->sys_file->fd_list)
        && fd_instance->sys_file->to_be_removed)
    {
      filename = fd_instance->sys_file->name;
      filesys_remove (filename);
      list_remove (&fd_instance->sys_file->sys_file_elem);
      free ((void *) fd_instance->sys_file);
    }

  free (fd_instance);
}

static bool
check_pointer (const void *pointer, unsigned size)
{
  unsigned i;
  for (i = 0; i < size; i++)
    {
      if (pointer + i == NULL || is_kernel_vaddr (pointer + i))
        return false;
      else if (pagedir_get_page (active_pd (), pointer + i) == NULL)
        return false;
      else
        return true;
    }
}

static struct sys_fd*
get_fd_item (int fd)
{
  struct list_elem *e;
  struct sys_fd* fd_instance;
  for (e = list_begin (&used_fds);
        e != list_end (&used_fds);
       e = list_next(e))
    {
      fd_instance = list_entry (e, struct sys_fd, used_fds_elem);
      if (fd == fd_instance->value)
        {
          return fd_instance;
        }
    }
  return NULL;
}
