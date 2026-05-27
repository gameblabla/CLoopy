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

typedef struct ConfigSystemInfo {
    ConfigCartInfo cart;
    ByteBuffer bios_rom;
    ByteBuffer sound_rom;
} ConfigSystemInfo;

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
