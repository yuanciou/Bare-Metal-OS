#ifndef BUDDY_H
#define BUDDY_H

#include "list.h"

struct frame {
    int order;
    int state;
    struct list_head node;
};

extern unsigned long G_MEMPOOL_START;
extern unsigned long G_MEMPOOL_SIZE;
extern unsigned long G_MEM_TOTAL_PAGE;

void buddy_set_region(unsigned long start, unsigned long size);
void buddy_mark_reserved_range(unsigned long start, unsigned long size);
void buddy_init(void);
void *buddy_alloc_pages(unsigned int order);
void buddy_free_pages(void *ptr);
void buddy_dump_free_areas(void);

#endif // BUDDY_H