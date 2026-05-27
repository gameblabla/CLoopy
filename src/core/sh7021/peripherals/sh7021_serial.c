#include "core/sh7021/peripherals/sh7021_serial.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/timing.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PORT_COUNT 2

static TimingFuncHandle tx_ev_func;

typedef struct PortMode {
    int clock_factor, mp_enable, stop_bit_length, parity_mode, parity_enable, seven_bit_mode, sync_mode;
} PortMode;
typedef struct PortCtrl {
    int clock_mode, tx_end_intr_enable, mp_intr_enable, rx_enable, tx_enable, rx_intr_enable, tx_intr_enable;
} PortCtrl;
typedef struct PortStatus { int tx_empty; } PortStatus;

typedef struct Port {
    TimingEventHandle tx_ev;
    DREQ rx_dreq_id, tx_dreq_id;
    int id;
    int bit_factor;
    int cycles_per_bit;
    PortMode mode;
    PortCtrl ctrl;
    PortStatus status;
    int tx_bits_left;
    uint8_t tx_shift_reg;
    uint8_t tx_buffer;
    uint8_t tx_prepared_data;
    SerialTxCallback tx_callback;
} Port;

typedef struct SerialState { Port ports[PORT_COUNT]; } SerialState;
static SerialState state;

static void port_calc_cycles_per_bit(Port *port) {
    assert(!port->mode.sync_mode);
    port->cycles_per_bit = (32 << (port->mode.clock_factor * 2)) * (port->bit_factor + 1);
}

static void port_sched_tx_ev(Port *port) {
    TimingUnitCycle sched_cycles = timing_convert_cpu(port->cycles_per_bit);
    port->tx_ev = timing_add_event(tx_ev_func, sched_cycles, (uint64_t)(uint32_t)port->id, TIMING_CPU_TIMER);
}

static void port_tx_start(Port *port, uint8_t value) {
    port->tx_bits_left = 8;
    port->tx_shift_reg = value;
    port->status.tx_empty = true;
    port_sched_tx_ev(port);
}

static void check_tx_dreqs(void) {
    for (int i = 0; i < PORT_COUNT; i++) {
        Port *port = &state.ports[i];
        if (port->status.tx_empty && port->ctrl.tx_enable) sh7021_ocpm_dmac_send_dreq(port->tx_dreq_id);
    }
}

static void tx_event(uint64_t param, int cycles_late) {
    (void)cycles_late;
    Port *port = NULL;
    if (param < PORT_COUNT) {
        port = &state.ports[(unsigned)param];
    } else {
        /* Older save states stored a host pointer as the TX event parameter.
         * Recover to the first port with an active TX event; new states use the
         * stable integer port id passed above. */
        for (int i = 0; i < PORT_COUNT; i++) {
            if (timing_event_handle_is_valid(state.ports[i].tx_ev)) { port = &state.ports[i]; break; }
        }
    }
    if (!port) return;
    bool bit = (port->tx_shift_reg & 0x1) != 0;
    port->tx_shift_reg >>= 1;
    port->tx_prepared_data >>= 1;
    port->tx_prepared_data |= (uint8_t)(bit << 7);
    port->tx_bits_left--;
    if (!port->tx_bits_left) {
        LOOPY_DEBUG_PRINTF("[Serial] port%d tx %02X\n", port->id, port->tx_prepared_data);
        if (port->tx_callback) port->tx_callback(port->tx_prepared_data);
        if (!port->status.tx_empty) {
            port_tx_start(port, port->tx_buffer);
            check_tx_dreqs();
        } else {
            LOOPY_DEBUG_PRINTF("[Serial] port%d finished tx\n", port->id);
        }
    } else {
        port_sched_tx_ev(port);
    }
}

void sh7021_ocpm_serial_initialize(void) {
    memset(&state, 0, sizeof(state));
    tx_ev_func = timing_register_func("Serial::tx_event", tx_event);
    for (int i = 0; i < PORT_COUNT; i++) {
        state.ports[i].id = i;
        state.ports[i].status.tx_empty = true;
        port_calc_cycles_per_bit(&state.ports[i]);
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
    LOOPY_DEBUG_PRINTF("[Serial] read port%d reg%d\n", port->id, reg);
    return 0;
}

void sh7021_ocpm_serial_write8(uint32_t addr, uint8_t value) {
    addr &= 0xFu;
    Port *port = &state.ports[addr >> 3];
    int reg = addr & 0x7;
    switch (reg) {
    case 0x00:
        LOOPY_DEBUG_PRINTF("[Serial] write port%d mode: %02X\n", port->id, value);
        port->mode.clock_factor = value & 0x3;
        port->mode.mp_enable = (value >> 2) & 0x1;
        port->mode.stop_bit_length = (value >> 3) & 0x1;
        port->mode.parity_mode = (value >> 4) & 0x1;
        port->mode.parity_enable = (value >> 5) & 0x1;
        port->mode.seven_bit_mode = (value >> 6) & 0x1;
        port->mode.sync_mode = (value >> 7) & 0x1;
        assert(!(value & ~0x3));
        break;
    case 0x01:
        LOOPY_DEBUG_PRINTF("[Serial] write port%d bitrate factor: %02X\n", port->id, value);
        port->bit_factor = value;
        port_calc_cycles_per_bit(port);
        LOOPY_DEBUG_PRINTF("[Serial] set port%d baudrate: %d bit/s\n", port->id, TIMING_F_CPU / port->cycles_per_bit);
        break;
    case 0x02:
        LOOPY_DEBUG_PRINTF("[Serial] write port%d ctrl: %02X\n", port->id, value);
        port->ctrl.clock_mode = value & 0x3;
        port->ctrl.tx_end_intr_enable = (value >> 2) & 0x1;
        port->ctrl.mp_intr_enable = (value >> 3) & 0x1;
        port->ctrl.rx_enable = (value >> 4) & 0x1;
        port->ctrl.tx_enable = (value >> 5) & 0x1;
        port->ctrl.rx_intr_enable = (value >> 6) & 0x1;
        port->ctrl.tx_intr_enable = (value >> 7) & 0x1;
        if (!port->ctrl.tx_enable) port->status.tx_empty = true;
        check_tx_dreqs();
        break;
    case 0x03:
        if (!port->ctrl.tx_enable) { port->tx_buffer = value; break; }
        if (!port->tx_bits_left) port_tx_start(port, value);
        else { port->tx_buffer = value; port->status.tx_empty = false; sh7021_ocpm_dmac_clear_dreq(port->tx_dreq_id); }
        break;
    case 0x04:
        LOOPY_DEBUG_PRINTF("[Serial write port%d status: %02X\n", port->id, value);
        break;
    default:
        LOOPY_DEBUG_PRINTF("[Serial] ignored write port%d reg%d: %02X\n", port->id, reg, value);
        break;
    }
}

void sh7021_ocpm_serial_set_tx_callback(int port, SerialTxCallback callback) {
    assert(port >= 0 && port < PORT_COUNT);
    state.ports[port].tx_callback = callback;
}

uint32_t sh7021_ocpm_serial_state_blob_size(void) { return (uint32_t)sizeof(state); }
void sh7021_ocpm_serial_get_state_blob(void *dst, uint32_t size) {
    if (!dst || size != sizeof(state)) return;
    SerialState tmp = state;
    for (int i = 0; i < PORT_COUNT; i++) tmp.ports[i].tx_callback = NULL;
    memcpy(dst, &tmp, sizeof(tmp));
}
void sh7021_ocpm_serial_set_state_blob(const void *src, uint32_t size) {
    if (!src || size != sizeof(state)) return;
    SerialTxCallback callbacks[PORT_COUNT];
    for (int i = 0; i < PORT_COUNT; i++) callbacks[i] = state.ports[i].tx_callback;
    memcpy(&state, src, sizeof(state));
    for (int i = 0; i < PORT_COUNT; i++) {
        state.ports[i].id = i;
        state.ports[i].tx_callback = callbacks[i];
    }
}
