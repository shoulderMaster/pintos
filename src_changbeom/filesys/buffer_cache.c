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
#define BUFFER_CACHE_ENTRY_NB (BUFFER_CACHE_SIZE / BLOCK_SECTOR_SIZE)

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
    buffer_head_table[i].bc_entry = p_buffer_cache + i*BLOCK_SIZE;
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

void bc_flush_all_entries (void) {
  int i = 0;
  /*  전역변수 buffer_head를 순회하며, dirty인 entry는 block_write 함수를 호출하여 디스크로 flush */
  /*  디스크로 flush한 후, buffer_head의 dirty 값 update */
  for (i = 0; i < BUFFER_CACHE_ENTRY_NB; i++) {
    if (buffer_head_table[i].dirty == true) {
      bc_flush_entry (&buffer_head_table[i]);
    }
  }
}

void bc_term (void) {
  /*  bc_flush_all_entries 함수를 호출하여 모든 buffer cache entry를 디스크로 flush */
  bc_flush_all_entries ();
  /*  buffer cache 영역 할당 해제 */
  palloc_free_multiple (p_buffer_cache, DIV_ROUND_UP (BUFFER_CACHE_SIZE, PGSIZE));
}

struct buffer_head* bc_lookup (block_sector_t sector) {
  /*  buffe_head를 순회하며, 전달받은 sector 값과 동일한 sector 값을 갖는 buffer cache entry가 있는지 확인 */
  int i = 0;
  for (i = 0; i < BUFFER_CACHE_ENTRY_NB; i++) {
    if (buffer_head_table[i].sector == sector && buffer_head_table[i].in_use == true) {
      return &buffer_head_table[i];
    }
  }
  /*  성공 : 찾은 buffer_head 반환, 실패 : NULL */
  return NULL;
}

struct buffer_head* bc_select_victim (void) {
  struct buffer_head *victim = NULL;
  int i = 0;
  /*  clock 알고리즘을 사용하여 victim entry를 선택 */
  /*  buffer_head 전역변수를 순회하며 clock_bit 변수를 검사 */
  /*  victim entry에 해당하는 buffer_head 값 update */
  for (i = 0; true; i = (i + 1) % BUFFER_CACHE_ENTRY_NB) {
    if (buffer_head_table[i].in_use == true) {
      if (buffer_head_table[i].clock_bit == true) {
        buffer_head_table[i].clock_bit = false;
        victim = &buffer_head_table[i];
        break;
      } else {
        buffer_head_table[i].clock_bit = true;
      }
    } else {
      victim = &buffer_head_table[i];
      break;
    }
  }
  ASSERT (victim == NULL);
  /*  선택된 victim entry가 dirty일 경우, 디스크로 flush */
  if (victim->dirty == true) {
    bc_flush_entry (victim);
  }
  /*  victim entry를 return */
  return victim;
}

bool bc_write (block_sector_t sector_idx, void *buffer,
    off_t bytes_written, int chunk_size, int sector_ofs) {
  bool success = false;
  struct buffer_head *bce = NULL;
  /*  sector_idx를 buffer_head에서 검색하여 buffer에 복사(구현)*/
  bce = bc_lookup (sector_idx);
  if (bce == NULL) {
    bce = bc_select_victim ();
    lock_acquire (&bce->bc_lock);
    block_read (fs_device, sector_idx, bce->bc_entry);
    lock_release (&bce->bc_lock);
  }

  lock_acquire (&bce->bc_lock);
  memcpy (bce->bc_entry + sector_ofs, buffer, bytes_written);
  bce->dirty = true;
  bce->clock_bit = false;
  lock_release (&bce->bc_lock);
  success = true;
  /*  update buffer head (구현) */
  return success;
}
