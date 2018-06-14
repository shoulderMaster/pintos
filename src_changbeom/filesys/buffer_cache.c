#include "filesys/buffer_cache.h"
#include "filesys/filesys.h"
#include "devices/block.h"
#include "threads/palloc.h"
#include "lib/round.h"
#include "threads/vaddr.h"
#include "lib/string.h"
#include "threads/synch.h"
#include <stdio.h>

/* buffer cache entry의 개수 (32kb) */
#define BUFFER_CACHE_SIZE 32*1024
#define BUFFER_CACHE_ENTRY_NB (BUFFER_CACHE_SIZE / BLOCK_SECTOR_SIZE)  //64

/* buffer cache 메모리 영역을 가리킴 */
void *p_buffer_cache = NULL;
struct lock bc_lock;

/* buffer head 배열 */
struct buffer_head buffer_head_table[BUFFER_CACHE_ENTRY_NB];

/* victim entry 선정 시 clock 알고리즘을 위한 변수 */
int clock_hand = 0;

void bc_init (void) {
  int i = 0;
  /*  Allocation buffer cache in Memory */
  /*  p_buffer_cache가 buffer cache 영역 포인팅 */
  p_buffer_cache = palloc_get_multiple (PAL_ZERO, DIV_ROUND_UP (BUFFER_CACHE_SIZE, PGSIZE));
  ASSERT (p_buffer_cache != NULL);
  lock_init (&bc_lock);
  /*  전역변수 buffer_head 자료구조 초기화 */
  memset (buffer_head_table, 0x00, sizeof (struct buffer_head)*BUFFER_CACHE_ENTRY_NB);
  for (i = 0; i < BUFFER_CACHE_ENTRY_NB; i++) {
    lock_init (&buffer_head_table[i].lock);
    buffer_head_table[i].bc_entry = p_buffer_cache + i*BLOCK_SECTOR_SIZE;
  }
}

void bc_flush_entry (struct buffer_head *p_flush_entry) {
  ASSERT (p_flush_entry->dirty == true);
  ASSERT (p_flush_entry->in_use == true);
  ASSERT (lock_held_by_current_thread (&p_flush_entry->lock));
  /*  block_write을 호출하여, 인자로 전달받은 buffer cache entry의 데이터를 디스크로 flush */
  block_write (fs_device, p_flush_entry->sector, p_flush_entry->bc_entry);

  /*  buffer_head의 dirty 값 update */
  p_flush_entry->dirty = false;
}

void bc_flush_all_entries (void) {
  int i = 0;
  /*  전역변수 buffer_head를 순회하며, dirty인 entry는 block_write 함수를 호출하여 디스크로 flush */
  /*  디스크로 flush한 후, buffer_head의 dirty 값 update */
  for (i = 0; i < BUFFER_CACHE_ENTRY_NB; i++) {
    if (buffer_head_table[i].dirty == true && buffer_head_table[i].in_use == true) {
      lock_acquire (&buffer_head_table[i].lock);
      bc_flush_entry (&buffer_head_table[i]);
      lock_release (&buffer_head_table[i].lock);
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
  struct buffer_head *bch = NULL;
  int i = 0;
  /* bc_lock은 캐시될 버퍼가 결정되는 과정이 atomic하게 수행되어질 수 있도록 유지되어야함. */
  lock_acquire (&bc_lock);
  for (i = 0; i < BUFFER_CACHE_ENTRY_NB; i++) {
    if (buffer_head_table[i].sector == sector && buffer_head_table[i].in_use == true) {
      bch = &buffer_head_table[i];
      /* 캐시될 버퍼가 정해지고 해당 캐피 버퍼에 read, write가 완료될 때까지 buffer head lock유지 */
      lock_acquire (&bch->lock);
      /* 캐시될 버퍼가 결정되었으므로 bc_lock을 해제함 */
      lock_release (&bc_lock);
      break;
    }
  }
  /*  성공 : 찾은 buffer_head 반환, 실패 : NULL */
  return bch;
}

struct buffer_head* bc_select_victim (void) {
  struct buffer_head *victim = NULL;
  ASSERT (lock_held_by_current_thread (&bc_lock));
  /*  clock 알고리즘을 사용하여 victim entry를 선택 */
  /*  buffer_head 전역변수를 순회하며 clock_bit 변수를 검사 */
  /*  victim entry에 해당하는 buffer_head 값 update */
  for (; true; clock_hand = (clock_hand + 1) % BUFFER_CACHE_ENTRY_NB) {
    struct buffer_head *bhe = &buffer_head_table[clock_hand];
    if (bhe->in_use == false || bhe->clock_bit == true) {
      victim = bhe;
      clock_hand = (clock_hand + 1) % BUFFER_CACHE_ENTRY_NB;
      break;
    }
    bhe->clock_bit = true;
    continue;
  }
  /* 해당 cache buffer에 read, write가 완료될 때 까지 lock유지 */
  lock_acquire (&victim->lock);
  /*  선택된 victim entry가 dirty일 경우, 디스크로 flush */
  if (victim->in_use == true && victim->dirty == true) {
    bc_flush_entry (victim);
  }
  /*  victim entry를 return */
  return victim;
}

bool bc_write (block_sector_t sector_idx, void *buffer,
    off_t bytes_written, int chunk_size, int sector_ofs) {
  printf ("bc_wrtite sector_idx : %d, buffer : %p, bytes_read : %d, chunk_size : %d, sector_ofs : %d\n", sector_idx, buffer, bytes_written, chunk_size, sector_ofs);
  bool success = false;
  struct buffer_head *bch = NULL;
  /*  sector_idx를 buffer_head에서 검색하여 buffer에 복사(구현)*/
  bch = bc_lookup (sector_idx);
  if (bch == NULL) {
    bch = bc_select_victim ();
    ASSERT (lock_held_by_current_thread (&bch->lock));
    bch->sector = sector_idx;
    bch->in_use = true;
    block_read (fs_device, sector_idx, bch->bc_entry);
    lock_release (&bc_lock);
  }
  ASSERT (lock_held_by_current_thread (&bch->lock));

  memcpy (bch->bc_entry + sector_ofs, buffer + bytes_written, chunk_size);
  printf ("memcpy dst : %p, src : %p , size : %d\n", bch->bc_entry + sector_ofs, buffer + bytes_written, chunk_size);
  bch->dirty = true;
  bch->clock_bit = false;
  lock_release (&bch->lock);
  success = true;
  /*  update buffer head (구현) */
  return success;
}

bool bc_read (block_sector_t sector_idx, void *buffer,
              off_t bytes_read, int chunk_size, int sector_ofs) {
  printf ("bc_read sector_idx : %d, buffer : %p, bytes_read : %d, chunk_size : %d, sector_ofs : %d\n", sector_idx, buffer, bytes_read, chunk_size, sector_ofs);
  /*  sector_idx를 buffer_head에서 검색 (bc_lookup 함수 이용) */
  struct buffer_head *bch = NULL;
  bch = bc_lookup (sector_idx);
  /*  검색 결과가 없을 경우, 디스크 블록을 캐싱 할 buffer entry의
      buffer_head를 구함 (bc_select_victim 함수 이용) */
  if (bch == NULL) {
    ASSERT (lock_held_by_current_thread (&bc_lock));
    bch = bc_select_victim ();
    ASSERT (lock_held_by_current_thread (&bch->lock));
    bch->sector = sector_idx;
    lock_release (&bc_lock);
    /*  block_read 함수를 이용해, 디스크 블록 데이터를 buffer cache로 read */
    block_read (fs_device, sector_idx, bch->bc_entry);
    bch->in_use = true;
    bch->dirty = false;
  }
  ASSERT (lock_held_by_current_thread (&bch->lock));
  ASSERT (bch->in_use == true);
  /*  memcpy 함수를 통해, buffer에 디스크 블록 데이터를 복사 */
  memcpy (buffer + bytes_read, bch->bc_entry + sector_ofs, chunk_size);
  printf ("memcpy src : %p, dst : %p , size : %d\n", bch->bc_entry + sector_ofs, buffer + bytes_read, chunk_size);
  /*  buffer_head의 clock bit을 setting */
  bch->clock_bit = false;

  lock_release (&bch->lock);
  return true;
}
