#include "core/sh7021/peripherals/sh7021_serial.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/timing.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int32_t cpu_cycles_left;
static int dreq_level[DREQ_NumDreq];
static int irq_asserted[IRQ_NumIrq];
static int irq_vector_offs[IRQ_NumIrq];
static int tx_count;
static uint8_t tx_bytes[8];

void sh7021_ocpm_dmac_send_dreq(DREQ dreq) { if ((int)dreq >= 0 && dreq < DREQ_NumDreq) dreq_level[dreq] = 1; }
void sh7021_ocpm_dmac_clear_dreq(DREQ dreq) { if ((int)dreq >= 0 && dreq < DREQ_NumDreq) dreq_level[dreq] = 0; }
void sh7021_ocpm_intc_assert_irq(IRQ irq, int offs) { irq_asserted[irq] = 1; irq_vector_offs[irq] = offs; }
void sh7021_ocpm_intc_deassert_irq(IRQ irq) { irq_asserted[irq] = 0; }

static void cpu_timer(void) { cpu_cycles_left = 0; }
static void tx_cb(uint8_t value) { if (tx_count < (int)sizeof(tx_bytes)) tx_bytes[tx_count] = value; tx_count++; }

static void run_cycles(int cycles) {
    while (cycles > 0) {
        int step = cycles > 512 ? 512 : cycles;
        timing_process_slice(TIMING_CPU_TIMER, step);
        cycles -= step;
    }
}

int main(void) {
    timing_initialize();
    timing_register_timer(TIMING_CPU_TIMER, &cpu_cycles_left, cpu_timer);
    sh7021_ocpm_serial_initialize();
    sh7021_ocpm_serial_set_tx_callback(1, tx_cb);
    assert(sh7021_ocpm_serial_state_blob_size() == 208);

    /* Hardware reset values, including the flags Doom polls. */
    assert(sh7021_ocpm_serial_read8(0x00) == 0x00);
    assert(sh7021_ocpm_serial_read8(0x01) == 0xFF);
    assert(sh7021_ocpm_serial_read8(0x02) == 0x00);
    assert(sh7021_ocpm_serial_read8(0x03) == 0xFF);
    assert(sh7021_ocpm_serial_read8(0x04) == 0x84); /* TDRE | TEND */
    assert(sh7021_ocpm_serial_read8(0x05) == 0x00);
    assert(sh7021_ocpm_serial_read8(0x0C) == 0x84);

    /* SCI1 31250 baud, 8N1. Enabling TE exposes TDRE as a DMA request. */
    sh7021_ocpm_serial_write8(0x08, 0x00);
    sh7021_ocpm_serial_write8(0x09, 15);
    sh7021_ocpm_serial_write8(0x0A, 0x20);
    assert(dreq_level[DREQ_TXI1]);

    /* Writing TDR does not clear TDRE.  Software clears SSR.TDRE after the
       write; that transfers TDR into the shift register and reasserts TDRE. */
    sh7021_ocpm_serial_write8(0x0B, 0x90);
    assert(sh7021_ocpm_serial_read8(0x0C) == 0x84);
    sh7021_ocpm_serial_write8(0x0C, 0x04);
    assert(sh7021_ocpm_serial_read8(0x0C) == 0x80);
    assert(dreq_level[DREQ_TXI1]);

    /* 8N1 is ten bit times: 10 * (32 * (15+1)) = 5120 CPU cycles. */
    run_cycles(5119);
    assert(tx_count == 0);
    run_cycles(1);
    assert(tx_count == 1 && tx_bytes[0] == 0x90);
    assert(sh7021_ocpm_serial_read8(0x0C) == 0x84);

    /* Double buffering: the second TDR waits while the first frame is active,
       then follows without losing data. */
    sh7021_ocpm_serial_write8(0x0B, 0x11);
    sh7021_ocpm_serial_write8(0x0C, 0x04);
    sh7021_ocpm_serial_write8(0x0B, 0x22);
    sh7021_ocpm_serial_write8(0x0C, 0x00);
    assert((sh7021_ocpm_serial_read8(0x0C) & 0x84) == 0x00);
    run_cycles(5120);
    assert(tx_count == 2 && tx_bytes[1] == 0x11);
    assert((sh7021_ocpm_serial_read8(0x0C) & 0x80) != 0);
    run_cycles(5120);
    assert(tx_count == 3 && tx_bytes[2] == 0x22);
    assert((sh7021_ocpm_serial_read8(0x0C) & 0x84) == 0x84);

    /* TXI is SCI vector +2 when TIE and TDRE are both set. */
    sh7021_ocpm_serial_write8(0x0A, 0xA0);
    assert(irq_asserted[IRQ_SCI1] && irq_vector_offs[IRQ_SCI1] == 2);

    /* TE=0 parks/aborts the transmitter and restores idle status. */
    sh7021_ocpm_serial_write8(0x0B, 0x33);
    sh7021_ocpm_serial_write8(0x0C, 0x04);
    sh7021_ocpm_serial_write8(0x0A, 0x00);
    assert((sh7021_ocpm_serial_read8(0x0C) & 0x84) == 0x84);
    run_cycles(6000);
    assert(tx_count == 3);

    /* A TX-DMA acknowledge performs the same TDRE handoff without requiring
       a CPU SSR write. */
    sh7021_ocpm_serial_write8(0x0A, 0x20);
    sh7021_ocpm_serial_write8(0x0B, 0x44);
    sh7021_ocpm_serial_dma_tx_ack(1);
    assert((sh7021_ocpm_serial_read8(0x0C) & 0x84) == 0x80);
    run_cycles(5120);
    assert(tx_count == 4 && tx_bytes[3] == 0x44);

    /* Valid synchronous-mode programming must not assert/crash. */
    sh7021_ocpm_serial_write8(0x08, 0x80);
    sh7021_ocpm_serial_write8(0x09, 3);
    assert(sh7021_ocpm_serial_read8(0x08) == 0x80);

    timing_shutdown();
    puts("sh7021 serial tests passed");
    return 0;
}
