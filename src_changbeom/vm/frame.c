#include <list.h>
#include "threads/synch.h"
#include "vm/page.h"
#include "vm/frame.h"

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

static struct list_elem *get_next_lru_clock () {
  if (lru_clock == NULL) {
    if (list_empty (&lru_list))
      return NULL;
    else
      lru_clock = list_begin (&lru_list);
  } else {
    do {
      if (lru_clock == list_end (&lru_list))
        lru_clock = list_begin (&lru_list);
      else 
        lru_clock = list_next (lru_clock);
    } while (lru_clock == list_end (&lru_list));
  }
  return lru_clock;
}
