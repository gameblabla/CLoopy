#include "frontend/cmdlist.h"
#include <stdlib.h>
#include <string.h>

static const char CMDLIST_MAGIC[8] = { 'L','P','C','M','D','L','1','0' };
static const char FRAME_MAGIC[4] = { 'F','R','A','M' };
static const uint32_t CMDLIST_VERSION = 1u;

static int write_exact(FILE *f, const void *data, size_t size) {
    return fwrite(data, 1, size, f) == size ? 0 : -1;
}

static int read_exact(FILE *f, void *data, size_t size) {
    return fread(data, 1, size, f) == size ? 0 : -1;
}

static int write_u32(FILE *f, uint32_t v) { return write_exact(f, &v, sizeof(v)); }
static int read_u32(FILE *f, uint32_t *v) { return read_exact(f, v, sizeof(*v)); }

int loopy_cmdlist_writer_open(LoopyCmdListWriter *writer, const char *path, uint32_t width, uint32_t height) {
    if (!writer || !path || width == 0 || height == 0) return -1;
    memset(writer, 0, sizeof(*writer));
    writer->file = fopen(path, "wb+");
    if (!writer->file) return -1;
    writer->width = width;
    writer->height = height;

    if (write_exact(writer->file, CMDLIST_MAGIC, sizeof(CMDLIST_MAGIC)) != 0 ||
        write_u32(writer->file, CMDLIST_VERSION) != 0 ||
        write_u32(writer->file, width) != 0 ||
        write_u32(writer->file, height) != 0 ||
        write_u32(writer->file, LOOPY_CMDLIST_PIXEL_FORMAT_RGB555) != 0) {
        loopy_cmdlist_writer_close(writer);
        return -1;
    }
    writer->frame_count_offset = ftell(writer->file);
    if (writer->frame_count_offset < 0 || write_u32(writer->file, 0) != 0) {
        loopy_cmdlist_writer_close(writer);
        return -1;
    }
    fflush(writer->file);
    return 0;
}

int loopy_cmdlist_writer_write_frame(LoopyCmdListWriter *writer, uint32_t frame_index, const void *vdp_state, uint32_t vdp_state_size, const uint16_t *framebuffer) {
    if (!writer || !writer->file || !framebuffer) return -1;
    const uint32_t fb_size = writer->width * writer->height * (uint32_t)sizeof(uint16_t);
    if (vdp_state_size && !vdp_state) return -1;

    if (write_exact(writer->file, FRAME_MAGIC, sizeof(FRAME_MAGIC)) != 0 ||
        write_u32(writer->file, frame_index) != 0 ||
        write_u32(writer->file, vdp_state_size) != 0 ||
        write_u32(writer->file, fb_size) != 0) return -1;
    if (vdp_state_size && write_exact(writer->file, vdp_state, vdp_state_size) != 0) return -1;
    if (write_exact(writer->file, framebuffer, fb_size) != 0) return -1;

    writer->frame_count++;

    long here = ftell(writer->file);
    if (here >= 0 && writer->frame_count_offset >= 0) {
        if (fseek(writer->file, writer->frame_count_offset, SEEK_SET) == 0) {
            (void)write_u32(writer->file, writer->frame_count);
            (void)fseek(writer->file, here, SEEK_SET);
        }
    }
    fflush(writer->file);
    return 0;
}

int loopy_cmdlist_writer_close(LoopyCmdListWriter *writer) {
    int result = 0;
    if (!writer) return -1;
    if (writer->file) {
        long here = ftell(writer->file);
        if (writer->frame_count_offset >= 0 && fseek(writer->file, writer->frame_count_offset, SEEK_SET) == 0) {
            if (write_u32(writer->file, writer->frame_count) != 0) result = -1;
        } else {
            result = -1;
        }
        if (here >= 0) (void)fseek(writer->file, here, SEEK_SET);
        if (fclose(writer->file) != 0) result = -1;
    }
    memset(writer, 0, sizeof(*writer));
    return result;
}

int loopy_cmdlist_reader_open(LoopyCmdListReader *reader, const char *path) {
    if (!reader || !path) return -1;
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (!reader->file) return -1;

    char magic[8];
    if (read_exact(reader->file, magic, sizeof(magic)) != 0 || memcmp(magic, CMDLIST_MAGIC, sizeof(magic)) != 0 ||
        read_u32(reader->file, &reader->version) != 0 || reader->version != CMDLIST_VERSION ||
        read_u32(reader->file, &reader->width) != 0 ||
        read_u32(reader->file, &reader->height) != 0 ||
        read_u32(reader->file, &reader->pixel_format) != 0 ||
        read_u32(reader->file, &reader->frame_count) != 0 ||
        reader->width == 0 || reader->height == 0 || reader->pixel_format != LOOPY_CMDLIST_PIXEL_FORMAT_RGB555) {
        loopy_cmdlist_reader_close(reader);
        return -1;
    }

    reader->frames = (LoopyCmdListFrameInfo *)calloc(reader->frame_count ? reader->frame_count : 1u, sizeof(reader->frames[0]));
    if (!reader->frames) {
        loopy_cmdlist_reader_close(reader);
        return -1;
    }

    for (uint32_t i = 0; i < reader->frame_count; i++) {
        char tag[4];
        LoopyCmdListFrameInfo *info = &reader->frames[i];
        if (read_exact(reader->file, tag, sizeof(tag)) != 0 || memcmp(tag, FRAME_MAGIC, sizeof(tag)) != 0 ||
            read_u32(reader->file, &info->frame_index) != 0 ||
            read_u32(reader->file, &info->state_size) != 0 ||
            read_u32(reader->file, &info->framebuffer_size) != 0) {
            loopy_cmdlist_reader_close(reader);
            return -1;
        }
        const uint32_t expected_fb_size = reader->width * reader->height * (uint32_t)sizeof(uint16_t);
        if (info->framebuffer_size != expected_fb_size) {
            loopy_cmdlist_reader_close(reader);
            return -1;
        }
        info->state_offset = ftell(reader->file);
        if (info->state_offset < 0 || fseek(reader->file, (long)info->state_size, SEEK_CUR) != 0) {
            loopy_cmdlist_reader_close(reader);
            return -1;
        }
        info->framebuffer_offset = ftell(reader->file);
        if (info->framebuffer_offset < 0 || fseek(reader->file, (long)info->framebuffer_size, SEEK_CUR) != 0) {
            loopy_cmdlist_reader_close(reader);
            return -1;
        }
    }
    return 0;
}

void loopy_cmdlist_reader_close(LoopyCmdListReader *reader) {
    if (!reader) return;
    if (reader->file) fclose(reader->file);
    free(reader->frames);
    memset(reader, 0, sizeof(*reader));
}

int loopy_cmdlist_reader_read_framebuffer(LoopyCmdListReader *reader, uint32_t frame_number, uint16_t *dst, uint32_t dst_pixels) {
    if (!reader || !reader->file || !dst || frame_number >= reader->frame_count) return -1;
    uint32_t pixels = reader->width * reader->height;
    if (dst_pixels < pixels) return -1;
    LoopyCmdListFrameInfo *info = &reader->frames[frame_number];
    if (fseek(reader->file, info->framebuffer_offset, SEEK_SET) != 0) return -1;
    return read_exact(reader->file, dst, (size_t)pixels * sizeof(uint16_t));
}

int loopy_cmdlist_reader_read_state(LoopyCmdListReader *reader, uint32_t frame_number, void *dst, uint32_t dst_size) {
    if (!reader || !reader->file || frame_number >= reader->frame_count) return -1;
    LoopyCmdListFrameInfo *info = &reader->frames[frame_number];
    if (dst_size != info->state_size || (dst_size && !dst)) return -1;
    if (fseek(reader->file, info->state_offset, SEEK_SET) != 0) return -1;
    return dst_size ? read_exact(reader->file, dst, dst_size) : 0;
}
