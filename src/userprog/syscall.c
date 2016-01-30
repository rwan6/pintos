#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h" /* For shutdown_power_off. */

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
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

}

/* Runs the executable.  Returns the new process's program id.
   Must return pid = -1. */
static pid_t
exec (const char *cmd_line)
{
  return -1;
}

/* Wait for a child process pid and retrieves the child's exit status. */
static int
wait (pid_t pid)
{
  return 0;
}

/* Creates a new file.  Returns true if successful, false otherwise. */
static bool
create (const char *file, unsigned initial_size)
{
  return true;
}

/* Deletes the file.  Returns true if successful, false otherwise.
   File can be removed while it is open or closed. */
static bool
remove (const char *file)
{
  return true;
}

/* Opens the file and returns a file descriptor.  If open fails,
   -1 is returned. */
static int
open (const char *file)
{
  return 2;
}

/* Returns the file size (in bytes) of the file open. */
static int
filesize (int fd)
{
  return 1;
}

/* Returns the number of bytes actually read,
   or -1 if the file could not be read.
   If fd = 0, function reads from the keyboard. */
static int
read (int fd, void *buffer, unsigned size)
{
  return 1;
}

/* Writes 'size' bytes from 'buffer' to file 'fd'.
   Returns number of bytes actually written. */
static int
write (int fd, const void *buffer, unsigned size)
{
  return 1;
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
  
}







