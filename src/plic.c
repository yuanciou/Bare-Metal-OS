#include "plic.h"
#include "config.h"
#include "../lib/fdt.h"

unsigned long plic_base = 0;

#define PLIC_PRIORITY(irq)   (plic_base + (irq) * 4)

// QEMU virt & Orange Pi use context 1 for Hart 0 S-mode.
#define PLIC_S_CONTEXT 1
#define PLIC_ENABLE_WORD(ctx, n) ((volatile uint32_t *)(plic_base + 0x002000 + 0x80UL * (ctx) + 4UL * (n)))
#define PLIC_THRESHOLD(ctx)      ((volatile uint32_t *)(plic_base + 0x200000 + 0x1000UL * (ctx)))
#define PLIC_CLAIM(ctx)          ((volatile uint32_t *)(plic_base + 0x200004 + 0x1000UL * (ctx)))

/**
 * @brief Enable the given IRQ (set the corresponding bit to 1)
 */
static void plic_enable_irq(uint32_t context, uint32_t irq) {
    uint32_t word = irq / 32U;
    uint32_t bit = irq % 32U;
    *PLIC_ENABLE_WORD(context, word) |= (1U << bit);
}

void plic_init(unsigned long hartid, const void *fdt) {
    plic_base = fdt_get_plic_base(fdt);
}

/**
 * @brief Enable the interrupt for the given IRQ
 */
void plic_enable_interrupt(int irq) {
    if (plic_base == 0) return;

    // Set UART interrupt priority
    volatile uint32_t *p = (volatile uint32_t *)PLIC_PRIORITY(irq);
    *p = 1;

    // Enable UART IRQ for S-mode context
    plic_enable_irq(PLIC_S_CONTEXT, irq);

    // Set threshold to 0
    *PLIC_THRESHOLD(PLIC_S_CONTEXT) = 0;

    // Set sie.SEIE to allow supervisor external interrupts.
    unsigned long x;
    asm volatile("csrr %0, sie" : "=r"(x));
    x |= (1UL << 9); // SIE_SEIE
    asm volatile("csrw sie, %0" :: "r"(x));
}

/**
 * @brief Use PLIC_CLAIM to know the interrupt IRQ number
 */
int plic_claim() {
    if (plic_base == 0) return 0;
    return *PLIC_CLAIM(PLIC_S_CONTEXT);
}

/**
 * @brief When finish the interrupt handling, write the IRQ number back to PLIC_CLAIM
 */
void plic_complete(int irq) {
    if (plic_base == 0) return;
    *PLIC_CLAIM(PLIC_S_CONTEXT) = irq;
}