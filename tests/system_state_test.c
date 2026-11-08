#include "core/system.h"
#include "core/config.h"
#include "core/sh7021/sh7021_local.h"
#include "core/sh7021/sh7021_bus.h"
#include "core/sh7021/peripherals/sh7021_bsc.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_pfc.h"
#include "video/vdp_local.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct ChunkLoc {
    uint8_t *id;
    uint8_t *sizep;
    uint8_t *data;
    uint32_t size;
} ChunkLoc;

static uint32_t get_u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put_u32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void put_be32(void *p, uint32_t v) { uint8_t *b = (uint8_t *)p; b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16); b[2]=(uint8_t)(v>>8); b[3]=(uint8_t)v; }
static int64_t get_i64(const void *p) { int64_t v; memcpy(&v, p, 8); return v; }
static void put_i64(void *p, int64_t v) { memcpy(p, &v, 8); }

static int find_chunk(uint8_t *buf, uint32_t total, const char id[4], ChunkLoc *out, uint32_t *chunks_end) {
    if (!buf || total < 16 || memcmp(buf, "LPSTATE2", 8) != 0) return 0;
    uint32_t count = get_u32(buf + 12);
    uint8_t *p = buf + 16, *end = buf + total;
    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)(end - p) < 8) return 0;
        uint8_t *idp = p;
        uint8_t *sizep = p + 4;
        uint32_t n = get_u32(sizep);
        p += 8;
        if ((size_t)(end - p) < n) return 0;
        if (memcmp(idp, id, 4) == 0 && out) {
            out->id = idp; out->sizep = sizep; out->data = p; out->size = n;
        }
        p += n;
    }
    if (chunks_end) *chunks_end = (uint32_t)(p - buf);
    return out ? out->data != NULL : 1;
}

static void fail(const char *msg) {
    fprintf(stderr, "system_state_test: %s\n", msg);
    exit(1);
}

static void init_buf(ByteBuffer *b, size_t n, uint8_t fill) {
    b->data = (uint8_t *)malloc(n ? n : 1);
    if (!b->data) fail("allocation failed");
    b->size = n;
    if (n) memset(b->data, fill, n);
}

static void free_config_buffers(ConfigSystemInfo *c) {
    free(c->bios_rom.data);
    free(c->cart.rom.data);
    free(c->cart.sram.data);
}

static uint16_t pfc_raw_pbdr(void) {
    uint8_t pfc[64];
    uint32_t n = sh7021_ocpm_pfc_state_blob_size();
    if (n > sizeof(pfc)) fail("PFC blob unexpectedly large");
    sh7021_ocpm_pfc_get_state_blob(pfc, n);
    uint16_t v; memcpy(&v, pfc + 2, 2); return v;
}

static void expect_bus_field_u32(const uint8_t *b, size_t off, uint32_t want, const char *name) {
    uint32_t got = get_u32(b + off);
    if (got != want) {
        fprintf(stderr, "%s: got %u expected %u\n", name, got, want);
        exit(1);
    }
}

int main(void) {
    ConfigSystemInfo cfg;
    memset(&cfg, 0, sizeof(cfg));
    init_buf(&cfg.bios_rom, 0x8000, 0x00);
    init_buf(&cfg.cart.rom, 0x1000, 0x00);
    init_buf(&cfg.cart.sram, 0x1000, 0xFF);
    cfg.idle_skip_mode = LOOPY_IDLE_SKIP_OFF;
    /* Power-on reset must come from vectors 0/1, not an emulator-side jump
       into cartridge code. */
    put_be32(cfg.bios_rom.data + 0, 0x00000100u);
    put_be32(cfg.bios_rom.data + 4, 0x0ffffffcu);
    system_initialize(&cfg);
    if (sh7021.pc != 0x00000100u || sh7021.gpr[15] != 0x0ffffffcu ||
        sh7021.vbr != 0 || ((sh7021.sr >> 4) & 0xfu) != 0xfu)
        fail("SH7021 power-on reset vectors/registers are wrong");

    /* Area 7 is a 1 KiB physical on-chip RAM mirrored throughout the whole
       logical area because A23..A10 are ignored.  The reset SP used by retail
       BIOS code can therefore wrap from zero into the top shadow. */
    sh7021_bus_write32(0x0ffffffcu, 0x12345678u);
    if (sh7021_bus_read32(0x0f0003fcu) != 0x12345678u)
        fail("area-7 on-chip RAM top shadow does not alias physical RAM");
    sh7021_bus_write16(0x0f1233feu, 0xa55au);
    if (sh7021_bus_read16(0x0ffffffeu) != 0xa55au)
        fail("area-7 on-chip RAM intermediate shadow does not alias");

    /* Area 1 is an 8-bit view of the same DRAM as area 9.  The high
       byte is not wired, even for word reads, and writes reach D0..D7. */
    sh7021_bus_write16(0x09000080u, 0x1357u);
    if (sh7021_bus_read16(0x01000080u) != 0x5757u ||
        sh7021_bus_read8(0x01000080u) != 0x57u ||
        sh7021_bus_read8(0x01000081u) != 0x57u)
        fail("area-1 work RAM low-byte lanes are incorrect");
    sh7021_bus_write16(0x01000080u, 0xA5EEu);
    if (sh7021_bus_read16(0x09000080u) != 0x13EEu)
        fail("area-1 word write incorrectly drives high DRAM byte");

    /* VDP open-bus reads return the last READ word, not the most recent
       write. Unmapped offsets and write-only registers share this latch. */
    sh7021_bus_write16(0x0C051000u, 0x5A3Cu);
    if (sh7021_bus_read16(0x0C051000u) != 0x5A3Cu)
        fail("palette read did not load VDP read-data latch");
    sh7021_bus_write16(0x0C051000u, 0x25C3u);
    if (sh7021_bus_read16(0x0C180000u) != 0x5A3Cu ||
        sh7021_bus_read16(0x0C058006u) != 0x5A3Cu)
        fail("VDP open bus or write-only read modified data latch");

    uint32_t size = system_state_blob_size();
    if (!size) fail("zero state size");
    uint8_t *state = (uint8_t *)malloc(size);
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!state || !tmp) fail("state allocation failed");

    /* Current-format round trip, including the timing-relevant bus phase. */
    sh7021.gpr[3] = 0x12345678u;
    sh7021.load_delay_reg = 7;
    sh7021.load_delay_valid = 1;
    sh7021_ocpm_pfc_write16(0x05ffffc2u, 0x55aau);
    sh7021_ocpm_pfc_write16(0x05ffffc8u, 0x0400u);
    /* The new line-latched raster fields must survive savestates, including
       a pending colour split when the CPU is paused mid-picture. */
    vdp.bitmap_line_scrollx[1] = 0x12Au;
    vdp.bitmap_frame_scrolly[2] = 0x15Bu;
    vdp.bitmap_frame_screenx[3] = 0x17Cu;
    vdp.layer_ctrl.bitmap_enable[1] = 1;
    vdp.bitmap_enable_frame[1] = 1;
    vdp.color_split_count = 1;
    vdp.color_splits[0].x_end = 113;
    vdp.color_splits[0].backdrops[0] = 0x2233u;
    vdp.color_splits[0].palette[30] = 0x4Eu;
    vdp.snow_tile_blank[42] = 1;
    vdp.snow_palette_valid[43] = 1;
    vdp.snow_palette_rgb[43] = 0x2345u;

    uint8_t bus_before[48];
    memset(bus_before, 0, sizeof(bus_before));
    put_u32(bus_before + 0, 0x0000007bu);            /* open DRAM row 123 */
    put_i64(bus_before + 4, 1234567);
    put_i64(bus_before + 12, 1234500);
    put_u32(bus_before + 20, 1);
    put_u32(bus_before + 24, 1);
    put_u32(bus_before + 28, 244);
    put_u32(bus_before + 32, 3);
    put_u32(bus_before + 36, 0x0e001234u);
    put_u32(bus_before + 40, 1);
    sh7021_bus_timing_set_state_blob(bus_before, sizeof(bus_before));
    sh7021_bus_timing_get_state_blob(bus_before, sizeof(bus_before));

    if (system_save_state_to_buffer(state, size) != 0) fail("save-to-buffer failed");
    if (get_u32(state + 8) != 2u) fail("state format version is not 2");
    ChunkLoc bust = {0};
    if (!find_chunk(state, size, "BUST", &bust, NULL) || bust.size != 48u) fail("BUST chunk missing/wrong size");

    sh7021.gpr[3] = 0;
    sh7021.load_delay_valid = 0;
    sh7021_ocpm_pfc_write16(0x05ffffc2u, 0u);
    sh7021_ocpm_pfc_write16(0x05ffffc8u, 0u);
    vdp.bitmap_line_scrollx[1] = 0;
    vdp.bitmap_frame_scrolly[2] = 0;
    vdp.bitmap_frame_screenx[3] = 0;
    vdp.bitmap_enable_frame[1] = 0;
    vdp.layer_ctrl.bitmap_enable[1] = 0;
    vdp.color_split_count = 0;
    vdp.color_splits[0].x_end = 0;
    vdp.color_splits[0].palette[30] = 0;
    vdp.snow_tile_blank[42] = 0;
    vdp.snow_palette_valid[43] = 0;
    vdp.snow_palette_rgb[43] = 0;
    sh7021_bus_timing_reset();
    if (system_load_state_from_buffer(state, size) != 0) fail("round-trip load failed");
    if (sh7021.gpr[3] != 0x12345678u || !sh7021.load_delay_valid || sh7021.load_delay_reg != 7u)
        fail("CPU state did not round-trip");
    if (pfc_raw_pbdr() != 0x55aau || sh7021_ocpm_pfc_read16(0x05ffffc8u) != 0x0400u)
        fail("PFC state did not round-trip");
    if (vdp.bitmap_line_scrollx[1] != 0x12Au ||
        vdp.bitmap_frame_scrolly[2] != 0x15Bu ||
        vdp.bitmap_frame_screenx[3] != 0x17Cu ||
        vdp.bitmap_enable_frame[1] != 1 ||
        vdp.color_split_count != 1 || vdp.color_splits[0].x_end != 113 ||
        vdp.color_splits[0].backdrops[0] != 0x2233u ||
        vdp.color_splits[0].palette[30] != 0x4Eu ||
        vdp.snow_tile_blank[42] != 1 ||
        vdp.snow_palette_valid[43] != 1 ||
        vdp.snow_palette_rgb[43] != 0x2345u)
        fail("VDP pending line-state fields did not round-trip");
    uint8_t bus_after[48];
    sh7021_bus_timing_get_state_blob(bus_after, sizeof(bus_after));
    if (memcmp(bus_before, bus_after, sizeof(bus_before)) != 0) fail("bus timing state did not round-trip");

    /* A complete v2 savestate has a 336-byte timer chunk and a 204200-byte
       video chunk (including the old VDP struct's tail padding). It must
       survive the v3 layout additions without disturbing unrelated chunks. */
    {
        const uint32_t old_timer_size = 336u, old_video_size = 204200u;
        const uint32_t old_dma_size = sh7021_ocpm_dmac_legacy_state_blob_size();
        const uint32_t new_dma_size = sh7021_ocpm_dmac_state_blob_size();
        const uint32_t new_timer_size = 376u, new_video_size = video_state_blob_size();
        uint32_t legacy_size = size - (new_timer_size - old_timer_size) -
                                      (new_video_size - old_video_size) -
                                      (new_dma_size - old_dma_size);
        uint8_t *legacy = (uint8_t *)malloc(legacy_size);
        if (!legacy) fail("legacy-state allocation failed");
        const uint8_t *from = state + 16;
        uint8_t *to = legacy + 16;
        memcpy(legacy, state, 16);
        uint32_t n_chunks = get_u32(state + 12);
        for (uint32_t i = 0; i < n_chunks; i++) {
            const uint8_t *data = from + 8;
            uint32_t n = get_u32(from + 4);
            if (memcmp(from, "TMRS", 4) == 0) {
                if (n != new_timer_size) fail("unexpected v3 timer size");
                memcpy(to, "TMRS", 4);
                put_u32(to + 4, old_timer_size);
                memcpy(to + 8, data, 16);
                for (int j = 0; j < 5; ++j) {
                    const uint8_t *modern = data + 16 + j * 72;
                    uint8_t *old = to + 8 + 16 + j * 64;
                    memcpy(old, modern, 40);
                    memcpy(old + 40, modern + 48, 24);
                }
                to += 8 + old_timer_size;
            } else if (memcmp(from, "VDP ", 4) == 0) {
                if (n != new_video_size) fail("unexpected v3 VDP size");
                memcpy(to, "VDP ", 4);
                put_u32(to + 4, old_video_size);
                memcpy(to + 8, data, old_video_size);
                to += 8 + old_video_size;
            } else if (memcmp(from, "DMAC", 4) == 0) {
                if (n != new_dma_size) fail("unexpected v5 DMAC size in v2 fixture");
                memcpy(to, "DMAC", 4);
                put_u32(to + 4, old_dma_size);
                memcpy(to + 8, data, old_dma_size);
                to += 8 + old_dma_size;
            } else {
                memcpy(to, from, n + 8);
                to += n + 8;
            }
            from += n + 8;
        }
        size_t trailing = (size_t)(state + size - from);
        memcpy(to, from, trailing);
        to += trailing;
        if ((size_t)(to - legacy) != legacy_size) fail("legacy-state size wrong");
        vdp.layer_ctrl.bitmap_enable[1] = 0;
        vdp.bitmap_enable_frame[1] = 0;
        sh7021.gpr[3] = 0;
        if (system_load_state_from_buffer(legacy, legacy_size) != 0)
            fail("valid v2 state failed to load");
        if (sh7021.gpr[3] != 0x12345678u ||
            vdp.layer_ctrl.bitmap_enable[1] != 1 ||
            vdp.bitmap_enable_frame[1] != 1)
            fail("v2 state fields did not migrate correctly");
        free(legacy);
    }

    /* A v3 state still contains the bitmap-enable latches but lacks the
       v4 scanline bus-interference arrays. Its latches must be restored
       without treating it as v2 and its new arrays must start empty. */
    {
        const uint32_t v3_video_size = 204216u;
        const uint32_t v4_video_size = video_state_blob_size();
        if (v4_video_size <= v3_video_size) fail("new VDP state fields missing");
        const uint32_t old_dma_size = sh7021_ocpm_dmac_legacy_state_blob_size();
        const uint32_t new_dma_size = sh7021_ocpm_dmac_state_blob_size();
        uint32_t v3_size = size - (v4_video_size - v3_video_size) -
                                  (new_dma_size - old_dma_size);
        uint8_t *legacy = (uint8_t *)malloc(v3_size);
        if (!legacy) fail("v3-state allocation failed");
        memcpy(legacy, state, 16);
        uint32_t nchunks = get_u32(state + 12);
        const uint8_t *from = state + 16;
        uint8_t *to = legacy + 16;
        for (uint32_t i = 0; i < nchunks; i++) {
            uint32_t n = get_u32(from + 4);
            uint32_t keep = n;
            if (memcmp(from, "VDP ", 4) == 0) {
                if (n != v4_video_size) fail("new VDP chunk size unexpected");
                keep = v3_video_size;
            } else if (memcmp(from, "DMAC", 4) == 0) {
                if (n != new_dma_size) fail("new DMAC chunk size unexpected");
                keep = old_dma_size;
            }
            memcpy(to, from, 4);
            put_u32(to + 4, keep);
            memcpy(to + 8, from + 8, keep);
            to += 8 + keep;
            from += 8 + n;
        }
        size_t trailing = (size_t)(state + size - from);
        memcpy(to, from, trailing);
        to += trailing;
        if ((size_t)(to - legacy) != v3_size) fail("v3-state size wrong");
        vdp.bitmap_enable_frame[1] = 0;
        vdp.snow_tile_blank[42] = 1;
        vdp.snow_palette_valid[43] = 1;
        vdp.snow_palette_rgb[43] = 1;
        if (system_load_state_from_buffer(legacy, v3_size) != 0) fail("v3 state failed to load");
        if (vdp.bitmap_enable_frame[1] != 1 || vdp.snow_tile_blank[42] != 0 ||
            vdp.snow_palette_valid[43] != 0 || vdp.snow_palette_rgb[43] != 0)
            fail("v3 migration corrupted bitmap latches or snow arrays");
        free(legacy);
    }

    /* v4 states contain only the DMAC registers, with no autonomous-DMA
       runtime.  Strip the newly appended runtime from an otherwise complete
       v5 state, and verify the legacy layout is migrated deterministically. */
    {
        const uint32_t old_dma_size = sh7021_ocpm_dmac_legacy_state_blob_size();
        const uint32_t new_dma_size = sh7021_ocpm_dmac_state_blob_size();
        if (new_dma_size <= old_dma_size) fail("new DMA runtime state missing");
        uint32_t legacy_size = size - (new_dma_size - old_dma_size);
        uint8_t *legacy = (uint8_t *)malloc(legacy_size);
        if (!legacy) fail("v4 DMA-state allocation failed");
        memcpy(legacy, state, 16);
        const uint8_t *from = state + 16;
        uint8_t *to = legacy + 16;
        uint32_t nchunks = get_u32(state + 12);
        for (uint32_t i = 0; i < nchunks; i++) {
            uint32_t n = get_u32(from + 4);
            uint32_t keep = n;
            if (memcmp(from, "DMAC", 4) == 0) {
                if (n != new_dma_size) fail("unexpected v5 DMAC chunk size");
                keep = old_dma_size;
            }
            memcpy(to, from, 4);
            put_u32(to + 4, keep);
            memcpy(to + 8, from + 8, keep);
            to += keep + 8;
            from += n + 8;
        }
        size_t trailing = (size_t)(state + size - from);
        memcpy(to, from, trailing);
        to += trailing;
        if ((size_t)(to - legacy) != legacy_size) fail("v4 DMA-state size wrong");
        if (system_load_state_from_buffer(legacy, legacy_size) != 0)
            fail("valid v4 DMA state failed to load");
        if (sh7021_ocpm_dmac_busy()) fail("v4 DMA migration retained runtime activity");
        free(legacy);
    }

    /* Restoring the same state must reproduce the same post-load bus/cycle
       trace, rather than inheriting row/refresh/fetch phase from the timeline
       that happened to precede the load. */
    if (system_load_state_from_buffer(state, size) != 0) fail("trace load A failed");
    uint16_t trace_a_ram = sh7021_bus_read16(0x09000000u);
    uint16_t trace_a_cart = sh7021_bus_read16(0x0e000000u);
    int32_t trace_a_cycles = sh7021.cycles_left;
    uint8_t trace_a_bus[48];
    sh7021_bus_timing_get_state_blob(trace_a_bus, sizeof(trace_a_bus));

    if (system_load_state_from_buffer(state, size) != 0) fail("trace load B failed");
    uint16_t trace_b_ram = sh7021_bus_read16(0x09000000u);
    uint16_t trace_b_cart = sh7021_bus_read16(0x0e000000u);
    int32_t trace_b_cycles = sh7021.cycles_left;
    uint8_t trace_b_bus[48];
    sh7021_bus_timing_get_state_blob(trace_b_bus, sizeof(trace_b_bus));
    if (trace_a_ram != trace_b_ram || trace_a_cart != trace_b_cart ||
        trace_a_cycles != trace_b_cycles || memcmp(trace_a_bus, trace_b_bus, sizeof(trace_a_bus)) != 0)
        fail("post-load bus/cycle trace is not deterministic");

    /* File and buffer representations are the same v2 container. */
    const char *path = "/tmp/cloopy_system_state_test.lpstate";
    if (system_save_state(path) != 0) fail("file save failed");
    sh7021.gpr[3] = 0;
    if (system_load_state(path) != 0 || sh7021.gpr[3] != 0x12345678u) fail("file load failed");
    unlink(path);

    /* Version 1 is intentionally unsupported: no partial legacy restore. */
    memcpy(tmp, state, size);
    put_u32(tmp + 8, 1u);
    if (system_load_state_from_buffer(tmp, size) == 0) fail("version 1 state was accepted");

    /* A known ID with the wrong size must be rejected, not silently skipped. */
    memcpy(tmp, state, size);
    ChunkLoc pfc = {0};
    if (!find_chunk(tmp, size, "PFC ", &pfc, NULL)) fail("could not locate PFC chunk");
    put_u32(pfc.sizep, pfc.size + 1u);
    sh7021.gpr[3] = 0xdeadbeefu;
    if (system_load_state_from_buffer(tmp, size) == 0) fail("wrong known-chunk size was accepted");
    if (sh7021.gpr[3] != 0xdeadbeefu) fail("failed state load partially mutated CPU state");

    /* A missing required current chunk must be rejected even if its bytes are
       otherwise well-formed and look like an unknown future chunk. */
    memcpy(tmp, state, size);
    memset(&pfc, 0, sizeof(pfc));
    if (!find_chunk(tmp, size, "PFC ", &pfc, NULL)) fail("could not relocate PFC chunk");
    memcpy(pfc.id, "FUTR", 4);
    if (system_load_state_from_buffer(tmp, size) == 0) fail("missing required PFC chunk was accepted");

    /* Unknown future chunks remain skippable when all required v2 chunks exist. */
    uint32_t chunks_end = 0;
    if (!find_chunk(state, size, "BUST", NULL, &chunks_end)) fail("could not locate chunks end");
    const uint32_t extra = 11u;
    uint8_t *future = (uint8_t *)malloc(size + extra);
    if (!future) fail("future-state allocation failed");
    memcpy(future, state, chunks_end);
    put_u32(future + 12, get_u32(state + 12) + 1u);
    memcpy(future + chunks_end, "NEW!", 4);
    put_u32(future + chunks_end + 4, 3u);
    future[chunks_end + 8] = 1; future[chunks_end + 9] = 2; future[chunks_end + 10] = 3;
    memcpy(future + chunks_end + extra, state + chunks_end, size - chunks_end);
    if (system_load_state_from_buffer(future, size + extra) != 0) fail("unknown future chunk was not skipped");
    free(future);

    /* Malformed load-delay metadata cannot reach an out-of-range shift. */
    memcpy(tmp, state, size);
    ChunkLoc cpu = {0};
    if (!find_chunk(tmp, size, "SH7C", &cpu, NULL) || cpu.size < 108u) fail("CPU chunk missing");
    cpu.data[106] = 0xffu; /* load_delay_reg */
    cpu.data[107] = 1u;    /* load_delay_valid */
    if (system_load_state_from_buffer(tmp, size) != 0) fail("correctly-sized CPU state was rejected");
    if (sh7021.load_delay_valid || sh7021.load_delay_reg != 0u) fail("invalid load-delay register was not sanitized");

    /* Refresh phase is discarded while disabled and rebased on re-enable or
       period changes.  A deliberately ancient deadline exercises the O(1)
       catch-up path; a loop-per-refresh implementation would effectively hang. */
    sh7021_ocpm_bsc_write16(0x05ffffa0u, 0x9000u);
    sh7021_ocpm_bsc_write16(0x05ffffa2u, 0xb9fdu);
    sh7021_ocpm_bsc_write16(0x05ffffa4u, 0xb9b9u);
    sh7021_ocpm_bsc_write16(0x05ffffa6u, 0xa800u);
    sh7021_ocpm_bsc_write16(0x05ffffa8u, 0x5d00u);
    sh7021_ocpm_bsc_write16(0x05ffffacu, 0x5a80u);
    sh7021_ocpm_bsc_write16(0x05ffffb2u, 0x967au);
    sh7021_ocpm_bsc_write16(0x05ffffb0u, 0x6900u);
    sh7021_ocpm_bsc_write16(0x05ffffaeu, 0xa508u);
    sh7021_bus_timing_reset();
    (void)sh7021_bus_read16(0x09000000u); /* establish boot refresh phase */
    sh7021_ocpm_bsc_write16(0x05ffffacu, 0x5a00u); /* disable refresh */
    /* Cart-only traffic must observe the disable too; otherwise re-enable can
       resurrect the stale deadline even without a DRAM access in between. */
    (void)sh7021_bus_read16(0x0e000000u);
    sh7021_bus_timing_get_state_blob(bus_after, sizeof(bus_after));
    expect_bus_field_u32(bus_after, 20, 0, "refresh phase after disable");
    if (get_i64(bus_after + 4) != 0) fail("disabled refresh kept a stale deadline");

    sh7021_ocpm_bsc_write16(0x05ffffacu, 0x5a80u); /* enable, CBR */
    (void)sh7021_bus_read16(0x09000000u);
    sh7021_bus_timing_get_state_blob(bus_after, sizeof(bus_after));
    expect_bus_field_u32(bus_after, 20, 1, "refresh phase after enable");
    expect_bus_field_u32(bus_after, 28, 244, "boot refresh period cache");

    sh7021_ocpm_bsc_write16(0x05ffffb2u, 0x9660u); /* RTCOR=0x60 -> 192 cycles */
    (void)sh7021_bus_read16(0x09000000u);
    sh7021_bus_timing_get_state_blob(bus_after, sizeof(bus_after));
    expect_bus_field_u32(bus_after, 28, 192, "changed refresh period cache");

    put_i64(bus_after + 4, -1000000000000LL);
    put_u32(bus_after + 20, 1);
    put_u32(bus_after + 24, 1);
    put_u32(bus_after + 28, 192);
    put_u32(bus_after + 32, 3);
    sh7021_bus_timing_set_state_blob(bus_after, sizeof(bus_after));
    (void)sh7021_bus_read16(0x09000000u);
    sh7021_bus_timing_get_state_blob(bus_after, sizeof(bus_after));
    if (get_i64(bus_after + 4) <= 0) fail("refresh catch-up did not advance ancient deadline");

    free(tmp);
    free(state);
    system_shutdown();
    free_config_buffers(&cfg);
    puts("system_state_test: OK");
    return 0;
}
