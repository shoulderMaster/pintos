#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

/* 한 프로세스에 있는 FDT의 entry 최대 개수
   multi-oom 테스트 케이스가 126개까지 fd를 열어봄 */
#define FILE_MAX 192

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
