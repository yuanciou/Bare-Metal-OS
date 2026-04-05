#ifndef ALLOCATOR_H
#define ALLOCATOR_H

void allocator_init(void);
void allocator_dump_pages(void);
void *allocate(unsigned long size);
void free(void *ptr);

#endif // ALLOCATOR_H