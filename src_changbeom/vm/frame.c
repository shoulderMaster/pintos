#include <list.h>
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/palloc.h"
#include "vm/frame.h"
#include "userprog/syscall.h"
#include "threads/vaddr.h"

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
  /* lru_clock이 NULL로 초기화 되어 있을 경우 lru_list의 맨 처음 멤버를 가져온다.
     list_elem이 비어있는 경우는 NULL을 리턴한다.*/
  if (lru_clock == NULL) {
    if (list_empty (&lru_list)) {
      return NULL;
    } else {
      lru_clock = list_begin (&lru_list);
    }
  } else {
    /* lru_clock이 list의 tail을 가리키지 않도록 clock을 다음 번째 elem으로 옮긴다 */
    do {
      if (lru_clock == list_end (&lru_list)) {
        lru_clock = list_begin (&lru_list);
      } else  {
        lru_clock = list_next (lru_clock);
      }
    } while (lru_clock == list_end (&lru_list));
  }
  return lru_clock;
}

void *try_to_free_pages (enum palloc_flags flags) {
  struct page *victim_page = NULL;
  void *kaddr = NULL;
  /* lru_list에서 victim page를 선정하고 해제 한다음 새 페이지 할당까지 atomic하게 수행하기 위해 lock으로 보호한다 */
  /* victim page를 찾는다
     해당 page의 vaddr에 해당하는 PTE가 access bit가 0인 경우에 재사용률이 낮다고 간주하여
     victim page로 선정한다.*/
  while (1) {
    struct list_elem *elem = get_next_lru_clock ();
    victim_page = (struct page*)list_entry (elem, struct page, lru);
    ASSERT (victim_page->vme);
    //ASSERT (victim_page->thread->magic == 0xcd6abf4b);
    if(!victim_page->thread->pagedir || victim_page->thread->magic != 0xcd6abf4b) {
      printf ("%d %s %p", victim_page->thread->tid, victim_page->thread->name, victim_page->thread);
    }
    ASSERT(pagedir_get_page (victim_page->thread->pagedir, victim_page->vme->vaddr) == victim_page->kaddr);
    ASSERT (victim_page->vme->is_loaded);
    if (pagedir_is_accessed (victim_page->thread->pagedir, victim_page->vme->vaddr)) {
      pagedir_set_accessed (victim_page->thread->pagedir, victim_page->vme->vaddr, false);
    } else {
      /* victim_page found */
      break;
    }
  }

  /* victim page가 anonymous라면 무조건 swap partition으로 swap out한다 */
  if (victim_page->vme->type == VM_ANON) {
    victim_page->vme->swap_slot = swap_out (victim_page->kaddr);
  } else if (pagedir_is_dirty (victim_page->thread->pagedir, victim_page->vme->vaddr)) {
    /* victim페이지가 FILE이거나 BIN일때 dirty하다면 디스크에 swap out한다 */
    switch (victim_page->vme->type) {
      case VM_FILE :
        /* 일반 file인경우 swap partition에 swap out하는것보다 원래 파일에 wrtie-back 한다 */
        lock_acquire (&rw_lock);
        file_write_at (victim_page->vme->file, victim_page->kaddr,
                       victim_page->vme->read_bytes, victim_page->vme->offset);
        lock_release (&rw_lock);
        break;
      case VM_BIN :
        /* 실행파일은 프로세스 실행중에 write를 못한다. 
           정적 변수가 저장되는 data영역을 swap_out할 수 있도록 vme_type을 anonymous로 바꾼다 */
        victim_page->vme->swap_slot = swap_out (victim_page->kaddr);
        victim_page->vme->type = VM_ANON;
        break;
      default :
        NOT_REACHED ();
        break;
    }
  }
  victim_page->vme->is_loaded = false;
  __free_page (victim_page);
  kaddr = palloc_get_page (flags);
  ASSERT (kaddr);
  return kaddr;
}
