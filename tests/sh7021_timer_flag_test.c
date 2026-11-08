#include "core/sh7021/peripherals/sh7021_timers.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/timing.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int32_t cycles_left;
static int itu_asserted;
static void cpu_timer(void) { cycles_left = 0; }

void sh7021_ocpm_intc_assert_irq(IRQ irq, int subirq) {
    if (irq == IRQ_ITU0 && subirq == 0) itu_asserted = 1;
}
void sh7021_ocpm_intc_deassert_irq(IRQ irq) {
    if (irq == IRQ_ITU0) itu_asserted = 0;
}
void sh7021_ocpm_dmac_pulse_dreq(DREQ req) { (void)req; }

static void run_cycles(int cycles) {
    while (cycles > 0) {
        int step = cycles > 512 ? 512 : cycles;
        timing_process_slice(TIMING_CPU_TIMER, step);
        cycles -= step;
    }
}

int main(void) {
    timing_initialize();
    timing_register_timer(TIMING_CPU_TIMER, &cycles_left, cpu_timer);
    sh7021_ocpm_timer_initialize();
    /* ITU0 has GRA at offset 0x0a and TIER/TSR at 0x06/0x07. */
    sh7021_ocpm_timer_write16(0x0a, 4);
    sh7021_ocpm_timer_write8(0x06, 1); /* Enable compare-A interrupt */
    sh7021_ocpm_timer_write8(0x00, 1); /* TSTR, two-cycle start delay */
    run_cycles(7);
    assert(itu_asserted);
    /* A write-only attempt to clear TSR has no effect: the CPU must first
       have read the asserted flag.  Flag clear is level-sensitive. */
    sh7021_ocpm_timer_write8(0x07, 0x00);
    assert(itu_asserted);
    assert((sh7021_ocpm_timer_read8(0x07) & 1) == 1);
    sh7021_ocpm_timer_write8(0x07, 0x00);
    assert(!itu_asserted);
    assert((sh7021_ocpm_timer_read8(0x07) & 1) == 0);
    timing_shutdown();
    puts("SH7021 ITU read-before-clear flag test passed");
    return 0;
}
