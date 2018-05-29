#include <list.h>
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/palloc.h"
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
  lock_release (&lru_lock);
}

void del_page_from_lru_list (struct page *page) {
  lock_acquire (&lru_lock);
  if (lru_clock == &page->lru) {
    lru_clock = list_next (&page->lru);
  }
  list_remove (&page->lru);
  lock_release (&lru_lock);
}

static struct list_elem *get_next_lru_clock (void) {
  lock_acquire (&lru_lock);
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
  lock_release (&lru_lock);
  return lru_clock;
}

void try_to_free_pages (enum palloc_flags flags) {
  struct page *victim_page = NULL;
  while (1) {
    struct list_elem *elem = get_next_lru_clock ();
    victim_page = (struct page*)list_entry (elem, struct page, lru);
    if (pagedir_is_accessed (victim_page->thread->pagedir, victim_page->vme->vaddr)) {
      pagedir_set_accessed (victim_page->thread->pagedir, victim_page->vme->vaddr, false);
    } else {
      /* victim_page found */
      break;
    }
  }
  /* swap 구현 */
  
}
