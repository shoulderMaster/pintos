#include <list.h>
#include "threads/synch.h"
#include "vm/page.h"

struct list lru_list;
struct lock lru_lock;
struct list_elem *lru_clock;

void lru_list_init (void) {
  list_init (&lru_list);
  lock_init (&lru_lock);
  lru_clock = NULL;
}

void add_page_to_lru_list (struct page *page) {
  lock_acquire (&lru_lock);
  list_push_back (&lru_list, &page->lru);
  lock_release (&lru_list);
}

void del_page_from_lru_list (struct page *page) {
  if (lru_clock == &page->lru) {
    lru_clock = &page->lru.next;
  }
  list_remove (&page->lru);
}
