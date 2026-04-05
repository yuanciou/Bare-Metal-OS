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
static int buddy_ready;

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

    while (idx < BUDDY_TOTAL_PAGES) {
        int order;
        for (order = BUDDY_MAX_ORDER; order >= 0; --order) {
            unsigned long block_pages = 1UL << order;
            if ((idx & (block_pages - 1)) == 0 && idx + block_pages <= BUDDY_TOTAL_PAGES) {
                add_free_block(idx, (unsigned int)order);
                idx += block_pages;
                break;
            }
        }
    }

    buddy_ready = 1;
    BUDDY_LOG("[Init] Buddy initialized at [0x%lx, 0x%lx), total pages: %u\r\n",
              (unsigned long)BUDDY_POOL_START,
              (unsigned long)(BUDDY_POOL_START + BUDDY_POOL_SIZE),
              (unsigned int)BUDDY_TOTAL_PAGES);
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
                  (unsigned long)(BUDDY_POOL_START + (idx << PAGE_SHIFT)),
                  order,
                  idx);
        return (void *)(BUDDY_POOL_START + (idx << PAGE_SHIFT));
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
    if (addr < BUDDY_POOL_START || addr >= BUDDY_POOL_START + BUDDY_POOL_SIZE) {
        return;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return;
    }

    idx = (addr - BUDDY_POOL_START) >> PAGE_SHIFT;
    if (idx >= BUDDY_TOTAL_PAGES || frame_array[idx].state != FRAME_ALLOC_HEAD) {
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
              (unsigned long)(BUDDY_POOL_START + (idx << PAGE_SHIFT)),
              order,
              idx);
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