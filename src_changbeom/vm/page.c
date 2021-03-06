#include "vm/page.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "lib/kernel/hash.h"
#include "vm/frame.h"
#include "filesys/file.h"
#include "threads/thread.h"
#include <string.h>

extern struct lock lru_lock;

bool load_file (void *kaddr, struct vm_entry *vme) {
  /* Using file_read_at()*/
  /* file_read_at으로 물리페이지에 read_bytes만큼 데이터를 씀*/
  /* file_read_at 여부 반환 */
  
  if (file_read_at (vme->file, kaddr, vme->read_bytes, vme->offset) != vme->read_bytes)
    return false;

  /* zero_bytes만큼 남는 부분을‘0’으로 패딩 */
  memset (kaddr + vme->read_bytes, 0, vme->zero_bytes);
  
  /*정상적으로 file을 메모리에 loading 하면 true 리턴*/
  return true;
}

void vm_init (struct hash *vm) {

  /* hash_init() 로 해시테이블 초기화 */
  /* 인자로 해시 테이블과 vm_hash_func과 vm_less_func 사용 */
  hash_init (vm, vm_hash_func, vm_less_func, NULL); 
}

static unsigned vm_hash_func (const struct hash_elem *e, void *aux) {

  /* hash_entry()로 element에 대한 vm_entry 구조체 검색*/
  struct vm_entry *ve = hash_entry (e, struct vm_entry, elem); 
  
  /* hash_int()를 이용하여 vm_entry의 멤버 vaddr에 대한 해시값을 구하고 반환 */
  return hash_int (ve->vaddr); 
}


static bool vm_less_func (const struct hash_elem *a, const struct hash_elem *b) {

  /* hash_entry()로 각각의 element에 대한 vm_entry 구조체를 얻은 후 vaddr값 비교.
     b가 크다면 true 반환.*/
  struct vm_entry *ve_a = hash_entry (a, struct vm_entry, elem);
  struct vm_entry *ve_b = hash_entry (b, struct vm_entry, elem); 

  return ve_a->vaddr < ve_b->vaddr;
}

bool insert_vme (struct hash *vm, struct vm_entry *vme) {
  /* hash_insert() 함수 이용하여 vm_entry를 해시 테이블에 삽입 */
  return !hash_insert (vm, &vme->elem);
}

bool delete_vme (struct hash *vm, struct vm_entry *vme) {
  /* hash_delete() 함수를 이용하여 vm_entry를 해시 테이블에서 제거 */
  return !!hash_delete (vm, &vme->elem);
}

struct vm_entry *find_vme (void *vaddr) {
  struct hash *vm = &thread_current ()->vm;
  struct vm_entry vme;

  /*  pg_round_down()으로 vaddr의 페이지 번호를 얻음 */
  vme.vaddr = pg_round_down (vaddr); 
  
  /*  hash_find() 함수를 사용해서 hash_elem 구조체 얻음 */
  struct hash_elem *elem = hash_find (vm, &vme.elem);
  
  /*  만약 존재하지 않는다면 NULL 리턴 */
  if (!elem) {
    return NULL;
  }

  /* 존재 하면  hash_entry()로 해당 hash_elem의 vm_entry 구조체 리턴 */
  return hash_entry (elem, struct vm_entry, elem); 
}

void vm_destroy (struct hash *vm) {
  /* hash_destroy()로 해시테이블의 버킷리스트와 vm_entry들을 제거 */
  hash_destroy (vm, vm_destroy_func);
}

void vm_destroy_func (struct hash_elem *e, void *aux) {
  
  /*  Get hash element (hash_entry() 사용) */
  struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);

  /*  load가 되어 있는 page의 vm_entry인 경우
   page의 할당 해제 및 page mapping 해제 (palloc_free_page()와
   pagedir_clear_page() 사용) */
  if (vme->is_loaded) {
    void *kaddr = pagedir_get_page (thread_current ()->pagedir, vme->vaddr);
    free_page (kaddr);
  }

  /*  vm_entry 객체 할당 해제 */
  delete_vme (&thread_current ()->vm, vme);
}

void check_valid_buffer (void *buffer, unsigned size,
                         void *esp, bool to_write) {
  void *from = NULL, *to = NULL;
  void *vaddr = NULL;
  struct vm_entry *vme = NULL;
  unsigned number_of_page, i;

  /*  인자로 받은 buffer부터 buffer + size까지의 크기가 한 페이지의
      크기를 넘을 수도 있음 */
  /* 검사해야할 페이지 개수를 구함 */
  from = (unsigned)pg_round_down (buffer);
  to = (unsigned)pg_round_down ((unsigned)buffer + size - 1);
  number_of_page = ((to - from) >> PGBITS) + 1; 
  
  for (i = 0; i < number_of_page; i++) {
    /*  check_address를 이용해서 주소의 유저영역 여부를 검사함과 동시
        에 vm_entry 구조체를 얻음 */ 
    if (i != number_of_page - 1) {
      vaddr = (unsigned)buffer + (PGSIZE * i);
    } else {
      vaddr = (unsigned)buffer + size - 1; 
    }
    vme = check_address (vaddr);

    /*  해당 주소에 대한 vm_entry 존재여부와 vm_entry의 writable 멤
        버가 true인지 검사 */
    // only if to_write is true, check writable
    if (vme == NULL ||
        (to_write == true && vme->writable == false)) {
      exit (-1);
    }
  }
  /*  위 내용을 buffer 부터 buffer + size까지의 주소에 포함되는
      vm_entry들에 대해 적용 */
}

void check_valid_string (const void *str, void *esp) {
  /* check virtual address without checking writeability */
  check_valid_buffer (str, strlen (str) + 1, esp, false);
}

struct page *alloc_page (enum palloc_flags flags) {
  struct page *page = NULL;
  lock_acquire (&lru_lock);
  page = (struct page*)malloc (sizeof (struct page));
  if (page == NULL) {
    return NULL;
  }
  memset (page, 0x00, sizeof (struct page));
  page->thread = thread_current ();
  page->kaddr = palloc_get_page (flags);
  /* 물리 페이지 할당에 실패하면 페이지 풀이 가득 찬것이므로
     victim page를 선정해 swap out을 시킨 후 page를 할당한다. */
  if (page->kaddr == NULL) {
    page->kaddr = try_to_free_pages (flags);
  }
  ASSERT (page->kaddr);
  lock_release (&lru_lock);
  return page;
}

void free_page (void *kaddr) {
  struct list_elem *e = NULL;
  struct page *page = NULL;
  lock_acquire (&lru_lock);
  /* lru_list를 순회하여 kaddr를 물리 페이지 주소로 같는 page구조체를 찾는다 */
  for (e = list_begin (&lru_list); e != list_end (&lru_list); e = list_next (e)) {
    page = list_entry (e, struct page, lru);
    if (page->kaddr == kaddr)
      break;
  }

  /* page가 존재하는 경우 해당 page해제 */
  if (page != NULL) {
    __free_page (page);
  }
  lock_release (&lru_lock);
}

void __free_page (struct page* page) {
  pagedir_clear_page (page->thread->pagedir, page->vme->vaddr);
  del_page_from_lru_list (page);
  palloc_free_page (page->kaddr);
  free (page);
}
