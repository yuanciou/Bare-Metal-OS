#include "allocator.h"

#include "../lib/buddy.h"
#include "../lib/list.h"
#include "../lib/stdio.h"
#include "config.h"

#if BUDDY_ENABLE_DEMO_LOG
#define ALLOC_LOG(...) printf(__VA_ARGS__)
#else
#define ALLOC_LOG(...) do { } while (0)
#endif

enum page_meta_type {
    PAGE_META_NONE = 0,
    PAGE_META_LARGE_HEAD,
    PAGE_META_LARGE_TAIL,
    PAGE_META_POOL,
};

struct chunk {
    struct list_head node;
};

struct chunk_pool {
    unsigned int chunk_size;
    struct list_head free_list;
};

#define CHUNK_POOL_COUNT 8
static const unsigned int g_pool_sizes[CHUNK_POOL_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static struct chunk_pool g_pools[CHUNK_POOL_COUNT];
static unsigned char g_page_meta_type[BUDDY_TOTAL_PAGES];
static signed char g_page_meta_order[BUDDY_TOTAL_PAGES];
static signed char g_page_meta_pool_idx[BUDDY_TOTAL_PAGES];
static int g_allocator_ready;

static unsigned int roundup_order(unsigned long pages) {
    unsigned int order = 0;
    unsigned long n = 1;

    while (n < pages) {
        n <<= 1;
        order++;
    }
    return order;
}

static int addr_in_pool(unsigned long addr) {
    return addr >= BUDDY_POOL_START && addr < BUDDY_POOL_START + BUDDY_POOL_SIZE;
}

static unsigned long addr_to_page_idx(unsigned long addr) {
    return (addr - BUDDY_POOL_START) >> PAGE_SHIFT;
}

static int find_pool_index(unsigned long size) {
    int i;
    for (i = 0; i < CHUNK_POOL_COUNT; ++i) {
        if (size <= g_pools[i].chunk_size) {
            return i;
        }
    }
    return -1;
}

static int expand_chunk_pool(int pool_idx) {
    void *page;
    unsigned long page_addr;
    unsigned long page_idx;
    unsigned int chunk_size;
    unsigned int chunk_cnt;
    unsigned int i;

    page = buddy_alloc_pages(0);
    if (!page) {
        return -1;
    }

    page_addr = (unsigned long)page;
    page_idx = addr_to_page_idx(page_addr);
    chunk_size = g_pools[pool_idx].chunk_size;
    chunk_cnt = PAGE_SIZE / chunk_size;

    g_page_meta_type[page_idx] = PAGE_META_POOL;
    g_page_meta_pool_idx[page_idx] = (signed char)pool_idx;
    g_page_meta_order[page_idx] = 0;

    for (i = 0; i < chunk_cnt; ++i) {
        unsigned long chunk_addr = page_addr + (unsigned long)i * chunk_size;
        struct chunk *ck = (struct chunk *)chunk_addr;
        list_add(&ck->node, &g_pools[pool_idx].free_list);
    }

    ALLOC_LOG("[Chunk] Refill pool size %u from page 0x%lx, total chunks: %u\r\n",
              chunk_size,
              page_addr,
              chunk_cnt);
    return 0;
}

static void mark_large_pages(unsigned long head_idx, unsigned int order) {
    unsigned long count = 1UL << order;
    unsigned long i;

    g_page_meta_type[head_idx] = PAGE_META_LARGE_HEAD;
    g_page_meta_order[head_idx] = (signed char)order;
    g_page_meta_pool_idx[head_idx] = -1;

    for (i = 1; i < count; ++i) {
        g_page_meta_type[head_idx + i] = PAGE_META_LARGE_TAIL;
        g_page_meta_order[head_idx + i] = -1;
        g_page_meta_pool_idx[head_idx + i] = -1;
    }
}

static void clear_page_meta(unsigned long head_idx, unsigned int order) {
    unsigned long count = 1UL << order;
    unsigned long i;

    for (i = 0; i < count; ++i) {
        g_page_meta_type[head_idx + i] = PAGE_META_NONE;
        g_page_meta_order[head_idx + i] = -1;
        g_page_meta_pool_idx[head_idx + i] = -1;
    }
}

void allocator_init(void) {
    unsigned int i;

    buddy_init();

    for (i = 0; i < CHUNK_POOL_COUNT; ++i) {
        g_pools[i].chunk_size = g_pool_sizes[i];
        INIT_LIST_HEAD(&g_pools[i].free_list);
    }

    for (i = 0; i < BUDDY_TOTAL_PAGES; ++i) {
        g_page_meta_type[i] = PAGE_META_NONE;
        g_page_meta_order[i] = -1;
        g_page_meta_pool_idx[i] = -1;
    }

    g_allocator_ready = 1;
    ALLOC_LOG("[Init] Dynamic allocator ready. Small alloc <= %u bytes\r\n",
              (unsigned int)SMALL_ALLOC_LIMIT);
}

void allocator_dump_pages(void) {
    buddy_dump_free_areas();
}

void *allocate(unsigned long size) {
    int pool_idx;

    if (!g_allocator_ready || size == 0 || size > BUDDY_MAX_ALLOC_SIZE) {
        if (size > BUDDY_MAX_ALLOC_SIZE) {
            ALLOC_LOG("[Alloc] Reject size %lu (> MAX_ALLOC_SIZE)\r\n", size);
        }
        return 0;
    }

    if (size <= SMALL_ALLOC_LIMIT) {
        struct list_head *n;
        struct chunk *ck;
        unsigned int chunk_size;

        pool_idx = find_pool_index(size);
        if (pool_idx < 0) {
            return 0;
        }

        if (list_empty(&g_pools[pool_idx].free_list)) {
            if (expand_chunk_pool(pool_idx) != 0) {
                return 0;
            }
        }

        n = g_pools[pool_idx].free_list.next;
        list_del(n);
        ck = list_entry(n, struct chunk, node);
        chunk_size = g_pools[pool_idx].chunk_size;

        ALLOC_LOG("[Chunk] Allocate 0x%lx at chunk size %u\r\n",
                  (unsigned long)ck,
                  chunk_size);
        return (void *)ck;
    }

    {
        unsigned long pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
        unsigned int order = roundup_order(pages);
        void *ptr = buddy_alloc_pages(order);

        if (!ptr) {
            return 0;
        }

        mark_large_pages(addr_to_page_idx((unsigned long)ptr), order);
        return ptr;
    }
}

void free(void *ptr) {
    unsigned long addr;
    unsigned long page_idx;
    unsigned char meta_type;

    if (!g_allocator_ready || !ptr) {
        return;
    }

    addr = (unsigned long)ptr;
    if (!addr_in_pool(addr)) {
        return;
    }

    page_idx = addr_to_page_idx(addr);
    if (page_idx >= BUDDY_TOTAL_PAGES) {
        return;
    }

    meta_type = g_page_meta_type[page_idx];

    if (meta_type == PAGE_META_POOL) {
        int pool_idx = g_page_meta_pool_idx[page_idx];
        unsigned long page_base = addr & ~(PAGE_SIZE - 1UL);
        unsigned int chunk_size;
        unsigned long offset;
        struct chunk *ck;

        if (pool_idx < 0 || pool_idx >= CHUNK_POOL_COUNT) {
            return;
        }

        chunk_size = g_pools[pool_idx].chunk_size;
        offset = addr - page_base;
        if (offset % chunk_size != 0) {
            return;
        }

        ck = (struct chunk *)ptr;
        list_add(&ck->node, &g_pools[pool_idx].free_list);
        ALLOC_LOG("[Chunk] Free 0x%lx at chunk size %u\r\n", addr, chunk_size);
        return;
    }

    if (meta_type == PAGE_META_LARGE_HEAD) {
        unsigned int order;

        if ((addr & (PAGE_SIZE - 1UL)) != 0) {
            return;
        }

        order = (unsigned int)g_page_meta_order[page_idx];
        buddy_free_pages(ptr);
        clear_page_meta(page_idx, order);
    }
}