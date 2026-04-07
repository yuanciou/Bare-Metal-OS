#ifndef ALIGN_H
#define ALIGN_H

#include <stddef.h>

unsigned long align_down_ul(unsigned long value, unsigned long alignment);
unsigned long align_up_ul(unsigned long value, unsigned long alignment);
int align_up_int(int value, int alignment);
const void* align_up_ptr(const void* ptr, size_t alignment);

#endif // ALIGN_H