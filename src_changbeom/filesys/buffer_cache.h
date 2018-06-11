#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "devices/block.h"
#include "threads/synch.h"
#include "filesys/off_t.h"


/* buffer cache의 각 entry를 관리하기 위한 구조체 */
struct buffer_head {
  /* 해당 entry가 dirty인지를 나타내는 flag */
  bool dirty;
  /* 해당 entry의 사용 여부를 나타내는 flag */ 
  bool in_use;
  /* 해당 entry의 disk sector 주소 */  
  block_sector_t sector;
  /* clock algorithm을 위한 clock bit */  
  bool clock_bit;
  /* lock 변수 (struct lock) */  
  struct lock bc_lock;
  /* buffer cache entry를 가리키기 위한 데이터 포인터 */   
  void *bc_entry;
};

void bc_init (void);
bool bc_read (block_sector_t sector_idx, void *buffer, off_t bytes_read, int chunk_size, int sector_ofs);
bool bc_write (block_sector_t sector_idx, void *buffer, off_t bytes_written, int chunk_size, int sector_ofs);

#endif
