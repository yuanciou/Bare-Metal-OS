#include "buddy.h"

#include "list.h"
#include "stdio.h"
#include "config.h"
#include "align.h"

// __VA_ARGS__ -> passed all parameters in ... to the location of __VA_ARGS__
// use do {} while (0) to avoid the syntax error when using in `if`
#if ALLOC_ENABLE_DEMO_LOG
#define BUDDY_LOG(...) printf(__VA_ARGS__)
#else
#define BUDDY_LOG(...) do { } while (0)
#endif

// g_ -> global variable prefix
struct frame *frame_array;
static struct list_head free_area[BUDDY_MAX_ORDER + 1]; // free area list for each order

// prefix sum array for reserved pages
// if reserved_prefix[end] - reserved_prefix[start] > 0 -> there exist reserved pages -> find quickly 
static unsigned int reserved_prefix[BUDDY_TOTAL_PAGES + 1];
unsigned long G_MEMPOOL_START = BUDDY_DEFAULT_POOL_START; // memory pool start address
unsigned long G_MEMPOOL_SIZE = BUDDY_DEFAULT_POOL_SIZE; // memory pool size
unsigned long G_MEM_TOTAL_PAGE = BUDDY_DEFAULT_POOL_SIZE >> PAGE_SHIFT; // total pages in the memory pool
static int buddy_ready; // check the buddy_init() is done

static unsigned long page_to_addr(unsigned long idx) {
    // addr = start + idx * PAGE_SIZE
    // (<< PAGE_SHIFT) = * 2^PAGE_SHIFT
    return G_MEMPOOL_START + (idx << PAGE_SHIFT);
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
static void mark_block(unsigned long idx,
                       unsigned int order,
                       enum page_state head_state,
                       enum page_state tail_state) {
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
    mark_block(idx, order, PAGE_STATE_FREE_HEAD, PAGE_STATE_FREE_TAIL);

    // add the head page (frame_array[idx]) to the free list of this order (free_area[order])
    // frame_array -> the struct frame (metadata)
    // free_area -> only the list_head 
    list_add(&frame_array[idx].node, &free_area[order]);

    BUDDY_LOG("[+ buddy] Add page %lu to order %u. Range of pages: [%lu, %lu]\r\n",
              idx, order, idx, idx + (1UL << order) - 1);
}

static void remove_free_block(unsigned long idx, unsigned int order) {
    list_del(&frame_array[idx].node); // remove from the free list
    frame_array[idx].state = PAGE_STATE_ALLOC_PAGE_HEAD; // mark as ALLOC

    BUDDY_LOG("[- buddy] Remove page %lu from order %u. Range of pages: [%lu, %lu]\r\n",
              idx, order, idx, idx + (1UL << order) - 1);
}

/**
 * @brief Build the prefix sum of the reserved_map
 */
static void build_reserved_prefix(void) {
    unsigned long i;

    reserved_prefix[0] = 0;
    for (i = 0; i < G_MEM_TOTAL_PAGE; ++i) {
        reserved_prefix[i + 1] = reserved_prefix[i] +
                                 (frame_array[i].state == PAGE_STATE_RESERVED ? 1U : 0U);
    }
}

/**
 * @brief Set the memory pool region (set G_MEMPOOL_START, G_MEMPOOL_SIZE and G_MEM_TOTAL_PAGE)
 *
 * @param start the start address of the memory pool
 * @param size the size of the memory pool
 */
void buddy_set_region(unsigned long start, unsigned long size) {
    unsigned long i;

    if (size == 0) { // use default region if size is 0 or invalid
        G_MEMPOOL_START = BUDDY_DEFAULT_POOL_START;
        G_MEMPOOL_SIZE = BUDDY_DEFAULT_POOL_SIZE;
    } else {
        // use align_down_ul() to make sure the page-aligned region is safe
        G_MEMPOOL_START = align_up_ul(start, PAGE_SIZE);
        G_MEMPOOL_SIZE = align_down_ul(size, PAGE_SIZE);
        
        // check the upper bound limit to avoid overflow
        if (G_MEMPOOL_SIZE > BUDDY_MAX_POOL_SIZE) {
            G_MEMPOOL_SIZE = BUDDY_MAX_POOL_SIZE;
        }
    }

    // check the lower bound limit to avoid invalid region
    if (G_MEMPOOL_SIZE < PAGE_SIZE) {
        // if the size less than a page -> use default region
        G_MEMPOOL_SIZE = BUDDY_DEFAULT_POOL_SIZE;
        G_MEMPOOL_START = BUDDY_DEFAULT_POOL_START;
    }

    // cal the #pages
    G_MEM_TOTAL_PAGE = G_MEMPOOL_SIZE >> PAGE_SHIFT;
    if (G_MEM_TOTAL_PAGE > BUDDY_TOTAL_PAGES) {
        // check the #page upper bound to avoid buffer overflow
        G_MEM_TOTAL_PAGE = BUDDY_TOTAL_PAGES;
    }
}

void buddy_mark_reserved_range(unsigned long start, unsigned long size) {
    unsigned long region_end;
    unsigned long reserve_end;
    unsigned long page_start;
    unsigned long page_end;
    unsigned long i;

    if (size == 0 || G_MEM_TOTAL_PAGE == 0) {
        return;
    }

    region_end = G_MEMPOOL_START + G_MEMPOOL_SIZE;
    reserve_end = start + size;
    
    // not in the memory pool region -> ignore
    if (reserve_end <= G_MEMPOOL_START || start >= region_end) {
        return;
    }

    if (start < G_MEMPOOL_START) {
        start = G_MEMPOOL_START;
    }
    if (reserve_end > region_end) {
        reserve_end = region_end;
    }

    // find the page idx range
    page_start = (start - G_MEMPOOL_START) >> PAGE_SHIFT; // >> PAGE_SHIFT = / PAGE_SIZE
    page_end = align_up_ul(reserve_end - G_MEMPOOL_START, PAGE_SIZE) >> PAGE_SHIFT; // align_up_ul() to make sure the tailing page is fully reserved
    if (page_end > G_MEM_TOTAL_PAGE) { // aviod overflow
        page_end = G_MEM_TOTAL_PAGE;
    }

    for (i = page_start; i < page_end; ++i) {
        frame_array[i].state = PAGE_STATE_RESERVED;
    }
}

void buddy_init(void) {
    unsigned long idx = 0;
    unsigned long i;

    for (i = 0; i < G_MEM_TOTAL_PAGE; ++i) {
        frame_array[i].order = -1;
        if (frame_array[i].state != PAGE_STATE_RESERVED) {
            frame_array[i].state = PAGE_STATE_ALLOC_PAGE_TAIL;
        }
        frame_array[i].meta_pool_idx = -1;
        INIT_LIST_HEAD(&frame_array[i].node);
    }

    for (i = 0; i <= BUDDY_MAX_ORDER; ++i) {
        INIT_LIST_HEAD(&free_area[i]);
    }

    build_reserved_prefix();

    while (idx < G_MEM_TOTAL_PAGE) {
        if (frame_array[idx].state == PAGE_STATE_RESERVED) {
            frame_array[idx].order = 0;
            idx++;
            continue;
        }

        int order;
        for (order = BUDDY_MAX_ORDER; order >= 0; --order) {
            unsigned long block_pages = 1UL << order;
            if ((idx & (block_pages - 1)) == 0 &&
                idx + block_pages <= G_MEM_TOTAL_PAGE &&
                !range_has_reserved(idx, block_pages)) {
                add_free_block(idx, (unsigned int)order);
                idx += block_pages;
                break;
            }
        }
    }

    buddy_ready = 1;
    BUDDY_LOG("[Init] Buddy initialized at [0x%lx, 0x%lx), total pages: %u\r\n",
              G_MEMPOOL_START,
              G_MEMPOOL_START + G_MEMPOOL_SIZE,
              (unsigned int)G_MEM_TOTAL_PAGE);
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

        mark_block(idx, order, PAGE_STATE_ALLOC_PAGE_HEAD, PAGE_STATE_ALLOC_PAGE_TAIL);
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
    if (addr < G_MEMPOOL_START || addr >= G_MEMPOOL_START + G_MEMPOOL_SIZE) {
        return;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return;
    }

    idx = (addr - G_MEMPOOL_START) >> PAGE_SHIFT;
    if (idx >= G_MEM_TOTAL_PAGE || frame_array[idx].state != PAGE_STATE_ALLOC_PAGE_HEAD) {
        return;
    }

    order = (unsigned int)frame_array[idx].order;
    mark_block(idx, order, PAGE_STATE_FREE_HEAD, PAGE_STATE_FREE_TAIL);

    while (order < BUDDY_MAX_ORDER) {
        unsigned long buddy_idx = idx ^ (1UL << order);

        if (buddy_idx >= G_MEM_TOTAL_PAGE) {
            break;
        }
        if (frame_array[buddy_idx].state != PAGE_STATE_FREE_HEAD ||
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
        mark_block(idx, order, PAGE_STATE_FREE_HEAD, PAGE_STATE_FREE_TAIL);
        BUDDY_LOG("[Merge] Merge iteratively to order %u at page %lu\r\n", order, idx);
    }

    add_free_block(idx, order);
    BUDDY_LOG("[Page] Free 0x%lx and add back to order %u, page %lu\r\n",
              page_to_addr(idx),
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