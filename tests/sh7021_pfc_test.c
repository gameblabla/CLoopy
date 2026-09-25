#include "core/sh7021/peripherals/sh7021_pfc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nar_high;
static int irq1_pin_enabled;
static uint16_t last_portb_old, last_portb_new;
static int cart_present = 1;

int cart_is_present(void) { return cart_present; }

uint16_t sound_cart_portb_read(uint16_t current_value) {
    return nar_high ? (uint16_t)(current_value | 0x0080u)
                    : (uint16_t)(current_value & ~0x0080u);
}
void sound_cart_portb_write(uint16_t old_value, uint16_t new_value) {
    last_portb_old = old_value;
    last_portb_new = new_value;
}
void sh7021_ocpm_intc_set_irq1_pin_enabled(int enabled) { irq1_pin_enabled = enabled != 0; }

static uint16_t raw_pbdr(void) {
    uint8_t blob[64];
    uint32_t n = sh7021_ocpm_pfc_state_blob_size();
    if (n > sizeof(blob)) exit(2);
    memset(blob, 0, sizeof(blob));
    sh7021_ocpm_pfc_get_state_blob(blob, n);
    uint16_t v;
    memcpy(&v, blob + 2, sizeof(v));
    return v;
}

static void expect16(const char *name, uint16_t got, uint16_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %04X expected %04X\n", name, got, want);
        exit(1);
    }
}

int main(void) {
    sh7021_ocpm_pfc_initialize();
    expect16("PBDR reset latch", raw_pbdr(), 0x0000u);
    expect16("PACR1 power-on reset", sh7021_ocpm_pfc_read16(0x05ffffc8u), 0x3302u);
    expect16("PACR2 power-on reset", sh7021_ocpm_pfc_read16(0x05ffffcau), 0xff95u);
    expect16("CASCR power-on reset", sh7021_ocpm_pfc_read16(0x05ffffeeu), 0x5fffu);

    /* A high-byte write must preserve raw PBDR bit 7 even when the external
       NAR input makes a read report the opposite value. */
    sh7021_ocpm_pfc_write16(0x05ffffc2u, 0x0080u);
    nar_high = 0;
    expect16("read synthesizes NAR low", sh7021_ocpm_pfc_read16(0x05ffffc2u), 0x0000u);
    sh7021_ocpm_pfc_write8(0x05ffffc2u, 0x12u);
    expect16("high-byte write preserves raw bit7=1", raw_pbdr(), 0x1280u);

    sh7021_ocpm_pfc_write16(0x05ffffc2u, 0x1200u);
    nar_high = 1;
    expect16("read synthesizes NAR high", sh7021_ocpm_pfc_read16(0x05ffffc2u), 0x1280u);
    sh7021_ocpm_pfc_write8(0x05ffffc2u, 0x34u);
    expect16("high-byte write preserves raw bit7=0", raw_pbdr(), 0x3400u);

    /* Low-byte writes still update exactly the requested low byte. */
    nar_high = 1;
    sh7021_ocpm_pfc_write8(0x05ffffc3u, 0x55u);
    expect16("low-byte write", raw_pbdr(), 0x3455u);
    expect16("port write sees raw old value", last_portb_old, 0x3400u);
    expect16("port write sees raw new value", last_portb_new, 0x3455u);

    /* Longword PFC accesses are architecturally supported and are how the
       Loopy BIOS programs paired PFC registers. */
    sh7021_ocpm_pfc_write32(0x05ffffc8u, 0x0c02bf99u);
    if (sh7021_ocpm_pfc_read32(0x05ffffc8u) != 0x0c02bf99u) {
        fprintf(stderr, "PFC longword roundtrip failed\n");
        return 1;
    }

    /* PADR returns the physical PA8 cartridge-detect pin while PA8 is GPIO
       input; the latch remains independently writable. */
    sh7021_ocpm_pfc_write16(0x05ffffc8u, 0x0c02u);
    sh7021_ocpm_pfc_write16(0x05ffffc4u, 0x0400u);
    sh7021_ocpm_pfc_write16(0x05ffffc0u, 0x0014u);
    cart_present = 1;
    expect16("PA8 cartridge detect", sh7021_ocpm_pfc_read16(0x05ffffc0u), 0x0114u);
    cart_present = 0;
    expect16("PA8 no cartridge", sh7021_ocpm_pfc_read16(0x05ffffc0u), 0x0014u);

    /* Keep the PA13/IRQ1 mux side effect covered too. */
    sh7021_ocpm_pfc_write16(0x05ffffc8u, 0x0400u);
    if (!irq1_pin_enabled) {
        fprintf(stderr, "PACR1 IRQ1 pin mode was not enabled\n");
        return 1;
    }

    puts("sh7021_pfc_test: OK");
    return 0;
}
