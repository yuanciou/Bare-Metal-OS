#include "video.h"
#include "mmu.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include <stdint.h>

#ifdef QEMU
#define FB_BASE   (0x87000000UL + PAGE_OFFSET)
#define FB_WIDTH  1920
#define FB_HEIGHT 1080
#define FB_BPP    4
#define XRGB8888  875713112

#define QEMU_PACKED __attribute__((packed))
#define bswap64(x) \
    (((x) & 0x00000000000000FFULL) << 56 | \
     ((x) & 0x000000000000FF00ULL) << 40 | \
     ((x) & 0x0000000000FF0000ULL) << 24 | \
     ((x) & 0x00000000FF000000ULL) << 8  | \
     ((x) & 0x000000FF00000000ULL) >> 8  | \
     ((x) & 0x0000FF0000000000ULL) >> 24 | \
     ((x) & 0x00FF000000000000ULL) >> 40 | \
     ((x) & 0xFF00000000000000ULL) >> 56)
#define bswap32(x) \
    (((x) & 0x000000FFu) << 24 | \
     ((x) & 0x0000FF00u) << 8  | \
     ((x) & 0x00FF0000u) >> 8  | \
     ((x) & 0xFF000000u) >> 24)
#define bswap16(x) \
    (((x) & 0x00FFu) << 8 | \
     ((x) & 0xFF00u) >> 8)

struct QEMU_PACKED RAMFBCfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

#define FW_CFG_BASE   (0x10100000UL + PAGE_OFFSET)
#define FW_CFG_SELECT (uint16_t*)(FW_CFG_BASE + 0x08)
#define FW_CFG_DATA   (uint64_t*)(FW_CFG_BASE + 0x00)
#define FW_CFG_DMA    (uint64_t*)(FW_CFG_BASE + 0x10)

#define FW_CFG_DMA_CTL_ERROR  0x01
#define FW_CFG_DMA_CTL_READ   0x02
#define FW_CFG_DMA_CTL_SKIP   0x04
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE  0x10

#define FW_CFG_FILE_DIR 0x19

struct QEMU_PACKED FWCfgFile {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char name[56];
};

struct QEMU_PACKED FWCfgFiles {
    uint32_t count;
    struct FWCfgFile f[];
};

struct QEMU_PACKED FWCfgDmaAccess {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

static void fw_cfg_dma_transfer(void* address, uint32_t length, uint32_t control) {
    struct FWCfgDmaAccess access = {
        .control = bswap32(control),
        .length = bswap32(length),
        .address = bswap64((uint64_t)address & ~PAGE_OFFSET), // since DMA address should be physical
    };
    *FW_CFG_DMA = bswap64((uint64_t)&access & ~PAGE_OFFSET); // since DMA address should be physical
    while (bswap32(access.control) & ~FW_CFG_DMA_CTL_ERROR);
}

static void fw_cfg_read_entry(void* buf, int e, int len) {
    uint32_t control = (e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ;
    fw_cfg_dma_transfer(buf, len, control);
}

static void fw_cfg_write_entry(void* buf, int e, int len) {
    uint32_t control = (e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE;
    fw_cfg_dma_transfer(buf, len, control);
}

static int fw_cfg_find_file(const char* name) {
    uint32_t count = 0;
    fw_cfg_read_entry(&count, FW_CFG_FILE_DIR, sizeof(count));
    count = bswap32(count);
    for (int i = 0; i < count; i++) {
        struct FWCfgFile file;
        fw_cfg_dma_transfer(&file, sizeof(file), FW_CFG_DMA_CTL_READ);
        if (strncmp(name, file.name, sizeof(file.name)) == 0)
            return bswap16(file.select);
    }
    return -1;
}
#endif

#ifdef ORANGE_PI
#define FB_BASE   (0x7f700000UL + PAGE_OFFSET)
#define FB_WIDTH  1920
#define FB_HEIGHT 1080
#define FB_BPP    4
#endif

#define CACHE_BLOCK_SIZE 64

/**
 * @brief Flush the cache for a given memory region.
 *        move the parameter to a0 register
 *        machine code for "cbo.flush %0" instruction
 *
 *        input operand: the address to flush, passed in a general-purpose register
 *        clobber list: indicates that this assembly code may modify memory and the a0 register, preventing certain compiler optimizations
 * @param start The starting address of the region to flush.
 */
#define cbo_flush(start)                \
    ({                                  \
        asm volatile("mv a0, %0\n\t"    \
                     ".word 0x0025200F" \
                     :                  \
                     : "r"(start)       \
                     : "memory", "a0"); \
    })

/**
 * @brief Flush the cache for a given memory region.
 *        __sync_synchronize():
 *            - compiler barrier: prevent the instr reordering across this point
 *            - hardware barrier: ensure all memory operation before this point are completed.
 */
static void flush_dcache(void* addr, unsigned long len) {
#ifdef ORANGE_PI
    // Align down the start address according to the cache block size
    unsigned long start = (unsigned long)addr & ~(CACHE_BLOCK_SIZE - 1);
    __sync_synchronize(); // check memcpy is completed
    
    // Flush the cache line by line (CACHE_BLOCK_SIZE)
    for (unsigned long line = start; line < (unsigned long)addr + len; line += CACHE_BLOCK_SIZE) {
        cbo_flush(line);
        __sync_synchronize(); // ensure the flush is completed
    }
#endif
}

void video_init() {
#ifdef QEMU
    struct RAMFBCfg cfg = {
        .addr = bswap64(FB_BASE & ~PAGE_OFFSET), // since DMA address should be physical
        .fourcc = bswap32(XRGB8888),
        .flags = bswap32(0),
        .width = bswap32(FB_WIDTH),
        .height = bswap32(FB_HEIGHT),
        .stride = bswap32(FB_WIDTH * FB_BPP),
    };
    int file_select = fw_cfg_find_file("etc/ramfb");
    if (file_select != -1) {
        fw_cfg_write_entry(&cfg, file_select, sizeof(struct RAMFBCfg));
    } else {
        printf("Failed to find etc/ramfb in fw_cfg\r\n");
    }
#endif
    // Orange Pi needs no initialization as per TODO.md
}

void video_get_info(struct framebuffer_info* info) {
    info->width = FB_WIDTH;
    info->height = FB_HEIGHT;
    info->bpp = FB_BPP;
}

void video_flush(void* addr, unsigned long len) {
    flush_dcache(addr, len);
}

void* video_get_base(void) {
    return (void*)FB_BASE;
}

/**
 * @brief Display the BMP image on the screen by copying the image data to the framebuffer row by row.
 *        call flush_dcache after copying each row to ensure the data is written to DRAM and visible to HDMI controller.
 */
void video_bmp_display(unsigned int* bmp_image, unsigned int width, unsigned int height) {
    unsigned int* fb = (unsigned int*)FB_BASE;
    
    // Center the image on the screen
    int start_x = (FB_WIDTH - (int)width) / 2;
    int start_y = (FB_HEIGHT - (int)height) / 2;

    // Copy the BMP image to the framebuffer line by line, flushing the cache after each line
    // since the image size may smaller than the FB size.
    for (int y = 0; y < (int)height; y++) {
        void* dst = fb + (start_y + y) * FB_WIDTH + start_x;
        memcpy(dst, bmp_image + y * width, width * sizeof(unsigned int));
        
        // Flush the cache since CPU may write the image only on its cache not DRAM
        // But HDMI controller does not have the right to access CPU cache, it will directly read from (0x7f700000)
        flush_dcache(dst, width * sizeof(unsigned int));
    }
}
