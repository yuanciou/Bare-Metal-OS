#include "endian.h"

uint32_t bswap32(uint32_t x) {
    return ((x & 0xff000000U) >> 24) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x000000ffU) << 24);
}

uint64_t bswap64(uint64_t x) {
    return ((uint64_t)bswap32((uint32_t)(x & 0xFFFFFFFFULL)) << 32) |
           bswap32((uint32_t)(x >> 32));
}