#include "allocator.h"

#include <stdint.h>

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

static unsigned long roundup_order(unsigned long pages) {
    unsigned int order = 0;
    unsigned long n = 1;

    while (n < pages) {
        n <<= 1;
        order++;
    }
    return order;
}

static int addr_in_pool(unsigned long addr) {
    unsigned long start = G_MEMPOOL_START;
    unsigned long size = G_MEMPOOL_SIZE;

    return size != 0 && addr >= start && addr < start + size;
}

static unsigned long addr_to_page_idx(unsigned long addr) {
    return (addr - G_MEMPOOL_START) >> PAGE_SHIFT;
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

    frame_array[page_idx].state = PAGE_STATE_ALLOC_CHUNK_POOL;
    frame_array[page_idx].meta_pool_idx = (signed char)pool_idx;
    frame_array[page_idx].meta_order = 0;

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

    frame_array[head_idx].state = PAGE_STATE_ALLOC_PAGE_HEAD;
    frame_array[head_idx].meta_order = (signed char)order;
    frame_array[head_idx].meta_pool_idx = -1;

    for (i = 1; i < count; ++i) {
        frame_array[head_idx + i].state = PAGE_STATE_ALLOC_PAGE_TAIL;
        frame_array[head_idx + i].meta_order = -1;
        frame_array[head_idx + i].meta_pool_idx = -1;
    }
}

static void clear_page_meta(unsigned long head_idx, unsigned int order) {
    unsigned long count = 1UL << order;
    unsigned long i;

    for (i = 0; i < count; ++i) {
        // frame_array[head_idx + i].state = PAGE_STATE_FREE_HEAD;
        frame_array[head_idx + i].meta_order = -1;
        frame_array[head_idx + i].meta_pool_idx = -1;
    }
}

static void read_cells_from_node(const void *fdt,
                                 int node_offset,
                                 unsigned int *addr_cells,
                                 unsigned int *size_cells) {
    int len = 0;
    const void *prop;

    *addr_cells = 2;
    *size_cells = 2;

    prop = fdt_getprop(fdt, node_offset, "#address-cells", &len);
    if (prop && len >= 4) {
        *addr_cells = (unsigned int)bswap32(*(const uint32_t *)prop);
    }

    prop = fdt_getprop(fdt, node_offset, "#size-cells", &len);
    if (prop && len >= 4) {
        *size_cells = (unsigned int)bswap32(*(const uint32_t *)prop);
    }
}

static void reserve_dtb_blob(const void *fdt) {
    unsigned long dtb_size;

    if (!fdt) {
        return;
    }

    dtb_size = fdt_totalsize(fdt);
    if (dtb_size != 0) {
        ALLOC_LOG("Reserve DTB:\r\n");
        memory_reserve((unsigned long)fdt, dtb_size);
        ALLOC_LOG("----------------------------\r\n");
    }
}

static void reserve_kernel_image(void) {
    ALLOC_LOG("Reserve kernel image:\r\n");
    memory_reserve((unsigned long)&_start, (unsigned long)(&_end - &_start));
    ALLOC_LOG("----------------------------\r\n");
}

static void reserve_initramfs(const void *fdt) {
    unsigned long initrd_start;
    unsigned long initrd_end;

    if (!fdt) {
        return;
    }

    initrd_start = get_initrd_start(fdt);
    initrd_end = get_initrd_end(fdt);

    if (initrd_start && initrd_end > initrd_start) {
        ALLOC_LOG("Reserve initramfs:\r\n");
        memory_reserve(initrd_start, initrd_end - initrd_start);
        ALLOC_LOG("----------------------------\r\n");
    }
}

static void reserve_reserved_memory_ranges(const void *fdt) {
    int idx = 0;
    unsigned long start;
    unsigned long size;

    while (fdt_get_reserved_memory_region(fdt, idx, &start, &size) == 0) {
        ALLOC_LOG("Reserve reserved memory %d:\r\n", idx);
        memory_reserve(start, size);
        ALLOC_LOG("----------------------------\r\n");
        idx++;
    }
}

void memory_reserve(unsigned long start, unsigned long size) {
    unsigned long end;
    unsigned long pool_start;
    unsigned long pool_end;
    unsigned long reserve_start;
    unsigned long reserve_end;

    if (size == 0) {
        return;
    }

    end = start + size;
    pool_start = G_MEMPOOL_START;
    pool_end = pool_start + G_MEMPOOL_SIZE;

    if (end <= pool_start || start >= pool_end) {
        ALLOC_LOG("[Reserve] Skip address [0x%lx, 0x%lx): out of pool [0x%lx, 0x%lx)\r\n",
                  start,
                  end,
                  pool_start,
                  pool_end);
        return;
    }

    reserve_start = start < pool_start ? pool_start : start;
    reserve_end = end > pool_end ? pool_end : end;

    ALLOC_LOG("[Reserve] Reserve address [0x%lx, 0x%lx). Range of pages: [%lu, %lu)\r\n",
              reserve_start,
              reserve_end,
              (reserve_start - pool_start) >> PAGE_SHIFT,
              (reserve_end - pool_start + PAGE_SIZE - 1UL) >> PAGE_SHIFT);
    buddy_mark_reserved_range(reserve_start, reserve_end - reserve_start);
}

void allocator_init(const void *fdt) {
    unsigned long pool_start = BUDDY_DEFAULT_POOL_START;
    unsigned long pool_size = BUDDY_DEFAULT_POOL_SIZE;
    unsigned int i;

    if (fdt) {
        int memory_offset = fdt_path_offset(fdt, "/memory");

        if (memory_offset >= 0) {
            unsigned int addr_cells = 2;
            unsigned int size_cells = 2;
            int len = 0;
            const void *prop;

            read_cells_from_node(fdt, memory_offset, &addr_cells, &size_cells);
            prop = fdt_getprop(fdt, memory_offset, "reg", &len);
            if (prop) {
                if (addr_cells == 1 && size_cells == 1 && len >= 8) {
                    pool_start = (unsigned long)bswap32(*(const uint32_t *)prop);
                    pool_size = (unsigned long)bswap32(*((const uint32_t *)prop + 1));
                } else if (addr_cells >= 2 && size_cells >= 2 && len >= 16) {
                    pool_start = (unsigned long)bswap64(*(const uint64_t *)prop);
                    pool_size = (unsigned long)bswap64(*((const uint64_t *)prop + 1));
                } else if (len >= 16) {
                    pool_start = (unsigned long)bswap64(*(const uint64_t *)prop);
                    pool_size = (unsigned long)bswap64(*((const uint64_t *)prop + 1));
                } else if (len >= 8) {
                    pool_start = (unsigned long)bswap32(*(const uint32_t *)prop);
                    pool_size = (unsigned long)bswap32(*((const uint32_t *)prop + 1));
                }
            }
        }
    }

    buddy_set_region(pool_start, pool_size);

    reserve_kernel_image();
    if (fdt) {
        reserve_dtb_blob(fdt);
        reserve_initramfs(fdt);
        reserve_reserved_memory_ranges(fdt);
    }

    buddy_init();

    for (i = 0; i < CHUNK_POOL_COUNT; ++i) {
        g_pools[i].chunk_size = g_pool_sizes[i];
        INIT_LIST_HEAD(&g_pools[i].free_list);
    }

    for (i = 0; i < BUDDY_TOTAL_PAGES; ++i) {
        frame_array[i].meta_order = -1;
        frame_array[i].meta_pool_idx = -1;
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
    enum page_state state;

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

    state = frame_array[page_idx].state;

    if (state == PAGE_STATE_ALLOC_CHUNK_POOL) {
        int pool_idx = frame_array[page_idx].meta_pool_idx;
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

    if (state == PAGE_STATE_ALLOC_PAGE_HEAD) {
        unsigned int order;

        if ((addr & (PAGE_SIZE - 1UL)) != 0) {
            return;
        }

        order = (unsigned int)frame_array[page_idx].meta_order;
        buddy_free_pages(ptr);
        clear_page_meta(page_idx, order);
    }
}