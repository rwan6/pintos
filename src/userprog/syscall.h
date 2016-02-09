#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <list.h>

typedef int pid_t;
void syscall_init (void);

/* Struct to map system files to their list of open fd. */
struct sys_file
  {
    char name[15];                    /* Name of the file. */
    struct list_elem sys_file_elem;   /* List element for opened files. */
    struct file *file;                /* Pointer to the file's 
      'file' struct. */
    struct list fd_list;              /* List of fd's associated with
      this file. */
  };

/* Struct to map a fd to its system file.  Also contains the information
   pertaining to that file (such as its 'file' variable) and who owns
   the particular fd (since fd are not inherited). */
struct sys_fd
  {
    int value;                            /* The fd value. */
    int owner_tid;                        /* The tid of the owner. */
    struct sys_file *sys_file;            /* Pointer to the system 
      file struct. */
    struct file *file;                    /* Pointer ot the file's
      'file' struct. */
    struct list_elem sys_fd_elem;         /* List element for the 
      opened files. */
    struct list_elem used_fds_elem;       /* List element for used fds. */
    struct list_elem thread_opened_elem;  /* List element for the thread's
      personal list of fds. */
  };

struct list opened_files;       /* Global list of opened files. */
struct list used_fds;           /* Global list of used fds values. */
static int next_avail_fd;       /* Tracks the next available fd. */

#endif /* userprog/syscall.h */
