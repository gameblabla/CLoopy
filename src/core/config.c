#include "core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void byte_buffer_init(ByteBuffer *buf) {
    buf->data = NULL;
    buf->size = 0;
}

void byte_buffer_free(ByteBuffer *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
}

int byte_buffer_resize(ByteBuffer *buf, size_t new_size, uint8_t fill_value) {
    if (new_size == buf->size) return 0;
    uint8_t *new_data = NULL;
    if (new_size) {
        new_data = (uint8_t *)realloc(buf->data, new_size);
        if (!new_data) return -1;
        if (new_size > buf->size) {
            memset(new_data + buf->size, fill_value, new_size - buf->size);
        }
    } else {
        free(buf->data);
    }
    buf->data = new_data;
    buf->size = new_size;
    return 0;
}

int byte_buffer_copy(ByteBuffer *dst, const ByteBuffer *src) {
    byte_buffer_free(dst);
    if (!src->size) {
        dst->data = NULL;
        dst->size = 0;
        return 0;
    }
    dst->data = (uint8_t *)malloc(src->size);
    if (!dst->data) return -1;
    memcpy(dst->data, src->data, src->size);
    dst->size = src->size;
    return 0;
}

int byte_buffer_load_file(ByteBuffer *buf, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return -1; }
    rewind(f);
    byte_buffer_free(buf);
    if (len == 0) { fclose(f); return 0; }
    buf->data = (uint8_t *)malloc((size_t)len);
    if (!buf->data) { fclose(f); return -1; }
    size_t n = fread(buf->data, 1, (size_t)len, f);
    fclose(f);
    if (n != (size_t)len) {
        byte_buffer_free(buf);
        return -1;
    }
    buf->size = (size_t)len;
    return 0;
}

char *loopy_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    if (!copy) return NULL;
    memcpy(copy, s, n);
    return copy;
}
