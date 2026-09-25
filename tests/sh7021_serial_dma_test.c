#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/peripherals/sh7021_serial.h"
#include "core/timing.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int32_t cpu_cycles_left;
static uint8_t dma_source[8];
static uint8_t dma_writes[8];
static int dma_write_count;
static uint8_t tx_bytes[8];
static int tx_count;

void sh7021_ocpm_intc_assert_irq(IRQ irq, int offs) { (void)irq; (void)offs; }
void sh7021_ocpm_intc_deassert_irq(IRQ irq) { (void)irq; }

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
    assert(addr == 0x2000u);
    assert(dma_write_count < (int)sizeof(dma_writes));
    dma_writes[dma_write_count++] = value;
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

    timing_shutdown();
    puts("sh7021 serial/dma reentrancy test passed");
    return 0;
}
