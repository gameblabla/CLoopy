#ifndef LOOPY_SH7021_DISASM_H
#define LOOPY_SH7021_DISASM_H
#include <stdint.h>

/*
 * SH-1 disassembler for the SH7021.
 *
 * Decoding is strict SH-1: encodings the interpreter rejects as SH-2-only
 * (BRAF, BSRF, MUL.L, MAC.L, DMULS.L, DMULU.L, DT, BT/S, BF/S) disassemble as
 * illegal here too, so a listing never suggests an instruction this CPU would
 * refuse to execute.
 *
 * Decoding is pure: it touches neither the bus nor CPU state, so it is safe to
 * run against a live machine without perturbing it.  Resolving PC-relative
 * literals needs memory, so that is opt-in through a caller-supplied reader.
 */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SH7021_DISASM_FLOW_NONE = 0,
    SH7021_DISASM_FLOW_BRANCH,      /* BRA/BT/BF/JMP: goes somewhere else. */
    SH7021_DISASM_FLOW_CALL,        /* BSR/JSR: expected to come back. */
    SH7021_DISASM_FLOW_RETURN,      /* RTS/RTE. */
    SH7021_DISASM_FLOW_TRAP         /* TRAPA. */
};

typedef struct SH7021DisasmInsn {
    uint32_t addr;
    uint16_t opcode;
    char text[72];          /* Mnemonic and operands, e.g. "MOV.L @(4,R5),R3". */
    int flow;               /* SH7021_DISASM_FLOW_*. */
    int conditional;        /* Nonzero for BT/BF. */
    int has_delay_slot;     /* Next instruction executes before the transfer. */
    int illegal;            /* Not a valid SH-1 encoding; text is ".WORD 0x...". */

    /* Statically known branch target (PC-relative forms only).  JMP/JSR go
       through a register, so their target is not knowable without run state. */
    uint32_t target;
    int has_target;

    /* Address of the literal a PC-relative MOV/MOVA refers to.  MOVA computes
       the address itself, so it reports `literal_addr` with nothing to read;
       the MOV forms load from it, and `literal_value` holds that word when a
       reader was supplied and the address turned out to be readable. */
    uint32_t literal_addr;
    int has_literal;
    uint32_t literal_value;
    int literal_resolved;
} SH7021DisasmInsn;

/* Reader for literal-pool resolution.  Must be side-effect free.  Returns
   nonzero and stores the value when the address is readable, zero otherwise. */
typedef int (*SH7021DisasmRead)(uint32_t addr, int bytes, uint32_t *out_value, void *userdata);

/* Decodes one instruction.  `reader` may be NULL, in which case PC-relative
   loads still report `literal_addr` but their value is left unresolved. */
void sh7021_disasm_one(uint32_t addr, uint16_t opcode, SH7021DisasmInsn *out,
                       SH7021DisasmRead reader, void *userdata);

/* Formats one instruction as a listing line:
   "0E000480  D103  MOV.L @(0x0C,PC),R1  ; = 0x09000100"
   Returns the number of characters written (excluding the terminator). */
int sh7021_disasm_format(const SH7021DisasmInsn *insn, char *out, int out_size);

#ifdef __cplusplus
}
#endif

#endif
