#include "input/input.h"
#include "core/loopy_io.h"
#include <stddef.h>

#define MAX_KEY_BINDINGS 64

typedef struct KeyBinding { int key; PadButton button; } KeyBinding;
static KeyBinding key_bindings[MAX_KEY_BINDINGS];
static int key_binding_count;
static InputPortDevice port_device;

static void input_release_all_state(void) {
    loopy_io_update_pad(PAD_START, false);
    loopy_io_update_pad(PAD_L1, false);
    loopy_io_update_pad(PAD_R1, false);
    loopy_io_update_pad(PAD_A, false);
    loopy_io_update_pad(PAD_B, false);
    loopy_io_update_pad(PAD_C, false);
    loopy_io_update_pad(PAD_D, false);
    loopy_io_update_pad(PAD_UP, false);
    loopy_io_update_pad(PAD_DOWN, false);
    loopy_io_update_pad(PAD_LEFT, false);
    loopy_io_update_pad(PAD_RIGHT, false);
    loopy_io_set_mouse_button(0, false);
    loopy_io_set_mouse_button(1, false);
}

void input_initialize(void) {
    key_binding_count = 0;
    port_device = INPUT_PORT_GAMEPAD;
    loopy_io_update_pad(PAD_PRESENCE, true);
    loopy_io_set_mouse_connected(false);
}
void input_shutdown(void) { }
void input_clear_key_bindings(void) { key_binding_count = 0; }
void input_set_pad_button(PadButton button, bool pressed) {
    if (port_device == INPUT_PORT_GAMEPAD) loopy_io_update_pad(button, pressed);
}

void input_set_key_state(int key, bool pressed) {
    if (port_device != INPUT_PORT_GAMEPAD) return;
    for (int i = 0; i < key_binding_count; i++) {
        if (key_bindings[i].key == key) {
            input_set_pad_button(key_bindings[i].button, pressed);
        }
    }
}
void input_add_key_binding(int key, PadButton pad_button) {
    if (key_binding_count >= MAX_KEY_BINDINGS) return;
    key_bindings[key_binding_count].key = key;
    key_bindings[key_binding_count].button = pad_button;
    key_binding_count++;
}
void input_set_port_device(InputPortDevice device) {
    input_release_all_state();
    if (device == INPUT_PORT_MOUSE) {
        /* Toggle connection while switching so stale counter/button state from
         * the previous virtual device cannot leak into the newly selected
         * device. */
        loopy_io_set_mouse_connected(false);
        port_device = INPUT_PORT_MOUSE;
        loopy_io_update_pad(PAD_PRESENCE, false);
        loopy_io_set_mouse_connected(true);
    } else {
        port_device = INPUT_PORT_GAMEPAD;
        loopy_io_set_mouse_connected(false);
        loopy_io_update_pad(PAD_PRESENCE, true);
    }
}
InputPortDevice input_get_port_device(void) { return port_device; }
void input_set_mouse_button(int button, bool pressed) {
    if (port_device == INPUT_PORT_MOUSE) loopy_io_set_mouse_button(button, pressed);
}
void input_add_mouse_delta(int dx, int dy) {
    if (port_device == INPUT_PORT_MOUSE) loopy_io_add_mouse_delta(dx, dy);
}
