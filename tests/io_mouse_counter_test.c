#include "core/loopy_io.h"
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
    loopy_io_set_mouse_connected(true);

    /* Mouse detect/buttons are visible on every controller input byte. */
    expect_u16("mouse-detect-control0", loopy_io_reg_read16(0x010), 0xD0D0);

    /* Movement must not accumulate until VDP.MODE.MCNT is set. */
    loopy_io_set_controller_mode(false, false);
    loopy_io_add_mouse_delta(7, -3);
    expect_u16("counter-disabled-x", loopy_io_reg_read16(0x050), 0x5000);
    expect_u16("counter-disabled-y", loopy_io_reg_read16(0x052), 0x0000);

    /* When MCNT is enabled, deltas accumulate and each axis resets on read. */
    loopy_io_set_controller_mode(false, true);
    loopy_io_add_mouse_delta(3, -2);
    expect_u16("counter-enabled-x", loopy_io_reg_read16(0x050), 0x5003);
    expect_u16("counter-enabled-y", loopy_io_reg_read16(0x052), 0x0002);
    expect_u16("counter-reset-x", loopy_io_reg_read16(0x050), 0x5000);
    expect_u16("counter-reset-y", loopy_io_reg_read16(0x052), 0x0000);

    /* Buttons are negative logic in CONTROL_IN and CONTROL_MOUSE[0]. */
    loopy_io_set_mouse_connected(false);
    loopy_io_set_mouse_connected(true);
    loopy_io_set_mouse_button(0, true);
    loopy_io_set_mouse_button(1, false);
    expect_u16("left-pressed-control0", loopy_io_reg_read16(0x010), 0xC0C0);
    expect_u16("left-pressed-mousex", loopy_io_reg_read16(0x050), 0x4000);

    loopy_io_shutdown();
    puts("io_mouse_counter_test: OK");
    return 0;
}
