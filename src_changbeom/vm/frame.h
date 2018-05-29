#ifndef VM_FRAME_H
#define VM_FRAME_H

void lru_list_init (void);
void add_page_to_lru_list (struct page *page);
void del_page_from_lru_list (struct page *page);



#endif
