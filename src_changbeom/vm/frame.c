#include <list.h>
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/palloc.h"
#include "vm/frame.h"
#include "userprog/syscall.h"

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
  if (lru_clock == &page->lru) {
    lru_clock = list_next (&page->lru);
  }
  list_remove (&page->lru);
}

static struct list_elem *get_next_lru_clock (void) {
  //lock_acquire (&lru_lock);
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
  //lock_release (&lru_lock);
  return lru_clock;
}

void *try_to_free_pages (enum palloc_flags flags) {
  struct page *victim_page = NULL;
  lock_acquire (&lru_lock);
  while (1) {
    struct list_elem *elem = get_next_lru_clock ();
    victim_page = (struct page*)list_entry (elem, struct page, lru);
    if (pagedir_is_accessed (victim_page->thread->pagedir, victim_page->vme->vaddr)) {
      pagedir_set_accessed (victim_page->thread->pagedir, victim_page->vme->vaddr, false);
    } else if (pagedir_get_page (victim_page->thread->pagedir, victim_page->vme->vaddr) != NULL) {
      /* victim_page found */
      break;
    }
  }

  //printf ("vme : %p | swap_out  | %s | read : %d | zero : %d | tid : %d\n", victim_page->vme->vaddr, victim_page->vme->type == 0 ? "VM_BIN" : (victim_page->vme->type == 1 ? "VM_FILE" : "VM_ANON"), victim_page->vme->read_bytes, victim_page->vme->zero_bytes, thread_current ()->tid);
  if (victim_page->vme->type == VM_ANON) {
    victim_page->vme->swap_slot = swap_out (victim_page->kaddr);
  } else if (pagedir_is_dirty (victim_page->thread->pagedir, victim_page->vme->vaddr)) {
    switch (victim_page->vme->type) {
      case VM_FILE :
        lock_acquire (&rw_lock);
        file_write_at (victim_page->vme->file, victim_page->vme->vaddr,
                       victim_page->vme->read_bytes, victim_page->vme->offset);
        lock_release (&rw_lock);
        break;
      case VM_BIN :
        victim_page->vme->swap_slot = swap_out (victim_page->kaddr);
        victim_page->vme->type = VM_ANON;
        break;
    }
  }
  victim_page->vme->is_loaded = false;
  __free_page (victim_page);
  lock_release (&lru_lock);

  return palloc_get_page (flags);
}
