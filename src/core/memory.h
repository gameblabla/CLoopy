#ifndef LOOPY_MEMORY_H
#define LOOPY_MEMORY_H
#include <stdint.h>
#include "core/config.h"

#define MEMORY_BIOS_START 0x00000000u
#define MEMORY_BIOS_SIZE 0x8000u
#define MEMORY_RAM_START 0x01000000u
#define MEMORY_RAM_SIZE 0x80000u
#define MEMORY_MMIO_START 0x05000000u

void memory_initialize(const ByteBuffer *bios_rom);
void memory_shutdown(void);
void memory_map_sh7021_pagetable(uint8_t *data, uint32_t start, uint32_t size);
uint8_t **memory_get_sh7021_pagetable(void);
uint32_t memory_state_blob_size(void);
void memory_get_state_blob(void *dst, uint32_t size);
void memory_set_state_blob(const void *src, uint32_t size);

#endif
