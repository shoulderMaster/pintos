#include "vm/page.h"
#include "lib/kernel/hash.h"


static bool vm_less_func (const struct hash_elem *a, const struct hash_elem *b); 
static unsigned vm_hash_func (const struct hash_elem *e, void *aux);
void vm_init (struct hash *vm); 
bool insert_vme (struct hash *vm, struct vm_entry *vme);
bool delete_vme (struct hash *vm, struct vm_entry *vme);
struct vm_entry *find_vme (void *vaddr); 
  

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

