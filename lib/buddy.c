#include "buddy.h"

#include "list.h"
#include "stdio.h"
#include "config.h"

#define PAGE_SHIFT 12UL
#define PAGE_SIZE (1UL << PAGE_SHIFT)

#define FRAME_FREE_HEAD 0
#define FRAME_FREE_TAIL 1
#define FRAME_ALLOC_HEAD 2
#define FRAME_ALLOC_TAIL 3

#if BUDDY_ENABLE_DEMO_LOG
#define BUDDY_LOG(...) printf(__VA_ARGS__)
#else
#define BUDDY_LOG(...) do { } while (0)
#endif

struct frame {
    int order;
    int state;
    struct list_head node;
};

static struct frame frame_array[BUDDY_TOTAL_PAGES];
static struct list_head free_area[BUDDY_MAX_ORDER + 1];
static unsigned char reserved_map[BUDDY_TOTAL_PAGES];
static unsigned int reserved_prefix[BUDDY_TOTAL_PAGES + 1];
static unsigned long g_pool_start = BUDDY_DEFAULT_POOL_START;
static unsigned long g_pool_size = BUDDY_DEFAULT_POOL_SIZE;
static unsigned long g_total_pages = BUDDY_DEFAULT_POOL_SIZE >> PAGE_SHIFT;
static int buddy_ready;

static unsigned long align_down(unsigned long value) {
    return value & ~(PAGE_SIZE - 1UL);
}

static unsigned long align_up(unsigned long value) {
    return (value + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);
}

static unsigned long page_to_addr(unsigned long idx) {
    return g_pool_start + (idx << PAGE_SHIFT);
}

static int range_has_reserved(unsigned long idx, unsigned long count) {
    return reserved_prefix[idx + count] != reserved_prefix[idx];
}

static void mark_block(unsigned long idx, unsigned int order, int head_state, int tail_state) {
    unsigned long count = 1UL << order;
    unsigned long i;

    frame_array[idx].state = head_state;
    frame_array[idx].order = (int)order;
    for (i = 1; i < count; ++i) {
        frame_array[idx + i].state = tail_state;
        frame_array[idx + i].order = -1;
    }
}

static void add_free_block(unsigned long idx, unsigned int order) {
    mark_block(idx, order, FRAME_FREE_HEAD, FRAME_FREE_TAIL);
    list_add(&frame_array[idx].node, &free_area[order]);

    BUDDY_LOG("[+] Add page %lu to order %u. Range of pages: [%lu, %lu]\r\n",
              idx, order, idx, idx + (1UL << order) - 1);
}

static void remove_free_block(unsigned long idx, unsigned int order) {
    list_del(&frame_array[idx].node);
    frame_array[idx].state = FRAME_ALLOC_HEAD;

    BUDDY_LOG("[-] Remove page %lu from order %u. Range of pages: [%lu, %lu]\r\n",
              idx, order, idx, idx + (1UL << order) - 1);
}

static void build_reserved_prefix(void) {
    unsigned long i;

    reserved_prefix[0] = 0;
    for (i = 0; i < g_total_pages; ++i) {
        reserved_prefix[i + 1] = reserved_prefix[i] + (reserved_map[i] ? 1U : 0U);
    }
}

void buddy_set_region(unsigned long start, unsigned long size) {
    unsigned long i;

    if (size == 0) {
        g_pool_start = BUDDY_DEFAULT_POOL_START;
        g_pool_size = BUDDY_DEFAULT_POOL_SIZE;
    } else {
        g_pool_start = align_down(start);
        g_pool_size = align_down(size);
        if (g_pool_size > BUDDY_MAX_POOL_SIZE) {
            g_pool_size = BUDDY_MAX_POOL_SIZE;
        }
    }

    if (g_pool_size < PAGE_SIZE) {
        g_pool_size = BUDDY_DEFAULT_POOL_SIZE;
        g_pool_start = BUDDY_DEFAULT_POOL_START;
    }

    g_total_pages = g_pool_size >> PAGE_SHIFT;
    if (g_total_pages > BUDDY_TOTAL_PAGES) {
        g_total_pages = BUDDY_TOTAL_PAGES;
    }

    for (i = 0; i < BUDDY_TOTAL_PAGES; ++i) {
        reserved_map[i] = 0;
    }
}

void buddy_mark_reserved_range(unsigned long start, unsigned long size) {
    unsigned long region_end;
    unsigned long reserve_end;
    unsigned long page_start;
    unsigned long page_end;
    unsigned long i;

    if (size == 0 || g_total_pages == 0) {
        return;
    }

    region_end = g_pool_start + g_pool_size;
    reserve_end = start + size;
    if (reserve_end <= g_pool_start || start >= region_end) {
        return;
    }

    if (start < g_pool_start) {
        start = g_pool_start;
    }
    if (reserve_end > region_end) {
        reserve_end = region_end;
    }

    page_start = (start - g_pool_start) >> PAGE_SHIFT;
    page_end = align_up(reserve_end - g_pool_start) >> PAGE_SHIFT;
    if (page_end > g_total_pages) {
        page_end = g_total_pages;
    }

    for (i = page_start; i < page_end; ++i) {
        reserved_map[i] = 1;
    }
}

void buddy_init(void) {
    unsigned long idx = 0;
    unsigned long i;

    for (i = 0; i < BUDDY_TOTAL_PAGES; ++i) {
        frame_array[i].order = -1;
        frame_array[i].state = FRAME_ALLOC_TAIL;
        INIT_LIST_HEAD(&frame_array[i].node);
    }

    for (i = 0; i <= BUDDY_MAX_ORDER; ++i) {
        INIT_LIST_HEAD(&free_area[i]);
    }

    build_reserved_prefix();

    while (idx < g_total_pages) {
        if (reserved_map[idx]) {
            mark_block(idx, 0, FRAME_ALLOC_HEAD, FRAME_ALLOC_TAIL);
            idx++;
            continue;
        }

        int order;
        for (order = BUDDY_MAX_ORDER; order >= 0; --order) {
            unsigned long block_pages = 1UL << order;
            if ((idx & (block_pages - 1)) == 0 &&
                idx + block_pages <= g_total_pages &&
                !range_has_reserved(idx, block_pages)) {
                add_free_block(idx, (unsigned int)order);
                idx += block_pages;
                break;
            }
        }
    }

    buddy_ready = 1;
    BUDDY_LOG("[Init] Buddy initialized at [0x%lx, 0x%lx), total pages: %u\r\n",
              g_pool_start,
              g_pool_start + g_pool_size,
              (unsigned int)g_total_pages);
}

void *buddy_alloc_pages(unsigned int order) {
    unsigned int current;
    struct frame *blk;
    unsigned long idx;

    if (!buddy_ready || order > BUDDY_MAX_ORDER) {
        return 0;
    }

    for (current = order; current <= BUDDY_MAX_ORDER; ++current) {
        if (list_empty(&free_area[current])) {
            continue;
        }

        blk = list_first_entry(&free_area[current], struct frame, node);
        idx = (unsigned long)(blk - frame_array);
        remove_free_block(idx, current);

        while (current > order) {
            unsigned long buddy_idx;

            current--;
            buddy_idx = idx + (1UL << current);
            add_free_block(buddy_idx, current);

            BUDDY_LOG("[Split] Release redundant block: page %lu at order %u\r\n",
                      buddy_idx, current);
        }

        mark_block(idx, order, FRAME_ALLOC_HEAD, FRAME_ALLOC_TAIL);
        BUDDY_LOG("[Page] Allocate 0x%lx at order %u, page %lu\r\n",
                  page_to_addr(idx),
                  order,
                  idx);
        return (void *)page_to_addr(idx);
    }

    return 0;
}

void buddy_free_pages(void *ptr) {
    unsigned long addr;
    unsigned long idx;
    unsigned int order;

    if (!buddy_ready || !ptr) {
        return;
    }

    addr = (unsigned long)ptr;
    if (addr < g_pool_start || addr >= g_pool_start + g_pool_size) {
        return;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return;
    }

    idx = (addr - g_pool_start) >> PAGE_SHIFT;
    if (idx >= g_total_pages || frame_array[idx].state != FRAME_ALLOC_HEAD) {
        return;
    }

    order = (unsigned int)frame_array[idx].order;
    mark_block(idx, order, FRAME_FREE_HEAD, FRAME_FREE_TAIL);

    while (order < BUDDY_MAX_ORDER) {
        unsigned long buddy_idx = idx ^ (1UL << order);

        if (buddy_idx >= BUDDY_TOTAL_PAGES) {
            break;
        }
        if (frame_array[buddy_idx].state != FRAME_FREE_HEAD ||
            (unsigned int)frame_array[buddy_idx].order != order) {
            break;
        }

        BUDDY_LOG("[*] Buddy found! buddy idx: %lu for page %lu with order %u\r\n",
                  buddy_idx,
                  idx,
                  order);

        remove_free_block(buddy_idx, order);

        if (buddy_idx < idx) {
            idx = buddy_idx;
        }

        order++;
        mark_block(idx, order, FRAME_FREE_HEAD, FRAME_FREE_TAIL);
        BUDDY_LOG("[Merge] Merge iteratively to order %u at page %lu\r\n", order, idx);
    }

    add_free_block(idx, order);
    BUDDY_LOG("[Page] Free 0x%lx and add back to order %u, page %lu\r\n",
              page_to_addr(idx),
              order,
              idx);
}

unsigned long buddy_pool_start(void) {
    return g_pool_start;
}

unsigned long buddy_pool_size(void) {
    return g_pool_size;
}

unsigned long buddy_total_pages(void) {
    return g_total_pages;
}

void buddy_dump_free_areas(void) {
    unsigned int i;
    for (i = 0; i <= BUDDY_MAX_ORDER; ++i) {
        unsigned int count = 0;
        struct list_head *it;

        for (it = free_area[i].next; it != &free_area[i]; it = it->next) {
            count++;
        }
        printf("free_area[%u] = %u\r\n", i, count);
    }
}