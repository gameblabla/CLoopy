/*
 * SH-1 disassembler checks.
 *
 * The interesting cases are the ones where being merely plausible is not good
 * enough: immediates whose signedness differs per instruction, PC-relative
 * operands with different alignment rules, and the SH-2 encodings this CPU does
 * not implement and must not be shown as if it did.
 *
 * The decoder was validated exhaustively against binutils sh-elf-objdump over
 * all 65536 encodings; these cases pin the parts of that agreement which are
 * easy to regress.
 */

#include "core/sh7021/sh7021_disasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static int literal_reader(uint32_t addr, int bytes, uint32_t *out_value, void *ud) {
    (void)ud;
    /* Stand-in literal pool: every readable longword is 0xDEADBEEF. */
    if (addr < 0x1000u) return 0;
    *out_value = (bytes == 2) ? 0xBEEFu : 0xDEADBEEFu;
    return 1;
}

static void expect(uint32_t addr, uint16_t opcode, const char *want) {
    SH7021DisasmInsn insn;
    sh7021_disasm_one(addr, opcode, &insn, NULL, NULL);
    if (strcmp(insn.text, want)) {
        printf("  FAIL %04X at %08X: got \"%s\", want \"%s\"\n", opcode, addr, insn.text, want);
        failures++;
    }
}

static void expect_illegal(uint16_t opcode, const char *why) {
    SH7021DisasmInsn insn;
    sh7021_disasm_one(0x1000u, opcode, &insn, NULL, NULL);
    if (!insn.illegal) {
        printf("  FAIL %04X decoded as \"%s\" but should be illegal (%s)\n", opcode, insn.text, why);
        failures++;
    }
}

int main(void) {
    /* Data moves and ALU. */
    expect(0x1000u, 0x2FE6u, "MOV.L R14,@-R15");
    expect(0x1000u, 0x4F22u, "STS.L PR,@-R15");
    expect(0x1000u, 0x6E43u, "MOV R4,R14");
    expect(0x1000u, 0x3F0Cu, "ADD R0,R15");
    expect(0x1000u, 0x0009u, "NOP");
    expect(0x1000u, 0x000Bu, "RTS");
    expect(0x1000u, 0x002Bu, "RTE");
    expect(0x1000u, 0x401Bu, "TAS.B @R0");

    /* CMP/EQ sign-extends its immediate; the logical immediates zero-extend.
       Confusing the two turns a test for -1 into a test for 255. */
    expect(0x1000u, 0x88FFu, "CMP/EQ #-1,R0");
    expect(0x1000u, 0x8801u, "CMP/EQ #1,R0");
    expect(0x1000u, 0xC9FFu, "AND #0xFF,R0");
    expect(0x1000u, 0xC8FFu, "TST #0xFF,R0");
    /* ADD #imm is signed too. */
    expect(0x1000u, 0x7FFFu, "ADD #-1,R15");
    expect(0x1000u, 0xE0FFu, "MOV #-1,R0");

    /* Branch targets are relative to PC+4, and the displacement is signed. */
    expect(0x1000u, 0xA000u, "BRA 0x00001004");
    expect(0x1000u, 0xAFFEu, "BRA 0x00001000");   /* -2 words: branch to self. */
    expect(0x1000u, 0x8BFAu, "BF 0x00000FF8");
    expect(0x1000u, 0x8900u, "BT 0x00001004");
    expect(0x1000u, 0xB000u, "BSR 0x00001004");

    /* SH-2 only: the interpreter rejects all of these, so the listing must too
       rather than suggest an instruction this CPU would refuse to run. */
    expect_illegal(0x0003u, "BSRF is SH-2 only");
    expect_illegal(0x0023u, "BRAF is SH-2 only");
    expect_illegal(0x0007u, "MUL.L is SH-2 only");
    expect_illegal(0x000Fu, "MAC.L is SH-2 only");
    expect_illegal(0x3005u, "DMULU.L is SH-2 only");
    expect_illegal(0x300Du, "DMULS.L is SH-2 only");
    expect_illegal(0x4010u, "DT is SH-2 only");
    expect_illegal(0x8D00u, "BT/S is SH-2 only");
    expect_illegal(0x8F00u, "BF/S is SH-2 only");
    expect_illegal(0xF000u, "the SH7021 has no FPU");

    /* Flow classification is what keeps the BIOS tracer from scoring an
       interrupt return as a call. */
    {
        SH7021DisasmInsn insn;
        struct { uint16_t op; int flow; const char *what; } cases[] = {
            { 0x400Bu, SH7021_DISASM_FLOW_CALL,   "JSR @R0" },
            { 0xB000u, SH7021_DISASM_FLOW_CALL,   "BSR" },
            { 0x402Bu, SH7021_DISASM_FLOW_BRANCH, "JMP @R0" },
            { 0xA000u, SH7021_DISASM_FLOW_BRANCH, "BRA" },
            { 0x000Bu, SH7021_DISASM_FLOW_RETURN, "RTS" },
            { 0x002Bu, SH7021_DISASM_FLOW_RETURN, "RTE" },
            { 0xC300u, SH7021_DISASM_FLOW_TRAP,   "TRAPA" },
            { 0x0009u, SH7021_DISASM_FLOW_NONE,   "NOP" },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            sh7021_disasm_one(0x1000u, cases[i].op, &insn, NULL, NULL);
            if (insn.flow != cases[i].flow) {
                printf("  FAIL %s: flow %d, want %d\n", cases[i].what, insn.flow, cases[i].flow);
                failures++;
            }
        }
        /* Every SH-1 delayed transfer has a slot; BT/BF do not. */
        sh7021_disasm_one(0x1000u, 0xA000u, &insn, NULL, NULL);
        if (!insn.has_delay_slot) { printf("  FAIL BRA should have a delay slot\n"); failures++; }
        sh7021_disasm_one(0x1000u, 0x8900u, &insn, NULL, NULL);
        if (insn.has_delay_slot) { printf("  FAIL BT must not have a delay slot\n"); failures++; }
    }

    /* PC-relative operands.  MOV.L masks PC to a longword boundary first, so an
       instruction in the upper half of a longword resolves to the same literal
       as one in the lower half.  MOV.W does not mask. */
    {
        SH7021DisasmInsn insn;
        sh7021_disasm_one(0x1002u, 0xD000u, &insn, literal_reader, NULL);
        if (insn.literal_addr != 0x1004u) {
            printf("  FAIL MOV.L @(0,PC) at 0x1002: literal 0x%08X, want 0x00001004 (PC must be masked)\n",
                   insn.literal_addr);
            failures++;
        }
        if (!insn.literal_resolved || insn.literal_value != 0xDEADBEEFu) {
            printf("  FAIL MOV.L literal not resolved through reader\n");
            failures++;
        }
        sh7021_disasm_one(0x1002u, 0x9000u, &insn, literal_reader, NULL);
        if (insn.literal_addr != 0x1006u) {
            printf("  FAIL MOV.W @(0,PC) at 0x1002: literal 0x%08X, want 0x00001006 (no masking)\n",
                   insn.literal_addr);
            failures++;
        }
        /* MOVA computes an address rather than loading, so there is nothing to
           resolve even when a reader is supplied. */
        sh7021_disasm_one(0x1002u, 0xC700u, &insn, literal_reader, NULL);
        if (!insn.has_literal || insn.literal_resolved || insn.literal_addr != 0x1004u) {
            printf("  FAIL MOVA: has=%d resolved=%d addr=0x%08X (want has=1 resolved=0 addr=0x00001004)\n",
                   insn.has_literal, insn.literal_resolved, insn.literal_addr);
            failures++;
        }
        /* An unreadable literal must leave the value unresolved, not zero. */
        sh7021_disasm_one(0x0002u, 0xD000u, &insn, literal_reader, NULL);
        if (insn.literal_resolved) { printf("  FAIL unreadable literal reported as resolved\n"); failures++; }
    }

    /* Illegal encodings render as data, so a listing never invents an opcode. */
    {
        SH7021DisasmInsn insn;
        sh7021_disasm_one(0x1000u, 0x0000u, &insn, NULL, NULL);
        if (!insn.illegal || strcmp(insn.text, ".WORD 0x0000")) {
            printf("  FAIL illegal rendering: \"%s\"\n", insn.text);
            failures++;
        }
    }

    if (failures) {
        printf("sh7021_disasm_test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("sh7021_disasm_test: OK\n");
    return 0;
}
