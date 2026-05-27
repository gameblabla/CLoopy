#include "core/sh7021/peripherals/sh7021_bsc.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/timing.h"
#include "common/bswp.h"
#include <stdint.h>
#include <string.h>

/* Keep the standalone bsc unit test linkable without the full timing/INTC core.
 * The normal emulator link provides strong definitions that override these. */
#if defined(__GNUC__)
__attribute__((weak)) int64_t timing_get_timestamp(int id) { (void)id; return 0; }
__attribute__((weak)) void sh7021_ocpm_intc_assert_irq(IRQ irq, int vector_offs) { (void)irq; (void)vector_offs; }
__attribute__((weak)) void sh7021_ocpm_intc_deassert_irq(IRQ irq) { (void)irq; }
#endif

typedef struct BSCState {
    uint16_t bcr;
    uint16_t wcr1;
    uint16_t wcr2;
    uint16_t wcr3;
    uint16_t dcr;
    uint16_t pcr;
    uint8_t rcr;
    uint8_t rtcsr;
    uint8_t rtcsr_read_cmf;
    uint8_t rtcnt;
    uint8_t rtcor;
    uint8_t pcr_read_pef;
    int64_t refresh_last_cpu_ts;
} BSCState;

static BSCState state;

#define BCR_VALID_MASK      0xf800u
#define WCR1_VALID_MASK     0xff02u
#define WCR1_RESERVED_ONES  0x00fdu
#define WCR3_VALID_MASK     0xf800u
#define DCR_VALID_MASK      0xff00u
#define PCR_VALID_MASK      0xf800u
#define RCR_VALID_MASK      0xf0u
#define RTCSR_VALID_MASK    0xf8u

static uint16_t normalize_wcr1(uint16_t v) { return (uint16_t)((v & WCR1_VALID_MASK) | WCR1_RESERVED_ONES); }
static uint16_t normalize_wcr3(uint16_t v) { return (uint16_t)(v & WCR3_VALID_MASK); }
static uint16_t normalize_bcr(uint16_t v) { return (uint16_t)(v & BCR_VALID_MASK); }
static uint16_t normalize_dcr(uint16_t v) { return (uint16_t)(v & DCR_VALID_MASK); }
static uint16_t normalize_pcr_write(uint16_t old, uint16_t v) {
    uint16_t writable = (uint16_t)(v & PCR_VALID_MASK);
    uint16_t pef = (uint16_t)(old & 0x8000u);
    if (state.pcr_read_pef && !(v & 0x8000u)) {
        pef = 0;
        state.pcr_read_pef = 0;
    } else if (v & 0x8000u) {
        pef = 0x8000u;
    }
    return (uint16_t)((writable & 0x7800u) | pef);
}

static uint8_t rtcsr_div_code(void) { return (uint8_t)((state.rtcsr >> 3) & 7u); }
static uint16_t rtcsr_div_cycles(void) {
    static const uint16_t divs[8] = { 0, 2, 8, 32, 128, 512, 2048, 4096 };
    return divs[rtcsr_div_code()];
}

static void update_ref_irq(void) {
    if ((state.rtcsr & 0xc0u) == 0xc0u) sh7021_ocpm_intc_assert_irq(IRQ_REF, 0);
    else sh7021_ocpm_intc_deassert_irq(IRQ_REF);
}

static void bsc_timer_update(void) {
    int64_t now = timing_get_timestamp(TIMING_CPU_TIMER);
    if (state.refresh_last_cpu_ts < 0) {
        state.refresh_last_cpu_ts = now;
        return;
    }
    uint16_t div = rtcsr_div_cycles();
    if (!div) {
        state.refresh_last_cpu_ts = now;
        return;
    }
    int64_t delta = now - state.refresh_last_cpu_ts;
    if (delta < div) return;
    int64_t ticks = delta / div;
    state.refresh_last_cpu_ts += ticks * div;

    /* RTCNT is an 8-bit up-counter.  On compare-match with RTCOR, CMF is set
     * and the counter clears to zero.  A simple loop is bounded by the timing
     * system's short CPU slices in normal emulation, and keeps edge cases easy
     * to audit. */
    while (ticks-- > 0) {
        state.rtcnt = (uint8_t)(state.rtcnt + 1u);
        if (state.rtcnt == state.rtcor) {
            state.rtcsr |= 0x80u;
            state.rtcnt = 0;
        }
    }
    update_ref_irq();
}

void sh7021_ocpm_bsc_initialize(void) {
    memset(&state, 0, sizeof(state));
    state.wcr1 = 0xffffu;
    state.wcr2 = 0xffffu;
    state.wcr3 = 0xf800u;
    state.rtcor = 0xffu;
    state.refresh_last_cpu_ts = -1;
}

static uint16_t read16_reg(uint32_t addr) {
    bsc_timer_update();
    switch (addr & 0x1ffu) {
    case 0x1a0: return normalize_bcr(state.bcr);
    case 0x1a2: return normalize_wcr1(state.wcr1);
    case 0x1a4: return state.wcr2;
    case 0x1a6: return normalize_wcr3(state.wcr3);
    case 0x1a8: return normalize_dcr(state.dcr);
    case 0x1aa:
        if (state.pcr & 0x8000u) state.pcr_read_pef = 1;
        return (uint16_t)(state.pcr & PCR_VALID_MASK);
    case 0x1ac: return state.rcr;
    case 0x1ae:
        if (state.rtcsr & 0x80u) state.rtcsr_read_cmf = 1;
        return state.rtcsr;
    case 0x1b0: return state.rtcnt;
    case 0x1b2: return state.rtcor;
    default: return 0;
    }
}

static int is_protected_refresh_reg(uint32_t addr) {
    uint32_t r = addr & 0x1ffu;
    return r >= 0x1acu && r <= 0x1b3u;
}

static void write16_reg(uint32_t addr, uint16_t value) {
    bsc_timer_update();
    switch (addr & 0x1ffu) {
    case 0x1a0: state.bcr = normalize_bcr(value); break;
    case 0x1a2: state.wcr1 = normalize_wcr1(value); break;
    case 0x1a4: state.wcr2 = value; break;
    case 0x1a6: state.wcr3 = normalize_wcr3(value); break;
    case 0x1a8: state.dcr = normalize_dcr(value); break;
    case 0x1aa: state.pcr = normalize_pcr_write(state.pcr, value); break;
    case 0x1ac:
        if ((value >> 8) == 0x5au) state.rcr = (uint8_t)(value & RCR_VALID_MASK);
        break;
    case 0x1ae:
        if ((value >> 8) == 0xa5u) {
            uint8_t old_cmf = (uint8_t)(state.rtcsr & 0x80u);
            uint8_t new_cmf = old_cmf;
            if (state.rtcsr_read_cmf && !(value & 0x80u)) {
                new_cmf = 0;
                state.rtcsr_read_cmf = 0;
            } else if (value & 0x80u) {
                new_cmf = 0x80u;
            }
            state.rtcsr = (uint8_t)(new_cmf | (value & 0x78u));
            state.refresh_last_cpu_ts = timing_get_timestamp(TIMING_CPU_TIMER);
            update_ref_irq();
        }
        break;
    case 0x1b0:
        if ((value >> 8) == 0x69u) {
            state.rtcnt = (uint8_t)value;
            state.refresh_last_cpu_ts = timing_get_timestamp(TIMING_CPU_TIMER);
        }
        break;
    case 0x1b2:
        if ((value >> 8) == 0x96u) state.rtcor = (uint8_t)value;
        break;
    default: break;
    }
}

uint8_t sh7021_ocpm_bsc_read8(uint32_t addr) {
    uint16_t v = read16_reg(addr & ~1u);
    return (addr & 1u) ? (uint8_t)v : (uint8_t)(v >> 8);
}

uint16_t sh7021_ocpm_bsc_read16(uint32_t addr) {
    return read16_reg(addr & ~1u);
}

uint32_t sh7021_ocpm_bsc_read32(uint32_t addr) {
    uint32_t hi = sh7021_ocpm_bsc_read16(addr);
    uint32_t lo = sh7021_ocpm_bsc_read16(addr + 2u);
    return (hi << 16) | lo;
}

void sh7021_ocpm_bsc_write8(uint32_t addr, uint8_t value) {
    if (is_protected_refresh_reg(addr)) return; /* RCR/RTCSR/RTCNT/RTCOR require word writes with keys. */
    uint16_t cur = read16_reg(addr & ~1u);
    if (addr & 1u) cur = (uint16_t)((cur & 0xff00u) | value);
    else cur = (uint16_t)((cur & 0x00ffu) | ((uint16_t)value << 8));
    write16_reg(addr & ~1u, cur);
}

void sh7021_ocpm_bsc_write16(uint32_t addr, uint16_t value) {
    write16_reg(addr & ~1u, value);
}

void sh7021_ocpm_bsc_write32(uint32_t addr, uint32_t value) {
    /* The refresh registers are documented as key-protected word-write-only;
     * for compatibility with CPU bus splitting, still split long writes into
     * two aligned word transfers rather than trying to interpret byte writes. */
    sh7021_ocpm_bsc_write16(addr, (uint16_t)(value >> 16));
    sh7021_ocpm_bsc_write16(addr + 2u, (uint16_t)value);
}

uint32_t sh7021_ocpm_bsc_state_blob_size(void) { return (uint32_t)sizeof(state); }
void sh7021_ocpm_bsc_get_state_blob(void *dst, uint32_t size) { if (dst && size == sizeof(state)) memcpy(dst, &state, sizeof(state)); }
void sh7021_ocpm_bsc_set_state_blob(const void *src, uint32_t size) { if (src && size == sizeof(state)) memcpy(&state, src, sizeof(state)); update_ref_irq(); }

uint16_t sh7021_bsc_bcr(void) { bsc_timer_update(); return normalize_bcr(state.bcr); }
uint16_t sh7021_bsc_wcr1(void) { bsc_timer_update(); return normalize_wcr1(state.wcr1); }
uint16_t sh7021_bsc_wcr2(void) { bsc_timer_update(); return state.wcr2; }
uint16_t sh7021_bsc_wcr3(void) { bsc_timer_update(); return normalize_wcr3(state.wcr3); }
uint16_t sh7021_bsc_dcr(void) { bsc_timer_update(); return normalize_dcr(state.dcr); }
uint8_t sh7021_bsc_refresh_enabled(void) { bsc_timer_update(); return (uint8_t)((state.rcr & 0x80u) != 0); }
uint8_t sh7021_bsc_refresh_constant(void) { bsc_timer_update(); return state.rtcor; }

uint8_t sh7021_bsc_refresh_wait_states(void) {
    /* RLW encodes 1..4 states.  Per the manual this applies only for CBR
     * refresh with area-1 read long-pitch (RW1=1). */
    bsc_timer_update();
    if (!(state.rcr & 0x80u) || (state.rcr & 0x40u)) return 0;
    if (!(normalize_wcr1(state.wcr1) & 0x0200u)) return 0;
    return (uint8_t)(((state.rcr >> 4) & 0x3u) + 1u);
}

uint16_t sh7021_bsc_refresh_period_cycles(void) {
    bsc_timer_update();
    uint16_t div = rtcsr_div_cycles();
    if (!div) return 0;
    uint32_t period = (uint32_t)(state.rtcor + 1u) * div;
    if (period > 0xffffu) period = 0xffffu;
    return (uint16_t)period;
}

int sh7021_bsc_area_wait_sample_read(int area) {
    uint16_t w = normalize_wcr1(state.wcr1);
    if (area < 0 || area > 7) return 0;
    return (w >> (8 + area)) & 1;
}

int sh7021_bsc_area_wait_sample_dma_read(int area) {
    if (area < 0 || area > 7) return 0;
    return (state.wcr2 >> (8 + area)) & 1;
}

int sh7021_bsc_area_wait_sample_dma_write(int area) {
    if (area < 0 || area > 7) return 0;
    return (state.wcr2 >> area) & 1;
}


int sh7021_bsc_area_from_addr(uint32_t addr) {
    return (int)((addr >> 24) & 7u);
}

static int bsc_bus_width_bytes_for_addr(uint32_t addr) {
    uint32_t area = addr & 0x0f000000u;
    switch (area) {
    case 0x00000000u: return 4; /* SH7021 internal BIOS ROM in Loopy boot mode. */
    case 0x02000000u: return 1; /* Cart SRAM, CS2. */
    case 0x04000000u: return 1; /* VDP low mirror, discouraged 8-bit path. */
    case 0x05000000u: return 1; /* Internal peripherals; individual regs are 8/16-bit. */
    case 0x09000000u: return 2; /* Work DRAM, A27=1 area 1. */
    case 0x0c000000u: return 2; /* VDP normal area, CS4. */
    case 0x0e000000u: return 2; /* Cart ROM, A27=1 area 6. */
    case 0x0f000000u: return 4; /* On-chip RAM, A27=1 area 7. */
    default: return 2;
    }
}

static int dma_transfers_for_access(uint32_t addr, int bytes) {
    int w = bsc_bus_width_bytes_for_addr(addr);
    if (w <= 0) w = 1;
    int n = (bytes + w - 1) / w;
    return n > 0 ? n : 1;
}

uint16_t sh7021_bsc_dma_single_cycle_states(uint32_t addr, int bytes, int write) {
    bsc_timer_update();
    int area = sh7021_bsc_area_from_addr(addr);
    if (area < 0 || area > 7) return 0;
    int sampled = write ? sh7021_bsc_area_wait_sample_dma_write(area) : sh7021_bsc_area_wait_sample_dma_read(area);
    int transfers = dma_transfers_for_access(addr, bytes);
    int states_per_transfer;

    if (area == 6 && ((addr & 0x08000000u) == 0) && (normalize_bcr(state.bcr) & 0x0800u)) {
        /* Area 6 address/data multiplexed I/O is documented as four states
         * plus WAIT regardless of DRW6/DWW6.  Loopy retail carts use A27=1
         * external ROM, but expose the rule for diagnostics and future users. */
        states_per_transfer = 4;
    } else if (area == 0 || area == 2 || area == 6) {
        states_per_transfer = 1 + ((area == 6) ? sh7021_bsc_long_wait_area6() : sh7021_bsc_long_wait_area02());
        /* When the WCR2 bit is 1, WAIT is sampled after the long-wait portion.
         * The actual external WAIT level is device-specific and handled by
         * Loopy's VDP/cart timing code, so do not invent extra cycles here. */
        (void)sampled;
    } else if (area == 1 && (normalize_bcr(state.bcr) & 0x8000u)) {
        /* DRAM column cycle short/long pitch for single-mode DMA.  This is a
         * BSC-level estimate only; row locality/refresh are still modeled by
         * the Loopy bus path that performs the actual memory operation. */
        states_per_transfer = sampled ? 2 : 1;
    } else {
        states_per_transfer = sampled ? 2 : 1;
    }

    int total = transfers * states_per_transfer;
    return (uint16_t)(total > 0xffff ? 0xffff : total);
}

uint8_t sh7021_bsc_long_wait_area02(void) { return (uint8_t)(((normalize_wcr3(state.wcr3) >> 13) & 3u) + 1u); }
uint8_t sh7021_bsc_long_wait_area6(void) { return (uint8_t)(((normalize_wcr3(state.wcr3) >> 11) & 3u) + 1u); }
