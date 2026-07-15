#ifndef LOOPY_DEBUG_H
#define LOOPY_DEBUG_H
#include <stdint.h>
#include <stdio.h>

/*
 * Inspection and profiling surface, shared by the headless CLI and the MCP
 * server.  Everything here is read-only with respect to the emulated machine:
 * reads go through sh7021_bus_peek(), which never touches MMIO, so a client may
 * sample state between frames without changing what the machine does next.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- regions -*/

typedef struct LoopyMemRegion {
    const char *name;
    uint32_t start;      /* Inclusive CPU address. */
    uint32_t end;        /* Inclusive CPU address. */
    int bus_bits;
    const char *desc;
} LoopyMemRegion;

int loopy_debug_region_count(void);
const LoopyMemRegion *loopy_debug_region_at(int index);
const LoopyMemRegion *loopy_debug_region_by_name(const char *name);

/* Reads `len` bytes into `dst`, stopping at the first address that is not
   backed by real memory.  Returns the number of bytes actually read, so a
   caller can tell a 512KB work RAM dump from a 2MB cart that only populates
   the first megabyte. */
uint32_t loopy_debug_read_block(uint32_t addr, void *dst, uint32_t len);

/* Writes a named region to `path`.  Returns bytes written, or 0 on failure. */
uint32_t loopy_debug_dump_region(const char *name, const char *path);

/* ------------------------------------------------------------ disassembly -*/

/* Disassembles `count` instructions starting at `addr`, one listing line per
   instruction, into `out`.  Literals are resolved through the peek path.
   Returns the number of instructions emitted. */
int loopy_debug_disassemble(uint32_t addr, int count, FILE *out);

/* ------------------------------------------------------------- BIOS calls -*/

/*
 * The hardware notes name BIOS routines (bios_vsync, bios_dma, bios_vdpMode,
 * bios_playBgm, ...) but publish neither their entry addresses nor a call ABI -
 * the general section defers them to a document that does not exist yet, and
 * the vsync helper is explicitly "not yet known how to call it".  Only four
 * entries are known at all, from this emulator's own printer hooks.
 *
 * So entries are not looked up, they are observed: a call or tail jump from
 * outside the BIOS ROM into it is recorded by target address, together with the
 * caller and the argument registers as they stood on entry.  Run a game, then
 * read the table back to learn the real entry points.
 */

#define LOOPY_BIOS_ROM_START 0x00000000u
#define LOOPY_BIOS_ROM_END   0x00008000u /* Exclusive. */

typedef struct LoopyBiosCallSite {
    uint32_t entry;          /* BIOS address that was entered. */
    uint64_t count;          /* Times entered (calls + tail jumps). */
    uint64_t via_call;       /* Entered by JSR/BSR: a call expecting to return. */
    uint64_t via_jump;       /* Entered by JMP/BRA: a tail call. */
    uint32_t first_caller;   /* PC that transferred in, first time. */
    uint32_t last_caller;
    uint32_t first_args[4];  /* R4-R7 on first entry: the SH calling convention
                                passes the first four arguments there. */
    uint64_t first_frame;
    const char *name;        /* Known name, or NULL if only observed. */
} LoopyBiosCallSite;

void loopy_debug_bios_trace_set_enabled(int enable);
int loopy_debug_bios_trace_enabled(void);
void loopy_debug_bios_trace_reset(void);
int loopy_debug_bios_call_count(void);
/* Call sites sorted by descending call count. */
const LoopyBiosCallSite *loopy_debug_bios_call_at(int index);
/* Name for a BIOS entry if this emulator already knows it, else NULL. */
const char *loopy_debug_bios_known_name(uint32_t entry);

/* ------------------------------------------------- CPU / hot-spot profile -*/

typedef struct LoopyPcSample {
    uint32_t pc;
    uint64_t executions;
    uint64_t cycles;   /* Includes bus wait states, so this is where time went. */
} LoopyPcSample;

void loopy_debug_cpu_profile_set_enabled(int enable);
int loopy_debug_cpu_profile_enabled(void);
void loopy_debug_cpu_profile_reset(void);
uint64_t loopy_debug_cpu_total_cycles(void);
uint64_t loopy_debug_cpu_total_instructions(void);
/* Hottest PCs by cycles, descending.  Fills up to `max` entries, returns how
   many were written. */
int loopy_debug_cpu_hot_pcs(LoopyPcSample *out, int max);

/* --------------------------------------------------------------- hooks ----*/

/* Called by the CPU core when a delayed control transfer commits, with the
   branch instruction that caused it.  The opcode matters: landing in the BIOS
   is not by itself a call.  An interrupt taken while the BIOS was executing
   returns through RTE and lands right back in the middle of BIOS code, which a
   plain "did we cross into the ROM" test scores as a call to whatever loop
   instruction happened to be interrupted.  Classifying the transferring
   instruction keeps resumption out of the table.

   Register arguments are sampled here rather than at the branch, because the
   delay slot has already run: this is the state the callee actually sees. */
void loopy_debug_note_transfer(uint32_t from_pc, uint32_t to_pc, uint16_t opcode);

/* Called by the CPU core at each instruction boundary.  Compiles down to a
   predictable branch when profiling is off. */
void loopy_debug_cpu_post(uint32_t pc, int cycles);

/* Frame counter, so traces can be correlated with video output. */
void loopy_debug_note_frame(void);
uint64_t loopy_debug_frame_count(void);

#ifdef __cplusplus
}
#endif

#endif
