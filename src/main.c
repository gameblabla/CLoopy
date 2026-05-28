#include "core/system.h"
#include "core/sh7021/sh7021_bus.h"
#include "frontend/loader.h"
#include "frontend/cmdlist.h"
#include "input/input.h"
#include "core/loopy_io.h"
#include "video/video.h"
#include "sound/sound.h"
#include "sound/oki_adpcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_all_pad_buttons_released(void) {
    input_set_pad_button(PAD_START, false);
    input_set_pad_button(PAD_A, false);
    input_set_pad_button(PAD_B, false);
    input_set_pad_button(PAD_C, false);
    input_set_pad_button(PAD_D, false);
    input_set_pad_button(PAD_UP, false);
    input_set_pad_button(PAD_DOWN, false);
    input_set_pad_button(PAD_LEFT, false);
    input_set_pad_button(PAD_RIGHT, false);
    input_set_pad_button(PAD_L1, false);
    input_set_pad_button(PAD_R1, false);
}

static void apply_cascadefx_autoinput(int frame_index, int dpad_test, int dpad_prehold_test, int dpad_tap_test) {
    /* Deterministic headless smoke path for CascadeFX: pass the title/menu
       screens and enter the first gameplay mode without depending on SDL. */
    set_all_pad_buttons_released();
    if ((frame_index >= 55 && frame_index < 70) ||
        (frame_index >= 115 && frame_index < 130)) {
        input_set_pad_button(PAD_START, true);
    }
    if ((frame_index >= 175 && frame_index < 190) ||
        (frame_index >= 235 && frame_index < 250)) {
        input_set_pad_button(PAD_A, true);
    }
    if (dpad_prehold_test) {
        /* Hold LEFT before the first gameplay piece is controllable.
           CascadeFX uses edge-triggered movement, so this tests whether the
           edge is consumed before the active piece exists. */
        if (frame_index >= 250 && frame_index < 450) input_set_pad_button(PAD_LEFT, true);
    } else if (dpad_tap_test) {
        /* Short human-like taps.  CascadeFX only samples pad edges once per
           completed game loop, so these can be missed when a loop spans many
           displayed frames. */
        if ((frame_index >= 360 && frame_index < 363) ||
            (frame_index >= 620 && frame_index < 623) ||
            (frame_index >= 880 && frame_index < 883)) {
            input_set_pad_button(PAD_LEFT, true);
        }
        if ((frame_index >= 430 && frame_index < 433) ||
            (frame_index >= 690 && frame_index < 693) ||
            (frame_index >= 950 && frame_index < 953)) {
            input_set_pad_button(PAD_RIGHT, true);
        }
        if ((frame_index >= 500 && frame_index < 503) ||
            (frame_index >= 760 && frame_index < 763) ||
            (frame_index >= 1020 && frame_index < 1023)) {
            input_set_pad_button(PAD_DOWN, true);
        }
    } else if (dpad_test) {
        /* Human-scale held inputs after gameplay is visible.  These are long
           enough that a VCOUNT-latched hardware matrix should capture them;
           short taps can be missed by a slow game loop and are not diagnostic. */
        if (frame_index >= 360 && frame_index < 450) input_set_pad_button(PAD_LEFT, true);
        if (frame_index >= 620 && frame_index < 710) input_set_pad_button(PAD_RIGHT, true);
        if (frame_index >= 880 && frame_index < 970) input_set_pad_button(PAD_DOWN, true);
    }
}


static void apply_anarch_autoinput(int frame_index) {
    /* Deterministic headless path for the Anarch Loopy homebrew.  The title
       menu uses SFG_KEY_A, which this port maps to the Loopy right trigger. */
    set_all_pad_buttons_released();
    if ((frame_index >= 50 && frame_index < 70) ||
        (frame_index >= 110 && frame_index < 130) ||
        (frame_index >= 170 && frame_index < 190)) {
        input_set_pad_button(PAD_R1, true);
        input_set_pad_button(PAD_START, true);
        input_set_pad_button(PAD_A, true);
    }
    /* Apply a simple movement pattern after entering gameplay. */
    if (frame_index >= 360 && frame_index < 900) input_set_pad_button(PAD_UP, true);
    if (frame_index >= 900 && frame_index < 1320) input_set_pad_button(PAD_RIGHT, true);
}

static void apply_little_romance_hold_left(int frame_index, int start_frame, int mouse_dx) {
    if (frame_index < start_frame) return;
    if (input_get_port_device() == INPUT_PORT_MOUSE) {
        input_add_mouse_delta(mouse_dx, 0);
    } else {
        input_set_pad_button(PAD_LEFT, true);
    }
}

static int16_t read_be_i16(uint32_t addr) {
    return (int16_t)sh7021_bus_read16(addr);
}

static void print_cascadefx_watch(int frame_index) {
    int16_t type = read_be_i16(0x09000620u);
    int16_t rot = read_be_i16(0x09000622u);
    int16_t x = read_be_i16(0x09000624u);
    int16_t y = read_be_i16(0x09000626u);
    printf("CASCADEFx frame=%d type=%d rot=%d x=%d y=%d\n", frame_index, type, rot, x, y);
}

static int is_little_romance_rom(const char *path) {
    const char needle[] = "little romance";
    if (!path) return 0;
    for (const char *p = path; *p; p++) {
        unsigned i = 0;
        while (needle[i] && p[i]) {
            char a = p[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != needle[i]) break;
            i++;
        }
        if (!needle[i]) return 1;
    }
    return 0;
}


typedef struct HeadlessY4MWriter {
    FILE *file;
} HeadlessY4MWriter;

static unsigned char clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

static int y4m_open(HeadlessY4MWriter *w, const char *path) {
    if (!w || !path) return -1;
    w->file = fopen(path, "wb");
    if (!w->file) return -1;
    /* NTSC Loopy timing is about 59.8261 Hz.  Use 598261/10000 so the
       inspection stream preserves emulator frame cadence without needing a
       nonstandard floating-point framerate. */
    fprintf(w->file, "YUV4MPEG2 W256 H240 F598261:10000 Ip A1:1 C444\n");
    return ferror(w->file) ? -1 : 0;
}

static void y4m_write_frame(HeadlessY4MWriter *w) {
    if (!w || !w->file) return;
    const uint16_t *fb = system_get_display_output();
    const int count = VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT;
    unsigned char *planes = (unsigned char *)malloc((size_t)count * 3u);
    if (!planes) return;
    unsigned char *Y = planes;
    unsigned char *U = planes + count;
    unsigned char *V = planes + count * 2;
    for (int i = 0; i < count; i++) {
        uint16_t c = fb[i];
        int r5 = (int)((c >> 10) & 31u);
        int g5 = (int)((c >> 5) & 31u);
        int b5 = (int)(c & 31u);
        int r = (r5 << 3) | (r5 >> 2);
        int g = (g5 << 3) | (g5 >> 2);
        int b = (b5 << 3) | (b5 >> 2);
        /* BT.601 full-range conversion for inspection video. */
        Y[i] = clamp_u8(( 77 * r + 150 * g +  29 * b) >> 8);
        U[i] = clamp_u8(128 + ((-43 * r -  85 * g + 128 * b) >> 8));
        V[i] = clamp_u8(128 + ((128 * r - 107 * g -  21 * b) >> 8));
    }
    fputs("FRAME\n", w->file);
    fwrite(Y, 1, (size_t)count, w->file);
    fwrite(U, 1, (size_t)count, w->file);
    fwrite(V, 1, (size_t)count, w->file);
    free(planes);
}

static void y4m_close(HeadlessY4MWriter *w) {
    if (w && w->file) fclose(w->file);
    if (w) w->file = NULL;
}



static void montage_wav_u16(FILE *f, uint16_t v) {
    fputc((int)(v & 0xFFu), f);
    fputc((int)(v >> 8), f);
}

static void montage_wav_u32(FILE *f, uint32_t v) {
    montage_wav_u16(f, (uint16_t)v);
    montage_wav_u16(f, (uint16_t)(v >> 16));
}

static void montage_write_samples(FILE *f, const int16_t *samples, int frames, uint32_t *bytes) {
    if (!f || !samples || frames <= 0 || !bytes) return;
    size_t wrote = fwrite(samples, sizeof(int16_t), (size_t)frames * 2u, f);
    *bytes += (uint32_t)(wrote * sizeof(int16_t));
}

static int write_wanwan_oki_montage(const char *path) {
    static const uint8_t order[22] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16
    };
    const int rate = SOUND_TARGET_SAMPLE_RATE;
    const int gap_frames = rate * 2;
    const int max_phrase_frames = rate * 5;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Could not open Wanwan OKI montage output: %s\n", path ? path : "(null)");
        return 1;
    }

    fwrite("RIFF", 1, 4, f);
    montage_wav_u32(f, 0);
    fwrite("WAVEfmt ", 1, 8, f);
    montage_wav_u32(f, 16);
    montage_wav_u16(f, 1);
    montage_wav_u16(f, 2);
    montage_wav_u32(f, (uint32_t)rate);
    montage_wav_u32(f, (uint32_t)rate * 2u * 2u);
    montage_wav_u16(f, 4);
    montage_wav_u16(f, 16);
    fwrite("data", 1, 4, f);
    montage_wav_u32(f, 0);

    OkiAdpcm *oki = oki_adpcm_create((float)rate, NULL, 0, 1);
    if (!oki) {
        fclose(f);
        fprintf(stderr, "Could not create Wanwan OKI decoder for montage\n");
        return 1;
    }

    int16_t zeros[512 * 2];
    int16_t block[512 * 2];
    memset(zeros, 0, sizeof(zeros));
    uint32_t data_bytes = 0;

    printf("Wanwan OKI montage order:");
    for (int i = 0; i < 22; i++) printf(" %02X", order[i]);
    printf("\n");

    for (int cmd_i = 0; cmd_i < 22; cmd_i++) {
        uint8_t command = order[cmd_i];
        printf("Wanwan OKI montage command %02X at item %d/22\n", command, cmd_i + 1);
        oki_adpcm_reset(oki);
        oki_adpcm_debug_play_command(oki, command);
        int phrase_frames = 0;
        int trailing_silence = 0;
        while (phrase_frames < max_phrase_frames) {
            int n = max_phrase_frames - phrase_frames;
            if (n > 512) n = 512;
            for (int i = 0; i < n; i++) {
                float v = oki_adpcm_generate(oki) * 0.85f;
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                int16_t s16 = (int16_t)(v * 32767.0f);
                block[i * 2 + 0] = s16;
                block[i * 2 + 1] = s16;
            }
            montage_write_samples(f, block, n, &data_bytes);
            phrase_frames += n;
            if (!oki_adpcm_debug_active(oki)) {
                trailing_silence += n;
                if (trailing_silence >= rate / 10) break;
            } else {
                trailing_silence = 0;
            }
        }
        int gap_left = gap_frames;
        while (gap_left > 0) {
            int n = gap_left > 512 ? 512 : gap_left;
            montage_write_samples(f, zeros, n, &data_bytes);
            gap_left -= n;
        }
    }

    oki_adpcm_destroy(oki);
    uint32_t riff_size = 36u + data_bytes;
    fseek(f, 4, SEEK_SET);
    montage_wav_u32(f, riff_size);
    fseek(f, 40, SEEK_SET);
    montage_wav_u32(f, data_bytes);
    fclose(f);
    printf("Wrote Wanwan OKI montage WAV: %s\n", path);
    return 0;
}

static int run_cmdlist_print_extract(const LoopyLaunchInfo *launch) {
    LoopyCmdListReader reader;
    if (loopy_cmdlist_reader_open(&reader, launch->replay_cmdlist_path) != 0) {
        fprintf(stderr, "Could not open command-list replay: %s\n", launch->replay_cmdlist_path);
        return 1;
    }
    uint32_t total = reader.frame_count;
    if (total == 0) {
        loopy_cmdlist_reader_close(&reader);
        fprintf(stderr, "Command-list replay has no frames\n");
        return 1;
    }
    uint32_t frame = launch->frames_set ? (uint32_t)launch->frames : (total - 1u);
    if (frame >= total) frame = total - 1u;
    uint16_t *fb = (uint16_t *)malloc((size_t)VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT * sizeof(uint16_t));
    if (!fb) { loopy_cmdlist_reader_close(&reader); return 1; }
    if (loopy_cmdlist_reader_read_framebuffer(&reader, frame, fb, VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT) != 0) {
        fprintf(stderr, "Could not read command-list frame %u\n", frame);
        free(fb);
        loopy_cmdlist_reader_close(&reader);
        return 1;
    }
    loopy_io_initialize();
    if (launch->printer_output_dir) loopy_io_printer_set_output_dir(launch->printer_output_dir);
    if (launch->printer_trace) loopy_io_printer_set_trace(true);
    loopy_io_printer_frame_snapshot(fb, VIDEO_DISPLAY_WIDTH, VIDEO_DISPLAY_HEIGHT);
    loopy_io_trigger_printer_sensors();
    loopy_io_reg_write16(0x040, 0x00FFu);
    loopy_io_reg_write16(0x042, 0x5A51u);
    loopy_io_reg_write16(0x042, 0x5A50u);
    loopy_io_shutdown();
    printf("[Printer] Extracted command-list frame %u/%u as composed-frame print snapshot\n", frame, total);
    free(fb);
    loopy_cmdlist_reader_close(&reader);
    return 0;
}

int main(int argc, char **argv) {
    LoopyLaunchInfo launch;
    if (loopy_parse_common_args(argc, argv, &launch, 4800) != 0) {
        printf("Args: <game ROM> <BIOS> [sound BIOS] [OKI ADPCM ROM] [--frames N] [--device gamepad|mouse] [--mouse|--gamepad] [--record-y4m file] [--record-wav file] [--load-state file] [--oki-rom file] [--wanwan-oki-montage-wav file] [--y4m-start N] [--y4m-step N] [--auto-cascadefx] [--cascadefx-watch] [--cascadefx-prof] [--cascadefx-dpad-test] [--cascadefx-dpad-prehold-test] [--cascadefx-dpad-tap-test] [--cascadefx-pad-trace] [--auto-anarch] [--anarch-prof] [--lr-hold-left] [--lr-hold-left-start N] [--lr-mouse-dx N] [--printer-output-dir dir] [--printer-trace]\n");
        printf("Default: --frames 4800, which waits long enough for Little Romance and the other retail title screens under the MAME-derived SH7021 core.\n");
        return 1;
    }

    if (launch.wanwan_oki_montage_path) {
        return write_wanwan_oki_montage(launch.wanwan_oki_montage_path);
    }

    if (launch.replay_cmdlist_path) {
        return run_cmdlist_print_extract(&launch);
    }

    ConfigSystemInfo config;
    if (loopy_load_config(&launch, &config) != 0) return 1;

    system_initialize(&config);
    const char *load_state_path = launch.load_state_path;
    if (!load_state_path || !*load_state_path) load_state_path = getenv("LOOPY_LOAD_STATE");
    if (load_state_path && *load_state_path) {
        if (system_load_state(load_state_path) != 0) {
            fprintf(stderr, "Could not load state %s\n", load_state_path);
            if (launch.load_state_path && *launch.load_state_path) { loopy_config_free(&config); return 1; }
        } else fprintf(stderr, "Loaded state %s\n", load_state_path);
    }
    if (launch.printer_output_dir) loopy_io_printer_set_output_dir(launch.printer_output_dir);
    if (launch.printer_trace) loopy_io_printer_set_trace(true);
    if (launch.input_device_set) input_set_port_device(launch.input_device ? INPUT_PORT_MOUSE : INPUT_PORT_GAMEPAD);
    else if (is_little_romance_rom(launch.cart_path)) input_set_port_device(INPUT_PORT_MOUSE);
    HeadlessY4MWriter y4m = {0};
    if (launch.record_y4m_path && y4m_open(&y4m, launch.record_y4m_path) != 0) {
        fprintf(stderr, "Could not open Y4M output: %s\n", launch.record_y4m_path);
    }
    if (launch.record_wav_path && sound_wav_open(launch.record_wav_path) != 0) {
        fprintf(stderr, "Could not open WAV output: %s\n", launch.record_wav_path);
    }
    for (int i = 0; i < launch.frames; i++) {
        LOOPY_DEBUG_PRINTF("[Main] Running frame %d/%d\n", i + 1, launch.frames);
        if (launch.auto_cascadefx) apply_cascadefx_autoinput(i + 1, launch.cascadefx_dpad_test, launch.cascadefx_dpad_prehhold_test, launch.cascadefx_dpad_tap_test);
        if (launch.auto_anarch) apply_anarch_autoinput(i + 1);
        if (launch.lr_hold_left) apply_little_romance_hold_left(i + 1, launch.lr_hold_left_start, launch.lr_mouse_dx);
        if (launch.cascadefx_pad_trace) loopy_io_controller_debug_reset();
        system_run();
        sound_wav_write_frame();
        if (launch.cascadefx_watch) print_cascadefx_watch(i + 1);
        if (launch.cascadefx_pad_trace) {
            unsigned r0, r1, r2; uint16_t v0, v1, v2;
            loopy_io_controller_debug_get(0, &r0, &v0);
            loopy_io_controller_debug_get(1, &r1, &v1);
            loopy_io_controller_debug_get(2, &r2, &v2);
            printf("CASCADEFx_PAD frame=%d r0=%u v0=%04X r1=%u v1=%04X r2=%u v2=%04X\n",
                   i + 1, r0, v0, r1, v1, r2, v2);
        }
        if (launch.cascadefx_prof || launch.anarch_prof) {
            long long vc, va, vb, rc, ra, cc, ca, ic, ia, dr, dmc, dma, dms;
            sh7021_bus_prof_get(&vc, &va, &vb, &rc, &ra);
            sh7021_bus_prof_get_cart(&cc, &ca);
            sh7021_bus_prof_get_internal(&ic, &ia, &dr);
            sh7021_bus_prof_get_dma(&dmc, &dma, &dms);
            printf("BUS_PROF frame=%d vdp_accesses=%lld vdp_bytes=%lld vdp_wait=%lld ram_accesses=%lld ram_wait=%lld cart_accesses=%lld cart_wait=%lld internal_accesses=%lld internal_wait=%lld dram_refresh=%lld dma_accesses=%lld dma_single=%lld dma_model_cycles=%lld\n",
                   i + 1, va, vb, vc, ra, rc, ca, cc, ia, ic, dr, dma, dms, dmc);
            if (launch.anarch_prof) {
                uint32_t anarch_ticks = sh7021_bus_read32(0x0900012Cu);
                uint32_t anarch_state = sh7021_bus_read8(0x09000650u);
                printf("ANARCH_WATCH frame=%d ticks=%u state=%u\n", i + 1, anarch_ticks, anarch_state);
            }
            sh7021_bus_prof_reset();
        }
        int frame_no = i + 1;
        if (y4m.file && frame_no >= launch.y4m_start && ((frame_no - launch.y4m_start) % launch.y4m_step) == 0) {
            y4m_write_frame(&y4m);
        }
    }
    y4m_close(&y4m);
    sound_wav_close();
    system_shutdown();
    loopy_config_free(&config);
    return 0;
}
