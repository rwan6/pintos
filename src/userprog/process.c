#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
int num_spaces (char *s);
static void push_args_to_stack (void **esp, char *token, char **save_ptr);

/* Struct to track whether a load was successful. */
struct load
  {
    char *file_name;      /* The file name being loaded. */
    bool load_success;    /* Denotes whether the load was successful. */
    struct semaphore s;   /* Semaphore for parent to be alerted if load
      was successful or not. */
  };

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created.
   If a child needs to load an executable, the parent will wait
   in this function until the child finished and return the status
   of the load. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy, *fn_copy2, *save_ptr;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (fn_copy, file_name, PGSIZE/8);

  /* Initialize the load struct to be passed to child thread. */
  struct load load_info;
  load_info.load_success = false;
  load_info.file_name = fn_copy;
  sema_init (&load_info.s, 0);

  fn_copy2 = palloc_get_page (0);
  if (fn_copy2 == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (fn_copy2, file_name, PGSIZE);
  file_name = strtok_r (fn_copy2, " ", &save_ptr);

  /* Create a new thread to execute FILE_NAME. Instead of passing
     the file_name char pointer, we pass the address of load_info,
     so that the child thread can update the load status and use the
     semaphore to signal parent to stop waiting. */
  tid = thread_create (file_name, PRI_DEFAULT, start_process,
    &load_info);

  struct child_process *cp = NULL;

  /* If the child was spawned successfully, add it to the caller's
     list of children. */
  if (tid != TID_ERROR)
    {
      struct thread *child_thread = get_caller_child (tid);
      if (child_thread == NULL)
        {
          palloc_free_page (fn_copy2);
          return -1;
        }

      /* Child inherits the working directory from its parent */
      child_thread->current_directory = thread_current ()->current_directory;
      cp = malloc (sizeof (struct child_process));
      if (cp == NULL)
        {
          palloc_free_page (fn_copy2);
          return -1;
        }

      cp->child = child_thread;
      cp->child->my_process = cp;
      cp->child_tid = tid;
      struct dir *dir = cp->child->current_directory;
      cp->child->executable = filesys_open (dir, file_name);

      /* Deny writes to executable. */
      if (cp->child->executable != NULL)
        file_deny_write (cp->child->executable);

      cp->status = -1;
      cp->terminated = false;
      cp->waited_on = false;
      list_push_back (&thread_current ()->children,
        &cp->child_elem);

      /* Waiting for succesfully created thread to load. */
      sema_down (&load_info.s);
      if (!load_info.load_success)
        {
          cp->status = -1;
          return -1;
        }
    }

  palloc_free_page (fn_copy2);

  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);

  return tid;
}

/* A thread function that loads a user process and starts it
   running.  Also performs argument parsing and sets up the
   user memory stack. */
static void
start_process (void *load_info)
{
  struct load *info = (struct load *) load_info;
  char *file_name = info->file_name;

  char *save_ptr;
  struct intr_frame if_;
  bool success;
  struct thread *cur = thread_current ();

  file_name = strtok_r (file_name, " ", &save_ptr);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  info->load_success = success;

  /* If load failed, quit. Whether load failed or succeeded,
     updates the semaphore to inform the parent. */
  if (!success)
    {
      palloc_free_page (file_name);
      cur->return_status = -1;
      sema_up (&info->s);
      thread_exit ();
    }
  else
    sema_up (&info->s);

  push_args_to_stack (&if_.esp, file_name, &save_ptr);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Returns number of spaces in string s */
int
num_spaces (char *s)
{
  int spaces = 0;
  while (*s)
    {
      if (*s == ' ')
        spaces++;
      s++;
    }
  return spaces;
}

/* Populate the stack with arguments. */
static void
push_args_to_stack (void **esp, char *file_name, char **save_ptr)
{
  int total_args = num_spaces (file_name) + 1;
  char **argv = malloc (sizeof (char *) * total_args);
  if (argv == NULL)
    {
      palloc_free_page (file_name);
      thread_current ()->return_status = -1;
      thread_exit ();
    }
  char *token;
  argv[0] = file_name;
  size_t length_args = strlen (file_name) + 1;
  int argc = 1, i;
  while ((token = strtok_r (NULL," ", save_ptr)))
    {
      length_args += strlen(token) + 1;
      /* If there are too many arguments, exit out. */
      if (length_args > PGSIZE)
        {
          palloc_free_page (file_name);
          free (argv);
          thread_current ()->return_status = -1;
          thread_exit ();
        }
      argv[argc] = token;
      argc++;
    }

  /* Array for keeping track of the pointers of each pushed argument. */
  char **ptrs = (char **) malloc (sizeof (char*) * argc);
  if (ptrs == NULL)
    {
      palloc_free_page (file_name);
      free (argv);
      thread_current ()->return_status = -1;
      thread_exit ();
    }

  /* Push each argument in reverse order. */
  for (i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      *esp = (void *) ((char *) *esp - len);
      strlcpy ((char *) *esp, argv[i], len);
      ptrs[i] = (char *) *esp;
    }

  /* Word align the stack pointer to a multiple of 4. */
  *esp = (void *) ((unsigned int) *esp & 0xfffffffc);

  /* Push null pointer sentinel. */
  *esp = (void *) ((char **) *esp - 1);
  *((char **) *esp) = 0;

  /* Push addresses of each argument in reverse order. */
  for (i = argc - 1; i >= 0; i--)
    {
      *esp = (void *) ((char **) *esp - 1);
      *((char **) *esp) = ptrs[i];
    }

  /* Push argv. */
  char **argv0_ptr = (char **) *esp;
  *esp = (void *) ((char **) *esp - 1);
  *((char ***) *esp) = argv0_ptr;

  /* Push argc. */
  *esp = (void *) ((int *) *esp - 1);
  *((int *) *esp) = argc;

  /* Push return address. */
  *esp = ((void **) *esp - 1);
  *((void **) *esp) = 0;

  palloc_free_page (file_name);
  free (argv);
  free (ptrs);
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int
process_wait (tid_t child_tid)
{
  struct thread *t = thread_current ();
  struct list_elem *e;
  struct child_process *cp;

  /* Wait on the child given by child_tid and return their status.
     This function also covers the cases when the child has already been
     waited on and if the child has already terminated. */
  for (e = list_begin (&t->children);
       e != list_end (&t->children);
       e = list_next(e))
    {
      cp = list_entry (e, struct child_process, child_elem);
      if (cp->child_tid == child_tid)
        {
          if (cp->waited_on)
            break;
          else if (cp->terminated)
            cp->waited_on = true;
          else
            {
              cp->waited_on = true;
              t->child_wait_tid = child_tid;
              /* Wait on my child. */
              lock_acquire (&t->wait_lock);
              cond_wait (&t->wait_cond, &t->wait_lock);
              lock_release (&t->wait_lock);
            }
          return cp->status;
        }
    }

  /* If the chld was already waited on or not found in this process'
     list of children, -1 should be returned. */
  return -1;
}

/* Free the current process's resources before exiting.  If parent
   is still alive, also wake them up so they are not caught in a
   deadlock.  This function covers the case when the thread dies
   abruptly.  Proper memory "cleanup" is ensured as well. */
void
process_exit (void)
{
  lock_acquire (&exit_lock);
  struct thread *cur = thread_current ();
  uint32_t *pd;
  printf ("%s: exit(%d)\n", cur->name, cur->return_status);

  /* Close any open file handles.  Closing a file also reenables
     writes. */
  close_fd (cur);

  /* If my parent is still alive, make sure they are not
     caught in a deadlock.  Otherwise, deallocate my child_process
     from my parent's list. */
  if (cur->parent != NULL)
    {
      cur->my_process->terminated = true;
      if (cur->parent->child_wait_tid == cur->tid)
        {
          lock_acquire (&cur->parent->wait_lock);
          cond_signal (&cur->parent->wait_cond, &cur->parent->wait_lock);
          lock_release (&cur->parent->wait_lock);
        }
    }
  else
    free (cur->my_process);

  /* Update each of my children's parents to NULL and free that child
     if they have already been terminated. */
  struct list_elem *e;
  struct child_process *cp;
  for (e = list_begin (&cur->children);
       e != list_end (&cur->children);)
    {
      /* Since we're deleting an item, we need to save the next
         pointer, since otherwise we might page fault. */
      struct list_elem *next = list_next (e);
      cp = list_entry (e, struct child_process, child_elem);
      cp->child->parent = NULL;
      if (cp->terminated)
        free (cp);
      e = next;
    }

  /* Reallow writes to executable. */
  if (cur->executable != NULL)
    file_allow_write (cur->executable);

  lock_release (&exit_lock);
  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (t->current_directory, file_name);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", file_name);
      goto done;
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
