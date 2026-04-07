#include "buddy.h"

#include "list.h"
#include "stdio.h"
#include "config.h"

// ---------- State --------------
// FREE -> the frame is free
// ALLOC -> the frame is allocated
// HEAD -> the head page of a continuous block
// TAIL -> the **non-head** page of a continuous block (won't be on the free list)
#define FRAME_FREE_HEAD 0
#define FRAME_FREE_TAIL 1
#define FRAME_ALLOC_HEAD 2
#define FRAME_ALLOC_TAIL 3

// __VA_ARGS__ -> passed all parameters in ... to the location of __VA_ARGS__
// use do {} while (0) to avoid the syntax error when using in `if`
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

// g_ -> global variable prefix
static struct frame frame_array[BUDDY_TOTAL_PAGES]; // page frame metadata array for buddy system
static struct list_head free_area[BUDDY_MAX_ORDER + 1]; // free area list for each order
static unsigned char reserved_map[BUDDY_TOTAL_PAGES]; // bitmap to track reserved pages during startup allocation, 1: reserved; 0: free

// prefix sum array for reserved_map
// if reserved_prefix[end] - reserved_prefix[start] > 0 -> there exist reserved pages -> find quickly 
static unsigned int reserved_prefix[BUDDY_TOTAL_PAGES + 1];
static unsigned long g_pool_start = BUDDY_DEFAULT_POOL_START; // memory pool start address
static unsigned long g_pool_size = BUDDY_DEFAULT_POOL_SIZE; // memory pool size
static unsigned long g_total_pages = BUDDY_DEFAULT_POOL_SIZE >> PAGE_SHIFT; // total pages in the memory pool
static int buddy_ready; // check the buddy_init() is done

// ------------- ALIGN -------------------
// PAGE_SIZE = 0001 0000 0000 0000 (4096)
// PAGE_SIZE - 1UL = 0000 1111 1111 1111 (4095)
// ~(PAGE_SIZE - 1UL) = 1111 0000 0000 0000 -> the mask reset the last 12 bits -> align down
static unsigned long align_down(unsigned long value) {
    return value & ~(PAGE_SIZE - 1UL);
}

static unsigned long align_up(unsigned long value) {
    // align down (value + PAGE_SIZE - 1)
    return (value + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);
}

static unsigned long page_to_addr(unsigned long idx) {
    // addr = start + idx * PAGE_SIZE
    // (<< PAGE_SHIFT) = * 2^PAGE_SHIFT
    return g_pool_start + (idx << PAGE_SHIFT);
}

static int range_has_reserved(unsigned long idx, unsigned long count) {
    return reserved_prefix[idx + count] != reserved_prefix[idx];
}

/**
 * @brief Find the head of the continuous block and the tailing pages of this block.
 *
 * @param idx the head page idx of the block
 * @param order the order of the block
 * @param head_state the state to mark the head page (FREE/ALLOC)
 * @param tail_state the state to mark the tailing pages (FREE/ALLOC)
 */
static void mark_block(unsigned long idx, unsigned int order, int head_state, int tail_state) {
    unsigned long count = 1UL << order; // #pages in this block
    unsigned long i;

    // mark the head page
    frame_array[idx].state = head_state;
    frame_array[idx].order = (int)order;

    // mark the tailing pages of this block
    for (i = 1; i < count; ++i) {
        frame_array[idx + i].state = tail_state;
        frame_array[idx + i].order = -1;
    }
}

static void add_free_block(unsigned long idx, unsigned int order) {
    mark_block(idx, order, FRAME_FREE_HEAD, FRAME_FREE_TAIL);

    // add the head page (frame_array[idx]) to the free list of this order (free_area[order])
    // frame_array -> the struct frame (metadata)
    // free_area -> only the list_head 
    list_add(&frame_array[idx].node, &free_area[order]);

    BUDDY_LOG("[+ buddy] Add page %lu to order %u. Range of pages: [%lu, %lu]\r\n",
              idx, order, idx, idx + (1UL << order) - 1);
}

static void remove_free_block(unsigned long idx, unsigned int order) {
    list_del(&frame_array[idx].node); // remove from the free list
    frame_array[idx].state = FRAME_ALLOC_HEAD; // mark as ALLOC

    BUDDY_LOG("[- buddy] Remove page %lu from order %u. Range of pages: [%lu, %lu]\r\n",
              idx, order, idx, idx + (1UL << order) - 1);
}

/**
 * @brief Build the prefix sum of the reserved_map
 */
static void build_reserved_prefix(void) {
    unsigned long i;

    reserved_prefix[0] = 0;
    for (i = 0; i < g_total_pages; ++i) {
        reserved_prefix[i + 1] = reserved_prefix[i] + (reserved_map[i] ? 1U : 0U);
    }
}

/**
 * @brief Set the memory pool region and init the reserved_map
 *
 * @param start the start address of the memory pool
 * @param size the size of the memory pool
 */
void buddy_set_region(unsigned long start, unsigned long size) {
    unsigned long i;

    if (size == 0) { // use default region if size is 0 or invalid
        g_pool_start = BUDDY_DEFAULT_POOL_START;
        g_pool_size = BUDDY_DEFAULT_POOL_SIZE;
    } else {
        // use align_down() to make sure the page-aligned region is safe
        g_pool_start = align_down(start);
        g_pool_size = align_down(size);
        
        // check the upper bound limit to avoid overflow
        if (g_pool_size > BUDDY_MAX_POOL_SIZE) {
            g_pool_size = BUDDY_MAX_POOL_SIZE;
        }
    }

    // check the lower bound limit to avoid invalid region
    if (g_pool_size < PAGE_SIZE) {
        // if the size less than a page -> use default region
        g_pool_size = BUDDY_DEFAULT_POOL_SIZE;
        g_pool_start = BUDDY_DEFAULT_POOL_START;
    }

    // cal the #pages
    g_total_pages = g_pool_size >> PAGE_SHIFT;
    if (g_total_pages > BUDDY_TOTAL_PAGES) {
        // check the #page upper bound to avoid buffer overflow
        g_total_pages = BUDDY_TOTAL_PAGES;
    }

    // init the reserved_map (find reserved will be called later)
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
    
    // not in the memory pool region -> ignore
    if (reserve_end <= g_pool_start || start >= region_end) {
        return;
    }

    if (start < g_pool_start) {
        start = g_pool_start;
    }
    if (reserve_end > region_end) {
        reserve_end = region_end;
    }

    // find the page idx range
    page_start = (start - g_pool_start) >> PAGE_SHIFT; // >> PAGE_SHIFT = / PAGE_SIZE
    page_end = align_up(reserve_end - g_pool_start) >> PAGE_SHIFT; // align_up() to make sure the tailing page is fully reserved
    if (page_end > g_total_pages) { // aviod overflow
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