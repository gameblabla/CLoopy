#ifndef LOOPY_FRONTEND_CMDLIST_H
#define LOOPY_FRONTEND_CMDLIST_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOOPY_CMDLIST_WIDTH 256u
#define LOOPY_CMDLIST_HEIGHT 240u
#define LOOPY_CMDLIST_PIXEL_FORMAT_RGB555 1u

typedef struct LoopyCmdListWriter {
    FILE *file;
    uint32_t frame_count;
    long frame_count_offset;
    uint32_t width;
    uint32_t height;
} LoopyCmdListWriter;

typedef struct LoopyCmdListFrameInfo {
    uint32_t frame_index;
    uint32_t state_size;
    uint32_t framebuffer_size;
    long state_offset;
    long framebuffer_offset;
} LoopyCmdListFrameInfo;

typedef struct LoopyCmdListReader {
    FILE *file;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t frame_count;
    LoopyCmdListFrameInfo *frames;
} LoopyCmdListReader;

int loopy_cmdlist_writer_open(LoopyCmdListWriter *writer, const char *path, uint32_t width, uint32_t height);
int loopy_cmdlist_writer_write_frame(LoopyCmdListWriter *writer, uint32_t frame_index, const void *vdp_state, uint32_t vdp_state_size, const uint16_t *framebuffer);
int loopy_cmdlist_writer_close(LoopyCmdListWriter *writer);

int loopy_cmdlist_reader_open(LoopyCmdListReader *reader, const char *path);
void loopy_cmdlist_reader_close(LoopyCmdListReader *reader);
int loopy_cmdlist_reader_read_framebuffer(LoopyCmdListReader *reader, uint32_t frame_number, uint16_t *dst, uint32_t dst_pixels);
int loopy_cmdlist_reader_read_state(LoopyCmdListReader *reader, uint32_t frame_number, void *dst, uint32_t dst_size);

#ifdef __cplusplus
}
#endif

#endif
