#ifndef LOOPY_CART_META_H
#define LOOPY_CART_META_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int loopy_cart_rom_is_wanwan(const uint8_t *rom, size_t size);

/* True for a retail Casio-published cartridge image.  Detection is by the
   copyright string Casio placed in the cartridge header area; homebrew and
   rebuilt/translated images do not carry it.  Used to keep behaviour that is
   only validated against the retail library off unknown cartridges. */
int loopy_cart_rom_is_official(const uint8_t *rom, size_t size);

#ifdef __cplusplus
}
#endif

#endif
