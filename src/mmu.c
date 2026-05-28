#include "mmu.h"
#include "config.h"
#include "thread.h"
#include "allocator.h"
#include "../lib/string.h"

// statically allocated page for the root page table to use during boot before the allocator is ready
// `__attribute__((aligned(PAGE_SIZE)))`: GCC compile instr -> make sure the variable addr align to PAGE_SIZE
unsigned long boot_pgd[512] __attribute__((aligned(PAGE_SIZE)));
unsigned long boot_pmd[512] __attribute__((aligned(PAGE_SIZE)));

// External symbols from linker script
extern char _start;
extern char _end;

/**
 * @brief Set up virtual memory with identity mapping and higher-half kernel mapping, then switch to the new page table.
 */
unsigned long setup_vm(void) {
    // Clear PGD
    memset(boot_pgd, 0, PAGE_SIZE);

    // Identify current physical address of kernel
    // '=r' -> r/w
    unsigned long kernel_pa;
    asm volatile("la %0, _start" : "=r"(kernel_pa));

    // Identity mapping (VA = PA)
    // Map the 1GB(1 << PGD_SHIFT) block containing the kernel
    unsigned long ram_base = kernel_pa & ~((1UL << PGD_SHIFT) - 1); // align down the kernel the 1GB
    unsigned long vpn2_ident = (ram_base >> PGD_SHIFT) & VPN_MASK;  // get the 9-bit avaliable VPN[2]
    boot_pgd[vpn2_ident] = (ram_base >> PGD_SHIFT << 28) | PROT_KERNEL; // put the PPN2 to Page Table Entry (since ther least 28 bit contain other)

    // Higher-half mapping (VA = PA + PAGE_OFFSET)
    // Map 4GB starting from physical 0 to PAGE_OFFSET
    // This covers RAM, FDT, and most SoC peripherals (UART, etc.)
    for (int i = 0; i < 4; i++) {
        unsigned long pa = (unsigned long)i << 30; // the start addr is 0GB, 1GB, 2GB, 3GB
        
        // `PAGE_OFFSET + pa` is the addr we expected
        // cal the VPN2 as PGD idx
        unsigned long vpn2 = ((PAGE_OFFSET + pa) >> PGD_SHIFT) & VPN_MASK;
        boot_pgd[vpn2] = (pa >> 30 << 28) | PROT_KERNEL; // put PPN2 to PTE
    }

    // Write satp and flush TLB
    // sfence.vma -> `fence` on VM -> clear the TLB cache
    // zero, zero -> clear all no matter what happen
    // memory -> Memory Barrier -> tell compiler don't move it for optimize 
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

/**
 * @brief Drop the identity mapping after the allocator is ready and we can use the higher-half mapping.
 *        In Sv39, PGD has 512 entries, and by sign-extension, it can seperate to 2 parts:
 *        - 0 ~ 255: low half for user space (and identity mapping)
 *        - 256 ~ 511: high half for kernel space
 *        So we can just clear the low 256 entries to drop the identity mapping.
 */
void drop_identity_map(void) {
    for (int i = 0; i < 256; i++) {
        boot_pgd[i] = 0;
    }
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

unsigned long* kernel_pgd = NULL;

extern void* allocate(unsigned long size);
extern void free(void* ptr);

/**
 * @brief Parse PGD -> PMD -> PT
 * @param pgd PGD base address
 * @param va virtual address

 * @return the PTE address for the given virtual address
 */
unsigned long* pagewalk(unsigned long* pgd, unsigned long va, int alloc) {
    // Calculate VPN[2], VPN[1], VPN[0] from the virtual address
    unsigned long vpn2 = (va >> PGD_SHIFT) & VPN_MASK;
    unsigned long vpn1 = (va >> PMD_SHIFT) & VPN_MASK;
    unsigned long vpn0 = (va >> PTE_SHIFT) & VPN_MASK;

    unsigned long* pte = &pgd[vpn2];

    // check if the PTE_V (Valid bit) of this entry is 1
    // if it is 0 -> PMD not exit
    if (!(*pte & PTE_V)) {
        if (!alloc) return 0;
        unsigned long* new_pg = (unsigned long*)allocate(PAGE_SIZE);
        if (!new_pg) return 0;
        memset(new_pg, 0, PAGE_SIZE);

    // new_pg: the VA allocated
    // - PAGE_OFFSET: map VA to PA
    // >> 12: devide 4096, remove low 12 bits -> get the PPN
    // << 10: the low 10 bits in PTW of Sv39 is flag -> PPN is put from bit 10
    // | PTE_V: set valid bit
        *pte = (((unsigned long)new_pg - PAGE_OFFSET) >> 12 << 10) | PTE_V;
    }

    // remove the flag bit (10 bits) to get the PPN, multiply 4096 to get the PA, then add PAGE_OFFSET to get the VA of the PMD
    unsigned long* pmd = (unsigned long*)(((*pte >> 10) << 12) + PAGE_OFFSET);
    // same as above, but for PMD -> PT
    pte = &pmd[vpn1];
    if (!(*pte & PTE_V)) {
        if (!alloc) return 0;
        unsigned long* new_pg = (unsigned long*)allocate(PAGE_SIZE);
        if (!new_pg) return 0;
        memset(new_pg, 0, PAGE_SIZE);
        *pte = (((unsigned long)new_pg - PAGE_OFFSET) >> 12 << 10) | PTE_V;
    }

    // find the final PT, return the PTE address for the given VA
    unsigned long* pt = (unsigned long*)(((*pte >> 10) << 12) + PAGE_OFFSET);
    return &pt[vpn0];
}

/**
 * @brief Map a range of virtual addresses to physical addresses with given permissions,
 *        starting from a specified PGD (used for both kernel and user mappings).
 */
void map_pages_at(unsigned long* pgd, unsigned long va, unsigned long pa, unsigned long size, unsigned long prot) {
    for (unsigned long a = va; a < va + size; a += PAGE_SIZE, pa += PAGE_SIZE) { // map page by page
        unsigned long* pte = pagewalk(pgd, a, 1); // find the PTE for this VA, alloc if not exist
        // >> 12 << 10: same as above, put PPN to bit 10 and set the flag bits
        if (pte) *pte = (pa >> 12 << 10) | prot;
    }
}

/**
 * @brief Map a range of VA to PA by auto finding the current thread's PGD, and set the user bit for user mappings.
 */
void map_pages(unsigned long va, unsigned long size, unsigned long pa, unsigned long prot) {
    thread* current = get_cur_thread(); // get cur thread's PGD
    if (current && current->pgd) {
        map_pages_at(current->pgd, va, pa, size, prot | PTE_U); // set user bit for user mappings
    }
}

// void map_user_pages(unsigned long va, unsigned long size, unsigned long pa, unsigned long prot) {
//     map_pages(va, size, pa, prot);
// }

/**
 * @brief Copy the user-space mappings from the parent process's PGD to a new PGD for the child process during fork with copy-on-write
 */
unsigned long* copy_pgd(unsigned long* pgd) {
    // new PGD for the child process
    unsigned long* new_pgd = (unsigned long*)allocate(PAGE_SIZE);
    if (!new_pgd) return NULL;
    memset(new_pgd, 0, PAGE_SIZE);
    
    // Copy kernel mappings (upper half)
    // Since the kernel mappings are shared, we can just copy the PGD entries without allocating new PMDs/PTs
    for (int i = 256; i < 512; i++) {
        new_pgd[i] = pgd[i];
    }

    // Copy user mappings (lower half)
    // for PGD -> PMD adnd PMD -> PT
    // 1. check valid: & PTE_V
    // 2. find parent VA: (((pgd[i] >> 10) << 12)) + PAGE_OFFSET convert PA in parent PTE to high half VA so that we can r/w
    // 3. allocate child page tables: use allocate(PAGE_SIZE) to allocate new PMD (or PT) for the child process. 
    // 4. link parent-child relationship: convert the allocated child page table's physical address (- PAGE_OFFSET) to the correct PTE format and fill it into the child process's higher-level page table (new_pgd[i] = ... | PTE_V).
    for (int i = 0; i < 256; i++) {
        if (pgd[i] & PTE_V) { // if this PGD entry is valid, we need to copy the PMD and PT
            // convert parent PMD to VA so we can read it
            unsigned long* parent_pmd = (unsigned long*)((((pgd[i] >> 10) << 12)) + PAGE_OFFSET);
            unsigned long* child_pmd = (unsigned long*)allocate(PAGE_SIZE);
            if (!child_pmd) { free_pgd(new_pgd); return NULL; }
            memset(child_pmd, 0, PAGE_SIZE);

            // convert the child PMD to VA and put it to the new PGD
            new_pgd[i] = (((unsigned long)child_pmd - PAGE_OFFSET) >> 12 << 10) | PTE_V;

            for (int j = 0; j < 512; j++) {
                if (parent_pmd[j] & PTE_V) {
                    unsigned long* parent_pt = (unsigned long*)((((parent_pmd[j] >> 10) << 12)) + PAGE_OFFSET);
                    unsigned long* child_pt = (unsigned long*)allocate(PAGE_SIZE);
                    if (!child_pt) { free_pgd(new_pgd); return NULL; }
                    memset(child_pt, 0, PAGE_SIZE);
                    child_pmd[j] = (((unsigned long)child_pt - PAGE_OFFSET) >> 12 << 10) | PTE_V;

                    for (int k = 0; k < 512; k++) {
                        // didn't memcpy
                        if (parent_pt[k] & PTE_V) {
                            unsigned long parent_pa = (parent_pt[k] >> 10) << 12;
                            // Clear W bit for both parent and child
                            parent_pt[k] &= ~PTE_W;
                            // Set child PTE same as parent but without W bit
                            child_pt[k] = parent_pt[k];
                            // Increment ref count
                            page_inc_ref((void*)(parent_pa + PAGE_OFFSET));
                        }
                    }
                }
            }
        }
    }
    asm volatile("sfence.vma");
    return new_pgd;
}

/**
 * @brief Bottom up free the page table of the PGD.
 */
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
    // Do after allocator -> directly use allocate to get PGD for kernel
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
    map_pages_at(kernel_pgd, G_MEMPOOL_START, ram_pa, G_MEMPOOL_SIZE, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D | PTE_G);

    // Map the kernel sections with proper permissions (overwrites previous R-W mapping)
    unsigned long text_size = (unsigned long)&_text_end - (unsigned long)&_start;
    map_pages_at(kernel_pgd, (unsigned long)&_start, kernel_pa, text_size, PTE_V | PTE_R | PTE_X | PTE_A | PTE_G);

    unsigned long rodata_size = (unsigned long)&_rodata_end - (unsigned long)&_text_end;
    map_pages_at(kernel_pgd, (unsigned long)&_text_end, kernel_pa + text_size, rodata_size, PTE_V | PTE_R | PTE_A | PTE_G);

    // Map MMIO regions
    if (uart_base_addr) {
        map_pages_at(kernel_pgd, uart_base_addr, uart_base_addr - PAGE_OFFSET, PAGE_SIZE, PROT_MMIO);
    }
    if (plic_base) {
        // PLIC is usually larger than 1 page, map 4MB for safety
        map_pages_at(kernel_pgd, plic_base, plic_base - PAGE_OFFSET, 0x400000, PROT_MMIO);
    }

#ifdef QEMU
    // Map fw_cfg for video.c
    map_pages_at(kernel_pgd, 0x10100000UL + PAGE_OFFSET, 0x10100000UL, PAGE_SIZE, PROT_MMIO);
    // Map framebuffer (RAMFB)
    map_pages_at(kernel_pgd, 0x87000000UL + PAGE_OFFSET, 0x87000000UL, 0x800000, PROT_MMIO); // Map 8MB for FB
#endif

#ifdef ORANGE_PI
    // Map framebuffer for Orange Pi
    map_pages_at(kernel_pgd, 0x7f700000UL + PAGE_OFFSET, 0x7f700000UL, 0x1000000, PROT_MMIO); // Map 16MB for FB
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

