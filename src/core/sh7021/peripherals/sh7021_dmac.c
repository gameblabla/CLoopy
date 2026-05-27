#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/sh7021_bus.h"
#include <string.h>

typedef struct ChannelCtrl {
    int enable;
    int finished;
    int irq_enable;
    int transfer_16bit;
    int is_burst;
    int dreq_select;
    int ack_level;
    int ack_mode;
    int mode;
    int src_step;
    int dst_step;
    int te_read_seen;
} ChannelCtrl;

typedef struct Channel {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t transfer_size;
    ChannelCtrl ctrl;
} Channel;

typedef struct DMACState {
    Channel chan[4];
    int dreqs[DREQ_NumDreq];
    uint16_t ctrl;
    uint8_t nmif_read_seen;
    uint8_t ae_read_seen;
    uint8_t priority_cursor;
} DMACState;

static DMACState state;

static uint16_t dmaor_read_value(void) {
    return (uint16_t)(state.ctrl & 0x0307u);
}

static int channel_index(Channel *chan) {
    return (int)(chan - state.chan);
}

static int channel_is_single_address(const Channel *chan) {
    int ch = (int)(chan - state.chan);
    return ch < 2 && (chan->ctrl.mode == 2 || chan->ctrl.mode == 3);
}

static uint16_t channel_peek_ctrl(const Channel *chan) {
    uint16_t result = (uint16_t)chan->ctrl.enable;
    result |= (uint16_t)(chan->ctrl.finished << 1);
    result |= (uint16_t)(chan->ctrl.irq_enable << 2);
    result |= (uint16_t)(chan->ctrl.transfer_16bit << 3);
    result |= (uint16_t)(chan->ctrl.is_burst << 4);
    result |= (uint16_t)(chan->ctrl.dreq_select << 5);
    result |= (uint16_t)(chan->ctrl.ack_level << 6);
    result |= (uint16_t)(chan->ctrl.ack_mode << 7);
    result |= (uint16_t)(chan->ctrl.mode << 8);
    result |= (uint16_t)(chan->ctrl.src_step << 12);
    result |= (uint16_t)(chan->ctrl.dst_step << 14);
    return result;
}

static uint16_t channel_get_ctrl(Channel *chan) {
    uint16_t result = channel_peek_ctrl(chan);
    if (chan->ctrl.finished) chan->ctrl.te_read_seen = 1;
    return result;
}

static void channel_set_ctrl(Channel *chan, uint16_t value) {
    int ch = channel_index(chan);
    chan->ctrl.enable = value & 0x1;
    if ((value & 0x2u) == 0 && chan->ctrl.te_read_seen) {
        chan->ctrl.finished = 0;
        chan->ctrl.te_read_seen = 0;
        sh7021_ocpm_intc_deassert_irq((IRQ)(IRQ_DMAC0 + ch));
    }
    chan->ctrl.irq_enable = (value >> 2) & 0x1;
    chan->ctrl.transfer_16bit = (value >> 3) & 0x1;
    chan->ctrl.is_burst = (value >> 4) & 0x1;
    chan->ctrl.mode = (value >> 8) & 0xF;
    chan->ctrl.src_step = (value >> 12) & 0x3;
    chan->ctrl.dst_step = (value >> 14) & 0x3;
    if (ch < 2) {
        chan->ctrl.dreq_select = (value >> 5) & 0x1;
        chan->ctrl.ack_level = (value >> 6) & 0x1;
        chan->ctrl.ack_mode = (value >> 7) & 0x1;
    } else {
        chan->ctrl.dreq_select = 0;
        chan->ctrl.ack_level = 0;
        chan->ctrl.ack_mode = 0;
    }
}

static int channel_request_valid(Channel *chan) {
    if (!chan->ctrl.enable || chan->ctrl.finished) return 0;
    if (!(state.ctrl & 0x0001u)) return 0;
    if (state.ctrl & 0x0006u) return 0;
    if (chan->ctrl.mode < 0 || chan->ctrl.mode >= DREQ_NumDreq) return 0;
    return state.dreqs[chan->ctrl.mode];
}

static void channel_finish(Channel *chan) {
    int ch = channel_index(chan);
    chan->ctrl.finished = 1;
    if (chan->ctrl.irq_enable) sh7021_ocpm_intc_assert_irq((IRQ)(IRQ_DMAC0 + ch), 0);
}

static void channel_start_transfer(Channel *chan) {
    int src_step = 0;
    switch (chan->ctrl.src_step) { case 1: src_step = 1; break; case 2: src_step = -1; break; default: break; }
    int dst_step = 0;
    switch (chan->ctrl.dst_step) { case 1: dst_step = 1; break; case 2: dst_step = -1; break; default: break; }
    if (chan->ctrl.src_step == 3 || chan->ctrl.dst_step == 3) {
        state.ctrl |= 0x0004u; /* Address/error flag: reserved address-step encoding. */
        return;
    }
    src_step <<= chan->ctrl.transfer_16bit;
    dst_step <<= chan->ctrl.transfer_16bit;
    int single = channel_is_single_address(chan);
    if (chan->ctrl.transfer_16bit) {
        while (chan->transfer_size && channel_request_valid(chan)) {
            uint16_t value = sh7021_bus_dma_read16(chan->src_addr, single);
            sh7021_bus_dma_write16(chan->dst_addr, value, single);
            chan->src_addr += (uint32_t)src_step;
            chan->dst_addr += (uint32_t)dst_step;
            chan->transfer_size--;
            if (!chan->ctrl.is_burst) break;
        }
    } else {
        while (chan->transfer_size && channel_request_valid(chan)) {
            uint8_t value = sh7021_bus_dma_read8(chan->src_addr, single);
            sh7021_bus_dma_write8(chan->dst_addr, value, single);
            chan->src_addr += (uint32_t)src_step;
            chan->dst_addr += (uint32_t)dst_step;
            chan->transfer_size--;
            if (!chan->ctrl.is_burst) break;
        }
    }
    if (!chan->transfer_size) channel_finish(chan);
}

static int priority_order_slot(int slot) {
    static const int fixed0[4] = {0, 3, 2, 1};
    static const int fixed1[4] = {1, 3, 2, 0};
    static const int rr0[4] = {0, 3, 2, 1};
    int mode = (state.ctrl >> 8) & 3;
    if (mode == 0) return fixed0[slot & 3];
    if (mode == 1) return fixed1[slot & 3];
    return rr0[(slot + state.priority_cursor) & 3];
}

static void check_pulsed_activation(DREQ dreq) {
    if ((int)dreq < 0 || dreq >= DREQ_NumDreq) return;
    state.dreqs[(int)dreq] = 1;
    for (int slot = 0; slot < 4; slot++) {
        int i = priority_order_slot(slot);
        Channel *x = &state.chan[i];
        if (x->ctrl.mode == (int)dreq && channel_request_valid(x)) {
            channel_start_transfer(x);
            if (((state.ctrl >> 8) & 3) >= 2) state.priority_cursor = (uint8_t)((slot + 1) & 3);
            break;
        }
    }
    state.dreqs[(int)dreq] = 0;
}

static void check_activations(void) {
    int progressed;
    int guard = 0;
    do {
        progressed = 0;
        if (++guard > 65536) break;
        for (int slot = 0; slot < 4; slot++) {
            int i = priority_order_slot(slot);
            Channel *x = &state.chan[i];
            uint32_t before = x->transfer_size;
            if (channel_request_valid(x)) {
                channel_start_transfer(x);
                if (x->transfer_size != before || x->ctrl.finished || (state.ctrl & 0x0004u)) {
                    progressed = 1;
                    if (((state.ctrl >> 8) & 3) >= 2) state.priority_cursor = (uint8_t)((slot + 1) & 3);
                }
            }
        }
    } while (progressed);
}

uint8_t sh7021_ocpm_dmac_read8(uint32_t addr) {
    addr &= 0x3Fu;
    uint32_t reg = addr & 0x0Fu;
    if (reg < 0x08u) {
        uint32_t value = sh7021_ocpm_dmac_read32(addr & ~3u);
        return (uint8_t)(value >> ((3u - (addr & 3u)) * 8u));
    }
    uint16_t value = sh7021_ocpm_dmac_read16(addr & ~1u);
    return (uint8_t)(value >> ((1u - (addr & 1u)) * 8u));
}

uint16_t sh7021_ocpm_dmac_read16(uint32_t addr) {
    addr &= 0x3Fu;
    if (addr == 0x08) {
        if (state.ctrl & 0x0002u) state.nmif_read_seen = 1;
        if (state.ctrl & 0x0004u) state.ae_read_seen = 1;
        return dmaor_read_value();
    }
    int reg = addr & 0x0F;
    Channel *chan = &state.chan[addr >> 4];
    switch (reg) {
    case 0x00: return (uint16_t)(chan->src_addr >> 16);
    case 0x02: return (uint16_t)(chan->src_addr & 0xFFFFu);
    case 0x04: return (uint16_t)(chan->dst_addr >> 16);
    case 0x06: return (uint16_t)(chan->dst_addr & 0xFFFFu);
    case 0x0A: return (uint16_t)(chan->transfer_size & 0xFFFFu);
    case 0x0E: return channel_get_ctrl(chan);
    default:
        LOOPY_DEBUG_PRINTF("[SH7021/DMAC] unmapped read16 %08X\n", addr);
        return 0;
    }
}

static void write_dmaor(uint16_t value) {
    uint16_t next = (uint16_t)(state.ctrl & 0x0006u);
    next |= (uint16_t)(value & 0x0301u);
    if (state.nmif_read_seen && !(value & 0x0002u)) { next &= (uint16_t)~0x0002u; state.nmif_read_seen = 0; }
    if (state.ae_read_seen && !(value & 0x0004u)) { next &= (uint16_t)~0x0004u; state.ae_read_seen = 0; }
    state.ctrl = next;
}

void sh7021_ocpm_dmac_write8(uint32_t addr, uint8_t value) {
    addr &= 0x3Fu;

    if (addr == 0x08u || addr == 0x09u) {
        uint16_t cur = dmaor_read_value();
        uint16_t next = (addr & 1u) ? (uint16_t)((cur & 0xFF00u) | value)
                                    : (uint16_t)((cur & 0x00FFu) | ((uint16_t)value << 8));
        write_dmaor(next);
        check_activations();
        return;
    }

    uint32_t reg = addr & 0x0Fu;
    Channel *chan = &state.chan[addr >> 4];
    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03: {
        unsigned shift = (unsigned)((3u - reg) * 8u);
        chan->src_addr = (chan->src_addr & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        break;
    }
    case 0x04: case 0x05: case 0x06: case 0x07: {
        unsigned shift = (unsigned)((3u - (reg - 0x04u)) * 8u);
        chan->dst_addr = (chan->dst_addr & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        break;
    }
    case 0x0A: case 0x0B: {
        uint16_t cur = (uint16_t)chan->transfer_size;
        uint16_t next = (reg & 1u) ? (uint16_t)((cur & 0xFF00u) | value)
                                   : (uint16_t)((cur & 0x00FFu) | ((uint16_t)value << 8));
        chan->transfer_size = next ? next : 0x10000u;
        break;
    }
    case 0x0E: case 0x0F: {
        uint16_t cur = channel_peek_ctrl(chan);
        uint16_t next = (reg & 1u) ? (uint16_t)((cur & 0xFF00u) | value)
                                   : (uint16_t)((cur & 0x00FFu) | ((uint16_t)value << 8));
        channel_set_ctrl(chan, next);
        check_activations();
        break;
    }
    default:
        LOOPY_DEBUG_PRINTF("[SH7021/DMAC] unmapped write8 %08X: %02X\n", addr, value);
        break;
    }
}

void sh7021_ocpm_dmac_write16(uint32_t addr, uint16_t value) {
    addr &= 0x3Fu;
    if (addr == 0x08) { write_dmaor(value); check_activations(); return; }
    int reg = addr & 0x0F;
    Channel *chan = &state.chan[addr >> 4];
    switch (reg) {
    case 0x00: chan->src_addr = (chan->src_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
    case 0x02: chan->src_addr = (chan->src_addr & 0xFFFF0000u) | value; break;
    case 0x04: chan->dst_addr = (chan->dst_addr & 0x0000FFFFu) | ((uint32_t)value << 16); break;
    case 0x06: chan->dst_addr = (chan->dst_addr & 0xFFFF0000u) | value; break;
    case 0x0A:
        chan->transfer_size = value;
        if (!chan->transfer_size) chan->transfer_size = 0x10000;
        break;
    case 0x0E:
        channel_set_ctrl(chan, value);
        check_activations();
        break;
    default:
        LOOPY_DEBUG_PRINTF("[SH7021/DMAC] unmapped write16 %08X: %04X\n", addr, value);
        break;
    }
}

void sh7021_ocpm_dmac_write32(uint32_t addr, uint32_t value) {
    addr &= 0x3Fu;
    int reg = addr & 0x0F;
    Channel *chan = &state.chan[addr >> 4];
    switch (reg) {
    case 0x00:
        chan->src_addr = value;
        break;
    case 0x04:
        chan->dst_addr = value;
        break;
    case 0x08:
        if ((addr >> 4) == 0) write_dmaor((uint16_t)(value >> 16));
        chan->transfer_size = (uint16_t)value;
        if (!chan->transfer_size) chan->transfer_size = 0x10000;
        check_activations();
        break;
    case 0x0C:
        channel_set_ctrl(chan, (uint16_t)value);
        check_activations();
        break;
    default:
        LOOPY_DEBUG_PRINTF("[SH7021/DMAC] unmapped write32 %08X: %08X\n", addr, value);
        break;
    }
}

uint32_t sh7021_ocpm_dmac_read32(uint32_t addr) {
    addr &= 0x3Fu;
    int reg = addr & 0x0F;
    Channel *chan = &state.chan[addr >> 4];
    switch (reg) {
    case 0x00: return chan->src_addr;
    case 0x04: return chan->dst_addr;
    case 0x08: return ((uint32_t)((addr >> 4) == 0 ? sh7021_ocpm_dmac_read16(0x08) : 0) << 16) | (uint16_t)chan->transfer_size;
    case 0x0C: return channel_get_ctrl(chan);
    default:
        LOOPY_DEBUG_PRINTF("[SH7021/DMAC] unmapped read32 %08X\n", addr);
        return 0;
    }
}

void sh7021_ocpm_dmac_initialize(void) {
    memset(&state, 0, sizeof(state));
    sh7021_ocpm_dmac_send_dreq(DREQ_Auto);
}

void sh7021_ocpm_dmac_send_dreq(DREQ dreq) {
    if ((int)dreq < 0 || dreq >= DREQ_NumDreq) return;
    state.dreqs[(int)dreq] = 1;
    check_activations();
}

void sh7021_ocpm_dmac_clear_dreq(DREQ dreq) {
    if ((int)dreq < 0 || dreq >= DREQ_NumDreq) return;
    state.dreqs[(int)dreq] = 0;
}

void sh7021_ocpm_dmac_pulse_dreq(DREQ dreq) {
    check_pulsed_activation(dreq);
}

uint32_t sh7021_ocpm_dmac_state_blob_size(void) { return (uint32_t)sizeof(state); }
void sh7021_ocpm_dmac_get_state_blob(void *dst, uint32_t size) { if (dst && size == sizeof(state)) memcpy(dst, &state, sizeof(state)); }
void sh7021_ocpm_dmac_set_state_blob(const void *src, uint32_t size) { if (src && size == sizeof(state)) memcpy(&state, src, sizeof(state)); }
