#include "core/loopy_io.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_file(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);
    *out = buf;
    *out_size = (size_t)sz;
    return 0;
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int png_dimensions(const char *path, uint32_t *w, uint32_t *h) {
    uint8_t *buf = NULL;
    size_t size = 0;
    if (read_file(path, &buf, &size) != 0) return -1;
    if (size < 24 || buf[0] != 137 || buf[1] != 'P' || buf[2] != 'N' || buf[3] != 'G') {
        free(buf);
        return -1;
    }
    *w = rd32be(buf + 16);
    *h = rd32be(buf + 20);
    free(buf);
    return 0;
}

int main(void) {
    remove("loopy_print_000.png");
    remove("loopy_print_raw_head_000.png");

    /* The user-facing print output must be the BIOS print source image, not
       VDP capture, a composed screen snapshot, or raw thermal-head bits. */
    loopy_io_initialize();
    uint16_t img[64 * 32];
    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            uint16_t r = (uint16_t)((x >> 1) & 0x1F);
            uint16_t g = (uint16_t)((y >> 0) & 0x1F);
            uint16_t b = (uint16_t)(((x ^ y) >> 1) & 0x1F);
            img[y * 64 + x] = (uint16_t)((r << 10) | (g << 5) | b);
        }
    }
    loopy_io_printer_write_source_image(img, 64, 32, "unit-test-source");
    loopy_io_shutdown();

    uint32_t w = 0, h = 0;
    if (png_dimensions("loopy_print_000.png", &w, &h) != 0 || w != 64u || h != 32u) {
        fprintf(stderr, "printer_png_test: BIOS source output wrong dimensions (%ux%u)\n", w, h);
        return 1;
    }
    remove("loopy_print_000.png");

    /* Capture/snapshot fallback should not create misleading user-facing output. */
    loopy_io_initialize();
    for (uint16_t y = 10; y < 34; y++) {
        uint16_t line[256];
        for (int x = 0; x < 256; x++) line[x] = (uint16_t)(((x >> 3) << 10) | (((y >> 1) & 0x1F) << 5));
        loopy_io_printer_capture_scanline(y, line, 256, 0);
    }
    loopy_io_shutdown();
    if (png_dimensions("loopy_print_000.png", &w, &h) == 0) {
        fprintf(stderr, "printer_png_test: capture incorrectly emitted user-facing PNG\n");
        return 1;
    }

    loopy_io_initialize();
    uint16_t frame[256 * 240];
    for (uint32_t i = 0; i < 256u * 240u; i++) frame[i] = 0x7FFFu;
    loopy_io_printer_frame_snapshot(frame, 256, 240);
    loopy_io_trigger_printer_sensors();
    loopy_io_reg_write16(0x040, 0x00FFu);
    loopy_io_reg_write16(0x042, 0x5A52u);
    loopy_io_reg_write16(0x042, 0x5A50u);
    loopy_io_shutdown();
    if (png_dimensions("loopy_print_000.png", &w, &h) == 0) {
        fprintf(stderr, "printer_png_test: screen snapshot incorrectly emitted user-facing PNG\n");
        return 1;
    }

    puts("printer_png_test: OK");
    return 0;
}
