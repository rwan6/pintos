#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <list.h>

typedef int pid_t;
void syscall_init (void);

struct sys_file
  {
    char name[15];
    struct list_elem sys_file_elem;
    struct list fd_list;
    bool to_be_removed;
  };

struct sys_fd
  {
    int value;
    struct sys_file *sys_file;
    struct file *file;
    struct list_elem fd_elem;
  };

struct list opened_files;
struct list used_fds;
int next_avail_fd;

#endif /* userprog/syscall.h */
