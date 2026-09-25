#include "core/sh7021/peripherals/sh7021_pfc.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/cart.h"
#include "sound/sound.h"
#include <string.h>

typedef struct PFCState {
    uint16_t padr;
    uint16_t pbdr;
    uint16_t paior;
    uint16_t pbior;
    uint16_t pacr1;
    uint16_t pacr2;
    uint16_t pbcr1;
    uint16_t pbcr2;
    uint16_t cascr;
} PFCState;

static PFCState state;

#define PA13_MODE_MASK 0x0C00u
#define PA13_MODE_IRQ1 0x0400u
#define PA8_MASK 0x0100u
#define PA8_MODE_BREQ 0x0001u

static void update_irq1_pin_mode(void) {
    sh7021_ocpm_intc_set_irq1_pin_enabled((state.pacr1 & PA13_MODE_MASK) == PA13_MODE_IRQ1);
}

void sh7021_ocpm_pfc_initialize(void) {
    memset(&state, 0, sizeof(state));
    /* SH7021 power-on reset values (hardware manual, PFC chapter).  Do not
       seed post-BIOS values here: the real Loopy BIOS runs from the reset
       vector and programs the board-specific pin mux itself. */
    state.pacr1 = 0x3302u;
    state.pacr2 = 0xff95u;
    state.cascr = 0x5fffu;
    update_irq1_pin_mode();
}

int sh7021_ocpm_pfc_handles(uint32_t addr) {
    addr = (addr & 0x1FFu) + 0xE00u;
    return (addr >= 0xFC0u && addr < 0xFD0u) || addr == 0xFEEu;
}

static uint16_t pfc_read16_raw(uint32_t addr) {
    addr = (addr & 0x1FFu) + 0xE00u;
    switch (addr) {
    case 0xFC0u: return state.padr;
    case 0xFC2u: return state.pbdr;
    case 0xFC4u: return state.paior;
    case 0xFC6u: return state.pbior;
    case 0xFC8u: return state.pacr1;
    case 0xFCAu: return state.pacr2;
    case 0xFCCu: return state.pbcr1;
    case 0xFCEu: return state.pbcr2;
    case 0xFEEu: return state.cascr;
    default: return 0;
    }
}

uint16_t sh7021_ocpm_pfc_read16(uint32_t addr) {
    uint32_t reg = (addr & 0x1FFu) + 0xE00u;
    uint16_t raw = pfc_read16_raw(addr);

    if (reg == 0xFC0u) {
        /* The SH7021 manual specifies that input pins read the physical pin,
           not the PADR latch.  On the Loopy cartridge connector PA8 is DET;
           retail cartridges (including Little Romance's Z544 board) strap
           DET high.  Model that board input when PA8 is ordinary GPIO input. */
        if (!(state.paior & PA8_MASK) && !(state.pacr1 & PA8_MODE_BREQ)) {
            if (cart_is_present()) raw |= PA8_MASK;
            else raw &= (uint16_t)~PA8_MASK;
        }
        return raw;
    }

    /* Port B bit 7 is an input reflection of the ADPCM NAR line on reads.
       Keep that synthesized pin state out of the stored PBDR latch. */
    if (reg == 0xFC2u) return sound_cart_portb_read(raw);
    return raw;
}

uint32_t sh7021_ocpm_pfc_read32(uint32_t addr) {
    /* PFC registers are physically 16-bit but explicitly allow longword
       accesses.  A longword is two consecutive big-endian register accesses. */
    uint32_t hi = sh7021_ocpm_pfc_read16(addr);
    uint32_t lo = sh7021_ocpm_pfc_read16(addr + 2u);
    return (hi << 16) | lo;
}

uint8_t sh7021_ocpm_pfc_read8(uint32_t addr) {
    uint16_t value = sh7021_ocpm_pfc_read16(addr & ~1u);
    return (addr & 1u) ? (uint8_t)value : (uint8_t)(value >> 8);
}

void sh7021_ocpm_pfc_write16(uint32_t addr, uint16_t value) {
    addr = (addr & 0x1FFu) + 0xE00u;
    switch (addr) {
    case 0xFC0u: state.padr = value; break;
    case 0xFC2u: {
        uint16_t old = state.pbdr;
        state.pbdr = value;
        sound_cart_portb_write(old, state.pbdr);
        break;
    }
    case 0xFC4u: state.paior = value; break;
    case 0xFC6u: state.pbior = value; break;
    case 0xFC8u:
        state.pacr1 = value;
        update_irq1_pin_mode();
        break;
    case 0xFCAu: state.pacr2 = value; break;
    case 0xFCCu: state.pbcr1 = value; break;
    case 0xFCEu: state.pbcr2 = value; break;
    case 0xFEEu: state.cascr = value; break;
    default: break;
    }
}


void sh7021_ocpm_pfc_write32(uint32_t addr, uint32_t value) {
    /* Preserve bus/register ordering: the upper halfword is at the lower
       address on the SH big-endian bus. */
    sh7021_ocpm_pfc_write16(addr, (uint16_t)(value >> 16));
    sh7021_ocpm_pfc_write16(addr + 2u, (uint16_t)value);
}

void sh7021_ocpm_pfc_write8(uint32_t addr, uint8_t value) {
    uint32_t base = addr & ~1u;
    /* Merge byte writes against the register latch, not the externally visible
       read value.  PBDR reads synthesize bit 7 from NAR, and using that value
       here would let a high-byte write accidentally persist an unrelated input
       pin into the low byte. */
    uint16_t old = pfc_read16_raw(base);
    uint16_t merged = (addr & 1u) ? (uint16_t)((old & 0xFF00u) | value)
                                  : (uint16_t)(((uint16_t)value << 8) | (old & 0x00FFu));
    sh7021_ocpm_pfc_write16(base, merged);
}

uint32_t sh7021_ocpm_pfc_state_blob_size(void) { return (uint32_t)sizeof(state); }
void sh7021_ocpm_pfc_get_state_blob(void *dst, uint32_t size) { if (dst && size == sizeof(state)) memcpy(dst, &state, sizeof(state)); }
void sh7021_ocpm_pfc_set_state_blob(const void *src, uint32_t size) {
    if (src && size == sizeof(state)) {
        memcpy(&state, src, sizeof(state));
        update_irq1_pin_mode();
    }
}
