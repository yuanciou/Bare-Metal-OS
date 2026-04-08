#include "allocator.h"

#include <stdint.h>

#include "../lib/align.h"
#include "../lib/buddy.h"
#include "../lib/endian.h"
#include "../lib/fdt.h"
#include "../lib/list.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "config.h"

extern char _start;
extern char _end;

#if ALLOC_ENABLE_DEMO_LOG
#define ALLOC_LOG(...) printf(__VA_ARGS__)
#else
#define ALLOC_LOG(...) do { } while (0)
#endif

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
static int g_allocator_ready;

// for startup memory reservation
struct reserved_region {
    unsigned long start;
    unsigned long size;
};

#define MAX_RESERVED_REGIONS 32
static struct reserved_region reserved_rgns[MAX_RESERVED_REGIONS];
static int reserved_rgn_count = 0;

/**
 * @brief Round up the number of pages to the nearest power of 2
 *
 * @param pages the number of pages
 * @return the order of the block that can hold at least `pages` pages
 */
static unsigned long roundup_order(unsigned long pages) {
    unsigned int order = 0;
    unsigned long n = 1;

    while (n < pages) {
        n <<= 1; // n = n * 2
        order++;
    }
    return order;
}

/**
 * @brief return the page idx in frame_array for the given addr
 */
static unsigned long addr_to_page_idx(unsigned long addr) {
    return (addr - G_MEMPOOL_START) >> PAGE_SHIFT;
}

/**
 * @brief return the idx in g_pool for the given size
 */
static int find_pool_index(unsigned long size) {
    int i;

    for (i = 0; i < CHUNK_POOL_COUNT; ++i) {
        if (size <= g_pools[i].chunk_size) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Alloc a new page from buddy system and split it into chunks to add them on free list
 */
static int expand_chunk_pool(int pool_idx) {
    void *page;
    unsigned long page_addr;
    unsigned long page_idx;
    unsigned int chunk_size;
    unsigned int chunk_cnt;
    unsigned int i;

    // allocate a new page from buddy system
    page = buddy_alloc_pages(0);
    if (!page) {
        return -1;
    }

    page_addr = (unsigned long)page;
    page_idx = addr_to_page_idx(page_addr);
    chunk_size = g_pools[pool_idx].chunk_size;
    chunk_cnt = PAGE_SIZE / chunk_size;

    // mark this page as used for chunk pool
    frame_array[page_idx].state = PAGE_STATE_ALLOC_CHUNK_POOL;
    frame_array[page_idx].meta_pool_idx = (signed char)pool_idx;
    frame_array[page_idx].order = 0;
    frame_array[page_idx].ref_count = 0;

    // directly change the page into chunks
    for (i = 0; i < chunk_cnt; ++i) {
        unsigned long chunk_addr = page_addr + (unsigned long)i * chunk_size;
        // set each chunk addr as `struct chunk` and add it to free list
        struct chunk *ck = (struct chunk *)chunk_addr;
        list_add(&ck->node, &g_pools[pool_idx].free_list);
    }

    ALLOC_LOG("[Chunk] Refill pool size %u from page 0x%lx, total chunks: %u\r\n",
              chunk_size,
              page_addr,
              chunk_cnt);
    return 0;
}

/**
 * @brief Reserve the kernel image, DTB blob, initramfs, region in reserved-memory node
 */
static void reserve_all_memory(const void *fdt) {
    unsigned long dtb_size;
    unsigned long initrd_start;
    unsigned long initrd_end;
    int idx;
    unsigned long start;
    unsigned long size;

    // Reserve kernel image
        // _start and _end are defined in the linker script
    ALLOC_LOG("----------------------------\r\n");
    ALLOC_LOG("Reserve kernel image:\r\n");
    memory_reserve((unsigned long)&_start, (unsigned long)(&_end - &_start));
    ALLOC_LOG("----------------------------\r\n");

    if (fdt) {
        // Reserve DTB blob
            // start addr is pointer `fdt`
            // dtb_size is from the FDT header filed `totalsize`
        dtb_size = fdt_totalsize(fdt);
        if (dtb_size != 0) {
            ALLOC_LOG("Reserve DTB:\r\n");
            memory_reserve((unsigned long)fdt, dtb_size);
            ALLOC_LOG("----------------------------\r\n");
        }

        // Reserve initramfs
            // from the /chosen node in FDT
        initrd_start = get_initrd_start(fdt);
        initrd_end = get_initrd_end(fdt);
        if (initrd_start && initrd_end > initrd_start) {
            ALLOC_LOG("Reserve initramfs:\r\n");
            memory_reserve(initrd_start, initrd_end - initrd_start);
            ALLOC_LOG("----------------------------\r\n");
        }

        // Reserve reserved memory ranges
            // from all region in `reserved-memory` node in FDT
        idx = 0;  // idx++ to avoid always return the first region in `reserved-memory` node
        while (fdt_get_reserved_memory_region(fdt, idx, &start, &size) == 0) {
            ALLOC_LOG("Reserve reserved memory %d:\r\n", idx);
            memory_reserve(start, size);
            ALLOC_LOG("----------------------------\r\n");
            idx++;
        }
    }
}

/**
 * @brief Save the reserved region for startup memory reservation in `reserved_rgns`
 */
void memory_reserve(unsigned long start, unsigned long size) {
    unsigned long end;
    unsigned long pool_start;
    unsigned long pool_end;
    unsigned long reserve_start;
    unsigned long reserve_end;

    if (size == 0 || reserved_rgn_count >= MAX_RESERVED_REGIONS) {
        return;
    }

    end = start + size;
    pool_start = G_MEMPOOL_START;
    pool_end = pool_start + G_MEMPOOL_SIZE;

    // skip the region that is out of memory pool
    if (end <= pool_start || start >= pool_end) {
        ALLOC_LOG("[Reserve] Skip address [0x%lx, 0x%lx): out of pool [0x%lx, 0x%lx)\r\n",
                  start,
                  end,
                  pool_start,
                  pool_end);
        return;
    }

    // only handle the region in the memory pool
    reserve_start = start < pool_start ? pool_start : start;
    reserve_end = end > pool_end ? pool_end : end;

    ALLOC_LOG("[Reserve] Reserve address [0x%lx, 0x%lx). Range of pages: [%lu, %lu)\r\n",
              reserve_start,
              reserve_end,
              (reserve_start - pool_start) >> PAGE_SHIFT,
              (reserve_end - pool_start + PAGE_SIZE - 1UL) >> PAGE_SHIFT);
    
    reserved_rgns[reserved_rgn_count].start = reserve_start;
    reserved_rgns[reserved_rgn_count].size = reserve_end - reserve_start;
    reserved_rgn_count++;
}

/**
 * @brief The startup allocation to reserved memory and
 *        find a region for `frame_array` with bump allocator
 * @param fdt the pointer to the FDT blob
 */
void *startup_alloc(const void *fdt) {
    unsigned long pool_start = G_MEMPOOL_START;
    unsigned long pool_size = G_MEMPOOL_SIZE;

    // Parse the memory pool region from FDT
    if (fdt) {
        int memory_offset = fdt_path_offset(fdt, "/memory");

        if (memory_offset >= 0) {
            int len = 0;
            const void *prop;

            prop = fdt_getprop(fdt, memory_offset, "reg", &len);
            if (prop) {
                if (len >= 16) { // Orange Pi
                    pool_start = (unsigned long)bswap64(*(const uint64_t *)prop);
                    pool_size = (unsigned long)bswap64(*((const uint64_t *)prop + 1));
                } else if (len >= 8) { // QEMU
                    pool_start = (unsigned long)bswap32(*(const uint32_t *)prop);
                    pool_size = (unsigned long)bswap32(*((const uint32_t *)prop + 1));
                } else {
                    pool_start = BUDDY_DEFAULT_POOL_START;
                    pool_size = BUDDY_DEFAULT_POOL_SIZE;
                }
            }
        }
    }

    // set `G_MEMPOOL_START`, `G_MEMPOOL_SIZE` and `G_MEM_TOTAL_PAGE`
    buddy_set_region(pool_start, pool_size);

    // set G_MEMPOOL_START, G_MEMPOOL_SIZE and G_MEM_TOTAL_PAGE
    reserve_all_memory(fdt);

    unsigned long pool_end = G_MEMPOOL_START + G_MEMPOOL_SIZE;
    unsigned long alloc_start = G_MEMPOOL_START; // init alloc start to the pool start
    unsigned long frame_array_size = G_MEM_TOTAL_PAGE * sizeof(struct frame);
    
    // find the region for `frame_array` with bump allocator
    while (1) {
        unsigned long alloc_end;
        int overlap = 0;
        int i;

        alloc_start = align_up_ul(alloc_start, PAGE_SIZE);
        alloc_end = alloc_start + frame_array_size;

        if (alloc_end > pool_end) {
            break; // Failed to allocate
        }

        // Check if [alloc_start, alloc_end) overlaps with any reserved_rgn
        for (i = 0; i < reserved_rgn_count; ++i) {
            unsigned long r_start = reserved_rgns[i].start;
            unsigned long r_end = r_start + reserved_rgns[i].size;

            if (alloc_start < r_end && alloc_end > r_start) {
                overlap = 1;
                alloc_start = r_end; // Skip to after this reserved region
                break;
            }
        }

        // If no overlap and in the memory pool region
        // -> reserve the region for `frame_array`
        if (!overlap) {
            ALLOC_LOG("Reserve frame array\r\n");
            memory_reserve(alloc_start, frame_array_size);
            ALLOC_LOG("----------------------------\r\n");
            return (void *)alloc_start;
        }
    }
    return 0;
}

/**
 * @brief call startup allocation, init buddy sytem and init chunk pools
 */
void allocator_init(const void *fdt) {
    unsigned int i;

    // Dynamic Frame Array size using bump allocator
    frame_array = (struct frame *)startup_alloc(fdt);

    if (!frame_array) {
        ALLOC_LOG("[Panic] Failed to allocate frame_array in startup_alloc.\r\n");
        return;
    }

    // Now that frame array is created, we can tell buddy to mark pages as reserved
    // since `reserved_rgns` is a local array, so we do this here
    for (i = 0; i < reserved_rgn_count; ++i) {
        buddy_mark_reserved_range(reserved_rgns[i].start, reserved_rgns[i].size);
    }

    // init the buddy system
    buddy_init();

    // init the chunk pools
    for (i = 0; i < CHUNK_POOL_COUNT; ++i) {
        g_pools[i].chunk_size = g_pool_sizes[i];
        INIT_LIST_HEAD(&g_pools[i].free_list);
    }

    g_allocator_ready = 1;
    ALLOC_LOG("[Init] Dynamic allocator ready. Small alloc <= %u bytes\r\n",
              (unsigned int)2048UL);
}

void allocator_dump_pages(void) {
    buddy_dump_free_areas();
}

void *allocate(unsigned long size) {
    int pool_idx;

    // wrong allocation
    if (!g_allocator_ready || size == 0 || size > MAX_ALLOC_SIZE) {
        if (size > MAX_ALLOC_SIZE) {
            ALLOC_LOG("[Alloc] Reject size %lu (> MAX_ALLOC_SIZE)\r\n", size);
        }
        return NULL;
    }

    // for small alloc request, use chunk pool
    if (size <= 2048UL) {
        struct list_head *n;
        struct chunk *ck;
        unsigned int chunk_size;

        // find the pool idx for this size
        pool_idx = find_pool_index(size);
        if (pool_idx < 0) {
            return NULL;
        }

        // if the free list is empty, expand a new page from buddy system
        if (list_empty(&g_pools[pool_idx].free_list)) {
            if (expand_chunk_pool(pool_idx) != 0) {
                return NULL;
            }
        }

        n = g_pools[pool_idx].free_list.next; // get the first free chunk
        list_del(n);    // del from free list
        ck = list_entry(n, struct chunk, node); // use the node to get the chunk address
        chunk_size = g_pools[pool_idx].chunk_size;

        frame_array[addr_to_page_idx((unsigned long)ck)].ref_count++;

        ALLOC_LOG("[Chunk] Allocate 0x%lx at chunk size %u\r\n",
                  (unsigned long)ck,
                  chunk_size);
        return (void *)ck;
    }

    // large alloc request -> buddy system
    {
        unsigned long pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT; // align the size to #pages
        unsigned int order = roundup_order(pages); // align the #pages to power of 2
        void *ptr = buddy_alloc_pages(order);

        if (!ptr) {
            return NULL;
        }
        return ptr;
    }
}

void free(void *ptr) {
    unsigned long addr;
    unsigned long page_idx;
    enum page_state state;

    if (!g_allocator_ready || !ptr) {
        return;
    }

    addr = (unsigned long)ptr;
    // check valid region
    if (!(G_MEMPOOL_SIZE != 0 && addr >= G_MEMPOOL_START && addr < G_MEMPOOL_START + G_MEMPOOL_SIZE)) {
        return;
    }

    // check the page idx of the addr in frame array 
    page_idx = addr_to_page_idx(addr);
    if (page_idx >= G_MEM_TOTAL_PAGE) {
        return;
    }

    state = frame_array[page_idx].state;

    // free a small chunk -> add back to chunk pool free list
    if (state == PAGE_STATE_ALLOC_CHUNK_POOL) {
        int pool_idx = frame_array[page_idx].meta_pool_idx;
        unsigned long page_base = align_down_ul(addr, PAGE_SIZE);   // align down the addr to its page base address
        unsigned int chunk_size;
        unsigned long offset;
        struct chunk *ck;

        if (pool_idx < 0 || pool_idx >= CHUNK_POOL_COUNT) {
            return;
        }

        chunk_size = g_pools[pool_idx].chunk_size;
        
        // check the valid alignment of chunk addr in the page
        offset = addr - page_base;
        if (offset % chunk_size != 0) {
            return;
        }

        // change the ptr to struct chunk and add it back to free list
        ck = (struct chunk *)ptr;
        list_add(&ck->node, &g_pools[pool_idx].free_list);
        
        frame_array[page_idx].ref_count--;

        // return back to buddy system if all chunks in this page are freed
        if (frame_array[page_idx].ref_count == 0) {
            struct list_head *pos, *q;
            
            // Remove all chunks belonging to this page from the free list
            pos = g_pools[pool_idx].free_list.next;
            while (pos != &g_pools[pool_idx].free_list) {
                q = pos->next;
                unsigned long node_addr = (unsigned long)list_entry(pos, struct chunk, node);
                if (node_addr >= page_base && node_addr < page_base + PAGE_SIZE) {
                    list_del(pos);
                }
                pos = q;
            }
            
            // Return page to buddy system
            frame_array[page_idx].state = PAGE_STATE_ALLOC_PAGE_HEAD;
            buddy_free_pages((void *)page_base);
            ALLOC_LOG("[Chunk] Page 0x%lx all chunks freed, return to buddy system\r\n", page_base);
        } else {
            ALLOC_LOG("[Chunk] Free 0x%lx at chunk size %u\r\n", addr, chunk_size);
        }
        return;
    }

    if (state == PAGE_STATE_ALLOC_PAGE_HEAD) {
        // check the addr is page-align
        if ((addr & (PAGE_SIZE - 1UL)) != 0) {
            return;
        }

        buddy_free_pages(ptr);
    }
}