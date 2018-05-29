#include "threads/synch.h"
#include <bitmap.h>
#include <list.h>
#include "vm/swap.h"
#include "threads/vaddr.h"

struct lock swap_lock;
struct bitmap *swap_bitmap;
struct list lru_list;


void swap_init (void) {
  lock_init (&swap_init);
  swap_bitmap = bitmap_create (SWAP_SIZE / PGSIZE);
}

void lru_list_init (void) {
  list_init (&lru_list);
}
