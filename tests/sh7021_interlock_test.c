#include "core/sh7021/sh7021_interlock.h"
#include <stdio.h>
#include <stdlib.h>

static void expecti(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d expected %d\n", name, got, want);
        exit(1);
    }
}

int main(void) {
    /* 0xD is MOV.L @(disp,PC),Rn on SH-1 and must create a memory load/use
       bubble.  0xE is MOV #imm,Rn and must not. */
    expecti("PC-relative long load producer", sh7021_interlock_loaded_gpr(0xD100u), 1);
    expecti("immediate MOV is not load producer", sh7021_interlock_loaded_gpr(0xE100u), -1);

    /* MOV R1,R2 consumes R1. */
    expecti("memory load/use stall", sh7021_interlock_stall_cycles(1, 1, 0x6213u), 1);
    expecti("memory load/independent no stall", sh7021_interlock_stall_cycles(1, 1, 0xE200u), 0);
    expecti("invalid saved load register is harmless", sh7021_interlock_stall_cycles(1, 31, 0x6213u), 0);

    /* An immediate MOV itself does not arm the interlock. */
    int imm_loaded = sh7021_interlock_loaded_gpr(0xE100u);
    expecti("immediate/use pair extra cycles", imm_loaded < 0 ? 0 : sh7021_interlock_stall_cycles(1, (unsigned)imm_loaded, 0x6213u), 0);

    puts("sh7021_interlock_test: OK");
    return 0;
}
