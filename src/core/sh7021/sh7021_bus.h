#ifndef LOOPY_SH7021_BUS_H
#define LOOPY_SH7021_BUS_H
#include <stdint.h>
uint8_t sh7021_bus_read8(uint32_t addr);
uint16_t sh7021_bus_read16(uint32_t addr);
uint16_t sh7021_bus_fetch16(uint32_t addr);
void sh7021_bus_fetch_reset(void);
uint32_t sh7021_bus_read32(uint32_t addr);
void sh7021_bus_write8(uint32_t addr, uint8_t value);
void sh7021_bus_write16(uint32_t addr, uint16_t value);
void sh7021_bus_write32(uint32_t addr, uint32_t value);
uint8_t sh7021_bus_dma_read8(uint32_t addr, int single_address_mode);
uint16_t sh7021_bus_dma_read16(uint32_t addr, int single_address_mode);
void sh7021_bus_dma_write8(uint32_t addr, uint8_t value, int single_address_mode);
void sh7021_bus_dma_write16(uint32_t addr, uint16_t value, int single_address_mode);
void sh7021_bus_prof_reset(void);
void sh7021_bus_prof_get(long long *vdp_cycles, long long *vdp_accesses, long long *vdp_bytes, long long *ram_cycles, long long *ram_accesses);
void sh7021_bus_prof_get_cart(long long *cart_cycles, long long *cart_accesses);
void sh7021_bus_prof_get_internal(long long *internal_cycles, long long *internal_accesses, long long *dram_refresh_stalls);
void sh7021_bus_prof_get_dma(long long *dma_model_cycles, long long *dma_accesses, long long *dma_single_accesses);
#endif
