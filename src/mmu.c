#include "mmu.h"
#include "config.h"
#include "../lib/string.h"

// We need a statically allocated page for the root page table (PGD)
// to use during boot before the allocator is ready.
unsigned long boot_pgd[512] __attribute__((aligned(PAGE_SIZE)));
unsigned long boot_pmd[512] __attribute__((aligned(PAGE_SIZE)));

// External symbols from linker script
extern char _start;
extern char _end;

void setup_vm(void) {
    // Clear PGD
    memset(boot_pgd, 0, PAGE_SIZE);
    memset(boot_pmd, 0, PAGE_SIZE);

    // Identify current physical address of kernel
    // We use medany so we can get it via PC-relative
    unsigned long kernel_pa;
    asm volatile("la %0, _start" : "=r"(kernel_pa));
    
    // We'll use 2MB pages (PMD leaves) or 1GB pages (PGD leaves) for simple boot mapping.
    // However, the requirement asks for finer granularity later.
    // For now, let's do identity and higher-half mapping for the kernel and RAM.

    // Calculate PGD index for higher-half
    // 0xffffffc000000000 >> 30 & 0x1FF = 256
    unsigned long va_offset = PAGE_OFFSET;
    
    // Map first 1GB physically to higher half (VA = PA + PAGE_OFFSET)
    // If we assume RAM starts at 0x80000000 (QEMU) or 0x00000000/0x40000000 (Board)
    // We can just map multiple 1GB blocks if needed, or just enough for kernel.
    
#ifdef QEMU
    unsigned long phys_base = 0x80000000UL;
    unsigned long uart_pa = 0x10000000UL;
#else
    // Default to 0 for Orange Pi or other boards, though it might vary
    unsigned long phys_base = 0x00000000UL;
    unsigned long uart_pa = 0x10000000UL; // UART on some Orange Pis is here
#endif

    // Identity mapping (VA = PA)
    // Map the 1GB block containing the kernel
    unsigned long vpn2_ident = (kernel_pa >> PGD_SHIFT) & VPN_MASK;
    boot_pgd[vpn2_ident] = (kernel_pa >> PGD_SHIFT << 28) | PROT_KERNEL;

    // Higher-half mapping (VA = PA + PAGE_OFFSET)
    // We want VA [PAGE_OFFSET + phys_base, PAGE_OFFSET + phys_base + 1GB)
    // to map to PA [phys_base, phys_base + 1GB)
    // Index in PGD for VA = PAGE_OFFSET + phys_base
    unsigned long vpn2_kernel_high = ((va_offset + phys_base) >> PGD_SHIFT) & VPN_MASK;
    boot_pgd[vpn2_kernel_high] = (phys_base >> PGD_SHIFT << 28) | PROT_KERNEL;

    // Also map UART region for higher-half access
    unsigned long vpn2_uart_high = ((va_offset + uart_pa) >> PGD_SHIFT) & VPN_MASK;
    if (boot_pgd[vpn2_uart_high] == 0) {
        boot_pgd[vpn2_uart_high] = (uart_pa >> PGD_SHIFT << 28) | PROT_MMIO;
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

extern void* allocate(unsigned long size);

static unsigned long* pagewalk(unsigned long* pgd, unsigned long va, int alloc) {
    unsigned long vpn2 = (va >> PGD_SHIFT) & VPN_MASK;
    unsigned long vpn1 = (va >> PMD_SHIFT) & VPN_MASK;
    unsigned long vpn0 = (va >> PTE_SHIFT) & VPN_MASK;

    unsigned long* pte = &pgd[vpn2];
    if (!(*pte & PTE_V)) {
        if (!alloc) return 0;
        unsigned long* new_pg = (unsigned long*)allocate(PAGE_SIZE);
        memset(new_pg, 0, PAGE_SIZE);
        *pte = (((unsigned long)new_pg - PAGE_OFFSET) >> 12 << 10) | PTE_V;
    }

    unsigned long* pmd = (unsigned long*)(((*pte >> 10) << 12) + PAGE_OFFSET);
    pte = &pmd[vpn1];
    if (!(*pte & PTE_V)) {
        if (!alloc) return 0;
        unsigned long* new_pg = (unsigned long*)allocate(PAGE_SIZE);
        memset(new_pg, 0, PAGE_SIZE);
        *pte = (((unsigned long)new_pg - PAGE_OFFSET) >> 12 << 10) | PTE_V;
    }

    unsigned long* pt = (unsigned long*)(((*pte >> 10) << 12) + PAGE_OFFSET);
    return &pt[vpn0];
}

void map_pages(unsigned long* pgd, unsigned long va, unsigned long pa, unsigned long size, unsigned long prot) {
    for (unsigned long a = va; a < va + size; a += PAGE_SIZE, pa += PAGE_SIZE) {
        unsigned long* pte = pagewalk(pgd, a, 1);
        *pte = (pa >> 12 << 10) | prot;
    }
}

extern char _text_end;
extern char _rodata_end;
extern char _data_end;
extern unsigned long uart_base_addr;
extern unsigned long plic_base;

void mmu_init(void) {
    unsigned long* new_pgd = (unsigned long*)allocate(PAGE_SIZE);
    memset(new_pgd, 0, PAGE_SIZE);

    // Identify kernel physical address
    unsigned long kernel_pa;
    asm volatile("la %0, _start" : "=r"(kernel_pa));
    kernel_pa -= PAGE_OFFSET; // We are in virtual memory, la gives VA

    // Map 256MB of RAM as R-W (covers memory pool, initramfs)
#ifdef QEMU
    unsigned long ram_pa = 0x80000000UL;
#else
    unsigned long ram_pa = 0x00000000UL;
#endif
    map_pages(new_pgd, ram_pa + PAGE_OFFSET, ram_pa, 0x10000000UL, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D | PTE_G);

    // Map the kernel sections with proper permissions (overwrites previous R-W mapping)
    // Align sizes up to PAGE_SIZE just in case, though map_pages walks page by page.
    // However, map_pages adds PAGE_SIZE each loop. If the size is unaligned, the last page might be missed if we aren't careful, 
    // but the loop is `a < va + size`. If size is unaligned, it still maps the last page because a < va + size.
    unsigned long text_size = (unsigned long)&_text_end - (unsigned long)&_start;
    map_pages(new_pgd, (unsigned long)&_start, kernel_pa, text_size, PTE_V | PTE_R | PTE_X | PTE_A | PTE_G);

    unsigned long rodata_size = (unsigned long)&_rodata_end - (unsigned long)&_text_end;
    map_pages(new_pgd, (unsigned long)&_text_end, kernel_pa + text_size, rodata_size, PTE_V | PTE_R | PTE_A | PTE_G);

    // No need to explicitly map .data and .bss since they fall under the general RAM R-W mapping,
    // but if we want to be explicit:
    // unsigned long data_bss_size = (unsigned long)&_end - (unsigned long)&_rodata_end;
    // map_pages(new_pgd, (unsigned long)&_rodata_end, kernel_pa + text_size + rodata_size, data_bss_size, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D | PTE_G);

    // Map MMIO regions
    if (uart_base_addr) {
        map_pages(new_pgd, uart_base_addr, uart_base_addr - PAGE_OFFSET, PAGE_SIZE, PROT_MMIO);
    }
    if (plic_base) {
        // PLIC is usually larger than 1 page, map 4MB for safety
        map_pages(new_pgd, plic_base, plic_base - PAGE_OFFSET, 0x400000, PROT_MMIO);
    }

#ifdef QEMU
    // Map fw_cfg for video.c
    map_pages(new_pgd, 0x10100000UL + PAGE_OFFSET, 0x10100000UL, PAGE_SIZE, PROT_MMIO);
#endif

    // Switch to new page table
    unsigned long satp_val = MAKE_SATP((unsigned long)new_pgd - PAGE_OFFSET);
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(satp_val)
        : "memory"
    );
}
