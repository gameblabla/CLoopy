#ifndef LOOPY_CART_H
#include <stdint.h>
#define LOOPY_CART_H
#include "core/config.h"

#define CART_SRAM_START 0x02000000u
#define CART_ROM_START 0x06000000u

void cart_initialize(const ConfigCartInfo *info);
void cart_shutdown(void);
void cart_sram_commit_check(void);

/* The live cartridge SRAM.  cart_initialize() copies the caller's buffer and
   may pad it up to a page multiple, so this is the only pointer that reflects
   what the game actually writes.  Frontends that own the battery file
   themselves (libretro) expose this rather than the config copy. */
void *cart_get_sram_data(void);
uint32_t cart_get_sram_size(void);
uint32_t cart_state_blob_size(void);
void cart_get_state_blob(void *dst, uint32_t size);
void cart_set_state_blob(const void *src, uint32_t size);

#endif
