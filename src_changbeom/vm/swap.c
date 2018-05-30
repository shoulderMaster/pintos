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
  lock_init (&swap_init);
  swap_bitmap = bitmap_create (SWAP_SIZE / PGSIZE);
  swap_block = block_get_role (BLOCK_SWAP);
}

size_t swap_out (void *kaddr) {
  size_t swap_index = 0;
  int i = 0;
  lock_acquire (&swap_lock);
  swap_index = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
  lock_acquire (&rw_lock);
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_write (swap_block, swap_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
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
  for (i = 0; i < SECTORS_PER_PAGE; i++)  {
    block_read (swap_block, sector_index + i, kaddr + BLOCK_SECTOR_SIZE * i);
  }
  bitmap_set (swap_bitmap, used_index, false);
  lock_release (&rw_lock);
  lock_release (&swap_lock);
}
