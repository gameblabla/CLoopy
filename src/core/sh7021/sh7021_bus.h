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
/* Side-effect-free read for debuggers and inspection tooling.  Returns nonzero
   and stores a big-endian value when the address is backed by real memory
   (BIOS ROM, work RAM, cart ROM/SRAM, OCRAM, VDP memory).  MMIO registers
   report failure by design: reading one for real can clear a status flag or
   move the VDP bus latch, so there is no way to sample them without changing
   what the machine does next. */
int sh7021_bus_peek(uint32_t addr, int bytes, uint32_t *out_value);

/* Bus areas, as divided by the top byte of the CPU address.  Mirrors the
   hardware notes' "Memory Areas" table, so each entry is a physically distinct
   target with its own bus width and access time. */
typedef enum SH7021BusRegion {
    SH7021_BUS_REGION_BIOS_ROM = 0,   /* 0x00: internal 32KB mask ROM, 32-bit. */
    SH7021_BUS_REGION_CART_SRAM,      /* 0x02: cart SRAM, 8-bit. */
    SH7021_BUS_REGION_VDP_LOW_MIRROR, /* 0x04: 8-bit VDP mirror; avoid. */
    SH7021_BUS_REGION_PERIPHERALS,    /* 0x05: on-chip peripheral registers. */
    SH7021_BUS_REGION_WORK_RAM,       /* 0x09: 512KB DRAM, 16-bit. */
    SH7021_BUS_REGION_VDP,            /* 0x0C: VDP and MMIO, 16-bit. */
    SH7021_BUS_REGION_CART_ROM,       /* 0x0E: cart ROM, 16-bit. */
    SH7021_BUS_REGION_OCRAM,          /* 0x0F: internal 1KB RAM, 32-bit. */
    SH7021_BUS_REGION_OPEN_BUS,       /* Anything else: invalid/open bus. */
    SH7021_BUS_REGION_COUNT
} SH7021BusRegion;

typedef struct SH7021BusRegionCounters {
    long long reads;
    long long writes;
    long long fetches;     /* Instruction fetches, counted apart from data. */
    long long bytes;       /* Total traffic, all three access kinds. */
    long long wait_cycles; /* Cycles stalled beyond a baseline 1-cycle access. */
} SH7021BusRegionCounters;

typedef struct SH7021BusProf {
    SH7021BusRegionCounters region[SH7021_BUS_REGION_COUNT];
    long long dram_refresh_stalls; /* Diagnostic only; not charged as cycles. */
    long long dma_model_cycles;
    long long dma_accesses;
    long long dma_single_accesses;
} SH7021BusProf;

/* Which bus area a CPU address targets. */
SH7021BusRegion sh7021_bus_region_of(uint32_t addr);

void sh7021_bus_prof_reset(void);
void sh7021_bus_prof_snapshot(SH7021BusProf *out);
const char *sh7021_bus_region_name(SH7021BusRegion region);
#endif
