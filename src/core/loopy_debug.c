#include "core/loopy_debug.h"

#include "core/sh7021/sh7021_bus.h"
#include "core/sh7021/sh7021_disasm.h"
#include "core/sh7021/sh7021_local.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- regions -*/

/* Address ranges come from the hardware notes' CPU Map.  The BIOS-reserved
   splits matter and are listed as regions in their own right: the BIOS keeps
   live state in the bottom 256 bytes of work RAM and the top 16 bytes of OCRAM,
   so a homebrew linker script that treats either as free will be corrupted by
   the next BIOS call, and a dump of "all of OCRAM" mixes game data with BIOS
   internals unless the two are separable. */
static const LoopyMemRegion regions[] = {
    { "bios",        0x00000000u, 0x00007FFFu, 32, "Internal 32KB mask ROM (BIOS)" },
    { "cart_sram",   0x02000000u, 0x023FFFFFu,  8, "Cartridge SRAM (max window; real chip is usually 8KB or 32KB)" },
    { "work_ram",    0x09000000u, 0x0907FFFFu, 16, "512KB DRAM work RAM (whole area)" },
    { "wram_bios",   0x09000000u, 0x090000FFu, 16, "Work RAM reserved for BIOS state" },
    { "wram_game",   0x09000100u, 0x0907FFFFu, 16, "Work RAM available for game code" },
    { "bitmap_vram", 0x0C000000u, 0x0C01FFFFu, 16, "VDP bitmap VRAM" },
    { "tile_vram",   0x0C040000u, 0x0C04FFFFu, 16, "VDP tile VRAM (tilemap + character data)" },
    { "oam",         0x0C050000u, 0x0C0501FFu, 16, "VDP object attribute memory (128 entries)" },
    { "palette",     0x0C051000u, 0x0C0511FFu, 16, "VDP palette (256 entries)" },
    { "capture",     0x0C052000u, 0x0C0521FFu, 16, "VDP scanline capture buffer" },
    { "cart_rom",    0x0E000000u, 0x0E3FFFFFu, 16, "Cartridge ROM (max window; up to 4MB)" },
    { "ocram",       0x0F000000u, 0x0F0003FFu, 32, "CPU on-chip RAM, 1KB (whole area)" },
    { "ocram_game",  0x0F000000u, 0x0F0003EFu, 32, "On-chip RAM available for game code (1008 bytes)" },
    { "ocram_bios",  0x0F0003F0u, 0x0F0003FFu, 32, "On-chip RAM reserved for BIOS state (16 bytes)" },
};

int loopy_debug_region_count(void) { return (int)(sizeof(regions) / sizeof(regions[0])); }

const LoopyMemRegion *loopy_debug_region_at(int index) {
    if (index < 0 || index >= loopy_debug_region_count()) return NULL;
    return &regions[index];
}

const LoopyMemRegion *loopy_debug_region_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < loopy_debug_region_count(); i++) {
        if (!strcmp(regions[i].name, name)) return &regions[i];
    }
    return NULL;
}

uint32_t loopy_debug_read_block(uint32_t addr, void *dst, uint32_t len) {
    uint8_t *out = (uint8_t *)dst;
    if (!out) return 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t value;
        if (!sh7021_bus_peek(addr + i, 1, &value)) return i;
        out[i] = (uint8_t)value;
    }
    return len;
}

uint32_t loopy_debug_dump_region(const char *name, const char *path) {
    const LoopyMemRegion *r = loopy_debug_region_by_name(name);
    if (!r || !path) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    uint32_t total = 0;
    uint8_t buf[4096];
    uint32_t remaining = r->end - r->start + 1u;
    uint32_t addr = r->start;
    while (remaining) {
        uint32_t chunk = remaining < sizeof(buf) ? remaining : (uint32_t)sizeof(buf);
        uint32_t got = loopy_debug_read_block(addr, buf, chunk);
        if (got) {
            if (fwrite(buf, 1, got, f) != got) break;
            total += got;
        }
        /* A short read means the region is not backed all the way to its
           documented end - a 2MB cart in a 4MB window, say.  Stop rather than
           padding, so the dump length reports the real size. */
        if (got != chunk) break;
        addr += chunk;
        remaining -= chunk;
    }
    fclose(f);
    return total;
}

/* ------------------------------------------------------------ disassembly -*/

static int disasm_reader(uint32_t addr, int bytes, uint32_t *out_value, void *userdata) {
    (void)userdata;
    return sh7021_bus_peek(addr, bytes, out_value);
}

int loopy_debug_disassemble(uint32_t addr, int count, FILE *out) {
    if (!out || count <= 0) return 0;
    int emitted = 0;
    for (int i = 0; i < count; i++) {
        uint32_t opcode;
        if (!sh7021_bus_peek(addr, 2, &opcode)) {
            fprintf(out, "%08X  <unreadable>\n", addr);
            break;
        }
        SH7021DisasmInsn insn;
        char line[192];
        sh7021_disasm_one(addr, (uint16_t)opcode, &insn, disasm_reader, NULL);
        sh7021_disasm_format(&insn, line, (int)sizeof(line));

        const char *known = loopy_debug_bios_known_name(addr);
        if (known) fprintf(out, "%s:\n", known);
        fprintf(out, "%s\n", line);

        addr += 2u;
        emitted++;
    }
    return emitted;
}

/* ------------------------------------------------------------- BIOS calls -*/

/* The only BIOS entries this emulator knows by name.  They were identified from
   its printer hooks (see sh7021.c), not from the hardware notes, which do not
   publish any addresses.  Anything else the tracer finds is reported by address
   alone - that is the point of tracing rather than table lookup. */
static const struct { uint32_t entry; const char *name; } bios_known[] = {
    { 0x000006D4u, "bios_printParts" },
    { 0x00000FD6u, "bios_printDirect" },
    { 0x0000101Cu, "bios_print15bpp" },
    { 0x00001064u, "bios_print8bpp" },
};

const char *loopy_debug_bios_known_name(uint32_t entry) {
    for (size_t i = 0; i < sizeof(bios_known) / sizeof(bios_known[0]); i++) {
        if (bios_known[i].entry == entry) return bios_known[i].name;
    }
    return NULL;
}

#define BIOS_TRACE_MAX 512

static int bios_trace_enabled = 0;
static LoopyBiosCallSite bios_calls[BIOS_TRACE_MAX];
static int bios_call_count = 0;
static LoopyBiosCallSite bios_sorted[BIOS_TRACE_MAX];
static uint64_t debug_frame_count = 0;

void loopy_debug_bios_trace_set_enabled(int enable) { bios_trace_enabled = enable ? 1 : 0; }
int loopy_debug_bios_trace_enabled(void) { return bios_trace_enabled; }

void loopy_debug_bios_trace_reset(void) {
    memset(bios_calls, 0, sizeof(bios_calls));
    bios_call_count = 0;
}

static int addr_in_bios(uint32_t pc) {
    return (pc & 0x0F000000u) == 0x00000000u && (pc & 0x00FFFFFFu) < LOOPY_BIOS_ROM_END;
}

static void bios_note_entry(uint32_t entry, uint32_t caller, int is_call) {
    for (int i = 0; i < bios_call_count; i++) {
        if (bios_calls[i].entry == entry) {
            bios_calls[i].count++;
            if (is_call) bios_calls[i].via_call++; else bios_calls[i].via_jump++;
            bios_calls[i].last_caller = caller;
            return;
        }
    }
    if (bios_call_count >= BIOS_TRACE_MAX) return;
    LoopyBiosCallSite *c = &bios_calls[bios_call_count++];
    c->entry = entry;
    c->count = 1;
    c->via_call = is_call ? 1 : 0;
    c->via_jump = is_call ? 0 : 1;
    c->first_caller = caller;
    c->last_caller = caller;
    c->first_args[0] = sh7021.gpr[4];
    c->first_args[1] = sh7021.gpr[5];
    c->first_args[2] = sh7021.gpr[6];
    c->first_args[3] = sh7021.gpr[7];
    c->first_frame = debug_frame_count;
    c->name = loopy_debug_bios_known_name(entry);
}

static int bios_call_cmp(const void *ap, const void *bp) {
    const LoopyBiosCallSite *a = ap, *b = bp;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    return a->entry < b->entry ? -1 : (a->entry > b->entry);
}

int loopy_debug_bios_call_count(void) { return bios_call_count; }

const LoopyBiosCallSite *loopy_debug_bios_call_at(int index) {
    if (index < 0 || index >= bios_call_count) return NULL;
    /* Sorted lazily into a side buffer on the first index of a read pass, so
       the live table keeps insertion order and stays cheap to update. */
    if (index == 0) {
        memcpy(bios_sorted, bios_calls, sizeof(bios_calls[0]) * (size_t)bios_call_count);
        qsort(bios_sorted, (size_t)bios_call_count, sizeof(bios_sorted[0]), bios_call_cmp);
    }
    return &bios_sorted[index];
}

/* ------------------------------------------------- CPU / hot-spot profile -*/

/* Open-addressed PC -> counters map.  An emulated frame touches on the order of
   thousands of distinct PCs, so this stays small and never needs eviction;
   exact counting beats sampling when the "sampling" would cost the same. */
typedef struct PcEntry {
    uint32_t pc;
    uint32_t used;
    uint64_t executions;
    uint64_t cycles;
} PcEntry;

static int cpu_profile_enabled = 0;
static PcEntry *pc_table = NULL;
static uint32_t pc_capacity = 0;
static uint32_t pc_used = 0;
static uint64_t cpu_total_cycles = 0;
static uint64_t cpu_total_instructions = 0;

static uint32_t pc_hash(uint32_t pc) {
    /* PCs are 2-byte aligned and highly clustered; mix so the low bits that
       actually vary reach the whole table. */
    pc ^= pc >> 16;
    pc *= 0x7feb352du;
    pc ^= pc >> 15;
    return pc;
}

static int pc_table_grow(void) {
    uint32_t ncap = pc_capacity ? pc_capacity * 2u : 4096u;
    PcEntry *ntab = (PcEntry *)calloc(ncap, sizeof(PcEntry));
    if (!ntab) return 0;
    for (uint32_t i = 0; i < pc_capacity; i++) {
        if (!pc_table[i].used) continue;
        uint32_t j = pc_hash(pc_table[i].pc) & (ncap - 1u);
        while (ntab[j].used) j = (j + 1u) & (ncap - 1u);
        ntab[j] = pc_table[i];
    }
    free(pc_table);
    pc_table = ntab;
    pc_capacity = ncap;
    return 1;
}

static void pc_note(uint32_t pc, int cycles) {
    if (!pc_capacity && !pc_table_grow()) return;
    if ((pc_used + 1u) * 10u >= pc_capacity * 7u) {
        if (!pc_table_grow()) return;
    }
    uint32_t i = pc_hash(pc) & (pc_capacity - 1u);
    while (pc_table[i].used && pc_table[i].pc != pc) i = (i + 1u) & (pc_capacity - 1u);
    if (!pc_table[i].used) {
        pc_table[i].used = 1;
        pc_table[i].pc = pc;
        pc_used++;
    }
    pc_table[i].executions++;
    if (cycles > 0) pc_table[i].cycles += (uint64_t)cycles;
}

void loopy_debug_cpu_profile_set_enabled(int enable) { cpu_profile_enabled = enable ? 1 : 0; }
int loopy_debug_cpu_profile_enabled(void) { return cpu_profile_enabled; }

void loopy_debug_cpu_profile_reset(void) {
    if (pc_table) memset(pc_table, 0, sizeof(PcEntry) * pc_capacity);
    pc_used = 0;
    cpu_total_cycles = 0;
    cpu_total_instructions = 0;
}

uint64_t loopy_debug_cpu_total_cycles(void) { return cpu_total_cycles; }
uint64_t loopy_debug_cpu_total_instructions(void) { return cpu_total_instructions; }

static int pc_sample_cmp(const void *ap, const void *bp) {
    const LoopyPcSample *a = ap, *b = bp;
    if (a->cycles != b->cycles) return a->cycles > b->cycles ? -1 : 1;
    return a->pc < b->pc ? -1 : (a->pc > b->pc);
}

int loopy_debug_cpu_hot_pcs(LoopyPcSample *out, int max) {
    if (!out || max <= 0 || !pc_table) return 0;
    LoopyPcSample *all = (LoopyPcSample *)malloc(sizeof(LoopyPcSample) * (pc_used ? pc_used : 1u));
    if (!all) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < pc_capacity; i++) {
        if (!pc_table[i].used) continue;
        all[n].pc = pc_table[i].pc;
        all[n].executions = pc_table[i].executions;
        all[n].cycles = pc_table[i].cycles;
        n++;
    }
    qsort(all, n, sizeof(all[0]), pc_sample_cmp);
    int count = (int)((uint32_t)max < n ? (uint32_t)max : n);
    memcpy(out, all, sizeof(all[0]) * (size_t)count);
    free(all);
    return count;
}

/* --------------------------------------------------------------- hooks ----*/

void loopy_debug_note_transfer(uint32_t from_pc, uint32_t to_pc, uint16_t opcode) {
    if (!bios_trace_enabled) return;
    if (!addr_in_bios(to_pc) || addr_in_bios(from_pc)) return;

    SH7021DisasmInsn insn;
    sh7021_disasm_one(from_pc, opcode, &insn, NULL, NULL);

    /* RTS and RTE resume code that was already running.  When an interrupt
       fires while the BIOS holds the CPU, the handler returns by RTE straight
       back into the BIOS - a resumption, not an entry, and the address it lands
       on is wherever the interrupt happened to strike rather than a function
       entry point at all. */
    int is_call = insn.flow == SH7021_DISASM_FLOW_CALL;
    int is_jump = insn.flow == SH7021_DISASM_FLOW_BRANCH;
    if (!is_call && !is_jump) return;

    bios_note_entry(to_pc & 0x00FFFFFFu, from_pc, is_call);
}

void loopy_debug_cpu_post(uint32_t pc, int cycles) {
    if (!cpu_profile_enabled) return;
    cpu_total_instructions++;
    if (cycles > 0) cpu_total_cycles += (uint64_t)cycles;
    pc_note(pc, cycles);
}

void loopy_debug_note_frame(void) { debug_frame_count++; }
uint64_t loopy_debug_frame_count(void) { return debug_frame_count; }
