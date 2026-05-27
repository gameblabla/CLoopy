#include "core/sh7021/peripherals/sh7021_timers.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/timing.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TIMER_COUNT 5

static TimingFuncHandle ev_func;

typedef struct TimerCtrl { int clock, edge_mode, clear_mode; } TimerCtrl;
typedef struct SH7021Timer {
    TimingEventHandle ev;
    IRQ irq;
    int enabled;
    int id;
    TimerCtrl ctrl;
    int intr_enable;
    int intr_flag;
    uint32_t counter;
    uint32_t counter_when_started;
    uint32_t gen_reg[2];
    int64_t time_when_started;
} SH7021Timer;

typedef struct TimerState {
    int timer_enable;
    int sync_ctrl;
    int mode;
    SH7021Timer timers[TIMER_COUNT];
} TimerState;

typedef struct TimerDev { SH7021Timer *timer; int reg; } TimerDev;
static TimerState state;

static void timer_update_counter(SH7021Timer *timer) {
    if (!timing_event_handle_is_valid(timer->ev)) return;
    int clock = timer->ctrl.clock & 0x3;
    int64_t time_elapsed = timing_get_timestamp(TIMING_CPU_TIMER) - timer->time_when_started;
    timer->counter = timer->counter_when_started + (uint32_t)(time_elapsed >> clock);
    timer->counter &= 0xFFFFu;
}

static void timer_start(SH7021Timer *timer);

static void timer_set_enable(SH7021Timer *timer, bool new_enable) {
    timer->enabled = new_enable;
    if (!timing_event_handle_is_valid(timer->ev) && timer->enabled) {
        timer_start(timer);
    } else if (timing_event_handle_is_valid(timer->ev) && !timer->enabled) {
        timing_cancel_event(&timer->ev);
    }
}

static void timer_start(SH7021Timer *timer) {
    int clock = timer->ctrl.clock & 0x3;
    if (timer->ctrl.clear_mode == 3) timer->ctrl.clear_mode = 0;
    const uint32_t OVERFLOW_TARGET = 0x10000u;
    uint32_t nearest_target = OVERFLOW_TARGET;
    for (int i = 0; i < 2; i++) {
        if (timer->counter < timer->gen_reg[i] && timer->gen_reg[i] < nearest_target) nearest_target = timer->gen_reg[i];
    }
    uint32_t cycles = (nearest_target - timer->counter) << clock;
    if (cycles == 0) cycles = 1;
    TimingUnitCycle sched_cycles = timing_convert_cpu(cycles);
    timer->ev = timing_add_event(ev_func, sched_cycles, (uint64_t)(uint32_t)timer->id, TIMING_CPU_TIMER);
    timer->time_when_started = timing_get_timestamp(TIMING_CPU_TIMER);
    timer->counter_when_started = timer->counter;
}

static void update_timer_irq(SH7021Timer *timer) {
    int subirq = -1;
    for (int i = 0; i < 3; i++) {
        if (timer->intr_enable & timer->intr_flag & (1 << i)) { subirq = i; break; }
    }
    if (subirq >= 0) sh7021_ocpm_intc_assert_irq(timer->irq, subirq);
    else sh7021_ocpm_intc_deassert_irq(timer->irq);
}

static void update_timer_target(SH7021Timer *timer) {
    if (timer->enabled) {
        timer_set_enable(timer, false);
        timer_set_enable(timer, true);
    }
}

static void intr_event(uint64_t param, int cycles_late) {
    (void)cycles_late;
    SH7021Timer *timer = NULL;
    if (param < TIMER_COUNT) {
        timer = &state.timers[(unsigned)param];
    } else {
        /* Older save states stored a host pointer as the timer-event parameter.
         * That is not portable across processes or WebAssembly heap instances.
         * Recover by selecting the first enabled timer with a live event; new
         * save states use the stable integer timer id passed above. */
        for (int i = 0; i < TIMER_COUNT; i++) {
            if (state.timers[i].enabled && timing_event_handle_is_valid(state.timers[i].ev)) { timer = &state.timers[i]; break; }
        }
    }
    if (!timer) return;
    int clock = timer->ctrl.clock & 0x3;
    if (timer->ctrl.clear_mode == 3) timer->ctrl.clear_mode = 0;

    uint32_t start = timer->counter_when_started & 0xFFFFu;
    const uint32_t OVERFLOW_TARGET = 0x10000u;
    uint32_t nearest_target = OVERFLOW_TARGET;
    for (int i = 0; i < 2; i++) {
        if (start < timer->gen_reg[i] && timer->gen_reg[i] < nearest_target) nearest_target = timer->gen_reg[i];
    }

    int64_t elapsed_time = timing_get_timestamp(TIMING_CPU_TIMER) - timer->time_when_started;
    if (elapsed_time < 0) elapsed_time = 0;
    uint32_t elapsed_ticks = (uint32_t)(elapsed_time >> clock);
    uint32_t ticks_to_target = nearest_target - start;
    uint32_t late_ticks = (elapsed_ticks > ticks_to_target) ? (elapsed_ticks - ticks_to_target) : 0u;

    bool clear_counter = false;
    if (nearest_target == timer->gen_reg[0]) {
        timer->intr_flag |= 0x1;
        if (timer->id >= 0 && timer->id <= 3 && (timer->intr_enable & 0x1)) {
            sh7021_ocpm_dmac_pulse_dreq((DREQ)(DREQ_IMIA0 + timer->id));
        }
        if (timer->ctrl.clear_mode == 0x1) clear_counter = true;
    }
    if (nearest_target == timer->gen_reg[1]) {
        timer->intr_flag |= 0x2;
        if (timer->ctrl.clear_mode == 0x2) clear_counter = true;
    }
    if (nearest_target == OVERFLOW_TARGET) {
        timer->intr_flag |= 0x4;
    }

    if (clear_counter) timer->counter = late_ticks & 0xFFFFu;
    else timer->counter = (nearest_target + late_ticks) & 0xFFFFu;

    update_timer_irq(timer);
    timer_start(timer);
}

static TimerDev get_dev_from_addr(uint32_t addr) {
    addr &= 0x3Fu;
    if (addr >= 0x32) { TimerDev d = { &state.timers[4], (int)(addr - 0x32) }; return d; }
    if (addr >= 0x22 && addr < 0x30) { TimerDev d = { &state.timers[3], (int)(addr - 0x22) }; return d; }
    if (addr >= 0x04 && addr < 0x22) {
        addr -= 0x04;
        int id = (int)(addr / 0xA);
        int reg = (int)(addr % 0xA);
        TimerDev d = { &state.timers[id], reg }; return d;
    }
    TimerDev d = { NULL, (int)addr }; return d;
}

void sh7021_ocpm_timer_initialize(void) {
    memset(&state, 0, sizeof(state));
    ev_func = timing_invalid_func_handle();
    for (int i = 0; i < TIMER_COUNT; i++) { state.timers[i].id = i; state.timers[i].ev = timing_invalid_event_handle(); }
    state.timers[0].irq = IRQ_ITU0;
    state.timers[1].irq = IRQ_ITU1;
    state.timers[2].irq = IRQ_ITU2;
    state.timers[3].irq = IRQ_ITU3;
    state.timers[4].irq = IRQ_ITU4;
    ev_func = timing_register_func("Timer::intr_event", intr_event);
}

uint8_t sh7021_ocpm_timer_read8(uint32_t addr) {
    TimerDev dev = get_dev_from_addr(addr);
    SH7021Timer *timer = dev.timer;
    int reg = dev.reg;
    if (timer) {
        timer_update_counter(timer);
        switch (reg) {
        case 0x00: return (uint8_t)(timer->ctrl.clock | (timer->ctrl.edge_mode << 3) | (timer->ctrl.clear_mode << 5));
        case 0x01: return 0;
        case 0x02: return (uint8_t)timer->intr_enable;
        case 0x03: return (uint8_t)(timer->intr_flag | 0x78);
        case 0x04: return (uint8_t)(timer->counter >> 8);
        case 0x05: return (uint8_t)timer->counter;
        case 0x06: return (uint8_t)(timer->gen_reg[0] >> 8);
        case 0x07: return (uint8_t)timer->gen_reg[0];
        case 0x08: return (uint8_t)(timer->gen_reg[1] >> 8);
        case 0x09: return (uint8_t)timer->gen_reg[1];
        default: return 0;
        }
    }
    switch (reg) {
    case 0x00: return (uint8_t)(state.timer_enable | 0x60);
    case 0x01: return (uint8_t)(state.sync_ctrl | 0x60);
    case 0x02: return (uint8_t)state.mode;
    case 0x31: return 0;
    default: return 0;
    }
}

uint16_t sh7021_ocpm_timer_read16(uint32_t addr) {
    uint16_t hi = sh7021_ocpm_timer_read8(addr & ~1u);
    uint16_t lo = sh7021_ocpm_timer_read8((addr & ~1u) + 1u);
    return (uint16_t)((hi << 8) | lo);
}

void sh7021_ocpm_timer_write8(uint32_t addr, uint8_t value) {
    TimerDev dev = get_dev_from_addr(addr);
    SH7021Timer *timer = dev.timer;
    int reg = dev.reg;
    if (timer) {
        switch (reg) {
        case 0x00:
            LOOPY_DEBUG_PRINTF("[Timer] write timer%d ctrl: %02X\n", timer->id, value);
            timer_update_counter(timer);
            timer->ctrl.clock = value & 0x7;
            timer->ctrl.edge_mode = (value >> 3) & 0x3;
            timer->ctrl.clear_mode = (value >> 5) & 0x3;
            update_timer_target(timer);
            break;
        case 0x01:
            LOOPY_DEBUG_PRINTF("[Timer] write timer%d io ctrl: %02X\n", timer->id, value);
            break;
        case 0x02:
            LOOPY_DEBUG_PRINTF("[Timer] write timer%d intr enable: %02X\n", timer->id, value);
            timer->intr_enable = value;
            update_timer_irq(timer);
            break;
        case 0x03:
            LOOPY_DEBUG_PRINTF("[Timer] write timer%d intr flag: %02X\n", timer->id, value);
            timer->intr_flag &= value;
            update_timer_irq(timer);
            break;
        case 0x04:
            timer_update_counter(timer);
            timer->counter = (timer->counter & 0x00FFu) | ((uint32_t)value << 8);
            update_timer_target(timer);
            break;
        case 0x05:
            timer_update_counter(timer);
            timer->counter = (timer->counter & 0xFF00u) | value;
            update_timer_target(timer);
            break;
        case 0x06:
            timer_update_counter(timer);
            timer->gen_reg[0] = (timer->gen_reg[0] & 0x00FFu) | ((uint32_t)value << 8);
            update_timer_target(timer);
            break;
        case 0x07:
            timer_update_counter(timer);
            timer->gen_reg[0] = (timer->gen_reg[0] & 0xFF00u) | value;
            update_timer_target(timer);
            break;
        case 0x08:
            timer_update_counter(timer);
            timer->gen_reg[1] = (timer->gen_reg[1] & 0x00FFu) | ((uint32_t)value << 8);
            update_timer_target(timer);
            break;
        case 0x09:
            timer_update_counter(timer);
            timer->gen_reg[1] = (timer->gen_reg[1] & 0xFF00u) | value;
            update_timer_target(timer);
            break;
        default:
            LOOPY_DEBUG_PRINTF("[Timer] ignored write timer%d reg%02X: %02X\n", timer->id, reg, value);
            break;
        }
        return;
    }
    switch (reg) {
    case 0x00:
        LOOPY_DEBUG_PRINTF("[Timer] write master enable: %02X\n", value);
        state.timer_enable = value & 0x1F;
        for (int i = 0; i < TIMER_COUNT; i++) timer_set_enable(&state.timers[i], ((value >> i) & 0x1) != 0);
        break;
    case 0x01:
        LOOPY_DEBUG_PRINTF("[Timer] write sync ctrl: %02X\n", value);
        state.sync_ctrl = value & 0x1F;
        break;
    case 0x02:
        LOOPY_DEBUG_PRINTF("[Timer] write mode: %02X\n", value);
        state.mode = value & 0x7F;
        break;
    default:
        LOOPY_DEBUG_PRINTF("[Timer] ignored write master reg%02X: %02X\n", reg, value);
        break;
    }
}

void sh7021_ocpm_timer_write16(uint32_t addr, uint16_t value) {
    sh7021_ocpm_timer_write8(addr & ~1u, (uint8_t)(value >> 8));
    sh7021_ocpm_timer_write8((addr & ~1u) + 1u, (uint8_t)value);
}

uint32_t sh7021_ocpm_timer_state_blob_size(void) { return (uint32_t)sizeof(state); }
void sh7021_ocpm_timer_get_state_blob(void *dst, uint32_t size) { if (dst && size == sizeof(state)) memcpy(dst, &state, sizeof(state)); }
void sh7021_ocpm_timer_set_state_blob(const void *src, uint32_t size) { if (src && size == sizeof(state)) memcpy(&state, src, sizeof(state)); }
