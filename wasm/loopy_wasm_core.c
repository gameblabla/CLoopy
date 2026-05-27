#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "frontend/loader.h"
#include "core/cart.h"
#include "core/system.h"
#include "input/input.h"
#include "sound/sound.h"
#include "video/video.h"
#include "core/loopy_io.h"

extern void __loopy_wasm_heap_reset(void);
extern uint32_t __loopy_wasm_heap_used(void);

#define STATUS_NEED_ROMS 0u
#define STATUS_ROMS_READY 1u
#define STATUS_CART_READY 2u
#define STATUS_RUNNING 3u
#define STATUS_PAUSED 4u
#define STATUS_ERROR 5u

#define ERR_NONE 0u
#define ERR_BAD_BIOS 0xB1050001u
#define ERR_BAD_SOUND 0x500D0001u
#define ERR_BAD_CART 0xCA870001u
#define ERR_NO_CART 0xCA870002u
#define ERR_NO_ROMS 0xB1050002u
#define ERR_START_FAILED 0xE0000001u
#define ERR_FRAME_FAILED 0xE0000002u
#define ERR_STATE_FAILED 0x57A7E001u

#define AUDIO_RING_FRAMES 16384u
#define FRAME_RATE_NUM 598261u
#define FRAME_RATE_DEN 10000u

typedef struct LoopyWasmStateHeader {
    char magic[8];
    uint32_t version;
    uint32_t system_size;
    uint32_t audio_accum;
    uint32_t reserved;
} LoopyWasmStateHeader;

#define LOOPY_WASM_STATE_MAGIC "LPWASM2"
#define LOOPY_WASM_STATE_VERSION 1u

static ConfigSystemInfo cfg;
static uint32_t status_code = STATUS_NEED_ROMS;
static uint32_t error_code = ERR_NONE;
static uint32_t frame_counter;
static uint32_t audio_accum;
static int16_t audio_ring[AUDIO_RING_FRAMES * 2u];
static uint32_t audio_ring_frames;
static uint8_t *state_buf;
static uint32_t state_buf_size;
static uint8_t *state_load_buf;
static uint32_t state_load_buf_size;
static uint8_t *sram_buf;
static uint32_t sram_buf_size;
static uint8_t *rgba_fb;
static uint32_t rgba_fb_size;
static uint8_t *print_rgba;
static uint32_t print_rgba_size;
static bool running;
static bool paused;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t size)
{
    crc = ~crc;
    for(uint32_t i = 0; i < size; i++) {
        crc ^= data[i];
        for(int k = 0; k < 8; k++) crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return ~crc;
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void set_error(uint32_t err)
{
    error_code = err;
    status_code = STATUS_ERROR;
}

static void clear_audio(void)
{
    audio_ring_frames = 0;
    audio_accum = 0;
}

static void destroy_system_only(void)
{
    if(running) {
        system_shutdown();
        running = false;
    }
}

static void clear_config(void)
{
    loopy_config_free(&cfg);
    memset(&cfg, 0, sizeof(cfg));
}

static int copy_into_buffer(ByteBuffer *dst, const void *src, uint32_t size)
{
    if(!dst || !src || !size) return 0;
    byte_buffer_free(dst);
    dst->data = (uint8_t*)malloc(size);
    if(!dst->data) { dst->size = 0; return 0; }
    memcpy(dst->data, src, size);
    dst->size = size;
    return 1;
}

static uint32_t sram_size_from_cart(const uint8_t *cart, uint32_t size)
{
    if(!cart || size < 0x18u) return 0;
    uint32_t s = read_be32(cart + 0x10u);
    uint32_t e = read_be32(cart + 0x14u);
    if(e < s) return 0;
    uint32_t n = e - s + 1u;
    if(!n || n > (1024u * 1024u)) return 0;
    return n;
}

static void append_audio(uint32_t frames)
{
    if(frames > AUDIO_RING_FRAMES) frames = AUDIO_RING_FRAMES;
    if(audio_ring_frames + frames > AUDIO_RING_FRAMES) {
        uint32_t drop = (audio_ring_frames + frames) - AUDIO_RING_FRAMES;
        if(drop >= audio_ring_frames) audio_ring_frames = 0;
        else {
            memmove(audio_ring, audio_ring + drop * 2u, (audio_ring_frames - drop) * 2u * sizeof(audio_ring[0]));
            audio_ring_frames -= drop;
        }
    }
    sound_wasm_generate_i16(audio_ring + audio_ring_frames * 2u, (int)frames);
    audio_ring_frames += frames;
}

static void update_frame_audio(void)
{
    audio_accum += SOUND_TARGET_SAMPLE_RATE * FRAME_RATE_DEN;
    uint32_t frames = audio_accum / FRAME_RATE_NUM;
    audio_accum %= FRAME_RATE_NUM;
    if(frames) append_audio(frames);
}

static void rgb555_to_rgba(uint16_t c, uint8_t *out)
{
    uint8_t r5 = (uint8_t)((c >> 10) & 31u);
    uint8_t g5 = (uint8_t)((c >> 5) & 31u);
    uint8_t b5 = (uint8_t)(c & 31u);
    out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    out[1] = (uint8_t)((g5 << 3) | (g5 >> 2));
    out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    out[3] = 255u;
}

__attribute__((export_name("loopy_wasm_version")))
uint32_t loopy_wasm_version(void) { return 0x00000051u; }

__attribute__((export_name("loopy_wasm_malloc")))
uint32_t loopy_wasm_malloc(uint32_t size) { return (uint32_t)(uintptr_t)malloc(size ? size : 1u); }

__attribute__((export_name("loopy_wasm_heap_used")))
uint32_t loopy_wasm_heap_used(void) { return __loopy_wasm_heap_used(); }

__attribute__((export_name("loopy_wasm_reset_heap")))
void loopy_wasm_reset_heap(void)
{
    destroy_system_only();
    clear_config();
    __loopy_wasm_heap_reset();
    memset(&cfg, 0, sizeof(cfg));
    state_buf = NULL;
    state_buf_size = 0;
    state_load_buf = NULL;
    state_load_buf_size = 0;
    sram_buf = NULL;
    sram_buf_size = 0;
    rgba_fb = NULL;
    rgba_fb_size = 0;
    print_rgba = NULL;
    print_rgba_size = 0;
    clear_audio();
    frame_counter = 0;
    status_code = STATUS_NEED_ROMS;
    error_code = ERR_NONE;
    paused = false;
}

__attribute__((export_name("loopy_wasm_init")))
void loopy_wasm_init(void) { loopy_wasm_reset_heap(); }

__attribute__((export_name("loopy_wasm_crc32")))
uint32_t loopy_wasm_crc32(uint32_t ptr, uint32_t size)
{
    if(!ptr || !size) return 0;
    return crc32_update(0, (const uint8_t*)(uintptr_t)ptr, size);
}

__attribute__((export_name("loopy_wasm_load_bios")))
uint32_t loopy_wasm_load_bios(uint32_t ptr, uint32_t size)
{
    if(!ptr || size != 32768u) { set_error(ERR_BAD_BIOS); return 0; }
    if(!copy_into_buffer(&cfg.bios_rom, (const void*)(uintptr_t)ptr, size)) { set_error(ERR_BAD_BIOS); return 0; }
    if(cfg.sound_rom.data && cfg.bios_rom.data) status_code = cfg.cart.rom.data ? STATUS_CART_READY : STATUS_ROMS_READY;
    return crc32_update(0, cfg.bios_rom.data, (uint32_t)cfg.bios_rom.size);
}

__attribute__((export_name("loopy_wasm_load_sound_rom")))
uint32_t loopy_wasm_load_sound_rom(uint32_t ptr, uint32_t size)
{
    if(!ptr || size < 0x10000u) { set_error(ERR_BAD_SOUND); return 0; }
    if(!copy_into_buffer(&cfg.sound_rom, (const void*)(uintptr_t)ptr, size)) { set_error(ERR_BAD_SOUND); return 0; }
    if(cfg.sound_rom.data && cfg.bios_rom.data) status_code = cfg.cart.rom.data ? STATUS_CART_READY : STATUS_ROMS_READY;
    return crc32_update(0, cfg.sound_rom.data, (uint32_t)cfg.sound_rom.size);
}

__attribute__((export_name("loopy_wasm_load_cart")))
uint32_t loopy_wasm_load_cart(uint32_t ptr, uint32_t size)
{
    if(!ptr || size < 0x18u || size > (4u * 1024u * 1024u)) { set_error(ERR_BAD_CART); return 0; }
    if(!copy_into_buffer(&cfg.cart.rom, (const void*)(uintptr_t)ptr, size)) { set_error(ERR_BAD_CART); return 0; }
    uint32_t sram_size = sram_size_from_cart(cfg.cart.rom.data, (uint32_t)cfg.cart.rom.size);
    byte_buffer_free(&cfg.cart.sram);
    byte_buffer_resize(&cfg.cart.sram, sram_size, 0xFFu);
    status_code = (cfg.bios_rom.data && cfg.sound_rom.data) ? STATUS_CART_READY : STATUS_NEED_ROMS;
    return crc32_update(0, cfg.cart.rom.data, (uint32_t)cfg.cart.rom.size);
}

__attribute__((export_name("loopy_wasm_load_sram")))
uint32_t loopy_wasm_load_sram(uint32_t ptr, uint32_t size)
{
    if(!cfg.cart.sram.data || !cfg.cart.sram.size || !ptr || !size) return 0;
    uint32_t n = size < cfg.cart.sram.size ? size : (uint32_t)cfg.cart.sram.size;
    memcpy(cfg.cart.sram.data, (const void*)(uintptr_t)ptr, n);
    if(n < cfg.cart.sram.size) memset(cfg.cart.sram.data + n, 0xFF, cfg.cart.sram.size - n);
    return 1;
}

__attribute__((export_name("loopy_wasm_start")))
uint32_t loopy_wasm_start(uint32_t device)
{
    destroy_system_only();
    if(!cfg.bios_rom.data || !cfg.sound_rom.data) { set_error(ERR_NO_ROMS); return 0; }
    if(!cfg.cart.rom.data) { set_error(ERR_NO_CART); return 0; }
    system_initialize(&cfg);
    input_set_port_device(device ? INPUT_PORT_MOUSE : INPUT_PORT_GAMEPAD);
    clear_audio();
    frame_counter = 0;
    running = true;
    paused = false;
    error_code = ERR_NONE;
    status_code = STATUS_RUNNING;
    return 1;
}

__attribute__((export_name("loopy_wasm_soft_reset")))
uint32_t loopy_wasm_soft_reset(void)
{
    if(!running) return 0;
    InputPortDevice device = input_get_port_device();

    /* Preserve live SRAM across the reset.  system_shutdown() tears down the
     * runtime cart copy, while cfg.cart.sram is the template used for the next
     * cart_initialize(). */
    uint32_t sram_size = cart_state_blob_size();
    if(sram_size && cfg.cart.sram.data && cfg.cart.sram.size == sram_size) {
        cart_get_state_blob(cfg.cart.sram.data, sram_size);
    }

    system_shutdown();
    system_initialize(&cfg);
    input_set_port_device(device);
    clear_audio();
    frame_counter = 0;
    paused = false;
    sound_set_paused(false);
    error_code = ERR_NONE;
    status_code = STATUS_RUNNING;
    return 1;
}

__attribute__((export_name("loopy_wasm_set_paused")))
void loopy_wasm_set_paused(uint32_t pause_in)
{
    paused = pause_in ? true : false;
    sound_set_paused(paused);
    if(paused) clear_audio();
    status_code = paused ? STATUS_PAUSED : (running ? STATUS_RUNNING : status_code);
}

__attribute__((export_name("loopy_wasm_set_port_device")))
void loopy_wasm_set_port_device(uint32_t device)
{
    input_set_port_device(device ? INPUT_PORT_MOUSE : INPUT_PORT_GAMEPAD);
}

__attribute__((export_name("loopy_wasm_get_port_device")))
uint32_t loopy_wasm_get_port_device(void)
{
    return input_get_port_device() == INPUT_PORT_MOUSE ? 1u : 0u;
}

__attribute__((export_name("loopy_wasm_frame")))
uint32_t loopy_wasm_frame(uint32_t buttons, int32_t mouse_dx, int32_t mouse_dy, uint32_t mouse_buttons)
{
    if(!running || paused) return 0;
    input_set_pad_button(PAD_START, (buttons & PAD_START) != 0);
    input_set_pad_button(PAD_A, (buttons & PAD_A) != 0);
    input_set_pad_button(PAD_B, (buttons & PAD_B) != 0);
    input_set_pad_button(PAD_C, (buttons & PAD_C) != 0);
    input_set_pad_button(PAD_D, (buttons & PAD_D) != 0);
    input_set_pad_button(PAD_UP, (buttons & PAD_UP) != 0);
    input_set_pad_button(PAD_DOWN, (buttons & PAD_DOWN) != 0);
    input_set_pad_button(PAD_LEFT, (buttons & PAD_LEFT) != 0);
    input_set_pad_button(PAD_RIGHT, (buttons & PAD_RIGHT) != 0);
    input_set_pad_button(PAD_L1, (buttons & PAD_L1) != 0);
    input_set_pad_button(PAD_R1, (buttons & PAD_R1) != 0);
    input_set_mouse_button(0, (mouse_buttons & 1u) != 0);
    input_set_mouse_button(1, (mouse_buttons & 2u) != 0);
    if(mouse_dx || mouse_dy) input_add_mouse_delta(mouse_dx, mouse_dy);
    system_run();
    update_frame_audio();
    frame_counter++;
    return 1;
}

__attribute__((export_name("loopy_wasm_get_framebuffer_rgb555")))
uint32_t loopy_wasm_get_framebuffer_rgb555(void) { return (uint32_t)(uintptr_t)system_get_display_output(); }

__attribute__((export_name("loopy_wasm_get_framebuffer_rgba")))
uint32_t loopy_wasm_get_framebuffer_rgba(void)
{
    const uint32_t w = VIDEO_DISPLAY_WIDTH;
    const uint32_t h = (uint32_t)video_get_display_active_height();
    const uint32_t y_off = (uint32_t)video_get_display_active_y_offset();
    uint32_t need = w * h * 4u;
    if(rgba_fb_size < need) { rgba_fb = (uint8_t*)malloc(need); rgba_fb_size = rgba_fb ? need : 0; }
    if(!rgba_fb) return 0;
    const uint16_t *src = system_get_display_output();
    for(uint32_t y = 0; y < h; y++) {
        const uint32_t src_y = y + y_off;
        for(uint32_t x = 0; x < w; x++) rgb555_to_rgba(src[src_y * VIDEO_DISPLAY_WIDTH + x], rgba_fb + ((y * w + x) * 4u));
    }
    return (uint32_t)(uintptr_t)rgba_fb;
}

__attribute__((export_name("loopy_wasm_get_width"))) uint32_t loopy_wasm_get_width(void) { return VIDEO_DISPLAY_WIDTH; }
__attribute__((export_name("loopy_wasm_get_height"))) uint32_t loopy_wasm_get_height(void) { return (uint32_t)video_get_display_active_height(); }
__attribute__((export_name("loopy_wasm_get_active_height"))) uint32_t loopy_wasm_get_active_height(void) { return (uint32_t)video_get_display_active_height(); }
__attribute__((export_name("loopy_wasm_get_full_height"))) uint32_t loopy_wasm_get_full_height(void) { return VIDEO_DISPLAY_HEIGHT; }
__attribute__((export_name("loopy_wasm_get_y_offset"))) uint32_t loopy_wasm_get_y_offset(void) { return (uint32_t)video_get_display_active_y_offset(); }
__attribute__((export_name("loopy_wasm_get_active_y_offset"))) uint32_t loopy_wasm_get_active_y_offset(void) { return (uint32_t)video_get_display_active_y_offset(); }
__attribute__((export_name("loopy_wasm_get_status"))) uint32_t loopy_wasm_get_status(void) { return status_code; }
__attribute__((export_name("loopy_wasm_get_error"))) uint32_t loopy_wasm_get_error(void) { return error_code; }
__attribute__((export_name("loopy_wasm_get_frame_count"))) uint32_t loopy_wasm_get_frame_count(void) { return frame_counter; }
__attribute__((export_name("loopy_wasm_get_audio_rate"))) uint32_t loopy_wasm_get_audio_rate(void) { return SOUND_TARGET_SAMPLE_RATE; }
__attribute__((export_name("loopy_wasm_get_audio_ptr"))) uint32_t loopy_wasm_get_audio_ptr(void) { return (uint32_t)(uintptr_t)audio_ring; }
__attribute__((export_name("loopy_wasm_get_audio_frames"))) uint32_t loopy_wasm_get_audio_frames(void) { return audio_ring_frames; }
__attribute__((export_name("loopy_wasm_audio_consume"))) void loopy_wasm_audio_consume(uint32_t frames) { if(frames >= audio_ring_frames) { audio_ring_frames = 0; return; } memmove(audio_ring, audio_ring + frames * 2u, (audio_ring_frames - frames) * 2u * sizeof(audio_ring[0])); audio_ring_frames -= frames; }

static uint32_t load_state_blob_internal(const uint8_t *src, uint32_t size)
{
    if(!src || !size) return 0;
    uint32_t ok = 0;
    if(size >= sizeof(LoopyWasmStateHeader)) {
        LoopyWasmStateHeader h;
        memcpy(&h, src, sizeof(h));
        if(memcmp(h.magic, LOOPY_WASM_STATE_MAGIC, 7) == 0 &&
           h.version == LOOPY_WASM_STATE_VERSION &&
           h.system_size <= size - (uint32_t)sizeof(LoopyWasmStateHeader)) {
            ok = system_load_state_from_buffer(src + sizeof(LoopyWasmStateHeader), h.system_size) == 0;
            if(ok) {
                audio_ring_frames = 0;
                audio_accum = h.audio_accum;
            }
            return ok;
        }
    }
    ok = system_load_state_from_buffer(src, size) == 0;
    if(ok) clear_audio();
    return ok;
}

__attribute__((export_name("loopy_wasm_save_state")))
uint32_t loopy_wasm_save_state(void)
{
    if(!running) return 0;
    uint32_t system_size = system_state_blob_size();
    if(!system_size) return 0;
    uint32_t n = (uint32_t)sizeof(LoopyWasmStateHeader) + system_size;
    if(state_buf_size < n) { state_buf = (uint8_t*)malloc(n); state_buf_size = state_buf ? n : 0; }
    if(!state_buf) { set_error(ERR_STATE_FAILED); return 0; }

    LoopyWasmStateHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, LOOPY_WASM_STATE_MAGIC, 7);
    h.version = LOOPY_WASM_STATE_VERSION;
    h.system_size = system_size;
    h.audio_accum = audio_accum;
    memcpy(state_buf, &h, sizeof(h));
    if(system_save_state_to_buffer(state_buf + sizeof(h), system_size) != 0) { set_error(ERR_STATE_FAILED); return 0; }
    state_buf_size = n;
    return 1;
}
__attribute__((export_name("loopy_wasm_get_save_ptr"))) uint32_t loopy_wasm_get_save_ptr(void) { return (uint32_t)(uintptr_t)state_buf; }
__attribute__((export_name("loopy_wasm_get_save_size"))) uint32_t loopy_wasm_get_save_size(void) { return state_buf_size; }

__attribute__((export_name("loopy_wasm_prepare_load_state")))
uint32_t loopy_wasm_prepare_load_state(uint32_t size)
{
    if(!size) return 0;
    if(state_load_buf_size < size) {
        state_load_buf = (uint8_t*)malloc(size);
        state_load_buf_size = state_load_buf ? size : 0;
    }
    return (uint32_t)(uintptr_t)state_load_buf;
}

__attribute__((export_name("loopy_wasm_load_prepared_state")))
uint32_t loopy_wasm_load_prepared_state(uint32_t size)
{
    if(!state_load_buf || !size || size > state_load_buf_size) return 0;
    uint32_t ok = load_state_blob_internal(state_load_buf, size);
    return ok;
}

__attribute__((export_name("loopy_wasm_load_state"))) uint32_t loopy_wasm_load_state(uint32_t ptr, uint32_t size) { if(!ptr || !size) return 0; return load_state_blob_internal((const uint8_t*)(uintptr_t)ptr, size); }

__attribute__((export_name("loopy_wasm_save_sram")))
uint32_t loopy_wasm_save_sram(void)
{
    uint32_t n = cart_state_blob_size();
    if(!n) return 0;
    if(sram_buf_size < n) { sram_buf = (uint8_t*)malloc(n); sram_buf_size = sram_buf ? n : 0; }
    if(!sram_buf) return 0;
    cart_get_state_blob(sram_buf, n);
    sram_buf_size = n;
    return 1;
}
__attribute__((export_name("loopy_wasm_get_sram_ptr"))) uint32_t loopy_wasm_get_sram_ptr(void) { return (uint32_t)(uintptr_t)sram_buf; }
__attribute__((export_name("loopy_wasm_get_sram_size"))) uint32_t loopy_wasm_get_sram_size(void) { return sram_buf_size; }

__attribute__((export_name("loopy_wasm_print_pending"))) uint32_t loopy_wasm_print_pending(void) { return loopy_io_printer_has_pending_image() ? 1u : 0u; }
__attribute__((export_name("loopy_wasm_print_width"))) uint32_t loopy_wasm_print_width(void) { return loopy_io_printer_pending_width(); }
__attribute__((export_name("loopy_wasm_print_height"))) uint32_t loopy_wasm_print_height(void) { return loopy_io_printer_pending_height(); }
__attribute__((export_name("loopy_wasm_print_rgba")))
uint32_t loopy_wasm_print_rgba(void)
{
    const uint16_t *src = loopy_io_printer_pending_pixels();
    uint32_t w = loopy_io_printer_pending_width();
    uint32_t h = loopy_io_printer_pending_height();
    if(!src || !w || !h) return 0;
    uint32_t need = w * h * 4u;
    if(print_rgba_size < need) { print_rgba = (uint8_t*)malloc(need); print_rgba_size = print_rgba ? need : 0; }
    if(!print_rgba) return 0;
    for(uint32_t i = 0; i < w * h; i++) rgb555_to_rgba(src[i], print_rgba + i * 4u);
    return (uint32_t)(uintptr_t)print_rgba;
}
__attribute__((export_name("loopy_wasm_print_clear"))) void loopy_wasm_print_clear(void) { loopy_io_printer_clear_pending_image(); }
