#include "align.h"

#include <stdint.h>

// ------------- ALIGN -------------------
// PAGE_SIZE = 0001 0000 0000 0000 (4096)
// PAGE_SIZE - 1UL = 0000 1111 1111 1111 (4095)
// ~(PAGE_SIZE - 1UL) = 1111 0000 0000 0000 -> the mask reset the last 12 bits -> align down
// align up -> align_down (value + PAGE_SIZE - 1UL)

unsigned long align_down_ul(unsigned long value, unsigned long alignment) {
    return value & ~(alignment - 1UL);
}

unsigned long align_up_ul(unsigned long value, unsigned long alignment) {
    return (value + alignment - 1UL) & ~(alignment - 1UL);
}

int align_up_int(int value, int alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

const void* align_up_ptr(const void* ptr, size_t alignment) {
    return (const void*)(((uintptr_t)ptr + alignment - 1) & ~(alignment - 1));
}