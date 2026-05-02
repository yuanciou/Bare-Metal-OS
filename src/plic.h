#ifndef PLIC_H
#define PLIC_H

#include <stdint.h>

extern unsigned long plic_base;

void plic_init(unsigned long hartid, const void *fdt);
void plic_enable_interrupt(int irq);
int plic_claim(void);
void plic_complete(int irq);

#endif // PLIC_H