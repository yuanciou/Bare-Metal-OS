#ifndef FDT_H
#define FDT_H

#include <stddef.h>
#include <stdint.h>

int fdt_path_offset(const void* fdt, const char* path);
const void* fdt_getprop(const void* fdt, int nodeoffset, const char* name, int* lenp);
void init_uart_from_fdt(const void *fdt);
int fdt_get_node_by_phandle(const void *fdt, uint32_t phandle);
unsigned long fdt_totalsize(const void *fdt);
unsigned long get_initrd_start(const void *fdt);
unsigned long get_initrd_end(const void *fdt);
unsigned long fdt_get_plic_base(const void* fdt);
int uart_get_irq(const void* fdt);
int fdt_get_reserved_memory_region(const void *fdt,
								   int index,
								   unsigned long *start,
								   unsigned long *size);

#endif
