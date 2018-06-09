#include "filesys/buffer_cache.h"
#include "devices/block.h"

/* buffer cache entry의 개수 (32kb) */
#define BUFFER_CACHE_ENTRY_NB (32*1024 / BLOCK_SECTOR_SIZE)

/* buffer cache 메모리 영역을 가리킴 */
void *p_buffer_cache = NULL;

/* buffer head 배열 */
struct buffer_head buffer_head[BUFFER_CACHE_ENTRY_NB] = {0, };

/* victim entry 선정 시 clock 알고리즘을 위한 변수 */
int clock_hand = 0;



