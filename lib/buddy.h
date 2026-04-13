#ifndef BUDDY_H
#define BUDDY_H

#include "config.h"
#include "list.h"

enum page_state {
    /* --- Free states managed by buddy system --- */
    PAGE_STATE_FREE_HEAD = 0,       // the free head page
    PAGE_STATE_FREE_TAIL,           // the non-head free page

    /* --- Reserved state --- */
    PAGE_STATE_RESERVED,            // reserved page during startup

    /* --- Allocated states managed by allocator --- */
    PAGE_STATE_ALLOC_PAGE_HEAD,    // the large allocated head page
    PAGE_STATE_ALLOC_PAGE_TAIL,    // the non-head allocated page
    PAGE_STATE_ALLOC_CHUNK_POOL    // the allocated page for chunk pool
};

struct frame {
    int order;
    enum page_state state;
    struct list_head node;
    signed char meta_pool_idx;  // the pool idx (map by `g_pools[meta_pool_idx].chunk_size`)
    int ref_count;              // the count of allocated chunks
};

extern struct frame *frame_array;

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