#include "mmu.h"
#include "config.h"
#include "thread.h"
#include "../lib/string.h"

// We need a statically allocated page for the root page table (PGD)
// to use during boot before the allocator is ready.
unsigned long boot_pgd[512] __attribute__((aligned(PAGE_SIZE)));
unsigned long boot_pmd[512] __attribute__((aligned(PAGE_SIZE)));

// External symbols from linker script
extern char _start;
extern char _end;
unsigned long setup_vm(void) {
    // Clear PGD
    memset(boot_pgd, 0, PAGE_SIZE);

    // Identify current physical address of kernel
    unsigned long kernel_pa;
    asm volatile("la %0, _start" : "=r"(kernel_pa));

    // Identity mapping (VA = PA)
    // Map the 1GB block containing the kernel
    unsigned long ram_base = kernel_pa & ~((1UL << PGD_SHIFT) - 1);
    unsigned long vpn2_ident = (ram_base >> PGD_SHIFT) & VPN_MASK;
    boot_pgd[vpn2_ident] = (ram_base >> PGD_SHIFT << 28) | PROT_KERNEL;

    // Higher-half mapping (VA = PA + PAGE_OFFSET)
    // Map 4GB starting from physical 0 to PAGE_OFFSET
    // This covers RAM, FDT, and most SoC peripherals (UART, etc.)
    for (int i = 0; i < 4; i++) {
        unsigned long pa = (unsigned long)i << 30;
        unsigned long vpn2 = ((PAGE_OFFSET + pa) >> PGD_SHIFT) & VPN_MASK;
        boot_pgd[vpn2] = (pa >> 30 << 28) | PROT_KERNEL;
    }

    // Write satp and flush TLB
    unsigned long satp_val = MAKE_SATP(boot_pgd);
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(satp_val)
        : "memory"
    );

    return PAGE_OFFSET;
}


void drop_identity_map(void) {
    // We need to know which entry was the identity mapping.
    // Since it's boot_pgd, we can just zero out the low entries.
    // For Sv39, kernel is at 256+, so entries 0-255 are "low"
    for (int i = 0; i < 256; i++) {
        boot_pgd[i] = 0;
    }
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

unsigned long* kernel_pgd = NULL;

extern void* allocate(unsigned long size);
extern void free(void* ptr);

static unsigned long* pagewalk(unsigned long* pgd, unsigned long va, int alloc) {
    unsigned long vpn2 = (va >> PGD_SHIFT) & VPN_MASK;
    unsigned long vpn1 = (va >> PMD_SHIFT) & VPN_MASK;
    unsigned long vpn0 = (va >> PTE_SHIFT) & VPN_MASK;

    unsigned long* pte = &pgd[vpn2];
    if (!(*pte & PTE_V)) {
        if (!alloc) return 0;
        unsigned long* new_pg = (unsigned long*)allocate(PAGE_SIZE);
        if (!new_pg) return 0;
        memset(new_pg, 0, PAGE_SIZE);
        *pte = (((unsigned long)new_pg - PAGE_OFFSET) >> 12 << 10) | PTE_V;
    }

    unsigned long* pmd = (unsigned long*)(((*pte >> 10) << 12) + PAGE_OFFSET);
    pte = &pmd[vpn1];
    if (!(*pte & PTE_V)) {
        if (!alloc) return 0;
        unsigned long* new_pg = (unsigned long*)allocate(PAGE_SIZE);
        if (!new_pg) return 0;
        memset(new_pg, 0, PAGE_SIZE);
        *pte = (((unsigned long)new_pg - PAGE_OFFSET) >> 12 << 10) | PTE_V;
    }

    unsigned long* pt = (unsigned long*)(((*pte >> 10) << 12) + PAGE_OFFSET);
    return &pt[vpn0];
}

void map_pages(unsigned long* pgd, unsigned long va, unsigned long pa, unsigned long size, unsigned long prot) {
    for (unsigned long a = va; a < va + size; a += PAGE_SIZE, pa += PAGE_SIZE) {
        unsigned long* pte = pagewalk(pgd, a, 1);
        if (pte) *pte = (pa >> 12 << 10) | prot;
    }
}

void map_user_pages(unsigned long va, unsigned long size, unsigned long pa, unsigned long prot) {
    thread* current = get_cur_thread();
    if (current && current->pgd) {
        map_pages(current->pgd, va, pa, size, prot | PTE_U);
    }
}

unsigned long* copy_pgd(unsigned long* pgd) {
    unsigned long* new_pgd = (unsigned long*)allocate(PAGE_SIZE);
    if (!new_pgd) return NULL;
    memset(new_pgd, 0, PAGE_SIZE);
    
    // Copy kernel mappings (upper half)
    for (int i = 256; i < 512; i++) {
        new_pgd[i] = pgd[i];
    }

    // Copy user mappings (lower half)
    for (int i = 0; i < 256; i++) {
        if (pgd[i] & PTE_V) {
            unsigned long* parent_pmd = (unsigned long*)((((pgd[i] >> 10) << 12)) + PAGE_OFFSET);
            unsigned long* child_pmd = (unsigned long*)allocate(PAGE_SIZE);
            if (!child_pmd) { free_pgd(new_pgd); return NULL; }
            memset(child_pmd, 0, PAGE_SIZE);
            new_pgd[i] = (((unsigned long)child_pmd - PAGE_OFFSET) >> 12 << 10) | PTE_V;

            for (int j = 0; j < 512; j++) {
                if (parent_pmd[j] & PTE_V) {
                    unsigned long* parent_pt = (unsigned long*)((((parent_pmd[j] >> 10) << 12)) + PAGE_OFFSET);
                    unsigned long* child_pt = (unsigned long*)allocate(PAGE_SIZE);
                    if (!child_pt) { free_pgd(new_pgd); return NULL; }
                    memset(child_pt, 0, PAGE_SIZE);
                    child_pmd[j] = (((unsigned long)child_pt - PAGE_OFFSET) >> 12 << 10) | PTE_V;

                    for (int k = 0; k < 512; k++) {
                        if (parent_pt[k] & PTE_V) {
                            unsigned long parent_pa = (parent_pt[k] >> 10) << 12;
                            unsigned long* child_page = (unsigned long*)allocate(PAGE_SIZE);
                            if (!child_page) { free_pgd(new_pgd); return NULL; }
                            // Copy data from parent physical page to child physical page
                            memcpy(child_page, (void*)(parent_pa + PAGE_OFFSET), PAGE_SIZE);
                            child_pt[k] = (((unsigned long)child_page - PAGE_OFFSET) >> 12 << 10) | (parent_pt[k] & 0x3FF);
                        }
                    }
                }
            }
        }
    }
    return new_pgd;
}

void free_pgd(unsigned long* pgd) {
    // Free user mappings (lower half)
    for (int i = 0; i < 256; i++) {
        if (pgd[i] & PTE_V) {
            unsigned long* pmd = (unsigned long*)((((pgd[i] >> 10) << 12)) + PAGE_OFFSET);
            for (int j = 0; j < 512; j++) {
                if (pmd[j] & PTE_V) {
                    unsigned long* pt = (unsigned long*)((((pmd[j] >> 10) << 12)) + PAGE_OFFSET);
                    for (int k = 0; k < 512; k++) {
                        if (pt[k] & PTE_V) {
                            unsigned long pa = (pt[k] >> 10) << 12;
                            free((void*)(pa + PAGE_OFFSET));
                        }
                    }
                    free(pt);
                }
            }
            free(pmd);
        }
    }
    free(pgd);
}

extern char _text_end;
extern char _rodata_end;
extern char _data_end;
extern unsigned long uart_base_addr;
extern unsigned long plic_base;
extern unsigned long G_MEMPOOL_START;
extern unsigned long G_MEMPOOL_SIZE;

void mmu_init(void) {
    kernel_pgd = (unsigned long*)allocate(PAGE_SIZE);
    memset(kernel_pgd, 0, PAGE_SIZE);

    // Identify kernel physical address
    // Since we are in virtual memory, 'la' gives virtual address.
    // We subtract PAGE_OFFSET to get the physical address.
    unsigned long kernel_pa;
    asm volatile("la %0, _start" : "=r"(kernel_pa));
    kernel_pa -= PAGE_OFFSET;

    // Map the RAM as R-W using values from the allocator (already virtualized)
    unsigned long ram_pa = G_MEMPOOL_START - PAGE_OFFSET;
    map_pages(kernel_pgd, G_MEMPOOL_START, ram_pa, G_MEMPOOL_SIZE, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D | PTE_G);

    // Map the kernel sections with proper permissions (overwrites previous R-W mapping)
    unsigned long text_size = (unsigned long)&_text_end - (unsigned long)&_start;
    map_pages(kernel_pgd, (unsigned long)&_start, kernel_pa, text_size, PTE_V | PTE_R | PTE_X | PTE_A | PTE_G);

    unsigned long rodata_size = (unsigned long)&_rodata_end - (unsigned long)&_text_end;
    map_pages(kernel_pgd, (unsigned long)&_text_end, kernel_pa + text_size, rodata_size, PTE_V | PTE_R | PTE_A | PTE_G);

    // Map MMIO regions
    if (uart_base_addr) {
        map_pages(kernel_pgd, uart_base_addr, uart_base_addr - PAGE_OFFSET, PAGE_SIZE, PROT_MMIO);
    }
    if (plic_base) {
        // PLIC is usually larger than 1 page, map 4MB for safety
        map_pages(kernel_pgd, plic_base, plic_base - PAGE_OFFSET, 0x400000, PROT_MMIO);
    }

#ifdef QEMU
    // Map fw_cfg for video.c
    map_pages(kernel_pgd, 0x10100000UL + PAGE_OFFSET, 0x10100000UL, PAGE_SIZE, PROT_MMIO);
    // Map framebuffer (RAMFB)
    map_pages(kernel_pgd, 0x87000000UL + PAGE_OFFSET, 0x87000000UL, 0x800000, PROT_MMIO); // Map 8MB for FB
#endif

    // Switch to new page table
    unsigned long satp_val = MAKE_SATP((unsigned long)kernel_pgd - PAGE_OFFSET);
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(satp_val)
        : "memory"
    );
}

