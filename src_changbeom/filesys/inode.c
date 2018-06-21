#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/buffer_cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* direct_map_table의 엔트리 개수 */
#define DIRECT_BLOCK_ENTRIES 124
/* indirect_map_table의 엔트리 개수 */
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  off_t length;                       /* File size in bytes. */
  block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
  block_sector_t indirect_block_sec;
  block_sector_t double_indirect_block_sec;
  unsigned magic;                     /* Magic number. */
};

enum direct_t {
  NORMAL_DIRECT,
  INDIRECT,
  DOUBLE_INDIRECT,
  OUT_LIMIT
};

struct sector_location {
  int directness;
  int index1;
  int index2;
};

struct inode_indirect_block {
  block_sector_t map_table[INDIRECT_BLOCK_ENTRIES];
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock extend_lock;
};

static bool get_disk_inode (const struct inode *inode, struct inode_disk *inode_disk) {
  /*  inode->sector에 해당하는 on-disk inode를 buffer cache에서
      읽어 inode_disk에 저장 (bc_read() 함수 사용) */
  bc_read (inode->sector, inode_disk, 0, sizeof (struct inode_disk), 0);
  /*  true 반환 */
  return true;
}

static void locate_byte (off_t pos, struct sector_location *sec_loc)
{
  off_t pos_sector = pos / BLOCK_SECTOR_SIZE;
  /*  Direct 방식일 경우 */
  if (pos_sector < DIRECT_BLOCK_ENTRIES) {
    //sec_loc 자료구조의 변수 값 업데이트(구현)
    sec_loc->directness = NORMAL_DIRECT;
    sec_loc->index1 = pos_sector;
  } else if (pos_sector < (off_t)(INDIRECT_BLOCK_ENTRIES + DIRECT_BLOCK_ENTRIES)) {
    sec_loc->directness = INDIRECT;
    sec_loc->index1 = (pos_sector - DIRECT_BLOCK_ENTRIES);
  } else if (pos_sector < (off_t)(INDIRECT_BLOCK_ENTRIES * (INDIRECT_BLOCK_ENTRIES + 1) + DIRECT_BLOCK_ENTRIES)) {
    sec_loc->directness = DOUBLE_INDIRECT;
    sec_loc->index1 = (pos_sector - (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES)) / INDIRECT_BLOCK_ENTRIES;
    sec_loc->index2 = (pos_sector - (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES)) % INDIRECT_BLOCK_ENTRIES;
  } else {
    sec_loc->directness = OUT_LIMIT;
  }
  return;
}

static bool register_sector (struct inode_disk *inode_disk, 
    block_sector_t new_sector, struct sector_location sec_loc) 
{
  struct inode_indirect_block *new_block = NULL, *double_indirect_directory_block = NULL;
  bool to_write = false;
  locate_byte (new_sector, &sec_loc);
  switch (sec_loc.directness)
  {
    case NORMAL_DIRECT:
      /*  inode_disk에 새로 할당받은 디스크 번호 업데이트 */
      inode_disk->direct_map_table[sec_loc.index1] = new_sector;
      break;

    case INDIRECT:
      new_block = (struct inode_indirect_block*)malloc (BLOCK_SECTOR_SIZE);
      if (new_block == NULL)
        return false;
      /*  인덱스 블록에 새로 할당 받은 블록 번호 저장 */
      if (inode_disk->indirect_block_sec == (block_sector_t)-1) {
        if (!free_map_allocate (1, &inode_disk->indirect_block_sec))
          return false;
        memset (new_block, 0xFF, sizeof (struct inode_indirect_block));
      } else {
        bc_read (inode_disk->indirect_block_sec, new_block, 0, BLOCK_SECTOR_SIZE, 0);
      }

      new_block->map_table[sec_loc.index1] = new_sector;

      /*  인덱스 블록을 buffer cache에 기록 */
      bc_write (new_sector, new_block, 0, BLOCK_SECTOR_SIZE, 0);
      break;

    case DOUBLE_INDIRECT:
      double_indirect_directory_block = (struct inode_indirect_block*)malloc (BLOCK_SECTOR_SIZE);
      if (new_block == NULL)
        return false;
      /*  2차 인덱스 블록에 새로 할당 받은 블록 주소 저장 후,
       각 인덱스 블록을 buffer cache에 기록 */
      if (inode_disk->double_indirect_block_sec == (block_sector_t)-1) {
        if (!free_map_allocate (1, &inode_disk->double_indirect_block_sec))
          return false;
        memset (double_indirect_directory_block, 0xFF, sizeof (struct inode_indirect_block));
      } else {
        bc_read (inode_disk->double_indirect_block_sec, double_indirect_directory_block, 0, sizeof (struct inode_indirect_block), 0);
      }

      new_block = (struct inode_indirect_block*)malloc (BLOCK_SECTOR_SIZE);
      if (new_block == NULL)
        return false;

      if (double_indirect_directory_block->map_table[sec_loc.index1] == (block_sector_t)-1) {
        if (!free_map_allocate (1, &double_indirect_directory_block->map_table[sec_loc.index1]))
          return false;
        memset (new_block, 0xFF, sizeof (struct inode_indirect_block));
        to_write = true;
      } else {
        bc_read (double_indirect_directory_block->map_table[sec_loc.index1], new_block, 0, BLOCK_SECTOR_SIZE, 0);
      }
      
      ASSERT (new_block->map_table[sec_loc.index2] == (block_sector_t)-1);
      new_block->map_table[sec_loc.index2] = new_sector;

      if (to_write == true) {
        bc_write (inode_disk->double_indirect_block_sec, double_indirect_directory_block, 0, BLOCK_SECTOR_SIZE, 0);
        free (double_indirect_directory_block);
      }

      /*  인덱스 블록을 buffer cache에 기록 */
      bc_write (new_sector, new_block, 0, BLOCK_SECTOR_SIZE, 0);
      break;

    default:
      return false;
  }
  free(new_block);
  return true;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode_disk *inode_disk, off_t pos) 
{
  block_sector_t result_sec = -1;

  if (pos < inode_disk->length) {
    struct inode_indirect_block *ind_block;
    struct sector_location sec_loc;
    locate_byte(pos, &sec_loc); // 인덱스 블록 offset 계산

    switch (sec_loc.directness) {

      /*  Direct 방식일 경우 */
      case NORMAL_DIRECT :
        /*  on-disk inode의 direct_map_table에서 디스크 블록 번호를 얻음 */
        result_sec = inode_disk->direct_map_table[sec_loc.index1];
        break;

        /*  Indirect 방식일 경우 */
      case INDIRECT :
        ind_block = (struct inode_indirect_block*)malloc (BLOCK_SECTOR_SIZE);
        if (ind_block) {
          /*  buffer cache에서 인덱스 블록을 읽어 옴 */
          bc_read (inode_disk->indirect_block_sec, ind_block, 0, BLOCK_SECTOR_SIZE, 0);
          
          /*  인덱스 블록에서 디스크 블록 번호 확인 */
          result_sec = ind_block->map_table[sec_loc.index1];
        } else {
          NOT_REACHED ();
        }
        free (ind_block);
        break;

        /*  Double indirect 방식일 경우 */
      case DOUBLE_INDIRECT :
        ind_block = (struct inode_indirect_block *)malloc (BLOCK_SECTOR_SIZE);
        if (ind_block){
          /*  1차 인덱스 블록을 buffer cache에서 읽음 */
          bc_read (inode_disk->double_indirect_block_sec, ind_block, 0, BLOCK_SECTOR_SIZE, 0);

          /*  2차 인덱스 블록을 buffer cache에서 읽음 */
          ASSERT (ind_block->map_table[sec_loc.index1] != -1);
          bc_read (ind_block->map_table[sec_loc.index1], ind_block, 0, BLOCK_SECTOR_SIZE, 0);

          /*  2차 인덱스 블록에서 디스크 블록 번호 확인 */
          result_sec = ind_block->map_table[sec_loc.index2];
        } else {
          NOT_REACHED ();
        }
        free (ind_block);
        break;

      default :
        NOT_REACHED ();

    }
  }

  return result_sec;
}

bool inode_update_file_length (struct inode_disk* inode_disk, off_t start_pos, off_t end_pos) {

  off_t size = end_pos - (start_pos - 1);
  off_t offset = start_pos;
  void *zeroes = NULL;
  struct sector_location sec_loc;
  memset (&sec_loc, 0x00, sizeof (struct sector_location));
  zeroes = calloc (sizeof (char), BLOCK_SECTOR_SIZE);

  /*  블록 단위로 loop을 수행하며 새로운 디스크 블록 할당 */
  while (size > 0){

    /*  디스크 블록 내 오프셋 계산 */
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;
    int chunk_size = BLOCK_SECTOR_SIZE - sector_ofs;
    off_t sector_idx = 0;

    if (sector_ofs > 0) {
      /*  블록 오프셋이 0보다 클 경우, 이미 할당된 블록 */
    } else {
      /*  새로운 디스크 블록을 할당 */
      if (free_map_allocate (1, &sector_idx)) {
        locate_byte (offset, &sec_loc);

        /*  inode_disk에 새로 할당 받은 디스크 블록 번호 업데이트 */
        if (!register_sector (inode_disk, sector_idx, sec_loc)) {
          free (zeroes);
          return false;
        }

      } else {
        free (zeroes);
        return false;
      }
      /*  새로운 디스크 블록을 0으로 초기화 */
      bc_write (sector_idx, zeroes, 0, BLOCK_SECTOR_SIZE, 0);
    }
    /*  Advance. */
    size -= chunk_size;
    offset += chunk_size;

  }
  free (zeroes);
  return true;
}

static void free_inode_sectors (struct inode_disk *inode_disk){
  struct inode_indirect_block *ind_block_1 = NULL;
  struct inode_indirect_block *ind_block_2 = NULL;
  int i = 0, j = 0;

  ind_block_1 = (struct indirect_block_sec*)malloc (BLOCK_SECTOR_SIZE);
  ind_block_2 = (struct indirect_block_sec*)malloc (BLOCK_SECTOR_SIZE);

  /*  Double indirect 방식으로 할당된 블록 해지 */
  if (inode_disk->double_indirect_block_sec > 0) {
    /*  1차 인덱스 블록을 buffer cache에서 읽음 */
    bc_read (inode_disk->double_indirect_block_sec, ind_block_1, 0, BLOCK_SECTOR_SIZE, 0);
    i = 0;
    /*  1차 인덱스 블록을 통해 2차 인덱스 블록을 차례로 접근 */
    while (ind_block_1->map_table[i] > 0) {
      /*  2차 인덱스 블록을 buffer cache에서 읽음 */
      j = 0;
      bc_read (ind_block_2->map_table[j], ind_block_2, 0, BLOCK_SECTOR_SIZE, 0);
      /*  2차 인덱스 블록에 저장된 디스크 블록 번호를 접근 */
      while (ind_block_2->map_table[j] > 0) {
        /*  free_map 업데이틀 통해 디스크 블록 할당 해지 */
        free_map_release (ind_block_2->map_table[j], 1);
        j++;
      }
      /*  2차 인덱스 블록 할당 해지 */
      free_map_release (ind_block_1->map_table[i], 1);
      i++;
    }
  }

  /*  1차 인덱스 블록 할당 해지 */
  /*  Indirect 방식으로 할당된 디스크 블록 해지*/
  if(inode_disk->indirect_block_sec > 0){
    /*  인덱스 블록을 buffer cache에서 읽음 */
    bc_read (inode_disk->indirect_block_sec, ind_block_1, 0, BLOCK_SECTOR_SIZE, 0);
    i = 0;
    /*  인덱스 블록에 저장된 디스크 블록 번호를 접근 */
    while(ind_block_1->map_table[i] > 0){
      /*  free_map 업데이트를 통해 디스크 블록 할당 해지 */
      free_map_release (ind_block_1->map_table[i], 1);
      i++;
    }
    /*  Direct 방식으로 할당된 디스크 블록 해지*/
    while (inode_disk->direct_map_table[i] > 0){
      /*  free_map 업데이트를 통해 디스크 블록 할당 해지 */
      free_map_release (inode_disk->direct_map_table[i], 1);
      i++;
    }
  }

  free (ind_block_1);
  free (ind_block_2);

}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  ASSERT (sizeof (struct inode_disk) == BLOCK_SECTOR_SIZE);
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = (struct inode_disk*)malloc (sizeof (struct inode_disk));

  if (disk_inode != NULL) {

    size_t sectors = bytes_to_sectors (length);
    memset (disk_inode, 0xFF, sizeof (struct inode_disk));
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;

    if (length > 0) {
      /*  length 만큼의 디스크 블록을 inode_updafe_file_length()를 호출하여 할당 */
      inode_update_file_length (disk_inode, 0, length - 1);
    }

    /*  on—disk inode를 bc_write()를 통해buffer cache에 기록 */
    bc_write (sector, disk_inode, 0, sizeof (struct inode_disk), 0);

    /*  할당받은 disk_inode 변수 해제 */
    free (disk_inode);

    /*  success 변수 update */
    success = true;
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open (block_sector_t sector) {

  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
      e = list_next (e)) 
  {
    inode = list_entry (e, struct inode, elem);
    if (inode->sector == sector) 
    {
      inode_reopen (inode);
      return inode; 
    }
  }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  lock_init (&inode->extend_lock);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
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

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); 
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
  off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  //uint8_t *bounce = NULL;

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

    /*  if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
    //block_read (fs_device, sector_idx, buffer + bytes_read);
    bc_read (sector_idx, buffer, bytes_read, chunk_size);
    }
    else 
    {
    if (bounce == NULL) 
    {
    bounce = malloc (BLOCK_SECTOR_SIZE);
    if (bounce == NULL)
    break;
    }
    //block_read (fs_device, sector_idx, bounce);

    memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
    } */

    bc_read (sector_idx, buffer, bytes_read, chunk_size, sector_ofs);

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  //free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  //uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* // 한 block을 모두 꽉채워 write하는 경우
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      // 섹터의 일부만 write하는 경우 sector의 일부만 write할 수 없기 때문에 이렇게 처리함
      else 
        {
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else // sector offset이 0인 경우 
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        } */

      bc_write (sector_idx, buffer, bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  //free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
