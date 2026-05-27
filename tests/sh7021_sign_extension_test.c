#include "core/sh7021/sh7021_local.h"
#include "core/sh7021/sh7021_interpreter.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

SH7021CPU sh7021;
static uint16_t fake_read16 = 0;
static uint32_t fake_read32 = 0;
static uint32_t last_write32_addr = 0;
static uint32_t last_write32_data = 0;
static unsigned write32_count = 0;
static int exception_seen = 0;

uint8_t sh7021_bus_read8(uint32_t addr) { (void)addr; return (uint8_t)fake_read16; }
uint16_t sh7021_bus_read16(uint32_t addr) { (void)addr; return fake_read16; }
uint32_t sh7021_bus_read32(uint32_t addr) { (void)addr; return fake_read32; }
void sh7021_bus_write8(uint32_t addr, uint8_t data) { (void)addr; (void)data; }
void sh7021_bus_write16(uint32_t addr, uint16_t data) { (void)addr; (void)data; }
void sh7021_bus_write32(uint32_t addr, uint32_t data) {
    last_write32_addr = addr;
    last_write32_data = data;
    write32_count++;
}
void sh7021_raise_exception(int vector_id) { (void)vector_id; exception_seen = 1; }
void sh7021_irq_check(void) { }
void sh7021_block_irq_next(void) { sh7021.irq_delay = 1; }
int sh7021_service_pending_irq(void) { return 0; }
void sh7021_raise_slot_illegal(void) { exception_seen = 2; }
void sh7021_set_pc(uint32_t new_pc) { sh7021.pc = new_pc; sh7021.m_delay = 0; }
void sh7021_set_sr(uint32_t new_sr) { sh7021.sr = new_sr & 0x3F3u; }

static int expect_u32(const char *name, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %08X, expected %08X\n", name, got, want);
        return 1;
    }
    return 0;
}

static uint32_t mach10(uint32_t v) {
    v &= 0x3ffu;
    return (v & 0x200u) ? (v | 0xfffffc00u) : v;
}

static void reset_cpu(void) {
    memset(&sh7021, 0, sizeof(sh7021));
    fake_read16 = 0;
    fake_read32 = 0;
    last_write32_addr = 0;
    last_write32_data = 0;
    write32_count = 0;
    exception_seen = 0;
    sh7021.gpr[15] = 0x09001000u;
}

static int expect_illegal_opcode(uint16_t opcode, const char *name) {
    reset_cpu();
    fake_read32 = 0x0e00beefu;
    sh7021.pc = 0x0e001002u; /* post-fetch PC, as used by sh7021_run() */
    sh7021.vbr = 0;
    sh7021_interpreter_run(opcode);
    int fails = 0;
    fails += expect_u32(name, sh7021.pc, 0x0e00beefu);
    fails += expect_u32("illegal opcode pushes SR+PC", write32_count, 2u);
    return fails;
}

int main(void) {
    int fails = 0;
    reset_cpu();

    /* EXTS.W must discard stale high bits before sign-extension.  Cascade FX
       exposed the previous bug: 0xFFFEFF00 was kept as 0xFFFEFF00 instead of
       becoming 0xFFFFFF00, which broke projection division denominators. */
    sh7021.gpr[4] = 0xFFFEFF00u;
    sh7021_interpreter_run(0x654Fu); /* exts.w r4,r5 */
    fails += expect_u32("EXTS.W masks source width", sh7021.gpr[5], 0xFFFFFF00u);

    sh7021.gpr[4] = 0x00010180u;
    sh7021_interpreter_run(0x654Eu); /* exts.b r4,r5 */
    fails += expect_u32("EXTS.B masks source width", sh7021.gpr[5], 0xFFFFFF80u);

    fake_read16 = 0xFF00u;
    sh7021.gpr[4] = 0x09000000u;
    sh7021_interpreter_run(0x6541u); /* mov.w @r4,r5 */
    fails += expect_u32("MOV.W load sign-extension", sh7021.gpr[5], 0xFFFFFF00u);

    fails += expect_u32("No exception during sign extension checks", (uint32_t)exception_seen, 0u);

    /* SH-1/SH7021 has only bits 9..0 of MACH.  LDS into MACH and STS from
       MACH must sign-extend bit 9 through bits 31..10. */
    reset_cpu();
    sh7021.gpr[4] = 0x12345678u;
    sh7021_interpreter_run(0x440au); /* lds r4,mach */
    fails += expect_u32("LDS Rm,MACH canonicalizes 10-bit negative", sh7021.mach, mach10(0x12345678u));
    sh7021_interpreter_run(0x050au); /* sts mach,r5 */
    fails += expect_u32("STS MACH,Rn sign-extends SH-1 MACH", sh7021.gpr[5], mach10(0x12345678u));

    reset_cpu();
    sh7021.gpr[4] = 0x000001ffu;
    sh7021_interpreter_run(0x440au); /* lds r4,mach */
    fails += expect_u32("LDS Rm,MACH canonicalizes 10-bit positive", sh7021.mach, 0x000001ffu);

    reset_cpu();
    fake_read32 = 0x00000300u;
    sh7021.gpr[4] = 0x09000200u;
    sh7021_interpreter_run(0x4406u); /* lds.l @r4+,mach */
    fails += expect_u32("LDS.L @Rm+,MACH sign-extends", sh7021.mach, 0xffffff00u);
    fails += expect_u32("LDS.L @Rm+,MACH increments", sh7021.gpr[4], 0x09000204u);

    reset_cpu();
    sh7021.mach = 0x12345678u;
    sh7021.gpr[4] = 0x09000204u;
    sh7021_interpreter_run(0x4402u); /* sts.l mach,@-r4 */
    fails += expect_u32("STS.L MACH,@-Rn decrements", sh7021.gpr[4], 0x09000200u);
    fails += expect_u32("STS.L MACH,@-Rn writes SH-1 value", last_write32_data, mach10(0x12345678u));

    reset_cpu();
    sh7021.mach = 0x000001ffu;
    sh7021.macl = 0xffffffffu;
    sh7021.gpr[0] = 0x09001000u;
    sh7021.gpr[1] = 0x09002000u;
    fake_read16 = 0x7fffu;
    sh7021_interpreter_run(0x410fu); /* mac.w @r0+,@r1+ */
    fails += expect_u32("MAC.W SH-1 42-bit carry canonicalizes MACH", sh7021.mach, 0xfffffe00u);
    fails += expect_u32("MAC.W increments Rm", sh7021.gpr[0], 0x09001002u);
    fails += expect_u32("MAC.W increments Rn", sh7021.gpr[1], 0x09002002u);

    reset_cpu();
    sh7021.sr = 0x00000002u; /* S bit: saturating MAC.W */
    sh7021.macl = 0x7fffffffu;
    fake_read16 = 0x0001u;
    sh7021_interpreter_run(0x410fu); /* mac.w @r0+,@r1+ */
    fails += expect_u32("MAC.W saturation sets SH-1 MACH overflow flag", sh7021.mach, 0x00000001u);
    fails += expect_u32("MAC.W positive saturation clamps MACL", sh7021.macl, 0x7fffffffu);

    /* These encodings are valid on SH-2 but illegal on SH-1/SH7021. */
    fails += expect_illegal_opcode(0x8f00u, "BF/S is illegal on SH-1");
    fails += expect_illegal_opcode(0x8d00u, "BT/S is illegal on SH-1");
    fails += expect_illegal_opcode(0x0023u, "BRAF is illegal on SH-1");
    fails += expect_illegal_opcode(0x0003u, "BSRF is illegal on SH-1");
    fails += expect_illegal_opcode(0x4010u, "DT is illegal on SH-1");
    fails += expect_illegal_opcode(0x3105u, "DMULU.L is illegal on SH-1");
    fails += expect_illegal_opcode(0x310du, "DMULS.L is illegal on SH-1");
    fails += expect_illegal_opcode(0x010fu, "MAC.L is illegal on SH-1");
    fails += expect_illegal_opcode(0x0107u, "MUL.L is illegal on SH-1");

    if (fails) return 1;
    puts("SH7021 sign-extension / MACH / SH-1 opcode eligibility test passed");
    return 0;
}
