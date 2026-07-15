/*
 * SH-1 disassembler for the SH7021.
 *
 * The opcode map here is deliberately kept in step with
 * sh7021_interpreter.c: every encoding that executor rejects as SH-2-only is
 * rejected here too.  Where the two differ on purpose is strictness.  The
 * interpreter dispatches the 0x0000/0x4000 groups on `opcode & 0x3f`, which
 * ignores the top two bits of the register-select field, so it decodes (for
 * example) 0x005A as STC GBR the same as 0x001A.  That looseness is harmless
 * for code a real assembler produced, but a disassembler that mirrored it would
 * silently invent plausible instructions out of data.  So the selector fields
 * are matched in full below, and anything unrecognised is reported as .WORD.
 */

#include "core/sh7021/sh7021_disasm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define OP_N ((unsigned)((opcode >> 8) & 15u))
#define OP_M ((unsigned)((opcode >> 4) & 15u))

#if defined(__GNUC__) || defined(__clang__)
#define SH7021_DISASM_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define SH7021_DISASM_PRINTF(fmt_idx, arg_idx)
#endif

static void set(SH7021DisasmInsn *out, const char *fmt, ...) SH7021_DISASM_PRINTF(2, 3);

static int32_t sext(uint32_t value, unsigned bits) {
    const uint32_t sign = 1u << (bits - 1u);
    const uint32_t mask = (1u << bits) - 1u;
    value &= mask;
    return (int32_t)((value ^ sign) - sign);
}

static void set(SH7021DisasmInsn *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->text, sizeof(out->text), fmt, ap);
    va_end(ap);
}

static void illegal(SH7021DisasmInsn *out) {
    out->illegal = 1;
    snprintf(out->text, sizeof(out->text), ".WORD 0x%04X", out->opcode);
}

/* Displacement branches are relative to PC+4: the SH pipeline has already
   advanced past the delay slot by the time the target is computed. */
static void branch_target(SH7021DisasmInsn *out, int32_t disp_bytes) {
    out->target = out->addr + 4u + (uint32_t)disp_bytes;
    out->has_target = 1;
}

static void resolve_literal(SH7021DisasmInsn *out, int bytes,
                            SH7021DisasmRead reader, void *userdata) {
    out->has_literal = 1;
    if (!reader) return;
    uint32_t value = 0;
    if (reader(out->literal_addr, bytes, &value, userdata)) {
        out->literal_value = value;
        out->literal_resolved = 1;
    }
}

static void disasm_0000(uint32_t addr, uint16_t opcode, SH7021DisasmInsn *out) {
    (void)addr;
    switch (opcode & 15u) {
    case 0x2:
        switch (OP_M) {
        case 0: set(out, "STC SR,R%u", OP_N); break;
        case 1: set(out, "STC GBR,R%u", OP_N); break;
        case 2: set(out, "STC VBR,R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x4: set(out, "MOV.B R%u,@(R0,R%u)", OP_M, OP_N); break;
    case 0x5: set(out, "MOV.W R%u,@(R0,R%u)", OP_M, OP_N); break;
    case 0x6: set(out, "MOV.L R%u,@(R0,R%u)", OP_M, OP_N); break;
    case 0x8:
        if (OP_N != 0) { illegal(out); break; }
        switch (OP_M) {
        case 0: set(out, "CLRT"); break;
        case 1: set(out, "SETT"); break;
        case 2: set(out, "CLRMAC"); break;
        default: illegal(out); break;
        }
        break;
    case 0x9:
        switch (OP_M) {
        case 0: if (OP_N == 0) set(out, "NOP"); else illegal(out); break;
        case 1: if (OP_N == 0) set(out, "DIV0U"); else illegal(out); break;
        case 2: set(out, "MOVT R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0xA:
        switch (OP_M) {
        case 0: set(out, "STS MACH,R%u", OP_N); break;
        case 1: set(out, "STS MACL,R%u", OP_N); break;
        case 2: set(out, "STS PR,R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0xB:
        if (OP_N != 0) { illegal(out); break; }
        switch (OP_M) {
        case 0:
            set(out, "RTS");
            out->flow = SH7021_DISASM_FLOW_RETURN;
            out->has_delay_slot = 1;
            break;
        case 1: set(out, "SLEEP"); break;
        case 2:
            set(out, "RTE");
            out->flow = SH7021_DISASM_FLOW_RETURN;
            out->has_delay_slot = 1;
            break;
        default: illegal(out); break;
        }
        break;
    case 0xC: set(out, "MOV.B @(R0,R%u),R%u", OP_M, OP_N); break;
    case 0xD: set(out, "MOV.W @(R0,R%u),R%u", OP_M, OP_N); break;
    case 0xE: set(out, "MOV.L @(R0,R%u),R%u", OP_M, OP_N); break;
    /* 0x0/0x1 unassigned; 0x3 BSRF/BRAF, 0x7 MUL.L, 0xF MAC.L are SH-2 only. */
    default: illegal(out); break;
    }
}

static void disasm_0100(uint32_t addr, uint16_t opcode, SH7021DisasmInsn *out) {
    (void)addr;
    switch (opcode & 15u) {
    case 0x0:
        switch (OP_M) {
        case 0: set(out, "SHLL R%u", OP_N); break;
        case 2: set(out, "SHAL R%u", OP_N); break;
        default: illegal(out); break; /* M==1 is DT, SH-2 only. */
        }
        break;
    case 0x1:
        switch (OP_M) {
        case 0: set(out, "SHLR R%u", OP_N); break;
        case 1: set(out, "CMP/PZ R%u", OP_N); break;
        case 2: set(out, "SHAR R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x2:
        switch (OP_M) {
        case 0: set(out, "STS.L MACH,@-R%u", OP_N); break;
        case 1: set(out, "STS.L MACL,@-R%u", OP_N); break;
        case 2: set(out, "STS.L PR,@-R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x3:
        switch (OP_M) {
        case 0: set(out, "STC.L SR,@-R%u", OP_N); break;
        case 1: set(out, "STC.L GBR,@-R%u", OP_N); break;
        case 2: set(out, "STC.L VBR,@-R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x4:
        switch (OP_M) {
        case 0: set(out, "ROTL R%u", OP_N); break;
        case 2: set(out, "ROTCL R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x5:
        switch (OP_M) {
        case 0: set(out, "ROTR R%u", OP_N); break;
        case 1: set(out, "CMP/PL R%u", OP_N); break;
        case 2: set(out, "ROTCR R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x6:
        switch (OP_M) {
        case 0: set(out, "LDS.L @R%u+,MACH", OP_N); break;
        case 1: set(out, "LDS.L @R%u+,MACL", OP_N); break;
        case 2: set(out, "LDS.L @R%u+,PR", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x7:
        switch (OP_M) {
        case 0: set(out, "LDC.L @R%u+,SR", OP_N); break;
        case 1: set(out, "LDC.L @R%u+,GBR", OP_N); break;
        case 2: set(out, "LDC.L @R%u+,VBR", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x8:
        switch (OP_M) {
        case 0: set(out, "SHLL2 R%u", OP_N); break;
        case 1: set(out, "SHLL8 R%u", OP_N); break;
        case 2: set(out, "SHLL16 R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x9:
        switch (OP_M) {
        case 0: set(out, "SHLR2 R%u", OP_N); break;
        case 1: set(out, "SHLR8 R%u", OP_N); break;
        case 2: set(out, "SHLR16 R%u", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0xA:
        switch (OP_M) {
        case 0: set(out, "LDS R%u,MACH", OP_N); break;
        case 1: set(out, "LDS R%u,MACL", OP_N); break;
        case 2: set(out, "LDS R%u,PR", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0xB:
        switch (OP_M) {
        case 0:
            set(out, "JSR @R%u", OP_N);
            out->flow = SH7021_DISASM_FLOW_CALL;
            out->has_delay_slot = 1;
            break;
        case 1: set(out, "TAS.B @R%u", OP_N); break;
        case 2:
            set(out, "JMP @R%u", OP_N);
            out->flow = SH7021_DISASM_FLOW_BRANCH;
            out->has_delay_slot = 1;
            break;
        default: illegal(out); break;
        }
        break;
    case 0xE:
        switch (OP_M) {
        case 0: set(out, "LDC R%u,SR", OP_N); break;
        case 1: set(out, "LDC R%u,GBR", OP_N); break;
        case 2: set(out, "LDC R%u,VBR", OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0xF: set(out, "MAC.W @R%u+,@R%u+", OP_M, OP_N); break;
    default: illegal(out); break;
    }
}

static void disasm_1000(uint32_t addr, uint16_t opcode, SH7021DisasmInsn *out) {
    (void)addr;
    const unsigned disp = opcode & 15u;
    switch ((opcode >> 8) & 15u) {
    case 0x0: set(out, "MOV.B R0,@(0x%02X,R%u)", disp, OP_M); break;
    case 0x1: set(out, "MOV.W R0,@(0x%02X,R%u)", disp * 2u, OP_M); break;
    case 0x4: set(out, "MOV.B @(0x%02X,R%u),R0", disp, OP_M); break;
    case 0x5: set(out, "MOV.W @(0x%02X,R%u),R0", disp * 2u, OP_M); break;
    /* CMP/EQ sign-extends its immediate before comparing, unlike the logical
       immediates below, which mask to 8 bits.  Printing it signed keeps a
       compare against 0xFF from reading as 255 when it tests for -1. */
    case 0x8: set(out, "CMP/EQ #%d,R0", (int)sext(opcode & 0xFFu, 8)); break;
    case 0x9:
        branch_target(out, sext(opcode & 0xFFu, 8) * 2);
        set(out, "BT 0x%08X", out->target);
        out->flow = SH7021_DISASM_FLOW_BRANCH;
        out->conditional = 1;
        break;
    case 0xB:
        branch_target(out, sext(opcode & 0xFFu, 8) * 2);
        set(out, "BF 0x%08X", out->target);
        out->flow = SH7021_DISASM_FLOW_BRANCH;
        out->conditional = 1;
        break;
    /* 0xD BT/S and 0xF BF/S are SH-2 only. */
    default: illegal(out); break;
    }
}

static void disasm_1100(uint32_t addr, uint16_t opcode, SH7021DisasmInsn *out,
                        SH7021DisasmRead reader, void *userdata) {
    const unsigned imm = opcode & 0xFFu;
    switch ((opcode >> 8) & 15u) {
    case 0x0: set(out, "MOV.B R0,@(0x%02X,GBR)", imm); break;
    case 0x1: set(out, "MOV.W R0,@(0x%03X,GBR)", imm * 2u); break;
    case 0x2: set(out, "MOV.L R0,@(0x%03X,GBR)", imm * 4u); break;
    case 0x3:
        set(out, "TRAPA #0x%02X", imm);
        out->flow = SH7021_DISASM_FLOW_TRAP;
        break;
    case 0x4: set(out, "MOV.B @(0x%02X,GBR),R0", imm); break;
    case 0x5: set(out, "MOV.W @(0x%03X,GBR),R0", imm * 2u); break;
    case 0x6: set(out, "MOV.L @(0x%03X,GBR),R0", imm * 4u); break;
    case 0x7:
        /* MOVA produces the address rather than loading from it, so there is
           nothing to resolve; report it so callers can cross-reference. */
        out->literal_addr = (addr & ~3u) + 4u + imm * 4u;
        out->has_literal = 1;
        set(out, "MOVA @(0x%03X,PC),R0", imm * 4u);
        break;
    case 0x8: set(out, "TST #0x%02X,R0", imm); break;
    case 0x9: set(out, "AND #0x%02X,R0", imm); break;
    case 0xA: set(out, "XOR #0x%02X,R0", imm); break;
    case 0xB: set(out, "OR #0x%02X,R0", imm); break;
    case 0xC: set(out, "TST.B #0x%02X,@(R0,GBR)", imm); break;
    case 0xD: set(out, "AND.B #0x%02X,@(R0,GBR)", imm); break;
    case 0xE: set(out, "XOR.B #0x%02X,@(R0,GBR)", imm); break;
    case 0xF: set(out, "OR.B #0x%02X,@(R0,GBR)", imm); break;
    default: illegal(out); break;
    }
    (void)reader; (void)userdata;
}

void sh7021_disasm_one(uint32_t addr, uint16_t opcode, SH7021DisasmInsn *out,
                       SH7021DisasmRead reader, void *userdata) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->addr = addr;
    out->opcode = opcode;

    switch ((opcode >> 12) & 15u) {
    case 0x0: disasm_0000(addr, opcode, out); break;
    case 0x1: set(out, "MOV.L R%u,@(0x%02X,R%u)", OP_M, (unsigned)(opcode & 15u) * 4u, OP_N); break;
    case 0x2:
        switch (opcode & 15u) {
        case 0x0: set(out, "MOV.B R%u,@R%u", OP_M, OP_N); break;
        case 0x1: set(out, "MOV.W R%u,@R%u", OP_M, OP_N); break;
        case 0x2: set(out, "MOV.L R%u,@R%u", OP_M, OP_N); break;
        case 0x4: set(out, "MOV.B R%u,@-R%u", OP_M, OP_N); break;
        case 0x5: set(out, "MOV.W R%u,@-R%u", OP_M, OP_N); break;
        case 0x6: set(out, "MOV.L R%u,@-R%u", OP_M, OP_N); break;
        case 0x7: set(out, "DIV0S R%u,R%u", OP_M, OP_N); break;
        case 0x8: set(out, "TST R%u,R%u", OP_M, OP_N); break;
        case 0x9: set(out, "AND R%u,R%u", OP_M, OP_N); break;
        case 0xA: set(out, "XOR R%u,R%u", OP_M, OP_N); break;
        case 0xB: set(out, "OR R%u,R%u", OP_M, OP_N); break;
        case 0xC: set(out, "CMP/STR R%u,R%u", OP_M, OP_N); break;
        case 0xD: set(out, "XTRCT R%u,R%u", OP_M, OP_N); break;
        case 0xE: set(out, "MULU.W R%u,R%u", OP_M, OP_N); break;
        case 0xF: set(out, "MULS.W R%u,R%u", OP_M, OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x3:
        switch (opcode & 15u) {
        case 0x0: set(out, "CMP/EQ R%u,R%u", OP_M, OP_N); break;
        case 0x2: set(out, "CMP/HS R%u,R%u", OP_M, OP_N); break;
        case 0x3: set(out, "CMP/GE R%u,R%u", OP_M, OP_N); break;
        case 0x4: set(out, "DIV1 R%u,R%u", OP_M, OP_N); break;
        case 0x6: set(out, "CMP/HI R%u,R%u", OP_M, OP_N); break;
        case 0x7: set(out, "CMP/GT R%u,R%u", OP_M, OP_N); break;
        case 0x8: set(out, "SUB R%u,R%u", OP_M, OP_N); break;
        case 0xA: set(out, "SUBC R%u,R%u", OP_M, OP_N); break;
        case 0xB: set(out, "SUBV R%u,R%u", OP_M, OP_N); break;
        case 0xC: set(out, "ADD R%u,R%u", OP_M, OP_N); break;
        case 0xE: set(out, "ADDC R%u,R%u", OP_M, OP_N); break;
        case 0xF: set(out, "ADDV R%u,R%u", OP_M, OP_N); break;
        /* 0x5 DMULU.L and 0xD DMULS.L are SH-2 only. */
        default: illegal(out); break;
        }
        break;
    case 0x4: disasm_0100(addr, opcode, out); break;
    case 0x5: set(out, "MOV.L @(0x%02X,R%u),R%u", (unsigned)(opcode & 15u) * 4u, OP_M, OP_N); break;
    case 0x6:
        switch (opcode & 15u) {
        case 0x0: set(out, "MOV.B @R%u,R%u", OP_M, OP_N); break;
        case 0x1: set(out, "MOV.W @R%u,R%u", OP_M, OP_N); break;
        case 0x2: set(out, "MOV.L @R%u,R%u", OP_M, OP_N); break;
        case 0x3: set(out, "MOV R%u,R%u", OP_M, OP_N); break;
        case 0x4: set(out, "MOV.B @R%u+,R%u", OP_M, OP_N); break;
        case 0x5: set(out, "MOV.W @R%u+,R%u", OP_M, OP_N); break;
        case 0x6: set(out, "MOV.L @R%u+,R%u", OP_M, OP_N); break;
        case 0x7: set(out, "NOT R%u,R%u", OP_M, OP_N); break;
        case 0x8: set(out, "SWAP.B R%u,R%u", OP_M, OP_N); break;
        case 0x9: set(out, "SWAP.W R%u,R%u", OP_M, OP_N); break;
        case 0xA: set(out, "NEGC R%u,R%u", OP_M, OP_N); break;
        case 0xB: set(out, "NEG R%u,R%u", OP_M, OP_N); break;
        case 0xC: set(out, "EXTU.B R%u,R%u", OP_M, OP_N); break;
        case 0xD: set(out, "EXTU.W R%u,R%u", OP_M, OP_N); break;
        case 0xE: set(out, "EXTS.B R%u,R%u", OP_M, OP_N); break;
        case 0xF: set(out, "EXTS.W R%u,R%u", OP_M, OP_N); break;
        default: illegal(out); break;
        }
        break;
    case 0x7: set(out, "ADD #%d,R%u", (int)sext(opcode & 0xFFu, 8), OP_N); break;
    case 0x8: disasm_1000(addr, opcode, out); break;
    case 0x9:
        /* MOV.W @(disp,PC),Rn: word-aligned, so PC is used unmasked. */
        out->literal_addr = addr + 4u + (uint32_t)(opcode & 0xFFu) * 2u;
        resolve_literal(out, 2, reader, userdata);
        set(out, "MOV.W @(0x%03X,PC),R%u", (unsigned)(opcode & 0xFFu) * 2u, OP_N);
        break;
    case 0xA:
        branch_target(out, sext(opcode & 0xFFFu, 12) * 2);
        set(out, "BRA 0x%08X", out->target);
        out->flow = SH7021_DISASM_FLOW_BRANCH;
        out->has_delay_slot = 1;
        break;
    case 0xB:
        branch_target(out, sext(opcode & 0xFFFu, 12) * 2);
        set(out, "BSR 0x%08X", out->target);
        out->flow = SH7021_DISASM_FLOW_CALL;
        out->has_delay_slot = 1;
        break;
    case 0xC: disasm_1100(addr, opcode, out, reader, userdata); break;
    case 0xD:
        /* MOV.L @(disp,PC),Rn: the longword read forces PC to be masked down to
           a 4-byte boundary first, so an instruction in the upper half of a
           longword reads the same literal as one in the lower half. */
        out->literal_addr = (addr & ~3u) + 4u + (uint32_t)(opcode & 0xFFu) * 4u;
        resolve_literal(out, 4, reader, userdata);
        set(out, "MOV.L @(0x%03X,PC),R%u", (unsigned)(opcode & 0xFFu) * 4u, OP_N);
        break;
    case 0xE: set(out, "MOV #%d,R%u", (int)sext(opcode & 0xFFu, 8), OP_N); break;
    /* 0xF is the FPU space; the SH7021 has no FPU. */
    default: illegal(out); break;
    }
}

int sh7021_disasm_format(const SH7021DisasmInsn *insn, char *out, int out_size) {
    if (!insn || !out || out_size <= 0) return 0;
    int n = snprintf(out, (size_t)out_size, "%08X  %04X  %-28s", insn->addr, insn->opcode, insn->text);
    if (n < 0) return 0;
    if (n >= out_size) return out_size - 1;

    if (insn->has_literal) {
        if (insn->literal_resolved) {
            n += snprintf(out + n, (size_t)(out_size - n), "; [0x%08X] = 0x%08X",
                          insn->literal_addr, insn->literal_value);
        } else {
            n += snprintf(out + n, (size_t)(out_size - n), "; literal at 0x%08X", insn->literal_addr);
        }
    }
    if (n >= out_size) return out_size - 1;

    /* Trailing marker rather than a suffixed mnemonic: SH-1 has no delayed
       conditional branches, so the slot is a property of the listing, not of
       the opcode name. */
    if (insn->has_delay_slot) {
        n += snprintf(out + n, (size_t)(out_size - n), "%s; delay slot follows",
                      insn->has_literal ? " " : "");
    }
    if (n >= out_size) return out_size - 1;
    return n;
}
