#ifndef LOOPY_FRONTEND_LOADER_H
#define LOOPY_FRONTEND_LOADER_H

#include "core/config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LoopyLaunchInfo {
    const char *cart_path;
    const char *bios_path;
    const char *sound_path;
    int frames;
    int frames_set;
    int headless;
    int input_device;      /* 0 = gamepad, 1 = mouse */
    int input_device_set;
    const char *record_cmdlist_path;
    const char *replay_cmdlist_path;
    const char *record_y4m_path;
    const char *load_state_path;
    int y4m_start;
    int y4m_step;
    int auto_cascadefx;
    int cascadefx_watch;
    int cascadefx_prof;
    int cascadefx_dpad_test;
    int cascadefx_dpad_prehhold_test;
    int cascadefx_dpad_tap_test;
    int cascadefx_pad_trace;
    int auto_anarch;
    int anarch_prof;
    int lr_hold_left;
    int lr_hold_left_start;
    int lr_mouse_dx;
    const char *printer_output_dir;
    int printer_trace;
} LoopyLaunchInfo;

int loopy_parse_common_args(int argc, char **argv, LoopyLaunchInfo *out, int default_frames);
int loopy_load_config(const LoopyLaunchInfo *launch, ConfigSystemInfo *config);
void loopy_config_free(ConfigSystemInfo *config);
char *loopy_make_state_path(const char *cart_path, int slot);

#ifdef __cplusplus
}
#endif

#endif
