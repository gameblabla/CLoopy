#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/sh7021_local.h"
#include <assert.h>
#include <string.h>
#include <stdbool.h>

typedef struct INTCState {
    uint32_t vectors[IRQ_NumIrq];
    int prios[IRQ_NumIrq];
    int pending_irqs[IRQ_NumIrq];
    int irq_offs[IRQ_NumIrq];
} INTCState;

static INTCState state;

static void send_irq_signal(void) {
    int vector = 0;
    int highest_prio = 0;
    for (int id = 0; id < IRQ_NumIrq; id++) {
        if (state.pending_irqs[id] && state.prios[id] > highest_prio) {
            highest_prio = state.prios[id];
            vector = (int)state.vectors[id] + state.irq_offs[id];
        }
    }
    sh7021_assert_irq(vector, highest_prio);
}

void sh7021_ocpm_intc_initialize(void) {
    memset(&state, 0, sizeof(state));
    state.prios[IRQ_NMI] = 16;
    state.prios[IRQ_UserBreak] = 15;
    state.vectors[IRQ_NMI] = 11;
    state.vectors[IRQ_UserBreak] = 12;
    for (int i = 0; i < 8; i++) state.vectors[IRQ_IRQ0 + i] = 64 + i;
    for (int i = 0; i < 4; i++) state.vectors[IRQ_DMAC0 + i] = 72 + (i * 4);
    for (int i = 0; i < 5; i++) state.vectors[IRQ_ITU0 + i] = 80 + (i * 4);
    for (int i = 0; i < 2; i++) state.vectors[IRQ_SCI0 + i] = 100 + (i * 4);
    state.vectors[IRQ_PRT] = 108;
    state.vectors[IRQ_WDT] = 112;
    state.vectors[IRQ_REF] = 113;
}

uint16_t sh7021_ocpm_intc_read16(uint32_t addr) {
    addr &= 0xFu;
    switch (addr) {
    case 0x04: return (uint16_t)((state.prios[IRQ_IRQ0] << 12) | (state.prios[IRQ_IRQ1] << 8) | (state.prios[IRQ_IRQ2] << 4) | state.prios[IRQ_IRQ3]);
    case 0x06: return (uint16_t)((state.prios[IRQ_IRQ4] << 12) | (state.prios[IRQ_IRQ5] << 8) | (state.prios[IRQ_IRQ6] << 4) | state.prios[IRQ_IRQ7]);
    case 0x08: return (uint16_t)((state.prios[IRQ_DMAC0] << 12) | (state.prios[IRQ_DMAC2] << 8) | (state.prios[IRQ_ITU0] << 4) | state.prios[IRQ_ITU1]);
    case 0x0A: return (uint16_t)((state.prios[IRQ_ITU2] << 12) | (state.prios[IRQ_ITU3] << 8) | (state.prios[IRQ_ITU4] << 4) | state.prios[IRQ_SCI0]);
    case 0x0C: return (uint16_t)((state.prios[IRQ_SCI1] << 12) | (state.prios[IRQ_PRT] << 8) | (state.prios[IRQ_WDT] << 4));
    default: return 0;
    }
}

uint8_t sh7021_ocpm_intc_read8(uint32_t addr) {
    if ((addr & 1) == 0) return (uint8_t)(sh7021_ocpm_intc_read16(addr) >> 8);
    return (uint8_t)(sh7021_ocpm_intc_read16(addr - 1) & 0xFF);
}

void sh7021_ocpm_intc_write16(uint32_t addr, uint16_t value) {
    addr &= 0xFu;
    switch (addr) {
    case 0x04:
        state.prios[IRQ_IRQ0] = value >> 12;
        state.prios[IRQ_IRQ1] = (value >> 8) & 0x0F;
        state.prios[IRQ_IRQ2] = (value >> 4) & 0x0F;
        state.prios[IRQ_IRQ3] = value & 0x0F;
        break;
    case 0x06:
        state.prios[IRQ_IRQ4] = value >> 12;
        state.prios[IRQ_IRQ5] = (value >> 8) & 0x0F;
        state.prios[IRQ_IRQ6] = (value >> 4) & 0x0F;
        state.prios[IRQ_IRQ7] = value & 0x0F;
        break;
    case 0x08:
        state.prios[IRQ_DMAC0] = state.prios[IRQ_DMAC1] = value >> 12;
        state.prios[IRQ_DMAC2] = state.prios[IRQ_DMAC3] = (value >> 8) & 0x0F;
        state.prios[IRQ_ITU0] = (value >> 4) & 0x0F;
        state.prios[IRQ_ITU1] = value & 0x0F;
        break;
    case 0x0A:
        state.prios[IRQ_ITU2] = value >> 12;
        state.prios[IRQ_ITU3] = (value >> 8) & 0x0F;
        state.prios[IRQ_ITU4] = (value >> 4) & 0x0F;
        state.prios[IRQ_SCI0] = value & 0x0F;
        break;
    case 0x0C:
        state.prios[IRQ_SCI1] = value >> 12;
        state.prios[IRQ_PRT] = (value >> 8) & 0x0F;
        state.prios[IRQ_WDT] = state.prios[IRQ_REF] = (value >> 4) & 0x0F;
        break;
    default: break;
    }
}

void sh7021_ocpm_intc_write8(uint32_t addr, uint8_t value) {
    uint16_t tmp;
    if ((addr & 1) == 0) {
        tmp = (uint16_t)(sh7021_ocpm_intc_read16(addr) & 0x00FF);
        tmp |= (uint16_t)(value << 8);
        sh7021_ocpm_intc_write16(addr, tmp);
    } else {
        tmp = (uint16_t)(sh7021_ocpm_intc_read16(addr - 1) & 0xFF00);
        tmp |= value;
        sh7021_ocpm_intc_write16(addr - 1, tmp);
    }
}

void sh7021_ocpm_intc_assert_irq(IRQ irq, int vector_offs) {
    state.pending_irqs[(int)irq] = true;
    state.irq_offs[(int)irq] = vector_offs;
    send_irq_signal();
}

void sh7021_ocpm_intc_deassert_irq(IRQ irq) {
    state.pending_irqs[(int)irq] = false;
    send_irq_signal();
}

uint32_t sh7021_ocpm_intc_state_blob_size(void) { return (uint32_t)sizeof(state); }
void sh7021_ocpm_intc_get_state_blob(void *dst, uint32_t size) { if (dst && size == sizeof(state)) memcpy(dst, &state, sizeof(state)); }
void sh7021_ocpm_intc_set_state_blob(const void *src, uint32_t size) { if (src && size == sizeof(state)) memcpy(&state, src, sizeof(state)); }
