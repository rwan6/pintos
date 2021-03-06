#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdio.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Number of inode_disk objects that are not part of the inode's first level
   hierarchy.  Used to determine how many sectors the first level should
   have. */
#define NUM_METADATA_INDIR_DOUB 6

/* Size of the inode hierarchy first level. */
#define FIRSTLEVEL_SIZE ((BLOCK_SECTOR_SIZE / 4) - NUM_METADATA_INDIR_DOUB)

/* Size of the inode hierarchy indirect and doubly-indirect levels. */
#define INDIR_DOUB_SIZE (BLOCK_SECTOR_SIZE / 4)

/* Calculate the maximum location the doubly-indirect level can point to
   and ensure that a process does not try to access past this. */
#define MAX_BLOCK (FIRSTLEVEL_SIZE + INDIR_DOUB_SIZE + \
                   INDIR_DOUB_SIZE * INDIR_DOUB_SIZE)

/* Function prototypes. */
struct inode_disk;
static block_sector_t block_lookup (struct inode_disk *, unsigned);
static block_sector_t indirect_lookup (struct inode_disk *, unsigned);
static block_sector_t doub_indir_lookup (struct inode_disk *, unsigned);
static bool file_block_growth (struct inode_disk *);
static block_sector_t allocate_new_block (void);
static bool file_grow (struct inode_disk *, unsigned);
static bool inode_grab_lock (struct inode *);
static void inode_release_lock (struct inode *);

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;       /* Element in inode list. */
    block_sector_t sector;       /* Sector number of disk location. */
    int open_cnt;                /* Number of openers. */
    bool removed;                /* True if deleted, false otherwise. */
    int deny_write_cnt;          /* 0: writes ok, >0: deny writes. */
    struct lock inode_lock;      /* Inode synchronization lock. */
  };

/* On-disk inode.  Since it must be BLOCK_SECTOR_SIZE bytes long,
   the first level indexing is based on the metadata size.  In
   declaration order: 4 + 4 + 4 + 4 + 4*FIRSTLEVEL_SIZE + 4 + 4 = 512. */
struct inode_disk
  {
    uint32_t length;         /* File size in bytes. */
    uint32_t num_blocks;     /* Number of blocks allocated to this file. */
    unsigned magic;          /* Magic number. */
    unsigned is_file;        /* Is this inode a file? */
    block_sector_t first_level[FIRSTLEVEL_SIZE]; /* First level blocks. */
    block_sector_t indir_level;       /* Indirect sector. */
    block_sector_t doub_indir_level;  /* Doubly-indirect sector. */
  };

/* Indirect and doubly-indirect sector blocks.  Each sector is
   BLOCK_SECTOR_SIZE bytes long: 4*INDIRECT_SIZE = 512. */
struct indir_doub_indir_sectors
  {
    /* Indirect and doubly indirect blocks. */
    block_sector_t indir_blocks[INDIR_DOUB_SIZE];
  };

static struct list open_inodes; /* List of open inodes, so that opening a
                                   single inode twice returns the same
                                   'struct inode'. */
struct lock open_inodes_lock; /* Lock for open_inodes list. */

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Returns the block device sector that contains byte offset POS
   within INODE by searching the inode hierarchy for the block. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  ASSERT ((unsigned) pos < MAX_BLOCK * BLOCK_SECTOR_SIZE);

  /* Retrieve the inode_disk object associated with the inode. */
  struct inode_disk new_idisk;
  cache_read (inode->sector, &new_idisk, BLOCK_SECTOR_SIZE, 0);
  block_sector_t new_sector = block_lookup (&new_idisk,
                              pos / BLOCK_SECTOR_SIZE);
  return new_sector;
}

/* Search for an inode's block.  Depending on the block's position,
   the block might be located in the first level, the indirect level,
   or the doubly-indirect level. */
static block_sector_t
block_lookup (struct inode_disk *idisk, unsigned block_loc)
{
  /* Ensure block_loc does not exceed system constraints. */
  ASSERT (block_loc < MAX_BLOCK);

  if (block_loc < FIRSTLEVEL_SIZE) /* First level. */
    return idisk->first_level[block_loc];
  else if (block_loc < FIRSTLEVEL_SIZE + INDIR_DOUB_SIZE)
    return indirect_lookup (idisk, block_loc);
  else
    return doub_indir_lookup (idisk, block_loc);

  /* Unreachable code.  Prevents compiler warnings. */
  return (MAX_BLOCK + 1);
}

/* Search for an inode's block in the indirect level. */
static block_sector_t
indirect_lookup (struct inode_disk *idisk, unsigned block_loc)
{
  struct indir_doub_indir_sectors new_indir_sect;
  cache_read (idisk->indir_level, &new_indir_sect,
              BLOCK_SECTOR_SIZE, 0);
  return new_indir_sect.indir_blocks[block_loc - FIRSTLEVEL_SIZE];
}

/* Search for an inode's block in the doubly-indirect level. */
static block_sector_t
doub_indir_lookup (struct inode_disk *idisk, unsigned block_loc)
{
  struct indir_doub_indir_sectors new_doubindir_sect;
  cache_read (idisk->doub_indir_level, &new_doubindir_sect,
              BLOCK_SECTOR_SIZE, 0);

  /* Calculate doubly indirect and indirect entry. */
  int doubly_indir_entry = (block_loc - (FIRSTLEVEL_SIZE + INDIR_DOUB_SIZE))
                           / INDIR_DOUB_SIZE;
  block_sector_t indir_entry = new_doubindir_sect.
                               indir_blocks[doubly_indir_entry];

  struct indir_doub_indir_sectors new_indir_sect;
  cache_read (indir_entry, &new_indir_sect, BLOCK_SECTOR_SIZE, 0);
  int indir_block = (block_loc - (FIRSTLEVEL_SIZE + INDIR_DOUB_SIZE))
                    % INDIR_DOUB_SIZE;

  block_sector_t next_block = new_indir_sect.indir_blocks[indir_block];
  return next_block;
}

/* Returns the status of the inode (false if it is a directory,
   true if it is a file). */
bool
inode_is_file (const struct inode *inode)
{
  ASSERT (inode != NULL);

  struct inode_disk status_idisk;
  cache_read (inode->sector, &status_idisk, BLOCK_SECTOR_SIZE, 0);

  if (status_idisk.is_file == 0)
    return false;
  else
    return true;
}

/* Returns true if the inode has been deleted and is no longer in use. */
bool
inode_is_removed (struct inode *inode)
{
  ASSERT (inode != NULL);
  return inode->removed;
}

/* Initializes the inode module. */
void
inode_init (void)
{
  cache_init ();
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, unsigned is_file)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_file = is_file;
      disk_inode->num_blocks = 0;

      success = true;
      if (sectors > 0)
        {
          size_t i;
          for (i = 0; i < sectors; i++)
            {
              success = file_block_growth (disk_inode);
              if (!success)
                break;
            }
        }

      /* Write the disk_inode into the cache. */
      cache_write (sector, disk_inode, BLOCK_SECTOR_SIZE, 0);
    }
  free (disk_inode);
  return success;
}

/* Reads an inode from SECTOR
   and returns a 'struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire (&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          lock_release (&open_inodes_lock);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    {
      lock_release (&open_inodes_lock);
      return NULL;
    }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);

  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->inode_lock);
  lock_release (&open_inodes_lock);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      inode_grab_lock (inode);
      inode->open_cnt++;
      inode_release_lock (inode);
    }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  ASSERT (inode != NULL);
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire (&open_inodes_lock);
  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      lock_release (&open_inodes_lock);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          /* Read in and free all of a file's blocks in its inode
             hierarchy. */
          struct inode_disk close_idisk;
          cache_read (inode->sector, &close_idisk,
            BLOCK_SECTOR_SIZE, 0);

          size_t b;
          block_sector_t close_block;
          for (b = 0; b < close_idisk.num_blocks; b++)
            {
              close_block = block_lookup (&close_idisk, b);
              free_map_release (close_block, 1);
            }

          free_map_release (inode->sector, 1);
        }
      free (inode);
    }
  else
    lock_release (&open_inodes_lock);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode_grab_lock (inode);
  inode->removed = true;
  inode_release_lock (inode);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  ASSERT (inode != NULL);
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  bool lock_success = false;

  /* Determine whether the process is about to read past the end of the
     file.  If so, it should proceed atomically. */
  struct inode_disk new_idisk;
  cache_read (inode->sector, &new_idisk, BLOCK_SECTOR_SIZE, 0);

  /* Acquire the lock if the process is trying to read past the end of
     the file. */
  if ((unsigned) (size + offset) > new_idisk.length)
    lock_success = inode_grab_lock (inode);

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cache_read (sector_idx, buffer + bytes_read, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  /* If it attempted to read past the end of the file, it should release
     the inode's lock. */
  if ((unsigned) (size + offset) > new_idisk.length)
    {
      if (lock_success)
        inode_release_lock (inode);
    }

  /* If we are not yet at the end of the file, have the readahead
     thread fetch the next block. */
  if ((BLOCK_SECTOR_SIZE + offset) < inode_length (inode))
    {
      lock_acquire (&readahead_lock);
      next_readahead_entry = next_readahead_entry % READAHEAD_SIZE;
      int ra_sector = (int) byte_to_sector (inode,
                                            offset + BLOCK_SECTOR_SIZE);
      readahead_list[next_readahead_entry] = ra_sector;
      next_readahead_entry++;
      cond_signal (&readahead_cond, &readahead_lock);
      lock_release (&readahead_lock);
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   If the intended writing destination exceeds the current file
   size, grow the file before writing to it. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  ASSERT (inode != NULL);
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  bool lock_success = false;

  if (inode->deny_write_cnt)
    return 0;

  /* Determine whether file growth is necessary. */
  struct inode_disk new_idisk;
  cache_read (inode->sector, &new_idisk, BLOCK_SECTOR_SIZE, 0);
  bool file_grown = false;

  /* If file growth is needed, proceed to grow and write to the file
     atomically. */
  if ((uint32_t) (offset + size) > new_idisk.length)
    {
      lock_success = inode_grab_lock (inode);
      
      /* Check if we still need to grow the file.  It is possible that
         while the process was waiting for the lock, the file was
         already extended by another process.  Thus, a second file
         extension is not needed. */
      if (((uint32_t) (offset + size) > new_idisk.length))
        {
          uint32_t curr_blocks = new_idisk.num_blocks;
          int file_extended = (((int) (offset + size) -
                              (int) curr_blocks * BLOCK_SECTOR_SIZE)
                              / BLOCK_SECTOR_SIZE) + 1;
          int file_block_grown = ((int) (offset + size) -
                                 (int) curr_blocks * BLOCK_SECTOR_SIZE);
          if (file_extended > 0 && file_block_grown > 0)
            {
              bool grown_success = file_grow (&new_idisk,
                                             (unsigned) file_extended);

              /* If the necessary number of blocks could not be allocated,
                 return that 0 bytes were written. */
              if (!grown_success)
                {
                  if (lock_success)
                    inode_release_lock (inode);
                  return 0;
                }
            }

          /* Update file size at the end. */
          new_idisk.length = offset + size;
          cache_write (inode->sector, &new_idisk, BLOCK_SECTOR_SIZE, 0);
          file_grown = true;          
        }
      else
        {
          if (lock_success)
            inode_release_lock (inode);
          lock_success = false;
        }
    }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < sector_left ? size : sector_left;
      if (chunk_size <= 0)
        break;
      
      cache_write (sector_idx, (void *) buffer + bytes_written,
        chunk_size, sector_ofs);
        
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

    /* If the file was grown, release the lock associated with the inode. */
  if (file_grown)
    {
      if (lock_success)
        inode_release_lock (inode);
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode_grab_lock (inode);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode_release_lock (inode);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode != NULL);

  inode_grab_lock (inode);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  inode_release_lock (inode);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  ASSERT (inode != NULL);
  bool lock_success = inode_grab_lock ((struct inode *) inode);
  struct inode_disk length_idisk;
  cache_read (inode->sector, &length_idisk, BLOCK_SECTOR_SIZE, 0);
  if (lock_success)
    inode_release_lock ((struct inode *) inode);
  return (off_t) length_idisk.length;
}

/* Grows a file by calling file_block_grow until the number of necessary
   blocks have been allocated for inode_write_at to successfully write
   to the file.  Returns false if a block was unable to be allocated
   (most likely due to exceeding the disk size). */
static bool
file_grow (struct inode_disk *disk_inode, unsigned num_grow_blocks)
{
  unsigned i = 0;
  for (; i < num_grow_blocks; i++)
    {
      bool success = file_block_growth (disk_inode);
      if (!success)
        return false;
    }
  return true;
}

/* Grow a file by one block.  Determines what level the new block should
   be placed and allocates a new slot for it in the free map before
   adding it to the cache.  Returns true if successful, false if unable
   to allocate a new slot in the free map. */
static bool
file_block_growth (struct inode_disk *disk_inode)
{
  uint32_t inode_blocks = disk_inode->num_blocks;
  ASSERT ((unsigned) inode_blocks <= MAX_BLOCK);
  block_sector_t new_sector = (MAX_BLOCK + 1);

  /* First check if we need to set up the indirect or doubly-indirect
     levels before proceeding to check where the next block should live. */

   /* Indirect setup. */
  if (inode_blocks == FIRSTLEVEL_SIZE)
    {
      new_sector = allocate_new_block ();
      if (new_sector == (block_sector_t) (MAX_BLOCK + 1))
        return false;
      disk_inode->indir_level = new_sector;
    }
  /* Doubly-indirect setup. */
  else if (inode_blocks == (FIRSTLEVEL_SIZE + INDIR_DOUB_SIZE))
    {
      new_sector = allocate_new_block ();
      if (new_sector == (block_sector_t) (MAX_BLOCK + 1))
        return false;
      disk_inode->doub_indir_level = new_sector;
    }

  /* Find where block should live and allocate it. */
  if (inode_blocks < FIRSTLEVEL_SIZE)
    {
      new_sector = allocate_new_block ();
      if (new_sector == (block_sector_t) (MAX_BLOCK + 1))
        return false;
      disk_inode->first_level[inode_blocks] = new_sector;
    }
  else if (inode_blocks < (FIRSTLEVEL_SIZE + INDIR_DOUB_SIZE))
    {
      struct indir_doub_indir_sectors new_indir_sect;
      cache_read (disk_inode->indir_level, &new_indir_sect,
                  BLOCK_SECTOR_SIZE, 0);
      new_sector = allocate_new_block ();
      if (new_sector == (block_sector_t) (MAX_BLOCK + 1))
        return false;
      new_indir_sect.
        indir_blocks[inode_blocks - FIRSTLEVEL_SIZE] = new_sector;
      cache_write (disk_inode->indir_level, &new_indir_sect,
                  BLOCK_SECTOR_SIZE, 0);
    }
  else /* Lives in the doubly-indirect level. */
    {
      struct indir_doub_indir_sectors new_doubindir;
      cache_read (disk_inode->doub_indir_level,
        &new_doubindir, BLOCK_SECTOR_SIZE, 0);

      /* Calculate entry locations for both the doubly-indirect and the
         indirect level. */
      int doubly_indir_entry = (inode_blocks - (FIRSTLEVEL_SIZE +
                               INDIR_DOUB_SIZE)) / INDIR_DOUB_SIZE;
      int indir_entry = (inode_blocks - (FIRSTLEVEL_SIZE + INDIR_DOUB_SIZE))
                        % INDIR_DOUB_SIZE;

      /* If zero, we need to set up the doubly-indirect level's indirect
         level first. */
      if (indir_entry == 0)
        {
          new_sector = allocate_new_block ();
          if (new_sector == (block_sector_t) (MAX_BLOCK + 1))
            return false;
          new_doubindir.indir_blocks[doubly_indir_entry] = new_sector;
          cache_write (disk_inode->doub_indir_level,
            &new_doubindir, BLOCK_SECTOR_SIZE, 0);
        }
        
      block_sector_t indir_block =
        new_doubindir.indir_blocks[doubly_indir_entry];
      cache_read (indir_block, &new_doubindir, BLOCK_SECTOR_SIZE, 0);

      new_sector = allocate_new_block ();
      if (new_sector == (block_sector_t) (MAX_BLOCK + 1))
        return false;

      new_doubindir.indir_blocks[indir_entry] = new_sector;
      cache_write (indir_block, &new_doubindir, BLOCK_SECTOR_SIZE, 0);
    }

    /* Allocation was successful. */
    disk_inode->num_blocks++;
    return true;
}

/* Allocate a new block in the free map and add it to the cache.
   Returns the new block sector if successful and (MAX_BLOCK + 1)
   if a new block could not be allocated. */
static block_sector_t
allocate_new_block (void)
{
  block_sector_t new_block;
  bool success = free_map_allocate (1, &new_block);
  if (!success)
    return (block_sector_t) (MAX_BLOCK + 1);
  else
    {
      /* Create a new buffer and zero it out before writing it to
         the cache. */
      struct indir_doub_indir_sectors dummy_buffer;
      memset (&dummy_buffer, 0, BLOCK_SECTOR_SIZE);
      cache_write (new_block, &dummy_buffer, BLOCK_SECTOR_SIZE, 0);
    }

  return new_block;
}

/* Grab the inode's lock if it does not already hold it.  Returns
   false if the inode's lock was already held (which means it was
   acquired in a different function and is needed later on). */
static bool
inode_grab_lock (struct inode *inode)
{
  if (lock_held_by_current_thread (&inode->inode_lock))
    return false;
  else
    {
      lock_acquire (&inode->inode_lock);
      return true;
    }
}

/* Release the inode's lock.  Note that this function does not require
   any return value since it will only be called by the last function in
   the event of a nested function call sequence that may require grabbing
   the same lock. */
static void
inode_release_lock (struct inode *inode)
{
  lock_release (&inode->inode_lock);
}
