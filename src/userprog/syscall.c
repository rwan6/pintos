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

/* Initialize the system call interrupt. */
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
syscall_handler (struct intr_frame *f)
{  
  void *buffer;
  char *name, *cmd_line;
  int fd, status;
  unsigned initial_size, size, position;
  pid_t pid;
  
  printf("\n\n");
  hex_dump(f->esp, f->esp, 512, true);
  
  int syscall_num = *((int *) (f->esp));
  f->esp = (void *) ((int *) f->esp + 1);
  printf ("syscall_num: %d\n", syscall_num);  

  switch (syscall_num)
    {
      case SYS_HALT :
        halt();
        break;
      case SYS_EXIT :
        status = *((int *) (f->esp));
        f->esp = (void *) ((int *) f->esp + 1);
        exit (status);
        break;
      case SYS_EXEC :
        cmd_line = *((char **) (f->esp));
        f->esp = (void *) ((char **) f->esp + 1);
        exec (cmd_line);
        break;
      case SYS_WAIT :
        pid = *((pid_t *) (f->esp));
        f->esp = (void *) ((pid_t *) f->esp + 1);
        wait (pid);
        break;
      case SYS_CREATE :
        name = *((char **) (f->esp));
        f->esp = (void *) ((char **) f->esp + 1);
        initial_size = *((unsigned *) (f->esp));
        f->esp = (void *) ((unsigned *) f->esp + 1);
        create (name, initial_size);
        break;
      case SYS_REMOVE :
        name = *((char **) (f->esp));
        f->esp = (void *) ((char **) f->esp + 1);
        remove (name);
        break;
      case SYS_OPEN :
        name = *((char **) (f->esp));
        f->esp = (void *) ((char **) f->esp + 1);
        open (name);
        break;
      case SYS_FILESIZE :
        fd = *((int *) (f->esp));
        f->esp = (void *) ((int *) f->esp + 1);
        filesize (fd);
        break;
      case SYS_READ :
        fd = *((int *) (f->esp));
        f->esp = (void *) ((int *) f->esp + 1);
        buffer = *((void **) (f->esp));
        f->esp = (void *) ((void **) f->esp + 1);
        size = *((unsigned *) (f->esp));
        f->esp = (void *) ((unsigned *) f->esp + 1);
        read (fd, buffer, size);
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
        fd = *((int *) (f->esp));
        f->esp = (void *) ((int *) f->esp + 1);
        position = *((unsigned *) (f->esp));
        f->esp = (void *) ((unsigned *) f->esp + 1);
        seek (fd, position);
        break;
      case SYS_TELL :
        fd = *((int *) (f->esp));
        f->esp = (void *) ((int *) f->esp + 1);
        tell (fd);
        break;
      case SYS_CLOSE :
        fd = *((int *) (f->esp));
        f->esp = (void *) ((int *) f->esp + 1);
        close (fd);
        break;
    }

  //Destined for removal
  printf ("\nsystem call!\n");
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
      int fd = list_entry (e, struct sys_fd, thread_opened_elem)->value;
      close (fd);
    }

  printf ("%s: exit(%d)\n", t->name, status);
  thread_exit ();
}

/* Runs the executable.  Returns the new process's program id.
   Must return pid = -1. */
static pid_t
exec (const char *cmd_line)
{
  pid_t new_process_pid;
  if (!check_pointer ((const void *) cmd_line, strlen (cmd_line)))
    return -1;
  else
    {
      lock_acquire(&exec_lock);
      new_process_pid = (pid_t) process_execute (cmd_line);
      
      /* Wait until child process completes its initialization.  Note
         that the child will return -1 in the event that it failed to
         load its executable or initialize, which can then be return
         when this function terminates. */
      cond_wait(&exec_cond, &exec_lock);
      lock_release(&exec_lock);
      return new_process_pid;
    }
}

/* Wait for a child process pid and retrieves the child's exit status. */
static int
wait (pid_t pid UNUSED)
{
  return 0;
}

/* Creates a new file.  Returns true if successful, false otherwise.
   File system gets locked down with a coarse-grain lock. */
static bool
create (const char *file, unsigned initial_size)
{
  bool success;
  if (!check_pointer ((const void *) file, strlen (file)))
    return false;
  else
    {
      lock_acquire (&file_lock);
      success = filesys_create (file, initial_size);
      lock_release (&file_lock);
      return success;
    }
}

/* Deletes the file.  Returns true if successful, false otherwise.
   File can be removed while it is open or closed.  File system
   gets locked down with a coarse-grain lock. */
static bool
remove (const char *file)
{
  bool success;
  if (!check_pointer ((const void *) file, strlen (file)))
    return false;
  else
    {
      lock_acquire (&file_lock);
      success = filesys_remove (file);
      lock_release (&file_lock);
      return success;
    }
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
  /* Figure out if we have opened this file before */
  for (e = list_begin (&opened_files);
       e != list_end (&opened_files);
       e = list_next(e))
    {
      sf = list_entry (e, struct sys_file, sys_file_elem);
      if (!strcmp(file, sf->name))
        {
          found = true;
          break;
        }
    }
    
  lock_acquire (&file_lock);  
  struct file *f = filesys_open (file);
  lock_release (&file_lock); 
  
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

  /* Add to used_fds list */
  list_push_back (&used_fds, &fd->used_fds_elem);

  /* Add the fd to the thread's opened_fds list */
  struct thread *t = thread_current ();
  list_push_back (&t->opened_fds, &fd->thread_opened_elem);

  return fd->value;
}

/* Returns the file size (in bytes) of the file open.  File
   system gets locked down with a coarse-grain lock. */
static int
filesize (int fd)
{
  int file_size;
  struct sys_fd* fd_instance = get_fd_item (fd);
  
/* Should not be NULL unless the fd was invalid. */
  if (fd_instance != NULL)
    {
      lock_acquire (&file_lock);
      file_size = file_length (fd_instance->file);
      lock_release (&file_lock);
      return file_size;
    }

  return -1;
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
      return 1;
    }
  else
    {
      bool success = check_pointer(buffer, size);
      if (!success)
        return -1;
      else
        {
          struct sys_fd *fd_instance = get_fd_item (fd);
          if (fd_instance != NULL)
            {
              lock_acquire (&file_lock);
              num_read = file_read (fd_instance->file, buffer, size);
              lock_release (&file_lock);
              return num_read;
            }
          else
            return -1;
        }
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
      return 1;
    }
  else
    {
      bool success = check_pointer(buffer, size);
      if (!success)
        return -1;
      else
        {
          struct sys_fd *fd_instance = get_fd_item (fd);
          if (fd_instance != NULL)
            {
              lock_acquire (&file_lock);
              num_written = file_write (fd_instance->file, buffer, size);
              lock_release (&file_lock);
              return num_written;
            }
          else
            return -1;
        }
    }
}

/* Changes the next byte to be read or written in an open file to
   'position'.  Seeking past the end of a file is not an error.
   File system gets locked down with coarse-grain lock. */
static void
seek (int fd, unsigned position)
{
  struct sys_fd *fd_instance = get_fd_item (fd);
  
  /* Should not be NULL unless the fd was invalid. */
  if (fd_instance != NULL)
    {
      lock_acquire (&file_lock);
      file_seek (fd_instance->file, position);
      lock_release (&file_lock);
    }
}

/* Returns the position of the next byte to be read or written.
   File system gets locked down with coarse-grain lock. */
static unsigned
tell (int fd)
{
  unsigned position;
  struct sys_fd *fd_instance = get_fd_item (fd);
  
  /* Should not be NULL unless the fd was invalid.  Cast off_t to
     unsigned for return. */
  if (fd_instance != NULL)
    {
      lock_acquire (&file_lock);
      position = (unsigned) file_tell (fd_instance->file);
      lock_release (&file_lock);
      return position;
    }
  
  /* If fd_instance was NULL, then return 0.  This line should
     rarely (if ever) be reached! */
  return 0;
}

/* Closes file descriptor 'fd'.  Exiting or terminating a process
   will close all open file descriptors.  File system gets locked
   down with a coarse-grain lock. */
static void
close (int fd)
{
  struct sys_fd *fd_instance = get_fd_item (fd);
  
/* Should not be NULL unless the fd was invalid. */
if (fd_instance != NULL)
  {
    lock_acquire (&file_lock);
    file_close (fd_instance->file);
    lock_release (&file_lock);
  }
  
  /* Should not be NULL unless the fd was invalid.  Remove myself
     from the list of all the fds. */
  if (fd_instance != NULL)
    list_remove(&fd_instance->used_fds_elem);

  list_remove (&fd_instance->thread_opened_elem);

  list_remove (&fd_instance->sys_file_elem);

  if (list_empty (&fd_instance->sys_file->fd_list))
    {
      list_remove (&fd_instance->sys_file->sys_file_elem);
      free ((void *) fd_instance->sys_file);
    }

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
  unsigned i;
  for (i = 0; i < size; i++)
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
   list member. */
static struct sys_fd *
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
