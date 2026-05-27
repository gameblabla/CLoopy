#include "core/sh7021/peripherals/sh7021_bsc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void expect16(const char *name, uint16_t got, uint16_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %04X expected %04X\n", name, got, want);
        exit(1);
    }
}

int main(void) {
    sh7021_ocpm_bsc_initialize();
    expect16("default WCR1", sh7021_ocpm_bsc_read16(0x05ffffa2u), 0xffffu);
    expect16("default WCR2", sh7021_ocpm_bsc_read16(0x05ffffa4u), 0xffffu);
    expect16("default WCR3", sh7021_ocpm_bsc_read16(0x05ffffa6u), 0xf800u);
    expect16("default RTCOR", sh7021_ocpm_bsc_read16(0x05ffffb2u), 0x00ffu);

    sh7021_ocpm_bsc_write16(0x05ffffa6u, 0x1234u);
    expect16("write WCR3 masks reserved bits", sh7021_ocpm_bsc_read16(0x05ffffa6u), 0x1000u);

    sh7021_ocpm_bsc_write16(0x05ffffa0u, 0xffffu);
    expect16("BCR masks reserved bits", sh7021_ocpm_bsc_read16(0x05ffffa0u), 0xf800u);
    sh7021_ocpm_bsc_write16(0x05ffffa2u, 0x0000u);
    expect16("WCR1 reserved bits read as one", sh7021_ocpm_bsc_read16(0x05ffffa2u), 0x00fdu);
    sh7021_ocpm_bsc_write16(0x05ffffa8u, 0xffffu);
    expect16("DCR masks reserved bits", sh7021_ocpm_bsc_read16(0x05ffffa8u), 0xff00u);
    sh7021_ocpm_bsc_write16(0x05ffffaau, 0xffffu);
    expect16("PCR masks reserved bits", sh7021_ocpm_bsc_read16(0x05ffffaau), 0xf800u);

    sh7021_ocpm_bsc_write16(0x05ffffacu, 0x0055u);
    expect16("RCR ignores missing key", sh7021_ocpm_bsc_read16(0x05ffffacu), 0x0000u);
    sh7021_ocpm_bsc_write8(0x05ffffacu, 0x5au);
    sh7021_ocpm_bsc_write8(0x05ffffadu, 0x80u);
    expect16("RCR ignores byte key writes", sh7021_ocpm_bsc_read16(0x05ffffacu), 0x0000u);
    sh7021_ocpm_bsc_write16(0x05ffffacu, 0x5affu);
    expect16("RCR key write masks reserved bits", sh7021_ocpm_bsc_read16(0x05ffffacu), 0x00f0u);

    sh7021_ocpm_bsc_write16(0x05ffffaeu, 0x0000u);
    expect16("RTCSR ignores missing key", sh7021_ocpm_bsc_read16(0x05ffffaeu), 0x0000u);
    sh7021_ocpm_bsc_write16(0x05ffffaeu, 0xa590u); /* key + CKS=/32, reserved low bits set */
    expect16("RTCSR key write masks reserved low bits", sh7021_ocpm_bsc_read16(0x05ffffaeu), 0x0090u);
    sh7021_ocpm_bsc_write16(0x05ffffaeu, 0xa500u);
    expect16("RTCSR CMF clears after read then write zero", sh7021_ocpm_bsc_read16(0x05ffffaeu), 0x0000u);

    sh7021_ocpm_bsc_write16(0x05ffffb0u, 0x6912u);
    expect16("RTCNT key write", sh7021_ocpm_bsc_read16(0x05ffffb0u), 0x0012u);
    sh7021_ocpm_bsc_write16(0x05ffffb2u, 0x9633u);
    expect16("RTCOR key write", sh7021_ocpm_bsc_read16(0x05ffffb2u), 0x0033u);
    expect16("refresh wait state helper", sh7021_bsc_refresh_wait_states(), 0x0000u);

    sh7021_ocpm_bsc_write16(0x05ffffa2u, 0xffffu);
    sh7021_ocpm_bsc_write16(0x05ffffacu, 0x5ab0u); /* RFSHE + CBR + RLW=3 states */
    expect16("refresh wait state helper active", sh7021_bsc_refresh_wait_states(), 0x0004u);
    expect16("area 2 read WAIT sample bit", sh7021_bsc_area_wait_sample_read(2), 0x0001u);
    expect16("area02 long wait", sh7021_bsc_long_wait_area02(), 0x0001u);

    puts("sh7021_bsc_test: OK");
    return 0;
}
