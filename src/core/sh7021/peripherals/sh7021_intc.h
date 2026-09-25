#ifndef LOOPY_SH7021_INTC_H
#define LOOPY_SH7021_INTC_H
#include <stdint.h>

typedef enum IRQ {
    IRQ_NMI,
    IRQ_UserBreak,
    IRQ_IRQ0,
    IRQ_IRQ1,
    IRQ_IRQ2,
    IRQ_IRQ3,
    IRQ_IRQ4,
    IRQ_IRQ5,
    IRQ_IRQ6,
    IRQ_IRQ7,
    IRQ_DMAC0,
    IRQ_DMAC1,
    IRQ_DMAC2,
    IRQ_DMAC3,
    IRQ_ITU0,
    IRQ_ITU1,
    IRQ_ITU2,
    IRQ_ITU3,
    IRQ_ITU4,
    IRQ_SCI0,
    IRQ_SCI1,
    IRQ_PRT,
    IRQ_WDT,
    IRQ_REF,
    IRQ_NumIrq
} IRQ;

void sh7021_ocpm_intc_initialize(void);
uint8_t sh7021_ocpm_intc_read8(uint32_t addr);
void sh7021_ocpm_intc_write8(uint32_t addr, uint8_t value);
uint16_t sh7021_ocpm_intc_read16(uint32_t addr);
uint32_t sh7021_ocpm_intc_read32(uint32_t addr);
void sh7021_ocpm_intc_write16(uint32_t addr, uint16_t value);
void sh7021_ocpm_intc_write32(uint32_t addr, uint32_t value);
void sh7021_ocpm_intc_assert_irq(IRQ irq, int vector_offs);
void sh7021_ocpm_intc_deassert_irq(IRQ irq);
/* Latch a one-shot interrupt request.  This is used for hardware outputs that
 * are pulses rather than persistent level sources (VDP NMI/IRQ0/IRQ2). */
void sh7021_ocpm_intc_pulse_irq(IRQ irq, int vector_offs);
/* External IRQ1 is routed through PA13.  The PFC controls whether PA13 is in
 * IRQ1 mode; INTC.ICR bit 6 chooses falling-edge versus active-low sense. */
void sh7021_ocpm_intc_set_irq1_pin_enabled(int enabled);
void sh7021_ocpm_intc_set_irq1_line(int asserted_low);
/* Called by the CPU when it accepts the request currently presented by INTC. */
void sh7021_ocpm_intc_acknowledge(void);

uint32_t sh7021_ocpm_intc_state_blob_size(void);
void sh7021_ocpm_intc_get_state_blob(void *dst, uint32_t size);
void sh7021_ocpm_intc_set_state_blob(const void *src, uint32_t size);

#endif
