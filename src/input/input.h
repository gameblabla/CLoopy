#ifndef LOOPY_INPUT_H
#define LOOPY_INPUT_H
#include <stdbool.h>

typedef enum PadButton {
    PAD_PRESENCE = 0x0001,
    PAD_START = 0x0002,
    PAD_L1 = 0x0004,
    PAD_R1 = 0x0008,
    PAD_A = 0x0010,
    PAD_D = 0x0020,
    PAD_C = 0x0040,
    PAD_B = 0x0080,
    PAD_UP = 0x0100,
    PAD_DOWN = 0x0200,
    PAD_LEFT = 0x0400,
    PAD_RIGHT = 0x0800
} PadButton;

typedef enum InputPortDevice {
    INPUT_PORT_GAMEPAD = 0,
    INPUT_PORT_MOUSE = 1
} InputPortDevice;

void input_initialize(void);
void input_shutdown(void);
void input_set_key_state(int key, bool pressed);
void input_set_pad_button(PadButton button, bool pressed);
void input_add_key_binding(int key, PadButton pad_button);
void input_clear_key_bindings(void);
void input_set_port_device(InputPortDevice device);
InputPortDevice input_get_port_device(void);
void input_set_mouse_button(int button, bool pressed);
void input_add_mouse_delta(int dx, int dy);

#endif
