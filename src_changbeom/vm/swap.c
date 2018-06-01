#include "threads/synch.h"
#include <bitmap.h>
#include "vm/swap.h"
#include "threads/vaddr.h"
#include "devices/block.h"
#include "userprog/syscall.h"

struct lock swap_lock;
struct bitmap *swap_bitmap;
struct block *swap_block;

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE) 

void swap_init (void) {
  lock_init (&swap_lock);
  swap_bitmap = bitmap_create (SWAP_SIZE / PGSIZE);
  /* swap block 구조체를 가져온다 */
  swap_block = block_get_role (BLOCK_SWAP);
}

size_t swap_out (void *kaddr) {
  size_t swap_index = 0, sector_index = 0;
  int i = 0;
  lock_acquire (&swap_lock);
  /* swap_index에 bitmap을 linear search하면서 0을 만나면 1로 바꾸고 해당 인덱스 리턴 first-fit */
  swap_index = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
  /* 한 페이지에 8개의 block이 사용 되므로 block index는 8배가 됨 */
  sector_index = swap_index << 3;
  lock_acquire (&rw_lock);
  /* sector_index로부터 8개 block에 페이지의 내용을 write함 */
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_write (swap_block, sector_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
  }
  lock_release (&rw_lock);
  lock_release (&swap_lock);
  return swap_index;
}

void swap_in (size_t used_index, void *kaddr) {
  int i = 0;
  size_t sector_index = used_index << 3;
  lock_acquire (&swap_lock);
  lock_acquire (&rw_lock);
  /* sector_index로부터 8개 block으로부터 페이지로 load함 */
  for (i = 0; i < SECTORS_PER_PAGE; i++)  {
    block_read (swap_block, sector_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
  }
  /* used index번째 8의 block묶음이 다른 페이지 아웃에ㅑ
     사용될 수 있도록 bitmap에 used_index bit을 reset함 */
  bitmap_set (swap_bitmap, used_index, false);
  lock_release (&rw_lock);
  lock_release (&swap_lock);
}
