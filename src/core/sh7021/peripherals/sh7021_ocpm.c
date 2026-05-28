#include "core/sh7021/peripherals/sh7021_ocpm.h"
#include "common/bswp.h"
#include "core/sh7021/peripherals/sh7021_bsc.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/peripherals/sh7021_serial.h"
#include "core/sh7021/peripherals/sh7021_timers.h"
#include "sound/sound.h"
#include <stdio.h>
#include <string.h>

#define SERIAL_START 0xEC0u
#define SERIAL_END 0xED0u
#define TIMER_START 0xF00u
#define TIMER_END 0xF40u
#define DMAC_START 0xF40u
#define DMAC_END 0xF80u
#define INTC_START 0xF84u
#define INTC_END 0xF90u
#define BSC_START 0xFA0u
#define BSC_END 0xFB4u

static uint8_t oram[0x400];
static uint16_t portb_data = 0x0100u;

uint8_t sh7021_ocpm_io_read8(uint32_t addr) {
    addr = (addr & 0x1FFu) + 0xE00u;
    if (addr >= SERIAL_START && addr < SERIAL_END) return sh7021_ocpm_serial_read8(addr);
    if (addr >= TIMER_START && addr < TIMER_END) return sh7021_ocpm_timer_read8(addr);
    if (addr >= DMAC_START && addr < DMAC_END) return sh7021_ocpm_dmac_read8(addr);
    if (addr >= INTC_START && addr < INTC_END) return sh7021_ocpm_intc_read8(addr);
    if (addr >= BSC_START && addr < BSC_END) return sh7021_ocpm_bsc_read8(addr);
    if (addr == 0xFC2u || addr == 0xFC3u) {
        uint16_t v = sound_cart_portb_read(portb_data);
        return (addr & 1u) ? (uint8_t)v : (uint8_t)(v >> 8);
    }
    LOOPY_DEBUG_PRINTF("[OCPM] read8 %08X\n", addr);
    return 0;
}

uint16_t sh7021_ocpm_io_read16(uint32_t addr) {
    addr = (addr & 0x1FFu) + 0xE00u;
    if (addr >= TIMER_START && addr < TIMER_END) return sh7021_ocpm_timer_read16(addr);
    if (addr >= DMAC_START && addr < DMAC_END) return sh7021_ocpm_dmac_read16(addr);
    if (addr >= INTC_START && addr < INTC_END) return sh7021_ocpm_intc_read16(addr);
    if (addr >= BSC_START && addr < BSC_END) return sh7021_ocpm_bsc_read16(addr);
    if (addr == 0xFC2u) return sound_cart_portb_read(portb_data);
    LOOPY_DEBUG_PRINTF("[OCPM] read16 %08X\n", addr);
    return 0;
}

uint32_t sh7021_ocpm_io_read32(uint32_t addr) {
    addr = (addr & 0x1FFu) + 0xE00u;
    if (addr >= DMAC_START && addr < DMAC_END) return sh7021_ocpm_dmac_read32(addr);
    if (addr >= BSC_START && addr < BSC_END) return sh7021_ocpm_bsc_read32(addr);
    LOOPY_DEBUG_PRINTF("[OCPM] read32 %08X\n", addr);
    return 0;
}

void sh7021_ocpm_io_write8(uint32_t addr, uint8_t value) {
    addr = (addr & 0x1FFu) + 0xE00u;
    if (addr >= SERIAL_START && addr < SERIAL_END) { sh7021_ocpm_serial_write8(addr, value); return; }
    if (addr >= TIMER_START && addr < TIMER_END) { sh7021_ocpm_timer_write8(addr, value); return; }
    if (addr >= DMAC_START && addr < DMAC_END) { sh7021_ocpm_dmac_write8(addr, value); return; }
    if (addr >= INTC_START && addr < INTC_END) { sh7021_ocpm_intc_write8(addr, value); return; }
    if (addr >= BSC_START && addr < BSC_END) { sh7021_ocpm_bsc_write8(addr, value); return; }
    if (addr == 0xFC2u || addr == 0xFC3u) {
        uint16_t old = portb_data;
        if (addr & 1u) portb_data = (uint16_t)((portb_data & 0xFF00u) | value);
        else portb_data = (uint16_t)(((uint16_t)value << 8) | (portb_data & 0x00FFu));
        sound_cart_portb_write(old, portb_data);
        return;
    }
    LOOPY_DEBUG_PRINTF("[OCPM] write8 %08X: %02X\n", addr, value);
}

void sh7021_ocpm_io_write16(uint32_t addr, uint16_t value) {
    addr = (addr & 0x1FFu) + 0xE00u;
    if (addr >= TIMER_START && addr < TIMER_END) { sh7021_ocpm_timer_write16(addr, value); return; }
    if (addr >= DMAC_START && addr < DMAC_END) { sh7021_ocpm_dmac_write16(addr, value); return; }
    if (addr >= INTC_START && addr < INTC_END) { sh7021_ocpm_intc_write16(addr, value); return; }
    if (addr >= BSC_START && addr < BSC_END) { sh7021_ocpm_bsc_write16(addr, value); return; }
    if (addr == 0xFC2u) {
        uint16_t old = portb_data;
        portb_data = value;
        sound_cart_portb_write(old, portb_data);
        return;
    }
    LOOPY_DEBUG_PRINTF("[OCPM] write16 %08X: %04X\n", addr, value);
}

void sh7021_ocpm_io_write32(uint32_t addr, uint32_t value) {
    addr = (addr & 0x1FFu) + 0xE00u;
    if (addr >= DMAC_START && addr < DMAC_END) { sh7021_ocpm_dmac_write32(addr, value); return; }
    if (addr >= BSC_START && addr < BSC_END) { sh7021_ocpm_bsc_write32(addr, value); return; }
    LOOPY_DEBUG_PRINTF("[OCPM] write32 %08X: %08X\n", addr, value);
}

uint8_t sh7021_ocpm_oram_read8(uint32_t addr) { return oram[addr & 0x3FFu]; }
uint16_t sh7021_ocpm_oram_read16(uint32_t addr) { uint16_t v; memcpy(&v, &oram[addr & 0x3FFu], 2); return common_bswp16(v); }
uint32_t sh7021_ocpm_oram_read32(uint32_t addr) { uint32_t v; memcpy(&v, &oram[addr & 0x3FFu], 4); return common_bswp32(v); }
void sh7021_ocpm_oram_write8(uint32_t addr, uint8_t value) { oram[addr & 0x3FFu] = value; }
void sh7021_ocpm_oram_write16(uint32_t addr, uint16_t value) { value = common_bswp16(value); memcpy(&oram[addr & 0x3FFu], &value, 2); }
void sh7021_ocpm_oram_write32(uint32_t addr, uint32_t value) { value = common_bswp32(value); memcpy(&oram[addr & 0x3FFu], &value, 4); }

uint32_t sh7021_ocpm_oram_state_blob_size(void) { return (uint32_t)sizeof(oram); }
void sh7021_ocpm_oram_get_state_blob(void *dst, uint32_t size) { if (dst && size == sizeof(oram)) memcpy(dst, oram, sizeof(oram)); }
void sh7021_ocpm_oram_set_state_blob(const void *src, uint32_t size) { if (src && size == sizeof(oram)) memcpy(oram, src, sizeof(oram)); }
