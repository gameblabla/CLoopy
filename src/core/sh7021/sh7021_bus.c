#include "core/sh7021/sh7021_bus.h"
#include "common/bswp.h"
#include "core/sh7021/peripherals/sh7021_ocpm.h"
#include "core/sh7021/peripherals/sh7021_bsc.h"
#include "core/sh7021/sh7021_local.h"
#include "core/loopy_io.h"
#include "core/timing.h"
#include "video/video.h"
#include "sound/sound.h"
#include <stdio.h>
#include <string.h>

/* Bus traffic is accounted per memory area, using the same division the Loopy
   hardware notes use (see "Memory Areas"): the area is selected by the top byte
   of the CPU address, and each area is a physically distinct target with its
   own bus width and access time.  Splitting the counters this way means a
   caller can attribute a frame's wait cycles to the chip that actually imposed
   them rather than to a catch-all "internal"/"cart" bucket. */
static SH7021BusProf prof;

SH7021BusRegion sh7021_bus_region_of(uint32_t raw_addr) {
    switch (raw_addr & 0x0F000000u) {
    case 0x00000000u: return SH7021_BUS_REGION_BIOS_ROM;
    case 0x02000000u: return SH7021_BUS_REGION_CART_SRAM;
    case 0x04000000u: return SH7021_BUS_REGION_VDP_LOW_MIRROR;
    case 0x05000000u: return SH7021_BUS_REGION_PERIPHERALS;
    case 0x09000000u: return SH7021_BUS_REGION_WORK_RAM;
    case 0x0C000000u: return SH7021_BUS_REGION_VDP;
    case 0x0E000000u: return SH7021_BUS_REGION_CART_ROM;
    case 0x0F000000u: return SH7021_BUS_REGION_OCRAM;
    default: return SH7021_BUS_REGION_OPEN_BUS;
    }
}

#define prof_classify sh7021_bus_region_of

static void prof_note_access(uint32_t raw_addr, int bytes, int kind) {
    SH7021BusRegionCounters *c = &prof.region[prof_classify(raw_addr)];
    switch (kind) {
    case 0: c->reads++; break;
    case 1: c->writes++; break;
    default: c->fetches++; break;
    }
    c->bytes += bytes;
}

static void prof_note_wait(uint32_t raw_addr, int cycles) {
    if (cycles > 0) prof.region[prof_classify(raw_addr)].wait_cycles += cycles;
}

void sh7021_bus_prof_reset(void) {
    memset(&prof, 0, sizeof(prof));
}

void sh7021_bus_prof_snapshot(SH7021BusProf *out) {
    if (out) *out = prof;
}

const char *sh7021_bus_region_name(SH7021BusRegion region) {
    switch (region) {
    case SH7021_BUS_REGION_BIOS_ROM: return "bios_rom";
    case SH7021_BUS_REGION_CART_SRAM: return "cart_sram";
    case SH7021_BUS_REGION_VDP_LOW_MIRROR: return "vdp_low_mirror";
    case SH7021_BUS_REGION_PERIPHERALS: return "peripherals";
    case SH7021_BUS_REGION_WORK_RAM: return "work_ram";
    case SH7021_BUS_REGION_VDP: return "vdp";
    case SH7021_BUS_REGION_CART_ROM: return "cart_rom";
    case SH7021_BUS_REGION_OCRAM: return "ocram";
    case SH7021_BUS_REGION_OPEN_BUS: return "open_bus";
    default: return "?";
    }
}

static uint32_t translate_addr(uint32_t addr) {
    if ((addr & 0x0F000000u) != 0x0F000000u) return addr & ~0xF8000000u;
    return addr & ~0xF0000000u;
}

static int last_dram_row = -1;
static int64_t next_dram_refresh_cycle = 0;
static int refresh_phase_initialized = 0;
static int64_t refresh_busy_until = 0;

static void sync_refresh_to_now(void) {
    if (!sh7021_bsc_refresh_enabled()) return;
    int period = sh7021_bsc_refresh_period_cycles();
    if (period <= 0) return;
    int refresh_states = 3 + sh7021_bsc_refresh_wait_states();
    int64_t now = timing_get_timestamp(TIMING_CPU_TIMER);
    if (!refresh_phase_initialized) {
        next_dram_refresh_cycle = now + period;
        refresh_phase_initialized = 1;
    }

    /* A CBR refresh always raises RAS, including in RAS-down mode.  Advance
       refresh requests that happened since the previous external-bus access
       before deciding whether a DRAM access can use high-speed page mode. */
    while (next_dram_refresh_cycle < now) {
        int64_t end = next_dram_refresh_cycle + refresh_states;
        if (end > refresh_busy_until) refresh_busy_until = end;
        last_dram_row = -1;
        next_dram_refresh_cycle += period;
    }
}

static int apply_refresh_collision(uint32_t raw_addr, int bus_states) {
    if (!sh7021_bsc_refresh_enabled() || bus_states <= 0) return 0;
    int period = sh7021_bsc_refresh_period_cycles();
    if (period <= 0) return 0;
    int refresh_states = 3 + sh7021_bsc_refresh_wait_states();
    sync_refresh_to_now();
    int64_t now = timing_get_timestamp(TIMING_CPU_TIMER);

    int extra = 0;
    int64_t start = now;
    if (refresh_busy_until > start) {
        extra += (int)(refresh_busy_until - start);
        start = refresh_busy_until;
    }

    int64_t end = start + bus_states;
    if (next_dram_refresh_cycle < end) {
        /* A CBR request that arrives during an external bus transfer waits for
           that transfer, then owns the BSC bus for the complete refresh cycle.
           No cartridge or DRAM access can overlap it. */
        refresh_busy_until = end + refresh_states;
        extra += refresh_states;
        last_dram_row = -1;
        next_dram_refresh_cycle += period;
    }

    if (extra > 0) {
        prof.dram_refresh_stalls++;
        prof_note_wait(raw_addr, extra);
    }
    return extra;
}

/* The interpreter already spends one CPU cycle per executed opcode.  The Loopy
 * memory timing table gives total bus-cycle lengths for target areas, not
 * cycles to add on top of an ideal memory access.  For bus accesses issued via
 * these helpers, charge only the additional cycles over the baseline one-cycle
 * access.  Without this, code fetched from cartridge ROM is charged as
 * 1+3 cycles per 16-bit opcode instead of the documented 3-cycle ROM read, and
 * raycasting/homebrew workloads become much slower than hardware. */
static int extra_cycles_for_transfers(int total_cycles, int transfers) {
    int extra = total_cycles - transfers;
    return extra > 0 ? extra : 0;
}

static void apply_vdp_wait(uint32_t raw_addr, uint32_t translated_addr, int bytes, int write) {
    int cycles = video_bus_wait_cycles(raw_addr, translated_addr, bytes, write != 0);
    if (cycles > 0) {
        sh7021.cycles_left -= cycles;
        prof_note_wait(raw_addr, cycles);
    }
}

static int dram_row_shift(void) {
    uint16_t dcr = sh7021_bsc_dcr();
    switch ((dcr >> 8) & 3u) {
    case 0: return 8;
    case 1: return 9;
    case 2: return 10;
    default: return 8; /* MXC=3 is reserved; use reset comparison range. */
    }
}

static void apply_workram_wait(uint32_t raw_addr, int bytes, int write) {
    if ((raw_addr & 0x0F000000u) != 0x09000000u) return;

    /* Refresh can have ended the previously open row while the CPU was
       executing from cartridge/BIOS space.  Resolve that before BE's row
       comparison for this access. */
    sync_refresh_to_now();

    uint16_t dcr = sh7021_bsc_dcr();
    int page_mode = (dcr & 0x1000u) != 0; /* BE */
    int shift = dram_row_shift();
    uint32_t first = raw_addr & ~1u;
    uint32_t end = raw_addr + (uint32_t)bytes;
    int first_row = (int)((first & 0x0007FFFFu) >> shift);
    int continuation = page_mode && first_row == last_dram_row;

    int total_cycles = 0;
    int transfers = 0;

    /* In short-pitch high-speed page mode a continuing word write has one
       silent/open state before its column cycle.  Longword accesses are split
       into two bus-width transfers below and are accounted by their extended
       MA timing rather than by adding this word-access bubble a second time. */
    if (write && bytes <= 2 && continuation && !(sh7021_bsc_wcr1() & 0x0002u))
        total_cycles += 1;

    for (uint32_t a = first; a < end; a += 2u) {
        int row = (int)((a & 0x0007FFFFu) >> shift);
        int burst = page_mode && row == last_dram_row;
        /* Loopy's boot WCR1 selects short-pitch CPU DRAM reads/writes.  Keep
           the long-pitch form correct as well: a burst column is two states. */
        int long_pitch = write ? ((sh7021_bsc_wcr1() & 0x0002u) != 0)
                               : ((sh7021_bsc_wcr1() & 0x0200u) != 0);
        total_cycles += burst ? (long_pitch ? 2 : 1) : 3;
        transfers++;
        last_dram_row = row;
    }

    int cycles = extra_cycles_for_transfers(total_cycles, transfers);
    /* SH-1 instruction fetch (IF) and data-memory access (MA) share the
       external bus.  For a cart-resident word store into DRAM, the MA slot
       cannot overlap the next cartridge IF slot, so the pipeline splits by one
       state beyond the wait-state charge above.  Load-use bubbles are modeled
       separately in sh7021.c. */
    if (write && bytes == 2 &&
        (sh7021.current_opcode_pc & 0x0F000000u) == 0x0E000000u)
        cycles += 1;
    cycles += apply_refresh_collision(raw_addr, total_cycles);
    sh7021.cycles_left -= cycles;
    prof_note_wait(raw_addr, cycles);
}



static void apply_internal_peripheral_wait(uint32_t raw_addr, int bytes) {
    if ((raw_addr & 0x0F000000u) != 0x05000000u) return;
    /* SH7021 internal peripheral registers are a separate internal area with
     * 3-cycle 8/16-bit accesses in the Loopy BIOS bus setup.  A 32-bit CPU
     * access is treated as two 16-bit register transfers. */
    int transfers = (bytes == 4) ? 2 : 1;
    int total_cycles = transfers * 3;
    int cycles = extra_cycles_for_transfers(total_cycles, transfers);
    sh7021.cycles_left -= cycles;
    prof_note_wait(raw_addr, cycles);
}

static void apply_vdp_mmio_wait(uint32_t raw_addr, uint32_t translated_addr, int bytes, int write) {
    (void)write;
    uint32_t area = raw_addr & 0x0F000000u;
    if (area != VIDEO_VDP_AREA_NORMAL && area != VIDEO_VDP_AREA_LOW_MIRROR) return;
    if (!(translated_addr >= LOOPY_IO_BASE_ADDR && translated_addr < LOOPY_IO_END_ADDR)) return;
    int transfers = (bytes == 4) ? 2 : 1;
    int total_cycles = transfers * 3;
    int cycles = extra_cycles_for_transfers(total_cycles, transfers);
    if (area == VIDEO_VDP_AREA_LOW_MIRROR) cycles += bytes;
    sh7021.cycles_left -= cycles;
    prof_note_wait(raw_addr, cycles);
}

static int transfer_count_for_width(int bytes, int bus_width_bytes) {
    int transfers = (bytes + bus_width_bytes - 1) / bus_width_bytes;
    return transfers < 1 ? 1 : transfers;
}

static void apply_cart_wait(uint32_t raw_addr, int bytes, int write) {
    uint32_t area = raw_addr & 0x0F000000u;
    int total_cycles = 0;
    int transfers = 0;
    if (area == 0x0E000000u) {
        if (write) return;
        /* Cartridge ROM is a 16-bit external area.  Charge the documented
         * 3-cycle read as two extra cycles over the interpreter's baseline
         * one-cycle fetch/access. */
        transfers = transfer_count_for_width(bytes, 2);
        total_cycles = transfers * 3;
    } else if (area == 0x02000000u) {
        /* Cartridge SRAM is an 8-bit external area with 3-cycle accesses. */
        transfers = transfer_count_for_width(bytes, 1);
        total_cycles = transfers * 3;
    }
    int cycles = extra_cycles_for_transfers(total_cycles, transfers);
    /* CBR refresh is a BSC bus cycle.  It cannot run in parallel with a
       cartridge access on the same external bus; a pending refresh therefore
       delays both ROM reads/fetches and SRAM transfers until the refresh cycle
       has completed. */
    if (total_cycles > 0) cycles += apply_refresh_collision(raw_addr, total_cycles);
    if (cycles > 0) {
        sh7021.cycles_left -= cycles;
        prof_note_wait(raw_addr, cycles);
    }
}

/* Reads the idle-loop detector is allowed to consider repeatable.  Page-table
   memory (RAM/ROM/SRAM) qualifies implicitly - it changes only when something
   writes it, and a write is tracked separately.  Everything else is unsafe,
   most importantly the on-chip peripherals, where the free-running timer
   counters derive from the live timestamp and so do change mid-slice, and where
   a read can clear a status flag.

   Of the VDP control block only two words qualify:
     0x000 display mode - changes only when written.
     0x004 VCOUNT       - advanced only by the scanline event.
   HCOUNT at 0x002 is deliberately excluded.  video_current_hcount() derives it
   from timing_get_timestamp(), which inside a slice reads back as
   timestamp + slice_length - cycles_left, so it advances as the slice is
   consumed rather than only at events - it is a free-running counter in every
   sense that matters here, and treating it as repeatable would let a loop
   polling it be skipped past the values it was waiting to see.

   Takes the translated address and the access width, so a byte read of a
   neighbouring lane or a 32-bit read spanning HCOUNT is rejected too. */
static inline int idle_read_is_repeatable(uint32_t translated_addr, int bytes) {
    if (translated_addr < VIDEO_CTRL_REG_START || translated_addr >= VIDEO_CTRL_REG_END) return 0;
    uint32_t off = translated_addr & 0xFFFu;
    uint32_t end = off + (uint32_t)bytes;
    if (off >= 0x004u && end <= 0x006u) return 1; /* VCOUNT word */
    if (end <= 0x002u) return 1;                  /* display mode word */
    return 0;
}

static inline void idle_note_read(uint32_t translated_addr, int bytes) {
    if (!idle_read_is_repeatable(translated_addr, bytes)) sh7021_idle_unsafe_read = 1;
}

uint8_t unmapped_read8(uint32_t addr) { LOOPY_DEBUG_PRINTF("[SH7021] unmapped read8 %08X\n", addr); return 0; }
uint16_t unmapped_read16(uint32_t addr) { LOOPY_DEBUG_PRINTF("[SH7021] unmapped read16 %08X\n", addr); return 0; }
uint32_t unmapped_read32(uint32_t addr) { LOOPY_DEBUG_PRINTF("[SH7021] unmapped read32 %08X\n", addr); return 0; }
void unmapped_write8(uint32_t addr, uint8_t value) { LOOPY_DEBUG_PRINTF("[SH7021] unmapped write8 %08X: %02X\n", addr, value); }
void unmapped_write16(uint32_t addr, uint16_t value) { LOOPY_DEBUG_PRINTF("[SH7021] unmapped write16 %08X: %04X\n", addr, value); }
void unmapped_write32(uint32_t addr, uint32_t value) { LOOPY_DEBUG_PRINTF("[SH7021] unmapped write32 %08X: %08X\n", addr, value); }

#define MMIO_ACCESS(access, ...) \
    if (addr >= SH7021_OCPM_ORAM_BASE_ADDR && addr < SH7021_OCPM_ORAM_END_ADDR) return sh7021_ocpm_oram_##access(__VA_ARGS__); \
    if (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) return video_palette_##access(__VA_ARGS__); \
    if (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) return video_oam_##access(__VA_ARGS__); \
    if (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) return video_capture_##access(__VA_ARGS__); \
    if (addr >= VIDEO_CTRL_REG_START && addr < VIDEO_CTRL_REG_END) return video_ctrl_##access(__VA_ARGS__); \
    if (addr >= VIDEO_BITMAP_REG_START && addr < VIDEO_BITMAP_REG_END) return video_bitmap_reg_##access(__VA_ARGS__); \
    if (addr >= VIDEO_BGOBJ_REG_START && addr < VIDEO_BGOBJ_REG_END) return video_bgobj_##access(__VA_ARGS__); \
    if (addr >= VIDEO_DISPLAY_REG_START && addr < VIDEO_DISPLAY_REG_END) return video_display_##access(__VA_ARGS__); \
    if (addr >= VIDEO_IRQ_REG_START && addr < VIDEO_IRQ_REG_END) return video_irq_##access(__VA_ARGS__); \
    if (addr >= LOOPY_IO_BASE_ADDR && addr < LOOPY_IO_END_ADDR) return loopy_io_reg_##access(__VA_ARGS__); \
    if (addr >= VIDEO_DMA_CTRL_START && addr < VIDEO_DMA_CTRL_END) return video_dma_ctrl_##access(__VA_ARGS__); \
    if (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END) return video_dma_##access(__VA_ARGS__); \
    if (addr >= SH7021_OCPM_IO_BASE_ADDR && addr < SH7021_OCPM_IO_END_ADDR) return sh7021_ocpm_io_##access(__VA_ARGS__); \
    if (addr >= SOUND_CTRL_START && addr < SOUND_CTRL_END) return sound_ctrl_##access(__VA_ARGS__); \
    if (addr >= SOUND_EXP_DATA_START && addr < SOUND_EXP_DATA_END) return sound_exp_data_##access(__VA_ARGS__); \
    return unmapped_##access(__VA_ARGS__)


static uint32_t last_opcode_fetch_addr = 0xFFFFFFFFu;
static int last_opcode_fetch_valid = 0;

void sh7021_bus_timing_reset(void) {
    last_dram_row = -1;
    next_dram_refresh_cycle = 0;
    refresh_phase_initialized = 0;
    refresh_busy_until = 0;
    last_opcode_fetch_valid = 0;
    last_opcode_fetch_addr = 0xFFFFFFFFu;
}

void sh7021_bus_fetch_reset(void) {
    last_opcode_fetch_valid = 0;
    last_opcode_fetch_addr = 0xFFFFFFFFu;
}

uint16_t sh7021_bus_fetch16(uint32_t addr) {
    uint32_t raw_addr = addr;
    uint32_t translated = translate_addr(addr);
    /* Cartridge ROM is a 16-bit external bus target.  Hardware timing tests
       show that sequential opcode fetches pay the cartridge read time too; the
       SH-1 prefetch pipeline does not hide these external wait states.  Apply
       the cart access timing to every opcode fetch, just as for data reads. */
    uint8_t *mem = sh7021.pagetable[translated >> 12];
    if (mem) {
        prof_note_access(raw_addr, 2, 2);
        apply_workram_wait(raw_addr, 2, 0);
        apply_cart_wait(raw_addr, 2, 0);
        uint16_t value; memcpy(&value, mem + (translated & 0xFFFu), 2);
        last_opcode_fetch_addr = raw_addr;
        last_opcode_fetch_valid = 1;
        return common_bswp16(value);
    }
    last_opcode_fetch_addr = raw_addr;
    last_opcode_fetch_valid = 1;
    return sh7021_bus_read16(raw_addr);
}

uint8_t sh7021_bus_read8(uint32_t addr) {
    uint32_t raw_addr = addr;
    prof_note_access(raw_addr, 1, 0);
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { idle_note_read(addr, 1); apply_vdp_wait(raw_addr, addr, 1, 0); return video_bus_read8(raw_addr); }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 1, 0); apply_cart_wait(raw_addr, 1, 0); return mem[addr & 0xFFFu]; }
    idle_note_read(addr, 1);
    apply_vdp_mmio_wait(raw_addr, addr, 1, 0);
    apply_internal_peripheral_wait(raw_addr, 1);
    MMIO_ACCESS(read8, addr);
}
uint16_t sh7021_bus_read16(uint32_t addr) {
    uint32_t raw_addr = addr;
    prof_note_access(raw_addr, 2, 0);
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { idle_note_read(addr, 2); apply_vdp_wait(raw_addr, addr, 2, 0); return video_bus_read16(raw_addr); }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 2, 0); apply_cart_wait(raw_addr, 2, 0); uint16_t value; memcpy(&value, mem + (addr & 0xFFFu), 2); return common_bswp16(value); }
    idle_note_read(addr, 2);
    apply_vdp_mmio_wait(raw_addr, addr, 2, 0);
    apply_internal_peripheral_wait(raw_addr, 2);
    MMIO_ACCESS(read16, addr);
}
uint32_t sh7021_bus_read32(uint32_t addr) {
    uint32_t raw_addr = addr;
    prof_note_access(raw_addr, 4, 0);
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { idle_note_read(addr, 4); apply_vdp_wait(raw_addr, addr, 4, 0); return video_bus_read32(raw_addr); }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 4, 0); apply_cart_wait(raw_addr, 4, 0); uint32_t value; memcpy(&value, mem + (addr & 0xFFFu), 4); return common_bswp32(value); }
    idle_note_read(addr, 4);
    apply_vdp_mmio_wait(raw_addr, addr, 4, 0);
    apply_internal_peripheral_wait(raw_addr, 4);
    MMIO_ACCESS(read32, addr);
}
void sh7021_bus_write8(uint32_t addr, uint8_t value) {
    sh7021_idle_wrote_mem = 1;
    uint32_t raw_addr = addr;
    prof_note_access(raw_addr, 1, 1);
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { apply_vdp_wait(raw_addr, addr, 1, 1); video_bus_write8(raw_addr, value); return; }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 1, 1); apply_cart_wait(raw_addr, 1, 1); mem[addr & 0xFFFu] = value; return; }
    apply_vdp_mmio_wait(raw_addr, addr, 1, 1);
    apply_internal_peripheral_wait(raw_addr, 1);
    MMIO_ACCESS(write8, addr, value);
}
void sh7021_bus_write16(uint32_t addr, uint16_t value) {
    sh7021_idle_wrote_mem = 1;
    uint32_t raw_addr = addr;
    prof_note_access(raw_addr, 2, 1);
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { apply_vdp_wait(raw_addr, addr, 2, 1); video_bus_write16(raw_addr, value); return; }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 2, 1); apply_cart_wait(raw_addr, 2, 1); value = common_bswp16(value); memcpy(mem + (addr & 0xFFFu), &value, 2); return; }
    apply_vdp_mmio_wait(raw_addr, addr, 2, 1);
    apply_internal_peripheral_wait(raw_addr, 2);
    MMIO_ACCESS(write16, addr, value);
}
void sh7021_bus_write32(uint32_t addr, uint32_t value) {
    sh7021_idle_wrote_mem = 1;
    uint32_t raw_addr = addr;
    prof_note_access(raw_addr, 4, 1);
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { apply_vdp_wait(raw_addr, addr, 4, 1); video_bus_write32(raw_addr, value); return; }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 4, 1); apply_cart_wait(raw_addr, 4, 1); value = common_bswp32(value); memcpy(mem + (addr & 0xFFFu), &value, 4); return; }
    apply_vdp_mmio_wait(raw_addr, addr, 4, 1);
    apply_internal_peripheral_wait(raw_addr, 4);
    MMIO_ACCESS(write32, addr, value);
}


/* Inspection reads.  Every path here is a plain array load: no bus latch, no
   wait-state charge, no idle-detector poisoning, no status flag cleared.  That
   is the whole point - a debugger or MCP client can sample memory between
   instructions without altering what the machine does next.  MMIO registers are
   therefore not peekable at all, and report failure rather than a guess.

   Resolved a byte at a time so a read spanning a 4KB page boundary, or running
   off the end of a mapped region, is handled rather than walking off the page. */
static int sh7021_bus_peek_byte(uint32_t addr, uint8_t *out) {
    uint32_t translated = translate_addr(addr);
    if (video_bus_is_vdp_addr(addr, translated)) {
        uint32_t value;
        if (!video_debug_peek(translated, 1, &value)) return 0;
        *out = (uint8_t)value;
        return 1;
    }
    if (translated >= SH7021_OCPM_ORAM_BASE_ADDR && translated < SH7021_OCPM_ORAM_END_ADDR) {
        *out = sh7021_ocpm_oram_read8(translated);
        return 1;
    }
    if (!sh7021.pagetable) return 0;
    uint8_t *mem = sh7021.pagetable[translated >> 12];
    if (!mem) return 0;
    *out = mem[translated & 0xFFFu];
    return 1;
}

int sh7021_bus_peek(uint32_t addr, int bytes, uint32_t *out_value) {
    if (!out_value || bytes < 1 || bytes > 4) return 0;
    uint32_t value = 0;
    for (int i = 0; i < bytes; i++) {
        uint8_t byte;
        if (!sh7021_bus_peek_byte(addr + (uint32_t)i, &byte)) return 0;
        value = (value << 8) | byte;
    }
    *out_value = value;
    return 1;
}

static void note_dma_access(uint32_t addr, int bytes, int write, int single_address_mode) {
    prof.dma_accesses++;
    if (single_address_mode) {
        prof.dma_single_accesses++;
        prof.dma_model_cycles += sh7021_bsc_dma_single_cycle_states(addr, bytes, write);
    }
}

uint8_t sh7021_bus_dma_read8(uint32_t addr, int single_address_mode) {
    note_dma_access(addr, 1, 0, single_address_mode);
    return sh7021_bus_read8(addr);
}

uint16_t sh7021_bus_dma_read16(uint32_t addr, int single_address_mode) {
    note_dma_access(addr, 2, 0, single_address_mode);
    return sh7021_bus_read16(addr);
}

void sh7021_bus_dma_write8(uint32_t addr, uint8_t value, int single_address_mode) {
    note_dma_access(addr, 1, 1, single_address_mode);
    sh7021_bus_write8(addr, value);
}

void sh7021_bus_dma_write16(uint32_t addr, uint16_t value, int single_address_mode) {
    note_dma_access(addr, 2, 1, single_address_mode);
    sh7021_bus_write16(addr, value);
}
