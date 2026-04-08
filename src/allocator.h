#ifndef ALLOCATOR_H
#define ALLOCATOR_H

void allocator_init(const void *fdt);
void memory_reserve(unsigned long start, unsigned long size);
void *startup_alloc(const void *fdt, unsigned long align);
void allocator_dump_pages(void);
void *allocate(unsigned long size);
void free(void *ptr);

#endif // ALLOCATOR_H