#ifndef LOOPY_CART_META_H
#define LOOPY_CART_META_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int loopy_cart_rom_is_wanwan(const uint8_t *rom, size_t size);

#ifdef __cplusplus
}
#endif

#endif
