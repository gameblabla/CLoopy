#include "core/sh7021/peripherals/sh7021_serial.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/timing.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PORT_COUNT 2

/* SH7021 SCI register bits. */
#define SMR_CA   0x80u
#define SMR_CHR  0x40u
#define SMR_PE   0x20u
#define SMR_STOP 0x08u
#define SMR_MP   0x04u
#define SMR_CKS  0x03u

#define SCR_TIE  0x80u
#define SCR_RIE  0x40u
#define SCR_TE   0x20u
#define SCR_RE   0x10u
#define SCR_TEIE 0x04u

#define SSR_TDRE 0x80u
#define SSR_RDRF 0x40u
#define SSR_ORER 0x20u
#define SSR_FER  0x10u
#define SSR_PER  0x08u
#define SSR_TEND 0x04u
#define SSR_MPB  0x02u
#define SSR_MPBT 0x01u

static TimingFuncHandle tx_ev_func;

typedef struct Port {
    TimingEventHandle tx_ev;
    DREQ rx_dreq_id, tx_dreq_id;
    int id;

    /* Programmer-visible registers.  The reset values match the SH7021 SCI
       and the MiSTer Loopy implementation: SMR=00, BRR=FF, SCR=00,
       TDR=FF, SSR=84, RDR=00. */
    uint8_t smr;
    uint8_t brr;
    uint8_t scr;
    uint8_t tdr;
    uint8_t ssr;
    uint8_t rdr;

    int cycles_per_bit;
    int tx_active;
    uint8_t tx_shift_reg;
    SerialTxCallback tx_callback;

    /* Keep the serial save-state chunk at its historical 104 bytes/channel.
       Existing v2 system states therefore remain structurally loadable even
       though the programmer-visible SCI model is now more complete. */
    uint32_t save_magic;
    uint8_t save_reserved[52];
} Port;

typedef struct SerialState { Port ports[PORT_COUNT]; } SerialState;

/* Layout used by releases through edb5286.  It is only needed to migrate the
   serial chunk of an older save; active legacy TX events are safely allowed to
   expire after load rather than replaying the old bit-at-a-time model. */
typedef struct LegacyPortMode {
    int clock_factor, mp_enable, stop_bit_length, parity_mode, parity_enable, seven_bit_mode, sync_mode;
} LegacyPortMode;
typedef struct LegacyPortCtrl {
    int clock_mode, tx_end_intr_enable, mp_intr_enable, rx_enable, tx_enable, rx_intr_enable, tx_intr_enable;
} LegacyPortCtrl;
typedef struct LegacyPortStatus { int tx_empty; } LegacyPortStatus;
typedef struct LegacyPort {
    TimingEventHandle tx_ev;
    DREQ rx_dreq_id, tx_dreq_id;
    int id;
    int bit_factor;
    int cycles_per_bit;
    LegacyPortMode mode;
    LegacyPortCtrl ctrl;
    LegacyPortStatus status;
    int tx_bits_left;
    uint8_t tx_shift_reg;
    uint8_t tx_buffer;
    uint8_t tx_prepared_data;
    SerialTxCallback tx_callback;
} LegacyPort;
typedef struct LegacySerialState { LegacyPort ports[PORT_COUNT]; } LegacySerialState;

#define SERIAL_SAVE_MAGIC 0x53434932u /* SCI2 */
_Static_assert(sizeof(Port) == 104, "serial Port save layout must stay 104 bytes");
_Static_assert(sizeof(SerialState) == 208, "serial state chunk must remain backward-sized");
_Static_assert(sizeof(LegacySerialState) == 208, "legacy serial state layout mismatch");

static SerialState state;

static IRQ port_irq(const Port *port) { return (IRQ)(IRQ_SCI0 + port->id); }

static void port_calc_cycles_per_bit(Port *port) {
    int clock_scale = 1 << ((port->smr & SMR_CKS) * 2);
    int divisor = (int)port->brr + 1;

    /* The SH7021 divider ticks at 1/16 of an asynchronous bit and 1/4 of a
       synchronous bit.  One async divider tick is 2*4^CKS*(BRR+1) CPU
       cycles; one sync tick is 4^CKS*(BRR+1). */
    if (port->smr & SMR_CA) port->cycles_per_bit = 4 * clock_scale * divisor;
    else                    port->cycles_per_bit = 32 * clock_scale * divisor;
}

static int port_frame_bits(const Port *port) {
    if (port->smr & SMR_CA) return 8;
    int data_bits = (port->smr & SMR_CHR) ? 7 : 8;
    int extra_bit = (port->smr & (SMR_PE | SMR_MP)) ? 1 : 0;
    int stop_bits = (port->smr & SMR_STOP) ? 2 : 1;
    return 1 + data_bits + extra_bit + stop_bits; /* start + data + parity/MP + stop */
}

static void port_update_irq(Port *port) {
    int vector_offs = -1;
    if ((port->scr & SCR_RIE) && (port->ssr & (SSR_ORER | SSR_FER | SSR_PER))) vector_offs = 0; /* ERI */
    else if ((port->scr & SCR_RIE) && (port->ssr & SSR_RDRF)) vector_offs = 1;                    /* RXI */
    else if ((port->scr & SCR_TIE) && (port->ssr & SSR_TDRE)) vector_offs = 2;                    /* TXI */
    else if ((port->scr & SCR_TEIE) && (port->ssr & SSR_TEND)) vector_offs = 3;                   /* TEI */

    if (vector_offs >= 0) sh7021_ocpm_intc_assert_irq(port_irq(port), vector_offs);
    else sh7021_ocpm_intc_deassert_irq(port_irq(port));
}

static void port_update_tx_dreq(Port *port) {
    if ((port->scr & SCR_TE) && (port->ssr & SSR_TDRE)) sh7021_ocpm_dmac_send_dreq(port->tx_dreq_id);
    else sh7021_ocpm_dmac_clear_dreq(port->tx_dreq_id);
}

static void port_update_outputs(Port *port) {
    port_update_irq(port);
    port_update_tx_dreq(port);
}

static void port_schedule_frame(Port *port) {
    int64_t frame_cycles = (int64_t)port->cycles_per_bit * port_frame_bits(port);
    if (frame_cycles < 1) frame_cycles = 1;
    port->tx_ev = timing_add_event(tx_ev_func, timing_convert_cpu(frame_cycles),
                                   (uint64_t)(uint32_t)port->id, TIMING_CPU_TIMER);
}

/* Transfer TDR into the transmit shift register once software (or DMA) has
   cleared TDRE.  Real hardware does this on the next baud-divider tick; doing
   it at the register boundary keeps the CPU-visible buffering semantics while
   avoiding an artificial whole-frame busy wait. */
static void port_maybe_start_tx(Port *port) {
    if (!(port->scr & SCR_TE) || port->tx_active || (port->ssr & SSR_TDRE)) return;

    port->tx_shift_reg = port->tdr;
    port->tx_active = 1;
    port->ssr |= SSR_TDRE;
    port->ssr &= (uint8_t)~SSR_TEND;
    port_schedule_frame(port);
    port_update_outputs(port);
}

static void port_stop_tx(Port *port) {
    if (port->tx_active && timing_event_handle_is_valid(port->tx_ev)) {
        timing_cancel_event(&port->tx_ev);
    }
    port->tx_ev = timing_invalid_event_handle();
    port->tx_active = 0;
    port->ssr |= (SSR_TDRE | SSR_TEND);
    port_update_outputs(port);
}

static void tx_event(uint64_t param, int cycles_late) {
    (void)cycles_late;
    Port *port = NULL;
    if (param < PORT_COUNT) {
        port = &state.ports[(unsigned)param];
    } else {
        /* Older save states stored a host pointer as the TX event parameter.
           Recover to the first port with a live transmission. */
        for (int i = 0; i < PORT_COUNT; i++) {
            if (state.ports[i].tx_active) { port = &state.ports[i]; break; }
        }
    }
    if (!port || !port->tx_active) return;

    /* This event has already been popped from the scheduler. */
    port->tx_ev = timing_invalid_event_handle();

    LOOPY_DEBUG_PRINTF("[Serial] port%d tx %02X\n", port->id, port->tx_shift_reg);
    if (port->tx_callback) port->tx_callback(port->tx_shift_reg);
    port->tx_active = 0;

    if ((port->scr & SCR_TE) && !(port->ssr & SSR_TDRE)) {
        /* A second TDR byte was queued while this frame was on the wire. */
        port_maybe_start_tx(port);
    } else {
        port->ssr |= SSR_TEND;
        port_update_outputs(port);
        LOOPY_DEBUG_PRINTF("[Serial] port%d finished tx\n", port->id);
    }
}

void sh7021_ocpm_serial_initialize(void) {
    memset(&state, 0, sizeof(state));
    tx_ev_func = timing_register_func("Serial::tx_event", tx_event);
    for (int i = 0; i < PORT_COUNT; i++) {
        Port *port = &state.ports[i];
        port->id = i;
        port->tx_ev = timing_invalid_event_handle();
        port->smr = 0x00;
        port->brr = 0xFF;
        port->scr = 0x00;
        port->tdr = 0xFF;
        port->ssr = SSR_TDRE | SSR_TEND;
        port->rdr = 0x00;
        port->save_magic = SERIAL_SAVE_MAGIC;
        port_calc_cycles_per_bit(port);
    }
    state.ports[0].rx_dreq_id = DREQ_RXI0;
    state.ports[1].rx_dreq_id = DREQ_RXI1;
    state.ports[0].tx_dreq_id = DREQ_TXI0;
    state.ports[1].tx_dreq_id = DREQ_TXI1;
}

uint8_t sh7021_ocpm_serial_read8(uint32_t addr) {
    addr &= 0xFu;
    Port *port = &state.ports[addr >> 3];
    int reg = addr & 0x7;
    uint8_t value = 0xFF;
    switch (reg) {
    case 0x00: value = port->smr; break;
    case 0x01: value = port->brr; break;
    case 0x02: value = port->scr; break;
    case 0x03: value = port->tdr; break;
    case 0x04: value = port->ssr; break;
    case 0x05: value = port->rdr; break;
    default: break;
    }
    LOOPY_DEBUG_PRINTF("[Serial] read port%d reg%d -> %02X\n", port->id, reg, value);
    return value;
}

void sh7021_ocpm_serial_write8(uint32_t addr, uint8_t value) {
    addr &= 0xFu;
    Port *port = &state.ports[addr >> 3];
    int reg = addr & 0x7;
    switch (reg) {
    case 0x00:
        LOOPY_DEBUG_PRINTF("[Serial] write port%d mode: %02X\n", port->id, value);
        port->smr = value;
        port_calc_cycles_per_bit(port);
        break;
    case 0x01:
        LOOPY_DEBUG_PRINTF("[Serial] write port%d bitrate factor: %02X\n", port->id, value);
        port->brr = value;
        port_calc_cycles_per_bit(port);
        LOOPY_DEBUG_PRINTF("[Serial] set port%d baudrate: %d bit/s\n", port->id,
                           port->cycles_per_bit ? TIMING_F_CPU / port->cycles_per_bit : 0);
        break;
    case 0x02: {
        LOOPY_DEBUG_PRINTF("[Serial] write port%d ctrl: %02X\n", port->id, value);
        port->scr = value;
        if (!(value & SCR_TE)) {
            /* Clearing TE aborts/parks the transmitter and restores idle flags. */
            port_stop_tx(port);
        } else {
            port_maybe_start_tx(port);
            port_update_outputs(port);
        }
        break;
    }
    case 0x03:
        LOOPY_DEBUG_PRINTF("[Serial] write port%d TDR: %02X\n", port->id, value);
        /* TDR itself is only a holding register.  TDRE is cleared by software
           through SSR (or by a DMA acknowledge), exactly as on the SH7021. */
        port->tdr = value;
        break;
    case 0x04: {
        LOOPY_DEBUG_PRINTF("[Serial] write port%d status: %02X\n", port->id, value);
        /* SCI flags are clear-only from the CPU side.  On SH7021, clearing
           TDRE also clears TEND and hands the byte in TDR to the transmitter. */
        if (!(value & SSR_TDRE)) port->ssr &= (uint8_t)~(SSR_TDRE | SSR_TEND);
        if (!(value & SSR_RDRF)) port->ssr &= (uint8_t)~SSR_RDRF;
        if (!(value & SSR_ORER)) port->ssr &= (uint8_t)~SSR_ORER;
        if (!(value & SSR_FER))  port->ssr &= (uint8_t)~SSR_FER;
        if (!(value & SSR_PER))  port->ssr &= (uint8_t)~SSR_PER;
        if (value & SSR_MPBT) port->ssr |= SSR_MPBT;
        else port->ssr &= (uint8_t)~SSR_MPBT;
        port_maybe_start_tx(port);
        port_update_outputs(port);
        break;
    }
    default:
        LOOPY_DEBUG_PRINTF("[Serial] ignored write port%d reg%d: %02X\n", port->id, reg, value);
        break;
    }
}

void sh7021_ocpm_serial_dma_tx_ack(int id) {
    if (id < 0 || id >= PORT_COUNT) return;
    Port *port = &state.ports[id];
    port->ssr &= (uint8_t)~(SSR_TDRE | SSR_TEND);
    port_maybe_start_tx(port);
    port_update_outputs(port);
}

void sh7021_ocpm_serial_dma_rx_ack(int id) {
    if (id < 0 || id >= PORT_COUNT) return;
    Port *port = &state.ports[id];
    port->ssr &= (uint8_t)~SSR_RDRF;
    port_update_outputs(port);
}

void sh7021_ocpm_serial_set_tx_callback(int port, SerialTxCallback callback) {
    assert(port >= 0 && port < PORT_COUNT);
    state.ports[port].tx_callback = callback;
}

uint32_t sh7021_ocpm_serial_state_blob_size(void) { return (uint32_t)sizeof(state); }
void sh7021_ocpm_serial_get_state_blob(void *dst, uint32_t size) {
    if (!dst || size != sizeof(state)) return;
    SerialState tmp = state;
    for (int i = 0; i < PORT_COUNT; i++) {
        tmp.ports[i].tx_callback = NULL;
        tmp.ports[i].save_magic = SERIAL_SAVE_MAGIC;
    }
    memcpy(dst, &tmp, sizeof(tmp));
}

static uint8_t legacy_smr(const LegacyPort *old) {
    return (uint8_t)((old->mode.clock_factor & 3) |
                     ((old->mode.mp_enable & 1) << 2) |
                     ((old->mode.stop_bit_length & 1) << 3) |
                     ((old->mode.parity_mode & 1) << 4) |
                     ((old->mode.parity_enable & 1) << 5) |
                     ((old->mode.seven_bit_mode & 1) << 6) |
                     ((old->mode.sync_mode & 1) << 7));
}

static uint8_t legacy_scr(const LegacyPort *old) {
    return (uint8_t)((old->ctrl.clock_mode & 3) |
                     ((old->ctrl.tx_end_intr_enable & 1) << 2) |
                     ((old->ctrl.mp_intr_enable & 1) << 3) |
                     ((old->ctrl.rx_enable & 1) << 4) |
                     ((old->ctrl.tx_enable & 1) << 5) |
                     ((old->ctrl.rx_intr_enable & 1) << 6) |
                     ((old->ctrl.tx_intr_enable & 1) << 7));
}

void sh7021_ocpm_serial_set_state_blob(const void *src, uint32_t size) {
    if (!src || size != sizeof(state)) return;
    SerialTxCallback callbacks[PORT_COUNT];
    for (int i = 0; i < PORT_COUNT; i++) callbacks[i] = state.ports[i].tx_callback;

    const SerialState *saved = (const SerialState *)src;
    if (saved->ports[0].save_magic == SERIAL_SAVE_MAGIC &&
        saved->ports[1].save_magic == SERIAL_SAVE_MAGIC) {
        memcpy(&state, src, sizeof(state));
    } else {
        const LegacySerialState *legacy = (const LegacySerialState *)src;
        memset(&state, 0, sizeof(state));
        for (int i = 0; i < PORT_COUNT; i++) {
            Port *port = &state.ports[i];
            const LegacyPort *old = &legacy->ports[i];
            port->id = i;
            port->tx_ev = timing_invalid_event_handle();
            port->rx_dreq_id = i ? DREQ_RXI1 : DREQ_RXI0;
            port->tx_dreq_id = i ? DREQ_TXI1 : DREQ_TXI0;
            port->smr = legacy_smr(old);
            port->brr = (uint8_t)old->bit_factor;
            port->scr = legacy_scr(old);
            port->tdr = old->tx_buffer;
            port->ssr = SSR_TDRE | SSR_TEND;
            port->rdr = 0x00;
            port->save_magic = SERIAL_SAVE_MAGIC;
            port_calc_cycles_per_bit(port);
        }
    }

    for (int i = 0; i < PORT_COUNT; i++) {
        state.ports[i].id = i;
        state.ports[i].rx_dreq_id = i ? DREQ_RXI1 : DREQ_RXI0;
        state.ports[i].tx_dreq_id = i ? DREQ_TXI1 : DREQ_TXI0;
        state.ports[i].tx_callback = callbacks[i];
        state.ports[i].save_magic = SERIAL_SAVE_MAGIC;
        port_calc_cycles_per_bit(&state.ports[i]);
    }
    /* INTC and DMAC are restored from their own chunks.  Do not generate new
       handshakes while a system state is still being loaded. */
}
