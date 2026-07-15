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

static long long prof_vdp_wait_cycles = 0;
static long long prof_vdp_accesses = 0;
static long long prof_vdp_bytes = 0;
static long long prof_workram_wait_cycles = 0;
static long long prof_workram_accesses = 0;
static long long prof_cart_wait_cycles = 0;
static long long prof_cart_accesses = 0;
static long long prof_internal_wait_cycles = 0;
static long long prof_internal_accesses = 0;
static long long prof_dram_refresh_stalls = 0;
static long long prof_dma_model_cycles = 0;
static long long prof_dma_accesses = 0;
static long long prof_dma_single_accesses = 0;

void sh7021_bus_prof_reset(void) {
    prof_vdp_wait_cycles = 0;
    prof_vdp_accesses = 0;
    prof_vdp_bytes = 0;
    prof_workram_wait_cycles = 0;
    prof_workram_accesses = 0;
    prof_cart_wait_cycles = 0;
    prof_cart_accesses = 0;
    prof_internal_wait_cycles = 0;
    prof_internal_accesses = 0;
    prof_dram_refresh_stalls = 0;
    prof_dma_model_cycles = 0;
    prof_dma_accesses = 0;
    prof_dma_single_accesses = 0;
}

void sh7021_bus_prof_get(long long *vdp_cycles, long long *vdp_accesses, long long *vdp_bytes, long long *ram_cycles, long long *ram_accesses) {
    if (vdp_cycles) *vdp_cycles = prof_vdp_wait_cycles;
    if (vdp_accesses) *vdp_accesses = prof_vdp_accesses;
    if (vdp_bytes) *vdp_bytes = prof_vdp_bytes;
    if (ram_cycles) *ram_cycles = prof_workram_wait_cycles;
    if (ram_accesses) *ram_accesses = prof_workram_accesses;
}

void sh7021_bus_prof_get_cart(long long *cart_cycles, long long *cart_accesses) {
    if (cart_cycles) *cart_cycles = prof_cart_wait_cycles;
    if (cart_accesses) *cart_accesses = prof_cart_accesses;
}

void sh7021_bus_prof_get_internal(long long *internal_cycles, long long *internal_accesses, long long *dram_refresh_stalls) {
    if (internal_cycles) *internal_cycles = prof_internal_wait_cycles;
    if (internal_accesses) *internal_accesses = prof_internal_accesses;
    if (dram_refresh_stalls) *dram_refresh_stalls = prof_dram_refresh_stalls;
}

void sh7021_bus_prof_get_dma(long long *dma_model_cycles, long long *dma_accesses, long long *dma_single_accesses) {
    if (dma_model_cycles) *dma_model_cycles = prof_dma_model_cycles;
    if (dma_accesses) *dma_accesses = prof_dma_accesses;
    if (dma_single_accesses) *dma_single_accesses = prof_dma_single_accesses;
}

static uint32_t translate_addr(uint32_t addr) {
    if ((addr & 0x0F000000u) != 0x0F000000u) return addr & ~0xF8000000u;
    return addr & ~0xF0000000u;
}

static int last_dram_row = -1;
static int64_t next_dram_refresh_cycle = 0;

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
        prof_vdp_wait_cycles += cycles;
    }
    prof_vdp_accesses++;
    prof_vdp_bytes += bytes;
}

static void apply_workram_wait(uint32_t raw_addr, int bytes) {
    if ((raw_addr & 0x0F000000u) != 0x09000000u) return;
    int total_cycles = 0;
    int transfers = 0;
    uint32_t first = raw_addr & ~1u;
    uint32_t end = raw_addr + (uint32_t)bytes;
    for (uint32_t a = first; a < end; a += 2u) {
        int row = (int)((a & 0x0007FFFFu) >> 10); /* HM514260 row: 512 words / 1024 bytes. */
        total_cycles += (row == last_dram_row) ? 1 : 3;
        transfers++;
        last_dram_row = row;
    }

    int cycles = extra_cycles_for_transfers(total_cycles, transfers);

    /* Keep refresh accounting visible, but do not currently stall on it.  The
     * notes confirm a ~244-cycle refresh cadence, but explicitly leave the
     * collision length unknown.  Charging a guessed stall made real workloads
     * too slow, so this remains a diagnostic counter until measured. */
    int64_t now = timing_get_timestamp(TIMING_CPU_TIMER);
    if (next_dram_refresh_cycle <= 0 || next_dram_refresh_cycle < now - 244) {
        next_dram_refresh_cycle = now + 244;
    }
    if (cycles > 0) {
        int64_t after = now + cycles;
        while (next_dram_refresh_cycle <= after) {
            prof_dram_refresh_stalls++;
            next_dram_refresh_cycle += 244;
        }
    }

    sh7021.cycles_left -= cycles;
    prof_workram_wait_cycles += cycles;
    prof_workram_accesses++;
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
    prof_internal_wait_cycles += cycles;
    prof_internal_accesses++;
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
    prof_vdp_wait_cycles += cycles;
    prof_vdp_accesses++;
    prof_vdp_bytes += bytes;
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
    if (cycles > 0) {
        sh7021.cycles_left -= cycles;
        prof_cart_wait_cycles += cycles;
        prof_cart_accesses++;
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

void sh7021_bus_fetch_reset(void) {
    last_opcode_fetch_valid = 0;
    last_opcode_fetch_addr = 0xFFFFFFFFu;
}

uint16_t sh7021_bus_fetch16(uint32_t addr) {
    uint32_t raw_addr = addr;
    uint32_t translated = translate_addr(addr);
    /* SH instruction fetch is pipelined.  Sequential 16-bit opcode fetches from
       cartridge ROM should not be charged like independent data reads; the
       external bus latency is mostly hidden by the normal instruction stream.
       Charge non-sequential/cart-entry fetches, but let linear execution run at
       interpreter instruction timing.  Data reads still use sh7021_bus_read16(). */
    int sequential = last_opcode_fetch_valid && raw_addr == (last_opcode_fetch_addr + 2u);
    uint8_t *mem = sh7021.pagetable[translated >> 12];
    if (mem) {
        apply_workram_wait(raw_addr, 2);
        if (!sequential) apply_cart_wait(raw_addr, 2, 0);
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
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { idle_note_read(addr, 1); apply_vdp_wait(raw_addr, addr, 1, 0); return video_bus_read8(raw_addr); }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 1); apply_cart_wait(raw_addr, 1, 0); return mem[addr & 0xFFFu]; }
    idle_note_read(addr, 1);
    apply_vdp_mmio_wait(raw_addr, addr, 1, 0);
    apply_internal_peripheral_wait(raw_addr, 1);
    MMIO_ACCESS(read8, addr);
}
uint16_t sh7021_bus_read16(uint32_t addr) {
    uint32_t raw_addr = addr;
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { idle_note_read(addr, 2); apply_vdp_wait(raw_addr, addr, 2, 0); return video_bus_read16(raw_addr); }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 2); apply_cart_wait(raw_addr, 2, 0); uint16_t value; memcpy(&value, mem + (addr & 0xFFFu), 2); return common_bswp16(value); }
    idle_note_read(addr, 2);
    apply_vdp_mmio_wait(raw_addr, addr, 2, 0);
    apply_internal_peripheral_wait(raw_addr, 2);
    MMIO_ACCESS(read16, addr);
}
uint32_t sh7021_bus_read32(uint32_t addr) {
    uint32_t raw_addr = addr;
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { idle_note_read(addr, 4); apply_vdp_wait(raw_addr, addr, 4, 0); return video_bus_read32(raw_addr); }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 4); apply_cart_wait(raw_addr, 4, 0); uint32_t value; memcpy(&value, mem + (addr & 0xFFFu), 4); return common_bswp32(value); }
    idle_note_read(addr, 4);
    apply_vdp_mmio_wait(raw_addr, addr, 4, 0);
    apply_internal_peripheral_wait(raw_addr, 4);
    MMIO_ACCESS(read32, addr);
}
void sh7021_bus_write8(uint32_t addr, uint8_t value) {
    sh7021_idle_wrote_mem = 1;
    uint32_t raw_addr = addr;
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { apply_vdp_wait(raw_addr, addr, 1, 1); video_bus_write8(raw_addr, value); return; }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 1); apply_cart_wait(raw_addr, 1, 1); mem[addr & 0xFFFu] = value; return; }
    apply_vdp_mmio_wait(raw_addr, addr, 1, 1);
    apply_internal_peripheral_wait(raw_addr, 1);
    MMIO_ACCESS(write8, addr, value);
}
void sh7021_bus_write16(uint32_t addr, uint16_t value) {
    sh7021_idle_wrote_mem = 1;
    uint32_t raw_addr = addr;
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { apply_vdp_wait(raw_addr, addr, 2, 1); video_bus_write16(raw_addr, value); return; }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 2); apply_cart_wait(raw_addr, 2, 1); value = common_bswp16(value); memcpy(mem + (addr & 0xFFFu), &value, 2); return; }
    apply_vdp_mmio_wait(raw_addr, addr, 2, 1);
    apply_internal_peripheral_wait(raw_addr, 2);
    MMIO_ACCESS(write16, addr, value);
}
void sh7021_bus_write32(uint32_t addr, uint32_t value) {
    sh7021_idle_wrote_mem = 1;
    uint32_t raw_addr = addr;
    addr = translate_addr(addr);
    if (video_bus_is_vdp_addr(raw_addr, addr)) { apply_vdp_wait(raw_addr, addr, 4, 1); video_bus_write32(raw_addr, value); return; }
    uint8_t *mem = sh7021.pagetable[addr >> 12];
    if (mem) { apply_workram_wait(raw_addr, 4); apply_cart_wait(raw_addr, 4, 1); value = common_bswp32(value); memcpy(mem + (addr & 0xFFFu), &value, 4); return; }
    apply_vdp_mmio_wait(raw_addr, addr, 4, 1);
    apply_internal_peripheral_wait(raw_addr, 4);
    MMIO_ACCESS(write32, addr, value);
}


static void note_dma_access(uint32_t addr, int bytes, int write, int single_address_mode) {
    prof_dma_accesses++;
    if (single_address_mode) {
        prof_dma_single_accesses++;
        prof_dma_model_cycles += sh7021_bsc_dma_single_cycle_states(addr, bytes, write);
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
