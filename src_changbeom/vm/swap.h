#ifndef VM_SWAP_H
#define VM_SWAP_H

#define SWAP_SIZE 1024*PGSIZE

void swap_init (void);
void swap_in (size_t used_index, void *kaddr);
size_t swap_out (void *kaddr);




#endif
