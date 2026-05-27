#ifndef LOOPY_SH7021_BSC_H
#define LOOPY_SH7021_BSC_H
#include <stdint.h>

void sh7021_ocpm_bsc_initialize(void);
uint8_t sh7021_ocpm_bsc_read8(uint32_t addr);
uint16_t sh7021_ocpm_bsc_read16(uint32_t addr);
uint32_t sh7021_ocpm_bsc_read32(uint32_t addr);
void sh7021_ocpm_bsc_write8(uint32_t addr, uint8_t value);
void sh7021_ocpm_bsc_write16(uint32_t addr, uint16_t value);
void sh7021_ocpm_bsc_write32(uint32_t addr, uint32_t value);

uint32_t sh7021_ocpm_bsc_state_blob_size(void);
void sh7021_ocpm_bsc_get_state_blob(void *dst, uint32_t size);
void sh7021_ocpm_bsc_set_state_blob(const void *src, uint32_t size);

uint16_t sh7021_bsc_bcr(void);
uint16_t sh7021_bsc_wcr1(void);
uint16_t sh7021_bsc_wcr2(void);
uint16_t sh7021_bsc_wcr3(void);
uint16_t sh7021_bsc_dcr(void);
uint8_t sh7021_bsc_refresh_enabled(void);
uint8_t sh7021_bsc_refresh_constant(void);
uint8_t sh7021_bsc_refresh_wait_states(void);
uint16_t sh7021_bsc_refresh_period_cycles(void);
int sh7021_bsc_area_wait_sample_read(int area);
int sh7021_bsc_area_wait_sample_dma_read(int area);
int sh7021_bsc_area_wait_sample_dma_write(int area);
int sh7021_bsc_area_from_addr(uint32_t addr);
uint16_t sh7021_bsc_dma_single_cycle_states(uint32_t addr, int bytes, int write);
uint8_t sh7021_bsc_long_wait_area02(void);
uint8_t sh7021_bsc_long_wait_area6(void);

#endif
