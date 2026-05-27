#include "core/loopy_io.h"
#include "input/input.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void expect_u16(const char *name, uint16_t got, uint16_t expected) {
    if (got != expected) {
        fprintf(stderr, "%s: got %04X expected %04X\n", name, got, expected);
        exit(1);
    }
}

int main(void) {
    loopy_io_initialize();
    loopy_io_update_pad(PAD_PRESENCE, true);
    loopy_io_update_pad(PAD_START, true);
    loopy_io_update_pad(PAD_A, true);

    /* Boot/direct mode, CONTROL_OUT clear: a normal gamepad must not appear
     * through the matrix-latch registers.  This is the hardware behavior that
     * catches ROMs that forgot VDP.MODE.CMODE (bit 4). */
    loopy_io_set_controller_mode(false, false);
    expect_u16("direct-clear-control0", loopy_io_reg_read16(0x010), 0x0000);
    expect_u16("direct-clear-control1", loopy_io_reg_read16(0x012), 0x0000);
    expect_u16("direct-clear-control2", loopy_io_reg_read16(0x014), 0x0000);

    /* Matrix mode exposes automatically scanned, VCOUNT-timed gamepad rows. */
    loopy_io_set_controller_mode(true, false);
    loopy_io_matrix_scan_vcount(0x000);
    loopy_io_matrix_scan_vcount(0x020);
    loopy_io_matrix_scan_vcount(0x040);
    expect_u16("matrix-control0", loopy_io_reg_read16(0x010), 0x0103);
    expect_u16("matrix-control1", loopy_io_reg_read16(0x012), 0x0000);

    /* Writes to CONTROL_OUT are ignored in matrix mode.  They must not leave
     * stale direct outputs that make controls appear after CMODE is cleared. */
    loopy_io_reg_write16(0x054, 0x003F);
    loopy_io_set_controller_mode(false, false);
    expect_u16("direct-after-ignored-matrix-write", loopy_io_reg_read16(0x010), 0x0000);

    /* Direct mode still exposes raw input pins when software explicitly drives
     * the output lines.  This is direct-control behavior, not the automatic
     * matrix scan used by normal games. */
    loopy_io_reg_write16(0x054, 0x0001);
    expect_u16("direct-output-row0", loopy_io_reg_read16(0x010), 0x0303);
    loopy_io_reg_write16(0x054, 0x0002);
    expect_u16("direct-output-row1", loopy_io_reg_read16(0x010), 0x0101);

    loopy_io_shutdown();
    puts("io_controller_mode_test: OK");
    return 0;
}
