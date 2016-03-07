#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "filesys/inode.h"

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
static bool chdir (const char *);
static bool mkdir (const char *);
static bool readdir (int, char *);
static bool isdir (int);
static int inumber (int);
static bool filename_ends_in_slash (const char *);
static void clean_filename (char *, char *);
static bool check_pointer (const void *, unsigned);
static struct dir *get_last_dir (const char *, const char **);
static struct sys_fd* get_fd_item (int);

static int next_avail_fd;       /* Tracks the next available fd. */

/* Initialize the system call interrupt, as well as the next available
   file descriptor and the file lists. */
void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  list_init (&opened_files);
  list_init (&used_fds);
  next_avail_fd = 2; /* 0 and 1 are reserved. */
}

/* Takes the interrupt frame as an argument and traces the stack
   to determine which system call function needs to be invoked.
   Also performs stack pointer, argument, and buffer/filename
   address checks. */
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
      case SYS_CHDIR :
        f->eax = chdir ((char *) arg1);
        break;
      case SYS_MKDIR :
        f->eax = mkdir ((char *) arg1);
        break;
      case SYS_READDIR :
        f->eax = readdir (arg1, (char *) arg2);
        break;
      case SYS_ISDIR :
        f->eax = isdir (arg1);
        break;
      case SYS_INUMBER :
        f->eax = inumber (arg1);
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

/* Creates a new file.  Returns true if successful, false otherwise. */
static bool
create (const char *file, unsigned initial_size)
{
  if (strlen (file) == 0)
    exit (-1);
  bool success;
  const char *new_file;
  struct dir *last_dir = get_last_dir (file, &new_file);
  if (!last_dir)
    return false;
  success = filesys_create (last_dir, new_file, initial_size, true);
  dir_close (last_dir);
  return success;
}

/* Deletes the file.  Returns true if successful, false otherwise.
   File can be removed while it is open or closed. */
static bool
remove (const char *file)
{
  if (!strcmp(file, "/"))
    return false;
  bool success;
  const char *new_file;
  struct dir *last_dir = get_last_dir (file, &new_file);
  if (!last_dir)
    return false;
  success = filesys_remove (last_dir, file);
  dir_close (last_dir);
  return success;
}

/* Opens the file and returns a file descriptor.  If open fails,
   -1 is returned. */
static int
open (const char *file)
{
  /* If the file or directory is empty, return -1 immediately. */
  if (strlen (file) == 0)
    return -1;

  bool found = false;
  struct list_elem *e;
  struct sys_file* sf = NULL;

  /* Figure out if we have opened this file before. */
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
// printf("1\n");

  const char *new_file;
  struct dir *last_dir = get_last_dir (file, &new_file);
  if (!last_dir)
    return -1;
// printf("2 %x %s\n", last_dir, new_file);

  struct file *f = filesys_open (last_dir, new_file);
  if (!f)
    return -1;

  dir_close (last_dir);

  struct sys_fd *fd = malloc (sizeof (struct sys_fd));
  if (!fd)
    exit (-1);
  fd->value = next_avail_fd++;
  fd->file = f;
  fd->owner_tid = thread_current ()->tid;
// printf("4\n");

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

  /* If it is a directory, dir will be set to non-NULL. */
  if (isdir (fd->value))
    fd->dir = dir_open (fd->file->inode);
  else
    fd->dir = NULL;

  return fd->value;
}

/* Returns the file size (in bytes) of the file open. */
static int
filesize (int fd)
{
  int file_size;
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  file_size = file_length (fd_instance->file);
  return file_size;
}

/* Returns the number of bytes actually read, or -1 if the file could
   not be read.  If fd = 0, function reads from the keyboard. */
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

      num_read = file_read (fd_instance->file, buffer, size);
      return num_read;
    }
}

/* Writes 'size' bytes from 'buffer' to file 'fd'.  Returns number
   of bytes actually written, or -1 if there was an error in writing
   to the file. If fd = 1, function write to the console. */
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
      if (fd_instance == NULL || isdir (fd_instance->value))
        exit (-1);

      num_written = file_write (fd_instance->file, buffer, size);
      return num_written;
    }
}

/* Changes the next byte to be read or written in an open file to
   'position'.  Seeking past the end of a file is not an error. */
static void
seek (int fd, unsigned position)
{
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  file_seek (fd_instance->file, position);

}

/* Returns the position of the next byte to be read or written. */
static unsigned
tell (int fd)
{
  unsigned position;
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  position = (unsigned) file_tell (fd_instance->file);
  return position;
}

/* Closes file descriptor 'fd'.  Exiting or terminating a process
   will close all open file descriptors. */
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

  file_close (fd_instance->file);

  list_remove (&fd_instance->used_fds_elem);
  list_remove (&fd_instance->thread_opened_elem);
  list_remove (&fd_instance->sys_fd_elem);

  if (list_empty (&fd_instance->sys_file->fd_list))
    {
      list_remove (&fd_instance->sys_file->sys_file_elem);
      free (fd_instance->sys_file);
    }
  dir_close (fd_instance->dir);
  free (fd_instance);
}

/* Function to check all pointers that are passed to system calls.
   Returns false if the pointer is found to be invalid, and true
   if the pointer is valid.  Bound cases are checked. */
static bool
check_pointer (const void *pointer, unsigned size)
{
  struct thread *t = thread_current ();

  if (pointer == NULL || is_kernel_vaddr (pointer) ||
    pagedir_get_page (t->pagedir, pointer) == NULL)
    return false;
  else if (pointer + (size-1) == NULL ||
    is_kernel_vaddr (pointer + (size-1)) ||
    pagedir_get_page (t->pagedir, pointer + (size-1)) == NULL)
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

/* Removes the last file (or folder) from pathname in dir returns the dir
   corresponding to that path. Sets last_token to be the final file/folder
   name. Returns NULL if path is invalid. */
static struct dir *
get_last_dir (const char *dir, const char **last_token)
{
  struct dir *last_dir;
  struct dir *cur_dir = thread_current ()->current_directory;
  /* dir_copy holds the path not including the last file/folder */
  char *dir_copy = malloc (strlen (dir) + 1);
  if (!dir_copy)
    exit (-1);

  strlcpy (dir_copy, dir, strlen (dir) + 1);

  char *c = strrchr (dir_copy, '/');
  if (c)
    {
      /* If the '/' was at the beginning, the last_dir is the root directory.
         Otherwise, dir_copy should be the whole path name up to the '/'. */
      if (c == dir_copy)
        {
          last_dir = dir_open_root ();
        }
      else
        {
          *c = '\0';
          last_dir = get_dir_from_path (cur_dir, dir_copy);
        }
      *last_token = dir + (c - dir_copy) + 1;
    }
  else if (!strcmp (dir_copy, "."))
    {
      last_dir = cur_dir;
      *last_token = "";
    }
  else
    {
      last_dir = dir_reopen (cur_dir);
      *last_token = dir;
    }

  free (dir_copy);
  return last_dir;
}

/* Changes the current working directory of the process to dir, which may be
   relative or absolute. Returns true if successful, false on failure. */
static bool
chdir (const char *dir)
{
  if (strlen (dir) == 0 || filename_ends_in_slash (dir))
    return false;

  struct dir *cur_dir = thread_current ()->current_directory;
  struct dir *new_dir = get_dir_from_path (cur_dir, dir);

  if (new_dir)
    thread_current ()->current_directory = new_dir;

  return (new_dir != NULL);
}

/* Creates the directory named dir, which may be relative or absolute.
   Returns true if successful, false on failure. Fails if dir already
   exists or if any directory name in dir, besides the last, does not
   already exist. */
static bool
mkdir (const char *dir)
{
  if (strlen (dir) == 0 || filename_ends_in_slash (dir))
    return false;

  const char *new_dir;
  struct dir *last_dir = get_last_dir (dir, &new_dir);
  if (!last_dir)
    return false;

  bool success = filesys_create (last_dir, new_dir, 16, false);
  dir_close (last_dir);
  return success;
}

/* Reads a directory entry from file descriptor fd, which must represent a
   directory.  If successful, stores the null-terminated filename in name
   and returns true.  If no entries are left in the directory, returns
   false. */
static bool
readdir (int fd, char *name)
{
  /* If it is not a directory, return false. */
  if (!isdir (fd))
    return false;
  else
    {
      struct sys_fd *fd_instance = get_fd_item (fd);

      /* If the pointer returned to fd_instance is NULL, the fd was not
         found in the file list.  Thus, we should exit immediately. */
      if (fd_instance == NULL || inode_is_file (fd_instance->file->inode))
        exit (-1);

      return dir_readdir (fd_instance->dir, name);
    }
}

/* Returns true if fd represents a directory, false if it represents an
   ordinary file. */
static bool
isdir (int fd)
{
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  const struct inode *inode = fd_instance->file->inode;

  return !inode_is_file (inode);
}

/* Returns the inode number of the inode associated with fd, which may
   represent an ordinary file or a directory. */
static int
inumber (int fd)
{
  struct sys_fd *fd_instance = get_fd_item (fd);

  /* If the pointer returned to fd_instance is NULL, the fd was not
     found in the file list.  Thus, we should exit immediately. */
  if (fd_instance == NULL)
    exit (-1);

  const struct inode *inode = fd_instance->file->inode;

  return inode_get_inumber (inode);
}

/* Return whether the filename ends in a '/', excluding the root directory */
static bool
filename_ends_in_slash (const char *filename)
{
  if (strlen (filename) == 1)
    return false;
  return filename[strlen (filename) - 1] == '/';
}

/* Removes consecutive '/' and removes extraneous "/./" from the filename.
   Also removes extra "/../" from the start of filename */
/* TODO: maybe remove trailing '.' or "/." */
/* This function might be completely unnecessary */
static void
clean_filename (char *filename, char *cleaned)
{
  char *tmp1 = malloc (strlen (filename) + 1);
  if (!tmp1)
    exit (-1);
  char *tmp2 = malloc (strlen (filename) + 1);
  if (!tmp2)
    {
      free (tmp1);
      exit (-1);
    }
  /* Remove consecutive '/' from filename */
  char *cur_in = filename;
  char *cur_out = tmp1;
  do
    {
      *cur_out = *cur_in;
      if (*cur_in == '/')
        {
          while (*(cur_in + 1) == '/')
            cur_in++;
        }
      cur_in++;
      cur_out++;
    } while (*cur_in != '\0');

  /* Replace any instance of "/./" with "/" */
  cur_in = tmp1;
  cur_out = tmp2;
  do
    {
      *cur_out = *cur_in;
      if ((*cur_in == '/') && (*(cur_in + 1) == '.') &&
          (*(cur_in + 2) == '/'))
        cur_in += 2;
      cur_in++;
      cur_out++;
    } while (*cur_in != '\0');

   /* If a string starts with "/../", replace it with "/" */
   cur_in = tmp2;
   while ((*cur_in == '/') && (*(cur_in + 1) == '.') &&
          (*(cur_in + 2) == '.') && (*(cur_in + 3) == '/'))
    cur_in += 3;
   strlcpy (cleaned, cur_in, strlen (cur_in) + 1);
   free (tmp1);
   free (tmp2);
}
