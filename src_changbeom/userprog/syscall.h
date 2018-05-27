#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

void munmap (int mapping);
struct vm_entry *check_address (void *addr);
void syscall_init (void);
struct lock rw_lock;

typedef int pid_t;
typedef int mapid_t;


#endif /* userprog/syscall.h */
