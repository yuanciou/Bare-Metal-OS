#ifndef BUDDY_H
#define BUDDY_H

void buddy_set_region(unsigned long start, unsigned long size);
void buddy_mark_reserved_range(unsigned long start, unsigned long size);
void buddy_init(void);
void *buddy_alloc_pages(unsigned int order);
void buddy_free_pages(void *ptr);
void buddy_dump_free_areas(void);
unsigned long buddy_pool_start(void);
unsigned long buddy_pool_size(void);
unsigned long buddy_total_pages(void);

#endif // BUDDY_H