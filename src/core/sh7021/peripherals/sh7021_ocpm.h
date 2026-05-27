#ifndef LOOPY_SH7021_OCPM_H
#define LOOPY_SH7021_OCPM_H
#include <stdint.h>

#define SH7021_OCPM_IO_BASE_ADDR 0x05000000u
#define SH7021_OCPM_IO_END_ADDR 0x06000000u
#define SH7021_OCPM_ORAM_BASE_ADDR 0x0F000000u
#define SH7021_OCPM_ORAM_END_ADDR 0x0F000400u

uint8_t sh7021_ocpm_io_read8(uint32_t addr);
uint16_t sh7021_ocpm_io_read16(uint32_t addr);
uint32_t sh7021_ocpm_io_read32(uint32_t addr);
void sh7021_ocpm_io_write8(uint32_t addr, uint8_t value);
void sh7021_ocpm_io_write16(uint32_t addr, uint16_t value);
void sh7021_ocpm_io_write32(uint32_t addr, uint32_t value);
uint8_t sh7021_ocpm_oram_read8(uint32_t addr);
uint16_t sh7021_ocpm_oram_read16(uint32_t addr);
uint32_t sh7021_ocpm_oram_read32(uint32_t addr);
void sh7021_ocpm_oram_write8(uint32_t addr, uint8_t value);
void sh7021_ocpm_oram_write16(uint32_t addr, uint16_t value);
void sh7021_ocpm_oram_write32(uint32_t addr, uint32_t value);
uint32_t sh7021_ocpm_oram_state_blob_size(void);
void sh7021_ocpm_oram_get_state_blob(void *dst, uint32_t size);
void sh7021_ocpm_oram_set_state_blob(const void *src, uint32_t size);

#endif
