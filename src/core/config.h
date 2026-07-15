#ifndef LOOPY_CONFIG_H
#define LOOPY_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ByteBuffer {
    uint8_t *data;
    size_t size;
} ByteBuffer;

typedef struct ConfigCartInfo {
    ByteBuffer rom;
    ByteBuffer sram;
    char *sram_file_path;
} ConfigCartInfo;

/* Idle-loop skip policy.  AUTO applies the skip only to retail cartridges: it
   is validated against the retail library, and homebrew is exactly what this
   emulator exists to test against real hardware, so unknown images opt out
   rather than in.  ON/OFF override the detection either way. */
typedef enum LoopyIdleSkipMode {
    LOOPY_IDLE_SKIP_AUTO = 0,
    LOOPY_IDLE_SKIP_ON = 1,
    LOOPY_IDLE_SKIP_OFF = 2
} LoopyIdleSkipMode;

typedef struct ConfigSystemInfo {
    ConfigCartInfo cart;
    ByteBuffer bios_rom;
    ByteBuffer sound_rom;
    ByteBuffer oki_adpcm_rom;
    int cart_is_wanwan;
    int cart_is_official;
    int wanwan_replacement_pcm_enabled;
    int idle_skip_mode;
} ConfigSystemInfo;

/* Resolves idle_skip_mode against cart_is_official.  Shared so every frontend
   applies the same policy. */
int loopy_config_idle_skip_enabled(const ConfigSystemInfo *config);

void byte_buffer_init(ByteBuffer *buf);
void byte_buffer_free(ByteBuffer *buf);
int byte_buffer_resize(ByteBuffer *buf, size_t new_size, uint8_t fill_value);
int byte_buffer_copy(ByteBuffer *dst, const ByteBuffer *src);
int byte_buffer_load_file(ByteBuffer *buf, const char *path);
char *loopy_strdup(const char *s);

#ifdef __cplusplus
}
#endif

#endif
