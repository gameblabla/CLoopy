/* fdopen/fileno/dup are POSIX and -std=c11 hides them behind __STRICT_ANSI__,
   so the Makefile compiles this file with -D_POSIX_C_SOURCE.  It cannot be
   defined here: CPPFLAGS carries "-include common/log.h", which pulls in stdio
   ahead of anything in this file. */
#include "mcp/mcp_server.h"
#include "mcp/json.h"

#include "core/loopy_debug.h"
#include "core/system.h"
#include "core/sh7021/sh7021.h"
#include "core/sh7021/sh7021_bus.h"
#include "core/sh7021/sh7021_disasm.h"
#include "core/sh7021/sh7021_local.h"
#include "input/input.h"
#include "video/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define loopy_dup _dup
#define loopy_dup2 _dup2
#define LOOPY_FILENO _fileno
#else
#include <unistd.h>
#define loopy_dup dup
#define loopy_dup2 dup2
#define LOOPY_FILENO fileno
#endif

/* The JSON-RPC transport.  Set up by loopy_mcp_claim_stdout(); falls back to
   stdout so the server still works if the caller never claimed it. */
static FILE *mcp_out = NULL;

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_SERVER_NAME "cloopy"
#define MCP_SERVER_VERSION "1.0.0"

/* --------------------------------------------------------- state slots ----*/

/*
 * Snapshots live in memory rather than on disk.  The point of a persistent
 * session is to let a client park a known point in the timeline and come back
 * to it - "run to the title screen, snapshot, try input A, restore, try input
 * B" - and a round trip through the filesystem for each of those would dominate
 * the cost of the frames themselves.
 */
#define MCP_STATE_SLOTS 8

typedef struct StateSlot {
    void *data;
    uint32_t size;
    uint64_t frame;
    int used;
} StateSlot;

static StateSlot state_slots[MCP_STATE_SLOTS];

static void state_slots_free(void) {
    for (int i = 0; i < MCP_STATE_SLOTS; i++) {
        free(state_slots[i].data);
        state_slots[i].data = NULL;
        state_slots[i].used = 0;
    }
}

/* ------------------------------------------------------------- helpers ----*/

/* Tool results are text blocks.  Each handler renders into a JsonWriter that
   already holds the text payload, which is then wrapped as MCP content. */
typedef void (*ToolHandler)(const JsonValue *args, JsonWriter *text);

typedef struct Tool {
    const char *name;
    const char *description;
    const char *schema; /* Raw JSON Schema for inputSchema. */
    ToolHandler handler;
} Tool;

static uint32_t arg_u32(const JsonValue *args, const char *key, uint32_t fallback) {
    return (uint32_t)json_number_or(json_object_get(args, key), (double)fallback);
}

static int arg_int(const JsonValue *args, const char *key, int fallback) {
    return (int)json_number_or(json_object_get(args, key), (double)fallback);
}

/* ------------------------------------------------------------- tools ------*/

static void tool_run_frames(const JsonValue *args, JsonWriter *t) {
    int frames = arg_int(args, "frames", 1);
    if (frames < 1) frames = 1;
    if (frames > 100000) frames = 100000;
    for (int i = 0; i < frames; i++) system_run();
    json_write_fmt(t, "Ran %d frame(s). Total frames: %llu\n",
                   frames, (unsigned long long)loopy_debug_frame_count());
}

static void tool_list_regions(const JsonValue *args, JsonWriter *t) {
    (void)args;
    json_write_fmt(t, "%-12s %-9s %-9s %4s  %s\n", "NAME", "START", "END", "BUS", "DESCRIPTION");
    for (int i = 0; i < loopy_debug_region_count(); i++) {
        const LoopyMemRegion *r = loopy_debug_region_at(i);
        json_write_fmt(t, "%-12s %08X  %08X  %4d  %s\n", r->name, r->start, r->end, r->bus_bits, r->desc);
    }
}

static void tool_read_memory(const JsonValue *args, JsonWriter *t) {
    uint32_t addr = arg_u32(args, "addr", 0);
    int len = arg_int(args, "len", 64);
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;

    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { json_write_raw(t, "out of memory"); return; }
    uint32_t got = loopy_debug_read_block(addr, buf, (uint32_t)len);
    if (!got) {
        json_write_fmt(t, "0x%08X is not backed by readable memory. "
                          "MMIO registers are deliberately not peekable: reading one for real can "
                          "clear a status flag or move the VDP bus latch.\n", addr);
        free(buf);
        return;
    }
    json_write_fmt(t, "%u byte(s) at 0x%08X (region: %s)\n", got, addr,
                   sh7021_bus_region_name(sh7021_bus_region_of(addr)));
    for (uint32_t off = 0; off < got; off += 16u) {
        json_write_fmt(t, "%08X  ", addr + off);
        for (uint32_t i = 0; i < 16u; i++) {
            if (off + i < got) json_write_fmt(t, "%02X ", buf[off + i]);
            else json_write_raw(t, "   ");
        }
        json_write_raw(t, " |");
        for (uint32_t i = 0; i < 16u && off + i < got; i++) {
            uint8_t c = buf[off + i];
            json_write_fmt(t, "%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        json_write_raw(t, "|\n");
    }
    if (got < (uint32_t)len) json_write_fmt(t, "(stopped after %u of %d bytes: rest is unmapped)\n", got, len);
    free(buf);
}

static void tool_dump_region(const JsonValue *args, JsonWriter *t) {
    const char *region = json_string_or(json_object_get(args, "region"), NULL);
    const char *path = json_string_or(json_object_get(args, "path"), NULL);
    if (!region || !path) { json_write_raw(t, "error: 'region' and 'path' are required"); return; }
    uint32_t n = loopy_debug_dump_region(region, path);
    if (!n) json_write_fmt(t, "Dump failed for region '%s' -> %s\n", region, path);
    else json_write_fmt(t, "Wrote %u bytes of region '%s' to %s\n", n, region, path);
}

static int mcp_disasm_reader(uint32_t addr, int bytes, uint32_t *out_value, void *ud) {
    (void)ud;
    return sh7021_bus_peek(addr, bytes, out_value);
}

static void tool_disassemble(const JsonValue *args, JsonWriter *t) {
    uint32_t addr = arg_u32(args, "addr", sh7021.pc);
    int count = arg_int(args, "count", 32);
    if (count < 1) count = 1;
    if (count > 4096) count = 4096;

    for (int i = 0; i < count; i++) {
        uint32_t opcode;
        if (!sh7021_bus_peek(addr, 2, &opcode)) {
            json_write_fmt(t, "%08X  <unreadable>\n", addr);
            break;
        }
        SH7021DisasmInsn insn;
        char line[192];
        sh7021_disasm_one(addr, (uint16_t)opcode, &insn, mcp_disasm_reader, NULL);
        sh7021_disasm_format(&insn, line, (int)sizeof(line));
        const char *known = loopy_debug_bios_known_name(addr);
        if (known) json_write_fmt(t, "%s:\n", known);
        json_write_fmt(t, "%s\n", line);
        addr += 2u;
    }
}

static void tool_cpu_state(const JsonValue *args, JsonWriter *t) {
    (void)args;
    json_write_fmt(t, "PC  = %08X   (region: %s)\n", sh7021.pc,
                   sh7021_bus_region_name(sh7021_bus_region_of(sh7021.pc)));
    json_write_fmt(t, "PR  = %08X   SR = %08X   GBR = %08X   VBR = %08X\n",
                   sh7021.pr, sh7021.sr, sh7021.gbr, sh7021.vbr);
    json_write_fmt(t, "MACH= %08X   MACL = %08X\n", sh7021.mach, sh7021.macl);
    for (int i = 0; i < 16; i += 4) {
        json_write_fmt(t, "R%-2d = %08X   R%-2d = %08X   R%-2d = %08X   R%-2d = %08X\n",
                       i, sh7021.gpr[i], i + 1, sh7021.gpr[i + 1],
                       i + 2, sh7021.gpr[i + 2], i + 3, sh7021.gpr[i + 3]);
    }
    json_write_fmt(t, "frame = %llu\n", (unsigned long long)loopy_debug_frame_count());
}

static void tool_bus_profile(const JsonValue *args, JsonWriter *t) {
    SH7021BusProf p;
    sh7021_bus_prof_snapshot(&p);
    json_write_fmt(t, "%-15s %10s %10s %10s %12s %12s\n",
                   "REGION", "READS", "WRITES", "FETCHES", "BYTES", "WAIT_CYC");
    long long total_bytes = 0, total_wait = 0;
    for (int r = 0; r < SH7021_BUS_REGION_COUNT; r++) {
        const SH7021BusRegionCounters *c = &p.region[r];
        if (!c->reads && !c->writes && !c->fetches) continue;
        json_write_fmt(t, "%-15s %10lld %10lld %10lld %12lld %12lld\n",
                       sh7021_bus_region_name((SH7021BusRegion)r),
                       c->reads, c->writes, c->fetches, c->bytes, c->wait_cycles);
        total_bytes += c->bytes;
        total_wait += c->wait_cycles;
    }
    json_write_fmt(t, "%-15s %10s %10s %10s %12lld %12lld\n", "TOTAL", "", "", "", total_bytes, total_wait);
    json_write_fmt(t, "\ndram_refresh_stalls=%lld (diagnostic only, not charged)\n", p.dram_refresh_stalls);
    json_write_fmt(t, "dma_accesses=%lld dma_single=%lld dma_model_cycles=%lld\n",
                   p.dma_accesses, p.dma_single_accesses, p.dma_model_cycles);
    if (json_bool_or(json_object_get(args, "reset"), 0)) {
        sh7021_bus_prof_reset();
        json_write_raw(t, "\n(counters reset)\n");
    }
}

static void tool_cpu_profile(const JsonValue *args, JsonWriter *t) {
    if (json_object_get(args, "enable")) {
        int en = json_bool_or(json_object_get(args, "enable"), 1);
        loopy_debug_cpu_profile_set_enabled(en);
        json_write_fmt(t, "CPU profiling %s.\n", en ? "enabled" : "disabled");
        if (!en) return;
    }
    if (!loopy_debug_cpu_profile_enabled()) {
        json_write_raw(t, "CPU profiling is off. Call again with {\"enable\": true}, "
                          "run some frames, then read it back.\n");
        return;
    }

    int top = arg_int(args, "top", 20);
    if (top < 1) top = 1;
    if (top > 500) top = 500;

    uint64_t total = loopy_debug_cpu_total_cycles();
    json_write_fmt(t, "instructions=%llu cycles=%llu\n",
                   (unsigned long long)loopy_debug_cpu_total_instructions(),
                   (unsigned long long)total);
    if (!total) { json_write_raw(t, "(no samples yet: run some frames)\n"); return; }

    LoopyPcSample *s = (LoopyPcSample *)calloc((size_t)top, sizeof(LoopyPcSample));
    if (!s) { json_write_raw(t, "out of memory"); return; }
    int n = loopy_debug_cpu_hot_pcs(s, top);
    json_write_fmt(t, "\n%-10s %-14s %10s %12s %7s  %s\n", "PC", "REGION", "EXECS", "CYCLES", "SHARE", "INSTRUCTION");
    for (int i = 0; i < n; i++) {
        uint32_t opcode = 0;
        char text[72] = "?";
        if (sh7021_bus_peek(s[i].pc, 2, &opcode)) {
            SH7021DisasmInsn insn;
            sh7021_disasm_one(s[i].pc, (uint16_t)opcode, &insn, NULL, NULL);
            snprintf(text, sizeof(text), "%s", insn.text);
        }
        json_write_fmt(t, "%08X   %-14s %10llu %12llu %6.2f%%  %s\n",
                       s[i].pc, sh7021_bus_region_name(sh7021_bus_region_of(s[i].pc)),
                       (unsigned long long)s[i].executions, (unsigned long long)s[i].cycles,
                       100.0 * (double)s[i].cycles / (double)total, text);
    }
    free(s);
    if (json_bool_or(json_object_get(args, "reset"), 0)) {
        loopy_debug_cpu_profile_reset();
        json_write_raw(t, "\n(profile reset)\n");
    }
}

static void tool_bios_trace(const JsonValue *args, JsonWriter *t) {
    if (json_object_get(args, "enable")) {
        int en = json_bool_or(json_object_get(args, "enable"), 1);
        loopy_debug_bios_trace_set_enabled(en);
        json_write_fmt(t, "BIOS tracing %s.\n", en ? "enabled" : "disabled");
        if (!en) return;
    }
    if (!loopy_debug_bios_trace_enabled()) {
        json_write_raw(t, "BIOS tracing is off. Call again with {\"enable\": true}, "
                          "run some frames, then read it back.\n");
        return;
    }

    int n = loopy_debug_bios_call_count();
    json_write_fmt(t, "Observed %d distinct BIOS entry point(s).\n", n);
    json_write_raw(t, "Entries are discovered by tracing calls into BIOS ROM (0x0-0x7FFF), not looked up:\n"
                      "the hardware notes name BIOS routines but publish no addresses or call ABI.\n"
                      "Only entries this emulator already hooks are named.\n\n");
    if (!n) {
        json_write_raw(t, "(no calls into BIOS ROM observed yet: run some frames)\n");
        return;
    }
    json_write_fmt(t, "%-8s %10s %8s %8s %-10s %-18s %s\n",
                   "ENTRY", "TOTAL", "JSR/BSR", "JMP/BRA", "CALLER", "NAME", "ARGS R4-R7 (first call)");
    for (int i = 0; i < n; i++) {
        const LoopyBiosCallSite *c = loopy_debug_bios_call_at(i);
        json_write_fmt(t, "%08X %10llu %8llu %8llu %08X   %-18s %08X %08X %08X %08X\n",
                       c->entry, (unsigned long long)c->count,
                       (unsigned long long)c->via_call, (unsigned long long)c->via_jump,
                       c->first_caller, c->name ? c->name : "-",
                       c->first_args[0], c->first_args[1], c->first_args[2], c->first_args[3]);
    }
    if (json_bool_or(json_object_get(args, "reset"), 0)) {
        loopy_debug_bios_trace_reset();
        json_write_raw(t, "\n(trace reset)\n");
    }
}

static void tool_save_state(const JsonValue *args, JsonWriter *t) {
    int slot = arg_int(args, "slot", 0);
    if (slot < 0 || slot >= MCP_STATE_SLOTS) {
        json_write_fmt(t, "error: slot must be 0..%d\n", MCP_STATE_SLOTS - 1);
        return;
    }
    uint32_t size = system_state_blob_size();
    void *data = malloc(size);
    if (!data) { json_write_raw(t, "out of memory"); return; }
    if (system_save_state_to_buffer(data, size) != 0) {
        free(data);
        json_write_raw(t, "error: save failed\n");
        return;
    }
    free(state_slots[slot].data);
    state_slots[slot].data = data;
    state_slots[slot].size = size;
    state_slots[slot].frame = loopy_debug_frame_count();
    state_slots[slot].used = 1;
    json_write_fmt(t, "Saved %u bytes to slot %d at frame %llu.\n",
                   size, slot, (unsigned long long)state_slots[slot].frame);
}

static void tool_load_state(const JsonValue *args, JsonWriter *t) {
    int slot = arg_int(args, "slot", 0);
    if (slot < 0 || slot >= MCP_STATE_SLOTS || !state_slots[slot].used) {
        json_write_fmt(t, "error: slot %d is empty\n", slot);
        return;
    }
    if (system_load_state_from_buffer(state_slots[slot].data, state_slots[slot].size) != 0) {
        json_write_raw(t, "error: load failed\n");
        return;
    }
    json_write_fmt(t, "Restored slot %d (captured at frame %llu).\n",
                   slot, (unsigned long long)state_slots[slot].frame);
}

static void tool_set_input(const JsonValue *args, JsonWriter *t) {
    static const struct { const char *name; int button; } map[] = {
        { "up", PAD_UP }, { "down", PAD_DOWN }, { "left", PAD_LEFT }, { "right", PAD_RIGHT },
        { "a", PAD_A }, { "b", PAD_B }, { "c", PAD_C }, { "d", PAD_D },
        { "l1", PAD_L1 }, { "r1", PAD_R1 }, { "start", PAD_START },
    };
    const JsonValue *buttons = json_object_get(args, "buttons");
    if (!buttons || buttons->type != JSON_ARRAY) {
        json_write_raw(t, "error: 'buttons' must be an array of button names "
                          "(up down left right a b c d l1 r1 start); [] releases everything\n");
        return;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) input_set_pad_button(map[i].button, false);

    int set = 0;
    for (int i = 0; i < buttons->count; i++) {
        const char *want = json_string_or(buttons->values[i], NULL);
        if (!want) continue;
        int found = 0;
        for (size_t j = 0; j < sizeof(map) / sizeof(map[0]); j++) {
            if (!strcmp(map[j].name, want)) {
                input_set_pad_button(map[j].button, true);
                found = 1;
                set++;
                break;
            }
        }
        if (!found) json_write_fmt(t, "warning: unknown button '%s'\n", want);
    }
    json_write_fmt(t, "Pad state applied: %d button(s) held.\n", set);
}

static void tool_screenshot(const JsonValue *args, JsonWriter *t) {
    const char *path = json_string_or(json_object_get(args, "path"), NULL);
    if (!path) { json_write_raw(t, "error: 'path' is required\n"); return; }
    const uint16_t *fb = system_get_display_output();
    if (!fb) { json_write_raw(t, "error: no framebuffer\n"); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { json_write_fmt(t, "error: cannot open %s\n", path); return; }
    /* Binary PPM: no encoder needed and every image tool reads it. */
    fprintf(f, "P6\n%d %d\n255\n", VIDEO_DISPLAY_WIDTH, VIDEO_DISPLAY_HEIGHT);
    for (int i = 0; i < VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT; i++) {
        uint16_t c = fb[i];
        int r5 = (c >> 10) & 31, g5 = (c >> 5) & 31, b5 = c & 31;
        fputc((r5 << 3) | (r5 >> 2), f);
        fputc((g5 << 3) | (g5 >> 2), f);
        fputc((b5 << 3) | (b5 >> 2), f);
    }
    fclose(f);
    json_write_fmt(t, "Wrote %dx%d PPM screenshot to %s (frame %llu).\n",
                   VIDEO_DISPLAY_WIDTH, VIDEO_DISPLAY_HEIGHT, path,
                   (unsigned long long)loopy_debug_frame_count());
}

static const Tool tools[] = {
    { "loopy_run_frames",
      "Advance the emulator by N frames. The session is persistent, so this continues from wherever it is.",
      "{\"type\":\"object\",\"properties\":{\"frames\":{\"type\":\"integer\",\"description\":\"Frames to run (default 1)\"}}}",
      tool_run_frames },
    { "loopy_cpu_state",
      "Read the SH7021 register file: PC, PR, SR, GBR, VBR, MACH/MACL and R0-R15.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_cpu_state },
    { "loopy_list_regions",
      "List Loopy memory regions with address ranges and bus widths, including the BIOS-reserved splits in work RAM and OCRAM.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_list_regions },
    { "loopy_read_memory",
      "Hex-dump memory at a CPU address. Side-effect free: MMIO registers are not readable this way by design.",
      "{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":[\"integer\",\"string\"],\"description\":\"CPU address, e.g. 150994944 or \\\"0x09000100\\\"\"},\"len\":{\"type\":\"integer\",\"description\":\"Bytes to read (default 64, max 4096)\"}},\"required\":[\"addr\"]}",
      tool_read_memory },
    { "loopy_dump_region",
      "Write a whole named memory region to a file. Use loopy_list_regions for valid names.",
      "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"region\",\"path\"]}",
      tool_dump_region },
    { "loopy_disassemble",
      "Disassemble SH-1 instructions at an address, resolving PC-relative literals. Defaults to the current PC.",
      "{\"type\":\"object\",\"properties\":{\"addr\":{\"type\":[\"integer\",\"string\"]},\"count\":{\"type\":\"integer\",\"description\":\"Instructions (default 32)\"}}}",
      tool_disassemble },
    { "loopy_bus_profile",
      "Per-region memory traffic: reads, writes, instruction fetches, bytes and wait cycles, plus DRAM refresh and DMA counters. Shows where bus time goes.",
      "{\"type\":\"object\",\"properties\":{\"reset\":{\"type\":\"boolean\",\"description\":\"Zero the counters after reading\"}}}",
      tool_bus_profile },
    { "loopy_cpu_profile",
      "Hottest PCs by cycles consumed (including bus stalls), disassembled. Enable it, run frames, then read it back to find CPU bottlenecks.",
      "{\"type\":\"object\",\"properties\":{\"enable\":{\"type\":\"boolean\"},\"top\":{\"type\":\"integer\",\"description\":\"How many hot PCs (default 20)\"},\"reset\":{\"type\":\"boolean\"}}}",
      tool_cpu_profile },
    { "loopy_bios_trace",
      "BIOS entry points discovered by tracing calls into BIOS ROM, with call counts, callers and R4-R7 arguments. Enable it, run frames, then read it back.",
      "{\"type\":\"object\",\"properties\":{\"enable\":{\"type\":\"boolean\"},\"reset\":{\"type\":\"boolean\"}}}",
      tool_bios_trace },
    { "loopy_save_state",
      "Snapshot the whole machine into an in-memory slot, to return to later.",
      "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"description\":\"0-7 (default 0)\"}}}",
      tool_save_state },
    { "loopy_load_state",
      "Restore a snapshot previously taken with loopy_save_state.",
      "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"description\":\"0-7 (default 0)\"}}}",
      tool_load_state },
    { "loopy_set_input",
      "Set the held controller buttons. Persists until changed; pass [] to release everything.",
      "{\"type\":\"object\",\"properties\":{\"buttons\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"up\",\"down\",\"left\",\"right\",\"a\",\"b\",\"c\",\"d\",\"l1\",\"r1\",\"start\"]}}},\"required\":[\"buttons\"]}",
      tool_set_input },
    { "loopy_screenshot",
      "Write the current framebuffer to a binary PPM file.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
      tool_screenshot },
};

#define TOOL_COUNT ((int)(sizeof(tools) / sizeof(tools[0])))

/* ------------------------------------------------------------ transport ---*/

/* The id must be echoed back with its original type.  Rendering it into the
   reply verbatim avoids turning a string id into a number, or losing a large
   integer id through a double. */
static void write_id(JsonWriter *w, const JsonValue *id) {
    if (!id) { json_write_raw(w, "null"); return; }
    switch (id->type) {
    case JSON_STRING: json_write_string(w, id->string); break;
    case JSON_NUMBER: json_write_fmt(w, "%.17g", id->number); break;
    default: json_write_raw(w, "null"); break;
    }
}

static void send_message(JsonWriter *w) {
    FILE *out = mcp_out ? mcp_out : stdout;
    if (w->error || !w->buf) return;
    fputs(w->buf, out);
    fputc('\n', out);
    fflush(out);
}

static void send_error(const JsonValue *id, int code, const char *message) {
    JsonWriter w;
    json_writer_init(&w);
    json_write_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&w, id);
    json_write_fmt(&w, ",\"error\":{\"code\":%d,\"message\":", code);
    json_write_string(&w, message);
    json_write_raw(&w, "}}");
    send_message(&w);
    json_writer_free(&w);
}

static void handle_initialize(const JsonValue *id) {
    JsonWriter w;
    json_writer_init(&w);
    json_write_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&w, id);
    json_write_raw(&w, ",\"result\":{\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\","
                       "\"capabilities\":{\"tools\":{}},"
                       "\"serverInfo\":{\"name\":\"" MCP_SERVER_NAME "\",\"version\":\"" MCP_SERVER_VERSION "\"}}}");
    send_message(&w);
    json_writer_free(&w);
}

static void handle_tools_list(const JsonValue *id) {
    JsonWriter w;
    json_writer_init(&w);
    json_write_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&w, id);
    json_write_raw(&w, ",\"result\":{\"tools\":[");
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (i) json_write_raw(&w, ",");
        json_write_raw(&w, "{\"name\":");
        json_write_string(&w, tools[i].name);
        json_write_raw(&w, ",\"description\":");
        json_write_string(&w, tools[i].description);
        json_write_raw(&w, ",\"inputSchema\":");
        json_write_raw(&w, tools[i].schema);
        json_write_raw(&w, "}");
    }
    json_write_raw(&w, "]}}");
    send_message(&w);
    json_writer_free(&w);
}

static void handle_tools_call(const JsonValue *id, const JsonValue *params) {
    const char *name = json_string_or(json_object_get(params, "name"), NULL);
    const JsonValue *args = json_object_get(params, "arguments");
    if (!name) { send_error(id, -32602, "missing tool name"); return; }

    const Tool *tool = NULL;
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (!strcmp(tools[i].name, name)) { tool = &tools[i]; break; }
    }
    if (!tool) { send_error(id, -32602, "unknown tool"); return; }

    JsonWriter text;
    json_writer_init(&text);
    tool->handler(args, &text);

    JsonWriter w;
    json_writer_init(&w);
    json_write_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&w, id);
    json_write_raw(&w, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
    json_write_string(&w, text.buf ? text.buf : "");
    json_write_raw(&w, "}]}}");
    send_message(&w);
    json_writer_free(&w);
    json_writer_free(&text);
}

/* Reads one newline-delimited message.  Returns a malloc'd line, or NULL at
   EOF.  Grows to fit: a tools/call with a long argument must not be truncated
   into a parse error. */
static char *read_line(void) {
    size_t cap = 1024, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (len + 1u >= cap) {
            size_t ncap = cap * 2u;
            char *nbuf = (char *)realloc(buf, ncap);
            if (!nbuf) { free(buf); return NULL; }
            buf = nbuf;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    buf[len] = 0;
    return buf;
}

int loopy_mcp_claim_stdout(void) {
    if (mcp_out) return 0;
    int saved = loopy_dup(LOOPY_FILENO(stdout));
    if (saved < 0) return -1;
    mcp_out = fdopen(saved, "w");
    if (!mcp_out) return -1;
    setvbuf(mcp_out, NULL, _IOFBF, 1 << 16);
    /* Anything that still writes to stdout now ends up on stderr, where it is
       harmless. */
    fflush(stdout);
    if (loopy_dup2(LOOPY_FILENO(stderr), LOOPY_FILENO(stdout)) < 0) return -1;
    return 0;
}

int loopy_mcp_serve(void) {
    fprintf(stderr, "[cloopy] MCP server ready on stdio (%d tools)\n", TOOL_COUNT);

    char *line;
    while ((line = read_line()) != NULL) {
        if (!*line) { free(line); continue; }

        JsonValue *req = json_parse(line);
        free(line);
        if (!req) {
            send_error(NULL, -32700, "parse error");
            continue;
        }

        const char *method = json_string_or(json_object_get(req, "method"), NULL);
        const JsonValue *id = json_object_get(req, "id");
        const JsonValue *params = json_object_get(req, "params");

        if (!method) {
            send_error(id, -32600, "invalid request");
        } else if (!strcmp(method, "initialize")) {
            handle_initialize(id);
        } else if (!strcmp(method, "tools/list")) {
            handle_tools_list(id);
        } else if (!strcmp(method, "tools/call")) {
            handle_tools_call(id, params);
        } else if (!strncmp(method, "notifications/", 14)) {
            /* Notifications carry no id and must not be answered. */
        } else if (!id) {
            /* Any other id-less call is a notification too. */
        } else {
            send_error(id, -32601, "method not found");
        }
        json_free(req);
    }

    state_slots_free();
    return 0;
}
