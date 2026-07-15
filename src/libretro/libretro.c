/* CLoopy libretro core.
 *
 * Drives the emulator from the libretro pull model: one system_run() per
 * retro_run(), video and audio uploaded afterwards.  Savestates are the core's
 * existing in-memory state blob, which is what makes the frontend's rewind work
 * - rewind is just retro_serialize() every frame and retro_unserialize() to go
 * back, so it needs a blob that is fixed-size and cheap.  Cartridge SRAM is
 * exposed through RETRO_MEMORY_SAVE_RAM so the frontend owns the .srm rather
 * than the core writing files itself.
 */

#include "libretro.h"

#include "core/cart.h"
#include "core/cart_meta.h"
#include "core/config.h"
#include "core/sh7021/sh7021.h"
#include "core/system.h"
#include "frontend/loader.h" /* loopy_config_free() */
#include "input/input.h"
#include "sound/sound.h"
#include "video/video.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Documented NTSC Loopy timing: 21.477272 MHz / (1365 * 263). */
#define LOOPY_FPS 59.8261
#define FRAME_RATE_NUM 598261u
#define FRAME_RATE_DEN 10000u

#define MAX_AUDIO_FRAMES 2048

#if defined(_WIN32)
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static ConfigSystemInfo cfg;
static bool game_loaded = false;
static uint32_t audio_accum = 0;
static int16_t audio_buf[MAX_AUDIO_FRAMES * 2];

/* Option state, applied on load and on frontend option changes. */
static int opt_idle_skip_mode = LOOPY_IDLE_SKIP_AUTO;
static int opt_input_device = INPUT_PORT_GAMEPAD;

static void lr_state_freeze_bound(void);
static size_t lr_state_bound;

static void fallback_log(enum retro_log_level level, const char *fmt, ...) {
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void log_msg(enum retro_log_level level, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (log_cb) log_cb(level, "%s", line);
    else fallback_log(level, "%s", line);
}

/* ---------------------------------------------------------------- options */

#define OPT_IDLE_SKIP "cloopy_idle_skip"
#define OPT_INPUT_DEVICE "cloopy_input_device"

static const struct retro_core_option_v2_definition option_defs[] = {
    {OPT_IDLE_SKIP,
     "Idle Loop Skip",
     NULL,
     "Fast-forward the CPU through spin loops that provably cannot observe anything changing before the next "
     "hardware event. Emulation is bit-for-bit identical either way; this only decides how much host CPU is spent "
     "reproducing a wait. 'Retail only' applies it to Casio-published cartridges, which are what it is validated "
     "against, and leaves homebrew alone.",
     NULL,
     NULL,
     {{"auto", "Retail cartridges only"}, {"on", "Always"}, {"off", "Never"}, {NULL, NULL}},
     "auto"},
    {OPT_INPUT_DEVICE,
     "Controller Port Device",
     NULL,
     "The Loopy controller port holds either the gamepad or the Loopy Mouse. Only a few titles read the mouse "
     "(PC Collection, Little Romance, Lupiton's Wonder Palette, Loopy Town); the rest ignore it, so selecting Mouse "
     "leaves them with no working input.",
     NULL,
     NULL,
     {{"gamepad", "Gamepad"}, {"mouse", "Loopy Mouse"}, {NULL, NULL}},
     "gamepad"},
    {NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};

static struct retro_core_options_v2 options_v2 = {NULL, (struct retro_core_option_v2_definition *)option_defs};

static const char *option_value(const char *key, const char *fallback) {
    struct retro_variable var = {key, NULL};
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) return var.value;
    return fallback;
}

/* Reads the options the frontend currently holds. Returns true if anything that
   needs re-applying changed. */
static bool poll_options(void) {
    bool changed = false;
    const char *v = option_value(OPT_IDLE_SKIP, "auto");
    int mode = LOOPY_IDLE_SKIP_AUTO;
    if (!strcmp(v, "on")) mode = LOOPY_IDLE_SKIP_ON;
    else if (!strcmp(v, "off")) mode = LOOPY_IDLE_SKIP_OFF;
    if (mode != opt_idle_skip_mode) {
        opt_idle_skip_mode = mode;
        changed = true;
    }

    v = option_value(OPT_INPUT_DEVICE, "gamepad");
    int dev = !strcmp(v, "mouse") ? INPUT_PORT_MOUSE : INPUT_PORT_GAMEPAD;
    if (dev != opt_input_device) {
        opt_input_device = dev;
        changed = true;
    }
    return changed;
}

static void apply_options(void) {
    cfg.idle_skip_mode = opt_idle_skip_mode;
    if (game_loaded) {
        sh7021_set_idle_skip(loopy_config_idle_skip_enabled(&cfg));
        input_set_port_device((InputPortDevice)opt_input_device);
    }
}

/* ------------------------------------------------------------- basic info */

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "CLoopy";
    info->library_version = "0.1";
    info->valid_extensions = "bin|loopy";
    info->need_fullpath = false;
    info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = VIDEO_DISPLAY_WIDTH;
    info->geometry.base_height = VIDEO_DISPLAY_HEIGHT;
    info->geometry.max_width = VIDEO_DISPLAY_WIDTH;
    info->geometry.max_height = VIDEO_DISPLAY_HEIGHT;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = LOOPY_FPS;
    info->timing.sample_rate = (double)SOUND_TARGET_SAMPLE_RATE;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port;
    (void)device;
}

/* ------------------------------------------------------------- callbacks */

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
    } else {
        /* Older frontends: fall back to the v1 variable interface. */
        static struct retro_variable vars[] = {
            {OPT_IDLE_SKIP, "Idle Loop Skip; auto|on|off"},
            {OPT_INPUT_DEVICE, "Controller Port Device; gamepad|mouse"},
            {NULL, NULL},
        };
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
    }
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void) {
    struct retro_log_callback logging;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
    else log_cb = NULL;
    memset(&cfg, 0, sizeof(cfg));
}

void retro_deinit(void) { lr_state_bound = 0; }

/* ------------------------------------------------------------ game load */

/* Loads a file from the frontend's system directory into buf. */
static bool load_system_file(const char *name, ByteBuffer *buf, bool required) {
    const char *sysdir = NULL;
    char path[1024];
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) || !sysdir) {
        if (required) log_msg(RETRO_LOG_ERROR, "[CLoopy] No system directory; %s is required\n", name);
        return false;
    }
    snprintf(path, sizeof(path), "%s%c%s", sysdir, PATH_SEP, name);
    if (byte_buffer_load_file(buf, path) != 0) {
        log_msg(required ? RETRO_LOG_ERROR : RETRO_LOG_INFO, "[CLoopy] %s %s\n", required ? "Missing required" : "Optional",
                path);
        return false;
    }
    return true;
}

static const struct retro_input_descriptor input_desc[] = {
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "C"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "D"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
    {0},
};

bool retro_load_game(const struct retro_game_info *info) {
    if (!info || !info->data || !info->size) return false;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_0RGB1555;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log_msg(RETRO_LOG_ERROR, "[CLoopy] 0RGB1555 not supported by the frontend\n");
        return false;
    }
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)input_desc);

    memset(&cfg, 0, sizeof(cfg));

    if (byte_buffer_resize(&cfg.cart.rom, info->size, 0) != 0) return false;
    memcpy(cfg.cart.rom.data, info->data, info->size);

    if (!load_system_file("loopy_bios.bin", &cfg.bios_rom, true)) {
        loopy_config_free(&cfg);
        return false;
    }
    /* Both optional: without the sound BIOS the music chip is silent, and the
       OKI sample ROM only matters for Wanwan. */
    load_system_file("loopy_soundbios.bin", &cfg.sound_rom, false);
    load_system_file("loopy_okirom.bin", &cfg.oki_adpcm_rom, false);

    cfg.cart_is_wanwan = loopy_cart_rom_is_wanwan(cfg.cart.rom.data, cfg.cart.rom.size);
    cfg.cart_is_official = loopy_cart_rom_is_official(cfg.cart.rom.data, cfg.cart.rom.size);
    cfg.wanwan_replacement_pcm_enabled = 1;

    /* Cartridge SRAM size comes from the header, as in the native loader.  The
       frontend owns the contents via RETRO_MEMORY_SAVE_RAM, so no sram file
       path is set: cart.c must not write it itself. */
    uint32_t sram_start = 0, sram_end = 0;
    if (cfg.cart.rom.size >= 0x18u) {
        const uint8_t *h = cfg.cart.rom.data;
        sram_start = ((uint32_t)h[0x10] << 24) | ((uint32_t)h[0x11] << 16) | ((uint32_t)h[0x12] << 8) | h[0x13];
        sram_end = ((uint32_t)h[0x14] << 24) | ((uint32_t)h[0x15] << 16) | ((uint32_t)h[0x16] << 8) | h[0x17];
    }
    uint32_t sram_size = (sram_end > sram_start) ? (sram_end - sram_start + 1u) : 0u;
    if (sram_size == 0u || sram_size > 0x100000u) sram_size = 0x2000u;
    if (byte_buffer_resize(&cfg.cart.sram, sram_size, 0xFF) != 0) {
        loopy_config_free(&cfg);
        return false;
    }
    cfg.cart.sram_file_path = NULL;

    poll_options();
    cfg.idle_skip_mode = opt_idle_skip_mode;

    system_initialize(&cfg);
    game_loaded = true;
    input_set_port_device((InputPortDevice)opt_input_device);

    lr_state_freeze_bound();

    audio_accum = 0;
    log_msg(RETRO_LOG_INFO, "[CLoopy] Loaded %s cartridge, idle skip %s\n", cfg.cart_is_official ? "retail" : "unrecognised",
            loopy_config_idle_skip_enabled(&cfg) ? "on" : "off");
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    (void)type;
    (void)info;
    (void)num;
    return false;
}

void retro_unload_game(void) {
    if (game_loaded) {
        system_shutdown();
        loopy_config_free(&cfg);
        game_loaded = false;
    }
}

void retro_reset(void) {
    if (!game_loaded) return;
    system_shutdown();
    system_initialize(&cfg);
    input_set_port_device((InputPortDevice)opt_input_device);
    audio_accum = 0;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* ----------------------------------------------------------------- run */

static void poll_input(void) {
    if (!input_poll_cb || !input_state_cb) return;
    input_poll_cb();

    static const struct {
        unsigned id;
        PadButton pad;
    } map[] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, PAD_UP},       {RETRO_DEVICE_ID_JOYPAD_DOWN, PAD_DOWN},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, PAD_LEFT},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, PAD_RIGHT},
        {RETRO_DEVICE_ID_JOYPAD_B, PAD_A},         {RETRO_DEVICE_ID_JOYPAD_A, PAD_B},
        {RETRO_DEVICE_ID_JOYPAD_Y, PAD_C},         {RETRO_DEVICE_ID_JOYPAD_X, PAD_D},
        {RETRO_DEVICE_ID_JOYPAD_L, PAD_L1},        {RETRO_DEVICE_ID_JOYPAD_R, PAD_R1},
        {RETRO_DEVICE_ID_JOYPAD_START, PAD_START},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        int16_t s = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i].id);
        input_set_pad_button(map[i].pad, s != 0);
    }

    if (opt_input_device == INPUT_PORT_MOUSE) {
        int dx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
        int dy = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
        if (dx || dy) input_add_mouse_delta(dx, dy);
        input_set_mouse_button(0, input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0);
        input_set_mouse_button(1, input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0);
    }
}

/* Emits the audio for one video frame.  The Loopy frame rate is not a whole
   number of samples, so carry the remainder rather than rounding every frame. */
static void run_frame_audio(void) {
    audio_accum += SOUND_TARGET_SAMPLE_RATE * FRAME_RATE_DEN;
    uint32_t frames = audio_accum / FRAME_RATE_NUM;
    audio_accum %= FRAME_RATE_NUM;
    if (!frames) return;
    if (frames > MAX_AUDIO_FRAMES) frames = MAX_AUDIO_FRAMES;
    sound_generate_i16(audio_buf, (int)frames);
    if (audio_batch_cb) audio_batch_cb(audio_buf, frames);
}

void retro_run(void) {
    if (!game_loaded) return;

    bool updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        if (poll_options()) apply_options();
    }

    poll_input();
    system_run();

    /* The VDP can show 224 or 240 active lines; report only the active window
       so the frontend does not letterbox blank borders into the picture. */
    int active_h = video_get_display_active_height();
    int y_off = video_get_display_active_y_offset();
    const uint16_t *fb = system_get_display_output();
    if (video_cb) video_cb(fb + (size_t)y_off * VIDEO_DISPLAY_WIDTH, VIDEO_DISPLAY_WIDTH, (unsigned)active_h,
                           VIDEO_DISPLAY_WIDTH * sizeof(uint16_t));

    run_frame_audio();
}

/* ---------------------------------------------------------- savestates */

/* system_state_blob_size() is not constant: the timing chunk holds one record
   per scheduled event, so the total moves as events are queued and consumed
   (measured spread across a 3000-frame run: 64 bytes).  system_save_state_to_
   buffer() also insists on being handed exactly the current size.
   libretro cannot work that way - retro_serialize_size() is called once and the
   frontend sizes its rewind ring from the answer, so it has to be fixed and may
   never grow afterwards.  Publish a fixed bound with room to spare and keep the
   real blob length-prefixed inside it. */
#define LR_STATE_HDR 4u
#define LR_STATE_SLACK (64u * 1024u)

static void lr_state_freeze_bound(void) {
    lr_state_bound = (size_t)LR_STATE_HDR + system_state_blob_size() + LR_STATE_SLACK;
}

size_t retro_serialize_size(void) { return game_loaded ? lr_state_bound : 0; }

bool retro_serialize(void *data, size_t size) {
    if (!game_loaded || !data) return false;
    uint32_t n = system_state_blob_size();
    if (!n || (size_t)LR_STATE_HDR + n > size) {
        log_msg(RETRO_LOG_ERROR, "[CLoopy] State of %u bytes does not fit the %zu byte buffer\n", n, size);
        return false;
    }
    uint8_t *p = (uint8_t *)data;
    memcpy(p, &n, sizeof(n));
    if (system_save_state_to_buffer(p + LR_STATE_HDR, n) != 0) return false;
    /* Zero the tail so identical machine states serialize to identical bytes;
       the frontend's rewind compares/compresses these buffers. */
    memset(p + LR_STATE_HDR + n, 0, size - LR_STATE_HDR - n);
    return true;
}

bool retro_unserialize(const void *data, size_t size) {
    if (!game_loaded || !data) return false;
    uint32_t n = 0;
    if (size < LR_STATE_HDR) return false;
    memcpy(&n, data, sizeof(n));
    if (!n || (size_t)LR_STATE_HDR + n > size) return false;
    return system_load_state_from_buffer((const uint8_t *)data + LR_STATE_HDR, n) == 0;
}

/* ------------------------------------------------------------- memory */

/* cart.c copies the config buffer and may pad it, so hand the frontend the live
   allocation - otherwise .srm would capture the pre-boot image forever. */
void *retro_get_memory_data(unsigned id) {
    if (id == RETRO_MEMORY_SAVE_RAM && game_loaded) return cart_get_sram_data();
    return NULL;
}

size_t retro_get_memory_size(unsigned id) {
    if (id == RETRO_MEMORY_SAVE_RAM && game_loaded) return cart_get_sram_size();
    return 0;
}

/* --------------------------------------------------------------- cheats */

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index;
    (void)enabled;
    (void)code;
}
