#ifndef BUDDY_H
#define BUDDY_H

void buddy_init(void);
void *buddy_alloc_pages(unsigned int order);
void buddy_free_pages(void *ptr);
void buddy_dump_free_areas(void);

void *allocate(unsigned long size);
void free(void *ptr);

#endif // BUDDY_H