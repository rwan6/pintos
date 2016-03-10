#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

bool dir_entry_is_file (struct dir_entry *);

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  bool success = inode_create (sector, (entry_cnt) *
                               sizeof (struct dir_entry), 0);

}

/* Creates entries "." and ".." for a directory that is located in SECTOR. */
bool
setup_dir (struct dir *parent, block_sector_t sector)
{
  struct dir *dir = dir_open (inode_open (sector));
  block_sector_t parent_sector = inode_get_inumber (parent->inode);
  return dir_add (dir, ".", sector, false) &&
         dir_add (dir, "..", parent_sector, false);
}

/* Checks if the dir_entry is a file. */
bool
dir_entry_is_file (struct dir_entry *e)
{
   struct inode *inode = inode_open (e->inode_sector);
   bool is_file = inode_is_file (inode);
   inode_close (inode);
   return is_file;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode)
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;

      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL;
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  struct dir *root_dir = dir_open (inode_open (ROOT_DIR_SECTOR));
  return root_dir;
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir)
{
  struct dir *reopen_dir = dir_open (inode_reopen (dir->inode));
  return reopen_dir;
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir)
{
  if (dir != NULL && !(dir == thread_current ()->current_directory))
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir)
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp)
{
  struct dir_entry e;
  size_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (e.in_use && !strcmp (name, e.name))
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }

  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode)
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (inode_get_inumber (dir->inode) == ROOT_DIR_SECTOR && strlen (name) == 0)
    *inode = inode_open (ROOT_DIR_SECTOR);
  else if (inode_get_inumber (dir->inode) ==
            inode_get_inumber (thread_current ()->current_directory->inode)
           && strlen (name) == 0)
    *inode = inode_open (inode_get_inumber (dir->inode));
  else if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    return false;

  return true;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector,
         bool is_file)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;
  bool lock_success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
  if (success && !is_file && strcmp (name, ".") && strcmp (name, ".."))
    success = setup_dir (dir, inode_sector);

  done:
    return success;
}

/* Deletes entries "." and ".." for a directory that is located in SECTOR. */
bool
cleanup_dir (struct dir *dir)
{
  return dir_remove (dir, ".") &&
         dir_remove (dir, "..");
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name)
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  bool lock_success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  if (!dir_entry_is_file (&e) && strcmp (name, ".") && strcmp (name, ".."))
    {
      struct dir *child_dir = dir_open (inode);
      if (!dir_is_empty (child_dir))
        {
          dir_close (child_dir);
          goto done;
        }
    }
  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e)
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

  if (success && inode_get_inumber (inode) ==
      inode_get_inumber (thread_current ()->current_directory->inode)
      && strcmp (name, ".") && strcmp (name, ".."))
    success = cleanup_dir (dir);

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;
  bool lock_success = false;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e)
    {
      dir->pos += sizeof e;
      if (e.in_use && strcmp (e.name, ".") && strcmp (e.name, ".."))
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          /* Release lock if we grabbed it in this function before
             returning. */
          if (lock_success)
            inode_release_lock (dir->inode);
          return true;
        }
    }

  return false;
}

/* Returns the directory struct given a starting directory and an absolute
   or relative path. */
struct dir *
get_dir_from_path (struct dir *cur_dir, const char *path)
{
  if (*path == '/')
    {
      /* path is an absolute path */
      cur_dir = dir_open_root ();
      if (strlen (path) == 1)
        return cur_dir;
    }

  struct dir_entry e;

  /* If we don't need to traverse subdirectories */
  char *c = strrchr (path, '/');
  if (c == NULL || c == path)
    {
      const char *lookup_path = path;
      if (c != NULL && path[0] == '/' && path[1] != '\0') lookup_path++;
      if (lookup (cur_dir, lookup_path, &e, NULL) && !dir_entry_is_file (&e))
        {
          return dir_open (inode_open (e.inode_sector));
        }
      else
        {
          return NULL;
        }
    }

  char *s = malloc (strlen (path) + 1);
  if (!s)
    return NULL;
  strlcpy (s, path, strlen(path) + 1);
  char *token, *save_ptr;

  for (token = strtok_r (s, "/", &save_ptr); token != NULL;
        token = strtok_r (NULL, "/", &save_ptr))
    {
      if (lookup (cur_dir, token, &e, NULL))
        {
          dir_close (cur_dir);
          cur_dir = dir_open (inode_open (e.inode_sector));
        }
      else
        {
          free (s);
          return NULL;
        }
    }
  free (s);
  return cur_dir;
}

bool
dir_is_empty (struct dir *dir)
{
  off_t ofs;
  struct dir_entry e;

  /* If an entry is in use and it is not "." or "..", the directory
     is not free. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (e.in_use && strcmp (e.name, ".") && strcmp (e.name, ".."))
      return false;
  return true;
}
