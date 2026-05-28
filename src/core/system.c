#include "core/system.h"
#include "core/cart.h"
#include "core/loopy_io.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/sh7021/sh7021.h"
#include "core/sh7021/peripherals/sh7021_serial.h"
#include "input/input.h"
#include "sound/sound.h"
#include "video/video.h"
#include <stdint.h>

void system_initialize(const ConfigSystemInfo *config) {
    memory_initialize(&config->bios_rom);
    timing_initialize();
    sh7021_initialize();
    cart_initialize(&config->cart);
    loopy_io_initialize();
    input_initialize();
    video_initialize();
    sound_initialize(&config->sound_rom, &config->oki_adpcm_rom, config->cart_is_wanwan, config->wanwan_replacement_pcm_enabled);
    sh7021_ocpm_serial_set_tx_callback(1, sound_midi_byte_in);
}

void system_shutdown(void) {
    sound_shutdown();
    video_shutdown();
    input_shutdown();
    loopy_io_shutdown();
    cart_shutdown();
    sh7021_shutdown();
    timing_shutdown();
    memory_shutdown();
}

void system_run(void) {
    video_start_frame();
    while (!video_check_frame_end()) {
        int64_t slice_length = INT64_MAX;
        for (int i = 0; i < TIMING_NUM_TIMERS; i++) {
            int64_t v = timing_calc_slice_length(i);
            if (v < slice_length) slice_length = v;
        }
        for (int i = 0; i < TIMING_NUM_TIMERS; i++) timing_process_slice(i, (int32_t)slice_length);
    }
    cart_sram_commit_check();
}

uint16_t *system_get_display_output(void) { return video_get_display_output(); }

#include "core/sh7021/peripherals/sh7021_bsc.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/peripherals/sh7021_ocpm.h"
#include "core/sh7021/peripherals/sh7021_timers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t (*BlobSizeFn)(void);
typedef void (*BlobGetFn)(void *, uint32_t);
typedef void (*BlobSetFn)(const void *, uint32_t);

typedef struct StateChunkDef {
    char id[4];
    BlobSizeFn size_fn;
    BlobGetFn get_fn;
    BlobSetFn set_fn;
} StateChunkDef;

static const StateChunkDef state_chunks[] = {
    {{'S','H','7','C'}, sh7021_state_blob_size, sh7021_get_state_blob, sh7021_set_state_blob},
    {{'M','R','A','M'}, memory_state_blob_size, memory_get_state_blob, memory_set_state_blob},
    {{'C','A','R','T'}, cart_state_blob_size, cart_get_state_blob, cart_set_state_blob},
    {{'O','R','A','M'}, sh7021_ocpm_oram_state_blob_size, sh7021_ocpm_oram_get_state_blob, sh7021_ocpm_oram_set_state_blob},
    {{'D','M','A','C'}, sh7021_ocpm_dmac_state_blob_size, sh7021_ocpm_dmac_get_state_blob, sh7021_ocpm_dmac_set_state_blob},
    {{'B','S','C',' '}, sh7021_ocpm_bsc_state_blob_size, sh7021_ocpm_bsc_get_state_blob, sh7021_ocpm_bsc_set_state_blob},
    {{'I','N','T','C'}, sh7021_ocpm_intc_state_blob_size, sh7021_ocpm_intc_get_state_blob, sh7021_ocpm_intc_set_state_blob},
    {{'S','E','R','L'}, sh7021_ocpm_serial_state_blob_size, sh7021_ocpm_serial_get_state_blob, sh7021_ocpm_serial_set_state_blob},
    {{'T','M','R','S'}, sh7021_ocpm_timer_state_blob_size, sh7021_ocpm_timer_get_state_blob, sh7021_ocpm_timer_set_state_blob},
    {{'L','I','O',' '}, loopy_io_state_blob_size, loopy_io_get_state_blob, loopy_io_set_state_blob},
    {{'V','D','P',' '}, video_state_blob_size, video_get_state_blob, video_set_state_blob},
    {{'S','N','D',' '}, sound_state_blob_size, sound_get_state_blob, sound_set_state_blob},
};

static int write_exact(FILE *f, const void *data, size_t size) { return fwrite(data, 1, size, f) == size ? 0 : -1; }
static int read_exact(FILE *f, void *data, size_t size) { return fread(data, 1, size, f) == size ? 0 : -1; }

int system_save_state(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const char magic[8] = { 'L','P','S','T','A','T','E','2' };
    uint32_t version = 1;
    uint32_t chunk_count = (uint32_t)(sizeof(state_chunks) / sizeof(state_chunks[0]));
    int ok = 0;
    if (write_exact(f, magic, sizeof(magic)) != 0 || write_exact(f, &version, sizeof(version)) != 0 || write_exact(f, &chunk_count, sizeof(chunk_count)) != 0) ok = -1;
    for (uint32_t i = 0; ok == 0 && i < chunk_count; i++) {
        const StateChunkDef *c = &state_chunks[i];
        uint32_t size = c->size_fn();
        void *buf = malloc(size ? size : 1);
        if (!buf) { ok = -1; break; }
        c->get_fn(buf, size);
        if (write_exact(f, c->id, 4) != 0 || write_exact(f, &size, sizeof(size)) != 0 || write_exact(f, buf, size) != 0) ok = -1;
        free(buf);
    }
    if (ok == 0) {
        const char id[4] = { 'T','I','M','G' };
        uint32_t marker = 0xFFFFFFFFu;
        if (write_exact(f, id, 4) != 0 || write_exact(f, &marker, sizeof(marker)) != 0 || timing_save_state(f) != 0) ok = -1;
    }
    fclose(f);
    return ok;
}

static const StateChunkDef *find_chunk(const char id[4]) {
    for (size_t i = 0; i < sizeof(state_chunks) / sizeof(state_chunks[0]); i++) {
        if (memcmp(state_chunks[i].id, id, 4) == 0) return &state_chunks[i];
    }
    return NULL;
}


static uint32_t state_file_header_size(void) {
    return 8u + (uint32_t)sizeof(uint32_t) + (uint32_t)sizeof(uint32_t);
}

uint32_t system_state_blob_size(void) {
    uint64_t total = state_file_header_size();
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state_chunks) / sizeof(state_chunks[0])); i++) {
        total += 4u + sizeof(uint32_t) + (uint64_t)state_chunks[i].size_fn();
    }
    total += 4u + sizeof(uint32_t) + (uint64_t)timing_state_blob_size();
    return total > 0xFFFFFFFFu ? 0u : (uint32_t)total;
}

static int blob_write(uint8_t **p, uint8_t *end, const void *src, size_t n) {
    if (*p > end || (size_t)(end - *p) < n) return -1;
    if (n) memcpy(*p, src, n);
    *p += n;
    return 0;
}

static int blob_read(const uint8_t **p, const uint8_t *end, void *dst, size_t n) {
    if (*p > end || (size_t)(end - *p) < n) return -1;
    if (n) memcpy(dst, *p, n);
    *p += n;
    return 0;
}

int system_save_state_to_buffer(void *dst, uint32_t size) {
    uint32_t need = system_state_blob_size();
    if (!dst || !need || size != need) return -1;
    uint8_t *p = (uint8_t *)dst;
    uint8_t *end = p + size;
    const char magic[8] = { 'L','P','S','T','A','T','E','2' };
    uint32_t version = 1;
    uint32_t chunk_count = (uint32_t)(sizeof(state_chunks) / sizeof(state_chunks[0]));
    if (blob_write(&p, end, magic, sizeof(magic)) != 0 ||
        blob_write(&p, end, &version, sizeof(version)) != 0 ||
        blob_write(&p, end, &chunk_count, sizeof(chunk_count)) != 0) return -1;
    for (uint32_t i = 0; i < chunk_count; i++) {
        const StateChunkDef *c = &state_chunks[i];
        uint32_t chunk_size = c->size_fn();
        if (blob_write(&p, end, c->id, 4) != 0 || blob_write(&p, end, &chunk_size, sizeof(chunk_size)) != 0) return -1;
        if (chunk_size) {
            if ((size_t)(end - p) < chunk_size) return -1;
            c->get_fn(p, chunk_size);
            p += chunk_size;
        }
    }
    const char id[4] = { 'T','I','M','G' };
    uint32_t timing_size = timing_state_blob_size();
    if (blob_write(&p, end, id, 4) != 0 || blob_write(&p, end, &timing_size, sizeof(timing_size)) != 0) return -1;
    if (timing_get_state_blob(p, timing_size) != 0) return -1;
    p += timing_size;
    return p == end ? 0 : -1;
}

int system_load_state_from_buffer(const void *src, uint32_t size) {
    if (!src || size < state_file_header_size()) return -1;
    const uint8_t *p = (const uint8_t *)src;
    const uint8_t *end = p + size;
    char magic[8];
    uint32_t version = 0, chunk_count = 0;
    if (blob_read(&p, end, magic, sizeof(magic)) != 0 || memcmp(magic, "LPSTATE2", 8) != 0 ||
        blob_read(&p, end, &version, sizeof(version)) != 0 || version != 1 ||
        blob_read(&p, end, &chunk_count, sizeof(chunk_count)) != 0) return -1;
    for (uint32_t i = 0; i < chunk_count; i++) {
        char id[4];
        uint32_t chunk_size = 0;
        if (blob_read(&p, end, id, 4) != 0 || blob_read(&p, end, &chunk_size, sizeof(chunk_size)) != 0) return -1;
        if ((size_t)(end - p) < chunk_size) return -1;
        const StateChunkDef *c = find_chunk(id);
        if (c && c->size_fn() == chunk_size) c->set_fn(p, chunk_size);
        p += chunk_size;
    }
    char tid[4];
    uint32_t timing_size = 0;
    if (blob_read(&p, end, tid, 4) != 0 || blob_read(&p, end, &timing_size, sizeof(timing_size)) != 0) return -1;
    if (memcmp(tid, "TIMG", 4) != 0 || (size_t)(end - p) < timing_size) return -1;
    if (timing_set_state_blob(p, timing_size) != 0) return -1;
    p += timing_size;
    return p == end ? 0 : -1;
}

int system_load_state(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[8];
    uint32_t version = 0, chunk_count = 0;
    int ok = 0;
    if (read_exact(f, magic, sizeof(magic)) != 0 || memcmp(magic, "LPSTATE2", 8) != 0 || read_exact(f, &version, sizeof(version)) != 0 || version != 1 || read_exact(f, &chunk_count, sizeof(chunk_count)) != 0) ok = -1;
    for (uint32_t i = 0; ok == 0 && i < chunk_count; i++) {
        char id[4];
        uint32_t size = 0;
        if (read_exact(f, id, 4) != 0 || read_exact(f, &size, sizeof(size)) != 0) { ok = -1; break; }
        const StateChunkDef *c = find_chunk(id);
        void *buf = malloc(size ? size : 1);
        if (!buf) { ok = -1; break; }
        if (read_exact(f, buf, size) != 0) ok = -1;
        if (ok == 0 && c && c->size_fn() == size) c->set_fn(buf, size);
        free(buf);
    }
    if (ok == 0) {
        char id[4];
        uint32_t marker = 0;
        if (read_exact(f, id, 4) != 0 || read_exact(f, &marker, sizeof(marker)) != 0 || memcmp(id, "TIMG", 4) != 0 || marker != 0xFFFFFFFFu) ok = -1;
        else if (timing_load_state(f) != 0) ok = -1;
    }
    fclose(f);
    return ok;
}
