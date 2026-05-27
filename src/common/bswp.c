#include "common/bswp.h"
uint16_t common_bswp16(uint16_t value) { return (uint16_t)((value >> 8) | (value << 8)); }
uint32_t common_bswp32(uint32_t value) {
    return (value >> 24) |
        (((value >> 16) & 0xFFu) << 8) |
        (((value >> 8) & 0xFFu) << 16) |
        (value << 24);
}
