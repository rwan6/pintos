#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <list.h>
#include "threads/thread.h"

#define MAX_FNAME_LENGTH 14  /* Maximum filename length. */

typedef int pid_t;
void syscall_init (void);
void close_fd (struct thread *);

/* Struct to map system files to their list of open fd. */
struct sys_file
  {
    char name[MAX_FNAME_LENGTH + 1];  /* Name of the file. */
    struct list_elem sys_file_elem;   /* List element for opened files. */
    struct list fd_list;              /* List of fd's associated with
                                         this file. */
  };

/* Struct to map a fd to its system file.  Also contains the information
   pertaining to the file (such as its 'file' variable) and who owns
   the particular fd (since fd are not inherited). */
struct sys_fd
  {
    int value;                            /* The fd value. */
    tid_t owner_tid;                      /* The tid of the owner. */
    struct sys_file *sys_file;            /* Pointer to the system file
                                             struct. */
    struct file *file;                    /* If the fd corresponds to a file,
                                             points to the file's 'file'. */
    struct dir *dir;                      /* If the fd corresponds to a
                                             directory, points to the
                                             directory's 'dir'. */
    struct list_elem sys_fd_elem;         /* List element for sys_file's
                                             fd_list. */
    struct list_elem used_fds_elem;       /* List element for used fds. */
    struct list_elem thread_opened_elem;  /* List element for the thread's
                                             personal list of fds. */
  };

struct list opened_files;       /* Global list of opened files. */
struct list used_fds;           /* Global list of used fds values. */

#endif /* userprog/syscall.h */
