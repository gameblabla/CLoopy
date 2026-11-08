#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/peripherals/sh7021_serial.h"
#include "core/timing.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int32_t cpu_cycles_left;
static uint8_t dma_source[8];
static uint8_t dma_writes[8];
static int dma_write_count;
static uint8_t tx_bytes[8];
static int tx_count;
static uint8_t dma_irq_level[4];

void sh7021_ocpm_intc_assert_irq(IRQ irq, int offs) {
    (void)offs;
    if (irq >= IRQ_DMAC0 && irq <= IRQ_DMAC3)
        dma_irq_level[irq - IRQ_DMAC0] = 1;
}
void sh7021_ocpm_intc_deassert_irq(IRQ irq) {
    if (irq >= IRQ_DMAC0 && irq <= IRQ_DMAC3)
        dma_irq_level[irq - IRQ_DMAC0] = 0;
}

uint8_t sh7021_bus_dma_read8(uint32_t addr, int single) {
    (void)single;
    assert(addr >= 0x1000u && addr < 0x1000u + sizeof(dma_source));
    return dma_source[addr - 0x1000u];
}

uint16_t sh7021_bus_dma_read16(uint32_t addr, int single) {
    uint16_t hi = sh7021_bus_dma_read8(addr, single);
    uint16_t lo = sh7021_bus_dma_read8(addr + 1u, single);
    return (uint16_t)((hi << 8) | lo);
}

void sh7021_bus_dma_write8(uint32_t addr, uint8_t value, int single) {
    (void)single;
    assert(addr == 0x2000u || addr == 0x3000u);
    assert(dma_write_count < (int)sizeof(dma_writes));
    dma_writes[dma_write_count++] = value;
    if (addr == 0x2000u)
        sh7021_ocpm_serial_write8(0x0Bu, value); /* SCI1 TDR */
}

void sh7021_bus_dma_write16(uint32_t addr, uint16_t value, int single) {
    (void)addr; (void)value; (void)single;
    assert(!"unexpected 16-bit DMA transfer");
}

static void cpu_timer(void) { cpu_cycles_left = 0; }
static void tx_cb(uint8_t value) {
    assert(tx_count < (int)sizeof(tx_bytes));
    tx_bytes[tx_count++] = value;
}

static void run_cycles(int cycles) {
    while (cycles > 0) {
        int step = cycles > 512 ? 512 : cycles;
        timing_process_slice(TIMING_CPU_TIMER, step);
        cycles -= step;
    }
}

int main(void) {
    const uint8_t expected[] = {0xC0, 0x60, 0x90};
    memcpy(dma_source, expected, sizeof(expected));

    timing_initialize();
    timing_register_timer(TIMING_CPU_TIMER, &cpu_cycles_left, cpu_timer);
    sh7021_ocpm_dmac_initialize();
    sh7021_ocpm_serial_initialize();
    sh7021_ocpm_serial_set_tx_callback(1, tx_cb);

    /* Channel 0: byte transfers, source increment, fixed destination,
       request source TXI1, non-burst. */
    sh7021_ocpm_dmac_write32(0x00, 0x00001000u);
    sh7021_ocpm_dmac_write32(0x04, 0x00002000u);
    sh7021_ocpm_dmac_write16(0x0A, (uint16_t)sizeof(expected));
    sh7021_ocpm_dmac_write16(0x0E, 0x1701u);
    sh7021_ocpm_dmac_write16(0x08, 0x0001u); /* DMA master enable */

    /* SCI1 at 31250 baud, 8N1. Enabling TE asserts TXI1/DREQ. The first
       acknowledge immediately makes TDR empty again, so a second DMA request
       can nest synchronously. It must observe the already-advanced channel. */
    sh7021_ocpm_serial_write8(0x08, 0x00);
    sh7021_ocpm_serial_write8(0x09, 15);
    sh7021_ocpm_serial_write8(0x0A, 0x20);

    assert(dma_write_count == 2);
    assert(dma_writes[0] == 0xC0);
    assert(dma_writes[1] == 0x60);

    run_cycles(5120 * 3);
    assert(dma_write_count == 3);
    assert(memcmp(dma_writes, expected, sizeof(expected)) == 0);
    assert(tx_count == 3);
    assert(memcmp(tx_bytes, expected, sizeof(expected)) == 0);

    /* External DREQ0 channel 1 in edge-request mode must perform exactly one
       byte transfer per falling edge, not a continuous transfer while low. */
    sh7021_ocpm_dmac_initialize();
    dma_source[4] = 0x42u;
    dma_source[5] = 0x69u;
    sh7021_ocpm_dmac_write32(0x10, 0x00001004u);
    sh7021_ocpm_dmac_write32(0x14, 0x00003000u);
    sh7021_ocpm_dmac_write16(0x1A, 2);
    sh7021_ocpm_dmac_write16(0x1E, 0x1021u); /* external, DREQ select, source increment */
    sh7021_ocpm_dmac_write16(0x08, 0x0001u);
    assert(dma_write_count == 3);
    sh7021_ocpm_dmac_dreq0(1, 1);
    assert(dma_write_count == 4 && dma_writes[3] == 0x42u);
    sh7021_ocpm_dmac_dreq0(1, 0);
    assert(dma_write_count == 4); /* no repeated level transfer */
    sh7021_ocpm_dmac_dreq0(0, 0);
    sh7021_ocpm_dmac_dreq0(1, 1);
    assert(dma_write_count == 5 && dma_writes[4] == 0x69u);
    assert(sh7021_ocpm_dmac_read16(0x1A) == 0u);

    /* Auto DREQ must not transfer inside the channel-enable write. It
       starts after the measured delay, with the CPU resuming between
       cycle-steal units. In-flight transfers must survive savestates. */
    sh7021_ocpm_dmac_initialize();
    dma_write_count = 0;
    dma_source[0] = 0x11u;
    dma_source[1] = 0x22u;
    sh7021_ocpm_dmac_write32(0x00, 0x00001000u);
    sh7021_ocpm_dmac_write32(0x04, 0x00003000u);
    sh7021_ocpm_dmac_write16(0x0A, 2);
    sh7021_ocpm_dmac_write16(0x0E, 0x1C01u); /* auto, byte, cycle-steal, source++ */
    sh7021_ocpm_dmac_write16(0x08, 1);
    assert(sh7021_ocpm_dmac_busy() && dma_write_count == 0);
    assert(!sh7021_ocpm_dmac_run(0, NULL));
    run_cycles(4);
    assert(!sh7021_ocpm_dmac_run(0, NULL));
    run_cycles(1);
    assert(sh7021_ocpm_dmac_run(0, NULL));
    assert(dma_write_count == 1 && dma_writes[0] == 0x11u);
    assert(!sh7021_ocpm_dmac_run(0, NULL)); /* needs CPU bus access */

    uint32_t saved_size = sh7021_ocpm_dmac_state_blob_size();
    uint8_t *saved = (uint8_t *)malloc(saved_size);
    assert(saved);
    sh7021_ocpm_dmac_get_state_blob(saved, saved_size);
    sh7021_ocpm_dmac_cpu_accessed(1);
    assert(sh7021_ocpm_dmac_run(0, NULL));
    assert(dma_write_count == 2 && dma_writes[1] == 0x22u);
    assert(!sh7021_ocpm_dmac_busy());
    sh7021_ocpm_dmac_set_state_blob(saved, saved_size);
    assert(sh7021_ocpm_dmac_busy() && sh7021_ocpm_dmac_read16(0x0A) == 1u);
    sh7021_ocpm_dmac_cpu_accessed(1);
    assert(sh7021_ocpm_dmac_run(0, NULL));
    assert(dma_write_count == 3 && dma_writes[2] == 0x22u);
    assert(!sh7021_ocpm_dmac_busy());
    free(saved);

    /* On SH7021, the DEI interrupt is the level TE && IE, not a pulse
       emitted only when the final DMA unit moves. IE may be set after TE. */
    assert(!dma_irq_level[0]);
    sh7021_ocpm_dmac_write16(0x0E, 0x1C07u); /* Enable IE after TE */
    assert(dma_irq_level[0]);
    assert((sh7021_ocpm_dmac_read16(0x0E) & 0x2u) != 0);
    sh7021_ocpm_dmac_write16(0x0E, 0x1C03u); /* Disable IE, preserve TE */
    assert(!dma_irq_level[0]);
    sh7021_ocpm_dmac_write16(0x0E, 0x1C07u);
    assert(dma_irq_level[0]);
    /* The documented TE clearing sequence is read CHCR then write TE=0. */
    (void)sh7021_ocpm_dmac_read16(0x0E);
    sh7021_ocpm_dmac_write16(0x0E, 0x1C05u);
    assert(!dma_irq_level[0]);
    assert((sh7021_ocpm_dmac_read16(0x0E) & 0x2u) == 0);

    timing_shutdown();
    puts("sh7021 serial/DMA, raster DREQ and automatic cycle-steal tests passed");
    return 0;
}
