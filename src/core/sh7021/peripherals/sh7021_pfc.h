#ifndef LOOPY_SH7021_PFC_H
#define LOOPY_SH7021_PFC_H
#include <stdint.h>

void sh7021_ocpm_pfc_initialize(void);
uint8_t sh7021_ocpm_pfc_read8(uint32_t addr);
uint16_t sh7021_ocpm_pfc_read16(uint32_t addr);
uint32_t sh7021_ocpm_pfc_read32(uint32_t addr);
void sh7021_ocpm_pfc_write8(uint32_t addr, uint8_t value);
void sh7021_ocpm_pfc_write16(uint32_t addr, uint16_t value);
void sh7021_ocpm_pfc_write32(uint32_t addr, uint32_t value);
int sh7021_ocpm_pfc_handles(uint32_t addr);

uint32_t sh7021_ocpm_pfc_state_blob_size(void);
void sh7021_ocpm_pfc_get_state_blob(void *dst, uint32_t size);
void sh7021_ocpm_pfc_set_state_blob(const void *src, uint32_t size);

#endif
