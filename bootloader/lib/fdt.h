#ifndef FDT_H
#define FDT_H

#include <stddef.h>

int fdt_path_offset(const void* fdt, const char* path);
const void* fdt_getprop(const void* fdt, int nodeoffset, const char* name, int* lenp);
void init_uart_from_fdt(const void *fdt);
unsigned long get_initrd_start(const void *fdt);

#endif