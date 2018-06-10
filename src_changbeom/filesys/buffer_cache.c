#include "filesys/buffer_cache.h"
#include "filesys/filesys.h"
#include "devices/block.h"
#include "threads/palloc.h"
#include "lib/round.h"
#include "threads/vaddr.h"
#include "lib/string.h"
#include "threads/synch.h"

/* buffer cache entry의 개수 (32kb) */
#define BUFFER_CACHE_SIZE 32*1024
#define BUFFER_CACHE_ENTRY_NB (32*1024 / BLOCK_SECTOR_SIZE)

/* buffer cache 메모리 영역을 가리킴 */
void *p_buffer_cache = NULL;

/* buffer head 배열 */
struct buffer_head buffer_head_table[BUFFER_CACHE_ENTRY_NB];

/* victim entry 선정 시 clock 알고리즘을 위한 변수 */
int clock_hand = 0;

void bc_init (void) {
  int i = 0;
  /*  Allocation buffer cache in Memory */
  /*  p_buffer_cache가 buffer cache 영역 포인팅 */
  p_buffer_cache = palloc_get_multiple (PAL_ZERO, DIV_ROUND_UP (BUFFER_CACHE_SIZE, PGSIZE));
  /*  전역변수 buffer_head 자료구조 초기화 */
  memset (buffer_head_table, 0x00, sizeof (struct buffer_head));
  for (i = 0; i < BUFFER_CACHE_ENTRY_NB; i++) {
    lock_init (&buffer_head_table[i].bc_lock);
  }
}

void bc_flush_entry (struct buffer_head *p_flush_entry) {
  /*  block_write을 호출하여, 인자로 전달받은 buffer cache entry의 데이터를 디스크로 flush */
  lock_acquire (&p_flush_entry->bc_lock);
  block_write (fs_device, p_flush_entry->sector, p_flush_entry->bc_entry);

  /*  buffer_head의 dirty 값 update */
  p_flush_entry->dirty = false;
  lock_release (&p_flush_entry->bc_lock);
}

void bc_flush_all_entry (void) {
  int i = 0;
  /*  전역변수 buffer_head를 순회하며, dirty인 entry는 block_write 함수를 호출하여 디스크로 flush */
  /*  디스크로 flush한 후, buffer_head의 dirty 값 update */
  for (i = 0; i < BUFFER_CACHE_ENTRY_NB; i++) {
    if (buffer_head_table[i].dirty == true) {
      bc_flush_entry (&buffer_head_table[i]);
    }
  }
}
