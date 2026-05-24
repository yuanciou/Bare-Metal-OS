#ifndef MMU_H
#define MMU_H

#include "config.h"

#define PAGE_OFFSET 0xffffffc000000000UL

/* Sv39 VA bit-field shifts */
#define PGD_SHIFT 30
#define PMD_SHIFT 21
#define PTE_SHIFT 12
#define VPN_MASK  0x1FF

/* PTE descriptor bits (Sv39) */
#define PTE_V     (1UL << 0)
#define PTE_R     (1UL << 1)
#define PTE_W     (1UL << 2)
#define PTE_X     (1UL << 3)
#define PTE_U     (1UL << 4)
#define PTE_G     (1UL << 5)
#define PTE_A     (1UL << 6)
#define PTE_D     (1UL << 7)

#define PROT_KERNEL (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PROT_MMIO   (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)

#define SATP_SV39   (8UL << 60)
#define MAKE_SATP(pgd_pa) (SATP_SV39 | ((unsigned long)(pgd_pa) >> 12))

#ifndef __ASSEMBLER__

void setup_vm(void);
void mmu_init(void);
void drop_identity_map(void);
void map_pages(unsigned long* pgd, unsigned long va, unsigned long pa, unsigned long size, unsigned long prot);

#endif

#endif
