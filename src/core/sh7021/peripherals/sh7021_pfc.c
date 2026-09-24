#include "core/sh7021/peripherals/sh7021_pfc.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
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

static void update_irq1_pin_mode(void) {
    sh7021_ocpm_intc_set_irq1_pin_enabled((state.pacr1 & PA13_MODE_MASK) == PA13_MODE_IRQ1);
}

void sh7021_ocpm_pfc_initialize(void) {
    memset(&state, 0, sizeof(state));
    /* Existing Loopy wiring defaults used by the cartridge sound interface. */
    state.pbdr = 0x0100u;
    update_irq1_pin_mode();
}

int sh7021_ocpm_pfc_handles(uint32_t addr) {
    addr = (addr & 0x1FFu) + 0xE00u;
    return (addr >= 0xFC0u && addr < 0xFD0u) || addr == 0xFEEu;
}

uint16_t sh7021_ocpm_pfc_read16(uint32_t addr) {
    addr = (addr & 0x1FFu) + 0xE00u;
    switch (addr) {
    case 0xFC0u: return state.padr;
    case 0xFC2u: return sound_cart_portb_read(state.pbdr);
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

void sh7021_ocpm_pfc_write8(uint32_t addr, uint8_t value) {
    uint32_t base = addr & ~1u;
    uint16_t old = sh7021_ocpm_pfc_read16(base);
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
