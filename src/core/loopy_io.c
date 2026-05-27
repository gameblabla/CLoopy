#include "core/loopy_io.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOUSE_LEFT 0
#define MOUSE_RIGHT 1
#define PRINTER_CAPTURE_WIDTH 256u
#define PRINTER_CAPTURE_HEIGHT 240u

/*
 * The Loopy controller port is six output strobes by eight input pins.
 *
 * VDP.MODE bit 4 (CMODE) is significant.  With CMODE=1 the VDP performs
 * the automatic six-phase gamepad matrix scan and CONTROL_IN[0..2] expose
 * those latched rows.  With CMODE=0 the automatic scanner is off; the
 * registers expose only the immediate input pins selected by CONTROL_OUT.
 * A game that forgets to set CMODE must therefore see no normal gamepad
 * controls unless it is explicitly bit-banging CONTROL_OUT in direct mode.
 *
 * CONTROL_OUT writes are ignored while CMODE=1 on hardware, so keep the
 * direct-output latch unchanged in matrix mode.  This prevents stale writes
 * from accidentally making direct mode behave like the matrix scanner.
 *
 * The official mouse ignores the output strobes.  Its detect bit is visible
 * through CONTROL_IN; movement is decoded into CONTROL_MOUSE[0..1] only when
 * VDP.MODE.MCNT is set.
 */
typedef struct PrinterLine {
    uint32_t offset;
    uint16_t words;
} PrinterLine;

typedef struct LoopyPrinterState {
    uint16_t analog_ctrl;
    uint16_t adc_value;
    uint16_t sensors_latch;
    int sensors_enable;
    uint16_t head_data;
    uint16_t head_ctrl;
    uint16_t motor;
    uint8_t last_motor_phase;
    int8_t last_motor_index;
    int32_t motor_position;
    unsigned step_count;
    unsigned print_index;
    int active;
    int dirty;
    uint16_t *word_pool;
    uint32_t word_count;
    uint32_t word_capacity;
    PrinterLine *lines;
    uint32_t line_count;
    uint32_t line_capacity;
    uint16_t current_words[128];
    uint16_t current_count;
    uint16_t max_words_per_line;
    char output_dir[512];
    int trace;
    unsigned sensor_reads;
    unsigned sensor_triggers;
    unsigned motor_writes;
    unsigned head_ctrl_writes;
    unsigned head_words_written;
    uint16_t capture_rgb555[PRINTER_CAPTURE_WIDTH * PRINTER_CAPTURE_HEIGHT];
    uint8_t capture_valid[PRINTER_CAPTURE_HEIGHT];
    unsigned capture_line_count;
    unsigned capture_first_y;
    unsigned capture_last_y;
    unsigned capture_events;
    uint8_t capture_last_format;
    uint16_t latest_frame_rgb555[PRINTER_CAPTURE_WIDTH * PRINTER_CAPTURE_HEIGHT];
    uint8_t latest_frame_valid;
    uint16_t print_snapshot_rgb555[PRINTER_CAPTURE_WIDTH * PRINTER_CAPTURE_HEIGHT];
    uint8_t print_snapshot_valid;
    unsigned print_snapshot_index;
    uint8_t source_image_seen;
    uint16_t *pending_rgb555;
    uint32_t pending_width;
    uint32_t pending_height;
    uint32_t pending_serial;
} LoopyPrinterState;

typedef struct LoopyIOState {
    uint16_t pad_buttons;
    int matrix_mode;
    int mouse_counter_enable;
    int mouse_connected;
    int mouse_left;
    int mouse_right;
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_quad_x;
    uint8_t mouse_quad_y;
    uint8_t control_out;
    uint8_t control_latch[6];
    unsigned debug_ctrl_reads[3];
    uint16_t debug_ctrl_last[3];
    LoopyPrinterState printer;
} LoopyIOState;

static LoopyIOState state;

static uint32_t png_crc_table[256];
static int png_crc_table_ready;

static void png_crc_init(void) {
    if (png_crc_table_ready) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        png_crc_table[n] = c;
    }
    png_crc_table_ready = 1;
}

static uint32_t png_crc_update(uint32_t c, const uint8_t *buf, size_t len) {
    png_crc_init();
    for (size_t n = 0; n < len; n++) c = png_crc_table[(c ^ buf[n]) & 0xFFu] ^ (c >> 8);
    return c;
}

static uint32_t png_adler32(const uint8_t *buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a += buf[i];
        b += a;
        if ((i & 0x0FFFu) == 0x0FFFu) { a %= 65521u; b %= 65521u; }
    }
    a %= 65521u;
    b %= 65521u;
    return (b << 16) | a;
}

static int write_be32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static int write_png_chunk(FILE *f, const char type[4], const uint8_t *data, size_t len) {
    if (len > 0xFFFFFFFFu) return -1;
    if (write_be32(f, (uint32_t)len) != 0) return -1;
    if (fwrite(type, 1, 4, f) != 4) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    uint32_t c = 0xFFFFFFFFu;
    c = png_crc_update(c, (const uint8_t *)type, 4);
    if (len) c = png_crc_update(c, data, len);
    c ^= 0xFFFFFFFFu;
    return write_be32(f, c);
}

static int write_rgb_png_uncompressed(const char *path, const uint8_t *rgb, uint32_t width, uint32_t height) {
    if (!path || !rgb || width == 0 || height == 0) return -1;
    const size_t row_stride = (size_t)width * 3u;
    const size_t raw_size = (row_stride + 1u) * (size_t)height;
    uint8_t *raw = (uint8_t *)malloc(raw_size ? raw_size : 1u);
    if (!raw) return -1;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst = raw + (row_stride + 1u) * y;
        dst[0] = 0; /* PNG filter: none. */
        memcpy(dst + 1, rgb + row_stride * y, row_stride);
    }

    const size_t block_count = (raw_size + 65534u) / 65535u;
    const size_t z_size = 2u + raw_size + block_count * 5u + 4u;
    uint8_t *z = (uint8_t *)malloc(z_size ? z_size : 1u);
    if (!z) { free(raw); return -1; }
    size_t zp = 0, rp = 0;
    z[zp++] = 0x78; /* zlib header: deflate, 32K window */
    z[zp++] = 0x01; /* no compression/fastest check bits */
    while (rp < raw_size || (raw_size == 0 && zp == 2)) {
        size_t remaining = raw_size - rp;
        uint16_t block = (uint16_t)(remaining > 65535u ? 65535u : remaining);
        int final_block = (rp + block >= raw_size);
        z[zp++] = (uint8_t)(final_block ? 1 : 0); /* BFINAL + stored block. */
        z[zp++] = (uint8_t)(block & 0xFFu);
        z[zp++] = (uint8_t)(block >> 8);
        uint16_t nlen = (uint16_t)~block;
        z[zp++] = (uint8_t)(nlen & 0xFFu);
        z[zp++] = (uint8_t)(nlen >> 8);
        if (block) { memcpy(z + zp, raw + rp, block); zp += block; rp += block; }
        if (raw_size == 0) break;
    }
    uint32_t ad = png_adler32(raw, raw_size);
    z[zp++] = (uint8_t)(ad >> 24);
    z[zp++] = (uint8_t)(ad >> 16);
    z[zp++] = (uint8_t)(ad >> 8);
    z[zp++] = (uint8_t)ad;

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13] = {
        (uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
        (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
        8, 2, 0, 0, 0
    };
    int ok = 0;
    if (fwrite(sig, 1, sizeof(sig), f) != sizeof(sig) ||
        write_png_chunk(f, "IHDR", ihdr, sizeof(ihdr)) != 0 ||
        write_png_chunk(f, "IDAT", z, zp) != 0 ||
        write_png_chunk(f, "IEND", NULL, 0) != 0) ok = -1;
    if (fclose(f) != 0) ok = -1;
    free(raw);
    free(z);
    return ok;
}

static void printer_reset_capture(LoopyPrinterState *p) {
    p->active = 0;
    p->dirty = 0;
    p->word_count = 0;
    p->line_count = 0;
    p->current_count = 0;
    p->max_words_per_line = 0;
    memset(p->capture_valid, 0, sizeof(p->capture_valid));
    p->capture_line_count = 0;
    p->capture_first_y = PRINTER_CAPTURE_HEIGHT;
    p->capture_last_y = 0;
    p->capture_events = 0;
    p->capture_last_format = 0xFFu;
    p->print_snapshot_valid = 0;
}

static void printer_clear_pending_image(LoopyPrinterState *p) {
    if (!p) return;
    free(p->pending_rgb555);
    p->pending_rgb555 = NULL;
    p->pending_width = 0;
    p->pending_height = 0;
}

static void printer_init_defaults(LoopyPrinterState *p) {
    memset(p, 0, sizeof(*p));
    p->adc_value = 0x147u;
    p->sensors_enable = 0;
    /* Stock NTSC unit, seal cartridge installed at the documented initial position:
     * SCT=1, OPTO=100, RJMP=1.  ENP is latched by VDP.TRIGGER.PSEN. */
    p->sensors_latch = (uint16_t)((1u << 4) | (4u << 1) | 1u);
    p->capture_first_y = PRINTER_CAPTURE_HEIGHT;
    p->capture_last_y = 0;
    p->capture_last_format = 0xFFu;
    p->last_motor_index = -1;
    p->motor_position = 0;
    p->latest_frame_valid = 0;
    p->print_snapshot_valid = 0;
    p->print_snapshot_index = 0;
}

static int printer_append_words(LoopyPrinterState *p, const uint16_t *words, uint16_t count) {
    if (!count) return 0;
    if (p->line_count >= 4096u) return -1;
    if (p->word_count + count > p->word_capacity) {
        uint32_t new_cap = p->word_capacity ? p->word_capacity * 2u : 4096u;
        while (new_cap < p->word_count + count) new_cap *= 2u;
        uint16_t *nw = (uint16_t *)realloc(p->word_pool, (size_t)new_cap * sizeof(p->word_pool[0]));
        if (!nw) return -1;
        p->word_pool = nw;
        p->word_capacity = new_cap;
    }
    if (p->line_count >= p->line_capacity) {
        uint32_t new_cap = p->line_capacity ? p->line_capacity * 2u : 256u;
        PrinterLine *nl = (PrinterLine *)realloc(p->lines, (size_t)new_cap * sizeof(p->lines[0]));
        if (!nl) return -1;
        p->lines = nl;
        p->line_capacity = new_cap;
    }
    PrinterLine *line = &p->lines[p->line_count++];
    line->offset = p->word_count;
    line->words = count;
    memcpy(p->word_pool + p->word_count, words, (size_t)count * sizeof(words[0]));
    p->word_count += count;
    if (count > p->max_words_per_line) p->max_words_per_line = count;
    return 0;
}

static void printer_flush_current_line(LoopyPrinterState *p) {
    if (!p->current_count) return;
    (void)printer_append_words(p, p->current_words, p->current_count);
    p->current_count = 0;
}

static void printer_format_path(const LoopyPrinterState *p, char *out, size_t out_size, unsigned index) {
    if (p->output_dir[0]) {
        size_t n = strlen(p->output_dir);
        const char *sep = (n && (p->output_dir[n - 1] == '/' || p->output_dir[n - 1] == '\\')) ? "" : "/";
        snprintf(out, out_size, "%s%sloopy_print_%03u.png", p->output_dir, sep, index % 1000u);
    } else {
        snprintf(out, out_size, "loopy_print_%03u.png", index % 1000u);
    }
}

static void printer_format_named_path(const LoopyPrinterState *p, char *out, size_t out_size, unsigned index, const char *stem) {
    if (!stem || !*stem) stem = "loopy_print";
    if (p->output_dir[0]) {
        size_t n = strlen(p->output_dir);
        const char *sep = (n && (p->output_dir[n - 1] == '/' || p->output_dir[n - 1] == '\\')) ? "" : "/";
        snprintf(out, out_size, "%s%s%s_%03u.png", p->output_dir, sep, stem, index % 1000u);
    } else {
        snprintf(out, out_size, "%s_%03u.png", stem, index % 1000u);
    }
}

static void printer_make_unique_png_path(LoopyPrinterState *p, char *out, size_t out_size) {
    for (unsigned i = p->print_index; i < p->print_index + 1000u; i++) {
        printer_format_path(p, out, out_size, i);
        FILE *f = fopen(out, "rb");
        if (!f) { p->print_index = (i + 1u) % 1000u; return; }
        fclose(f);
    }
    printer_format_path(p, out, out_size, p->print_index++ % 1000u);
}

static void printer_make_unique_named_png_path(LoopyPrinterState *p, char *out, size_t out_size, const char *stem) {
    for (unsigned i = p->print_index; i < p->print_index + 1000u; i++) {
        printer_format_named_path(p, out, out_size, i, stem);
        FILE *f = fopen(out, "rb");
        if (!f) { p->print_index = (i + 1u) % 1000u; return; }
        fclose(f);
    }
    printer_format_named_path(p, out, out_size, p->print_index++ % 1000u, stem);
}

static void rgb555_to_rgb888(uint16_t c, uint8_t *rgb) {
    uint8_t r = (uint8_t)((c >> 10) & 0x1Fu);
    uint8_t g = (uint8_t)((c >> 5) & 0x1Fu);
    uint8_t b = (uint8_t)(c & 0x1Fu);
    rgb[0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[1] = (uint8_t)((g << 3) | (g >> 2));
    rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

static int printer_write_rgb555_png(const char *path, const uint16_t *rgb555, uint32_t width, uint32_t height) {
    if (!path || !rgb555 || width == 0 || height == 0) return 0;
    uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3u);
    if (!rgb) return 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            rgb555_to_rgb888(rgb555[(size_t)y * width + x], rgb + ((size_t)y * width + x) * 3u);
        }
    }
    int ok = write_rgb_png_uncompressed(path, rgb, width, height) == 0;
    free(rgb);
    return ok;
}

static void printer_begin_snapshot_if_needed(LoopyPrinterState *p, const char *reason) {
    if (!p || p->print_snapshot_valid || !p->latest_frame_valid) return;
    memcpy(p->print_snapshot_rgb555, p->latest_frame_rgb555, sizeof(p->print_snapshot_rgb555));
    p->print_snapshot_valid = 1;
    p->print_snapshot_index++;
    if (p->trace) {
        printf("[Printer] SNAPSHOT #%u from latest composed frame (%s)\n",
               p->print_snapshot_index, reason ? reason : "printer activity");
    }
}

static int printer_write_snapshot_png_if_ready(LoopyPrinterState *p) {
    if (!p->print_snapshot_valid) return 0;
    char path[1024];
    printer_make_unique_png_path(p, path, sizeof(path));
    if (printer_write_rgb555_png(path, p->print_snapshot_rgb555, PRINTER_CAPTURE_WIDTH, PRINTER_CAPTURE_HEIGHT)) {
        printf("[Printer] Wrote %s from composed-frame print snapshot (256x240)\n", path);
        return 1;
    }
    fprintf(stderr, "[Printer] Failed to write %s\n", path);
    return 0;
}

static int printer_write_capture_png_if_ready(LoopyPrinterState *p) {
    if (p->capture_line_count < 8u || p->capture_first_y >= PRINTER_CAPTURE_HEIGHT) return 0;
    uint32_t first = p->capture_first_y;
    uint32_t last = p->capture_last_y;
    if (last >= PRINTER_CAPTURE_HEIGHT) last = PRINTER_CAPTURE_HEIGHT - 1u;
    if (last < first) return 0;
    uint32_t width = PRINTER_CAPTURE_WIDTH;
    uint32_t height = last - first + 1u;
    uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3u);
    if (!rgb) return 0;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t src_y = first + y;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t *px = rgb + ((size_t)y * width + x) * 3u;
            if (src_y < PRINTER_CAPTURE_HEIGHT && p->capture_valid[src_y]) {
                rgb555_to_rgb888(p->capture_rgb555[src_y * PRINTER_CAPTURE_WIDTH + x], px);
            } else {
                px[0] = 255u; px[1] = 255u; px[2] = 255u;
            }
        }
    }

    char path[1024];
    printer_make_unique_png_path(p, path, sizeof(path));
    int ok = write_rgb_png_uncompressed(path, rgb, width, height);
    if (ok == 0) {
        printf("[Printer] Wrote %s from VDP capture (%ux%u, lines=%u/%u, y=%u..%u, last_format=%u)\n",
               path, width, height, p->capture_line_count, height, first, last,
               (unsigned)p->capture_last_format);
    } else {
        fprintf(stderr, "[Printer] Failed to write %s\n", path);
    }
    free(rgb);
    return ok == 0;
}

static int printer_raw_bit(const LoopyPrinterState *p, uint32_t raw_x, uint32_t raw_y, int invert) {
    if (raw_y >= p->line_count) return 0;
    const PrinterLine *line = &p->lines[raw_y];
    uint32_t word_index = raw_x >> 4;
    if (word_index >= line->words) return 0;
    uint16_t w = p->word_pool[line->offset + word_index];
    int set = (int)((w >> (15u - (raw_x & 15u))) & 1u);
    return invert ? !set : set;
}

static uint8_t density_to_u8(uint32_t on, uint32_t total) {
    if (total == 0) return 0;
    if (on >= total) return 255;
    return (uint8_t)((on * 255u + (total / 2u)) / total);
}

static void printer_choose_logical_layout(uint32_t raw_width, uint32_t raw_height,
                                          uint32_t *sx, uint32_t *sy, uint32_t *passes) {
    uint32_t xscale = 1;

    /* The head stream is already the VDP's dot-patterned/serialized thermal output,
     * not a display-ready bitmap.  Prefer a logical 256-pixel image when the raw
     * stream clearly contains oversampled printer dots; this matches the documented
     * 256-pixel VDP capture width used for sticker image generation.  Small unit-test
     * streams still get a 2x2 cell average so checkerboard halftone patterns do not
     * leak into the user-visible PNG. */
    if (raw_width >= 256u && (raw_width % 256u) == 0) {
        xscale = raw_width / 256u;
        if (xscale < 1u) xscale = 1u;
        if (xscale > 16u) xscale = 16u;
    } else if (raw_width >= 1024u) {
        xscale = 4u;
    } else if (raw_width >= 64u) {
        xscale = 2u;
    }

    uint32_t yscale = xscale;
    if (yscale < 1u) yscale = 1u;
    while (yscale > 1u && raw_height < yscale) yscale >>= 1;

    uint32_t pass_count = 1u;
    if (raw_height >= (yscale * 96u * 3u)) {
        pass_count = 3u;
    }

    *sx = xscale;
    *sy = yscale;
    *passes = pass_count;
}

static void printer_write_png_if_ready(LoopyPrinterState *p) {
    printer_flush_current_line(p);

    if (p->source_image_seen) {
        /* A BIOS print-source dump has already emitted the intended sticker image.
         * Do not overwrite it with any fallback representation at motor shutdown. */
        p->source_image_seen = 0;
        printer_reset_capture(p);
        return;
    }

    /* Do not present VDP capture, composed-screen snapshots, or thermal-head
     * streams as user-facing print output.  The Loopy printer image is supplied
     * to the BIOS print API as data/palette/dimension/format parts.  VDP capture
     * and PRINT_HEAD_DATA are later/intermediate stages and were producing
     * misleading stripes or overlay/cursor screenshots.  Keep raw-head output
     * only as an explicit trace diagnostic. */
    if (!p->trace) {
        printer_reset_capture(p);
        return;
    }

    uint32_t raw_width = p->max_words_per_line * 16u;
    uint32_t raw_height = p->line_count;
    if (raw_width == 0 || raw_height == 0 || raw_width > 4096u || raw_height > 8192u) { printer_reset_capture(p); return; }

    uint32_t sx = 0, sy = 0, passes = 0;
    printer_choose_logical_layout(raw_width, raw_height, &sx, &sy, &passes);
    if (sx == 0 || sy == 0 || passes == 0) { printer_reset_capture(p); return; }

    uint32_t width = raw_width / sx;
    uint32_t height = raw_height / (sy * passes);
    if (width == 0 || height == 0 || width > 4096u || height > 8192u) { printer_reset_capture(p); return; }

    uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3u);
    if (!rgb) { printer_reset_capture(p); return; }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            unsigned on[3] = {0,0,0};
            for (uint32_t pass = 0; pass < passes && pass < 3u; pass++) {
                for (uint32_t yy = 0; yy < sy; yy++) {
                    for (uint32_t xx = 0; xx < sx; xx++) {
                        uint32_t raw_x = x * sx + xx;
                        uint32_t raw_y = (y * passes + pass) * sy + yy;
                        int invert = (passes == 1u);
                        on[pass] += printer_raw_bit(p, raw_x, raw_y, invert) ? 1u : 0u;
                    }
                }
            }
            unsigned threshold = (sx * sy + 1u) / 2u;
            uint8_t *px = rgb + ((size_t)y * width + x) * 3u;
            if (passes >= 3u) {
                px[0] = (on[1] >= threshold) ? 0u : 255u;
                px[1] = (on[0] >= threshold) ? 0u : 255u;
                px[2] = (on[2] >= threshold) ? 0u : 255u;
            } else {
                uint8_t v = (on[0] >= threshold) ? 0u : 255u;
                px[0] = px[1] = px[2] = v;
            }
        }
    }

    char path[1024];
    printer_make_unique_named_png_path(p, path, sizeof(path), "loopy_print_raw_head");
    if (write_rgb_png_uncompressed(path, rgb, width, height) == 0) {
        printf("[Printer] Wrote %s from raw thermal-head diagnostic stream (%ux%u logical, %ux%u raw, %u pass%s, cell %ux%u, words=%u lines=%u steps=%u)\n",
               path, width, height, raw_width, raw_height, passes, passes == 1u ? "" : "es", sx, sy,
               p->word_count, p->line_count, p->step_count);
    } else {
        fprintf(stderr, "[Printer] Failed to write diagnostic raw-head PNG %s\n", path);
    }
    free(rgb);
    printer_reset_capture(p);
}

static uint8_t printer_current_opto(const LoopyPrinterState *p);
static void printer_update_sensor_latch(LoopyPrinterState *p);

static void printer_write_head_data(LoopyPrinterState *p, uint16_t value) {
    printer_begin_snapshot_if_needed(p, "head-data");
    p->head_data = value;
    p->head_words_written++;
    p->active = 1;
    p->dirty = 1;
    if (p->trace && (p->head_words_written <= 16u || (p->head_words_written & 0x0FFFu) == 0)) {
        printf("[Printer] HEAD_DATA #%u = %04X\n", p->head_words_written, value);
    }
    if (p->current_count >= (uint16_t)(sizeof(p->current_words) / sizeof(p->current_words[0]))) {
        printer_flush_current_line(p);
    }
    p->current_words[p->current_count++] = value;
}

static int printer_motor_phase_index(uint8_t phase) {
    /* The BIOS drives the 4-phase stepper as 0xC, 0x6, 0x3, 0x9 and writes
     * 0 when the motor is de-energized.  A change from one nonzero phase to
     * the next adjacent phase advances the mechanism by one signed step; the
     * reverse sequence moves it back.  Merely energizing the first phase from
     * zero does not by itself advance the paper/ribbon. */
    switch (phase & 0x0Fu) {
    case 0xCu: return 0;
    case 0x6u: return 1;
    case 0x3u: return 2;
    case 0x9u: return 3;
    default: return -1;
    }
}

static int printer_phase_delta(int old_index, int new_index) {
    if (old_index < 0 || new_index < 0 || old_index == new_index) return 0;
    int d = (new_index - old_index) & 3;
    if (d == 1) return +1;
    if (d == 3) return -1;
    return 0;
}

static void printer_write_motor(LoopyPrinterState *p, uint16_t value) {
    uint16_t key = value >> 4;
    uint8_t phase = (uint8_t)(value & 0xFu);
    if (key != 0x5A5u || !p->sensors_enable) return;
    printer_begin_snapshot_if_needed(p, phase ? "motor-start" : "motor-stop");
    p->motor_writes++;

    int new_index = printer_motor_phase_index(phase);
    int delta = printer_phase_delta(p->last_motor_index, new_index);

    if (p->trace && (p->motor_writes <= 16u || phase == 0 || delta || (p->motor_writes & 0xFFu) == 0)) {
        printf("[Printer] MOTOR #%u = %04X phase=%X pos=%d delta=%d steps=%u opto=%u\n",
               p->motor_writes, value, phase, (int)p->motor_position, delta, p->step_count,
               (unsigned)printer_current_opto(p));
    }
    if (phase != p->last_motor_phase) {
        if (p->current_count) printer_flush_current_line(p);
        if (delta) {
            p->motor_position += delta;
            p->step_count++;
        }
        if (new_index >= 0) p->last_motor_index = (int8_t)new_index;
        else p->last_motor_index = -1;
        printer_update_sensor_latch(p);
        p->last_motor_phase = phase;
    }
    p->motor = value;
    if (phase == 0 && p->dirty) printer_write_png_if_ready(p);
}

static void printer_write_head_ctrl(LoopyPrinterState *p, uint16_t value) {
    uint16_t key = value >> 4;
    if (key != 0xA5Au || !p->sensors_enable) return;
    printer_begin_snapshot_if_needed(p, "head-control");
    p->head_ctrl_writes++;
    p->head_ctrl = value;
    if (p->trace && (p->head_ctrl_writes <= 16u || (value & 0x7u) == 0)) {
        printf("[Printer] HEAD_CTRL #%u = %04X\n", p->head_ctrl_writes, value);
    }
    /* PHC low bits are cleared by the BIOS when halting the head.  Treat that as
     * an end-of-print hint but keep accumulating if more data arrives later. */
    if ((value & 0x7u) == 0 && p->dirty && p->current_count) printer_flush_current_line(p);
}

static unsigned printer_pos_mod(int32_t v, unsigned m) {
    int32_t r = (int32_t)(v % (int32_t)m);
    if (r < 0) r += (int32_t)m;
    return (unsigned)r;
}

static uint8_t printer_current_opto(const LoopyPrinterState *p) {
    /* Synthetic mechanism model.  Hardware exposes an ink/ribbon sensor and two
     * paper mark sensors.  The important behavior for the BIOS is that these
     * sensors are a function of the mechanism position, including reverse motor
     * movement, not a monotonic count of phase writes.  The phase decoder above
     * tracks signed stepper motion; this function maps that position to a stock
     * cartridge with cyan at the home position and periodic black paper marks. */
    unsigned pos = printer_pos_mod(p->motor_position, 768u);
    unsigned color = (pos < 256u) ? 1u : 0u;
    unsigned upper_mark = ((pos % 192u) < 10u) ? 1u : 0u;
    unsigned lower_mark = (((pos + 56u) % 192u) < 10u) ? 1u : 0u;

    /* At the documented initial/home position, the installed seal cartridge
     * reads OPTO=100: cyan ribbon, unmarked paper on both paper sensors. */
    if (pos < 8u || pos >= 760u) {
        upper_mark = 0u;
        lower_mark = 0u;
        color = 1u;
    }
    return (uint8_t)((color << 2) | (upper_mark << 1) | lower_mark);
}

static void printer_update_sensor_latch(LoopyPrinterState *p) {
    uint8_t opto = printer_current_opto(p);
    p->sensors_latch = (uint16_t)((1u << 4) | ((uint16_t)opto << 1) | 1u);
}

static uint16_t printer_adc_for_mux(uint16_t mux) {
    switch (mux & 7u) {
    case 0: return 0x147u; /* Thermistor: approx 20 C, safely below BIOS warnings. */
    case 1: return 0x1AFu; /* Head calibration resistor, from measured example. */
    case 2: return 0x200u; /* Contrast potentiometer centered. */
    case 3: return 0x3FFu; /* Floating cartridge ADC inputs. */
    default: return 0x3FFu;
    }
}

static int16_t clamp_mouse_delta(int v) {
    if (v > 2047) return 2047;
    if (v < -2048) return -2048;
    return (int16_t)v;
}

static uint16_t sign_extend12(uint16_t v) {
    return v & 0x0FFFu;
}

static uint8_t pad_matrix_row(unsigned row) {
    switch (row) {
    case 0:
        return (uint8_t)((state.pad_buttons & 0x000Fu) >> 0);   /* DET, ST, TL, TR */
    case 1:
        return (uint8_t)((state.pad_buttons & 0x00F0u) >> 4);   /* A, D, C, B */
    case 2:
        return (uint8_t)((state.pad_buttons & 0x0F00u) >> 8);   /* DU, DD, DL, DR */
    default:
        return 0;
    }
}

static uint8_t pad_direct_input(void) {
    uint8_t value = 0;
    for (unsigned row = 0; row < 6; row++) {
        if (state.control_out & (1u << row)) value |= pad_matrix_row(row);
    }
    return value;
}

static void set_direct_control_out(uint8_t value) {
    /* In matrix mode the VDP drives the scan phases itself.  Writes to the
     * direct output register are ignored for port output and must not arm a
     * hidden direct-mode scanner. */
    if (state.matrix_mode) return;
    state.control_out = (uint8_t)(value & 0x3Fu);
}

static uint8_t mouse_input_byte(void) {
    uint8_t v = 0;
    if (state.mouse_connected) v |= 0x80;       /* DET */
    if (!state.mouse_right) v |= 0x40;          /* RMB, inverted */
    if (!state.mouse_left) v |= 0x10;           /* LMB, inverted */
    v |= (uint8_t)((state.mouse_quad_y & 1u) << 3);          /* ENC YB */
    v |= (uint8_t)(((state.mouse_quad_y >> 1) & 1u) << 2);   /* ENC YA */
    v |= (uint8_t)((state.mouse_quad_x & 1u) << 1);          /* ENC XB */
    v |= (uint8_t)((state.mouse_quad_x >> 1) & 1u);          /* ENC XA */
    return v;
}

static uint8_t control_latch_byte(unsigned index) {
    if (state.mouse_connected) return mouse_input_byte();

    if (state.matrix_mode) {
        /* Automatic VDP matrix scan enabled by VDP.MODE.CMODE.  Hardware
         * exposes latched rows, not the instantaneous button state.  The
         * latches are updated from VCOUNT-timed scan phases by
         * loopy_io_matrix_scan_vcount(). */
        return state.control_latch[index < 6u ? index : 0u];
    }

    /* Direct mode: no automatic gamepad scan.  With CONTROL_OUT cleared this
     * reads as zero, matching real hardware and catching ROMs that forgot to
     * set VDP.MODE bit 4 before reading normal controls. */
    return pad_direct_input();
}

void loopy_io_initialize(void) {
    memset(&state, 0, sizeof(state));
    state.matrix_mode = 0;              /* Boot/direct mode until BIOS/game sets VDP.MODE. */
    state.mouse_counter_enable = 0;
    printer_init_defaults(&state.printer);
}

void loopy_io_shutdown(void) {
    printer_write_png_if_ready(&state.printer);
    printer_clear_pending_image(&state.printer);
    free(state.printer.word_pool);
    free(state.printer.lines);
}

uint8_t loopy_io_reg_read8(uint32_t addr) {
    uint16_t value = loopy_io_reg_read16(addr & ~1u);
    return (addr & 1u) ? (uint8_t)value : (uint8_t)(value >> 8);
}

uint16_t loopy_io_reg_read16(uint32_t addr) {
    addr &= 0xFFFu;
    switch (addr) {
    case 0x000:
        return (uint16_t)((state.printer.adc_value << 6) | (state.printer.analog_ctrl & 0x3Fu));
    case 0x020:
        return 0;
    case 0x030:
        state.printer.sensor_reads++;
        if (state.printer.trace && (state.printer.sensor_reads <= 16u || (state.printer.sensor_reads & 0xFFu) == 0)) {
            printf("[Printer] SENSORS read #%u -> %04X step=%u\n", state.printer.sensor_reads, (uint16_t)(state.printer.sensors_latch | (state.printer.sensors_enable ? 0x0100u : 0)), state.printer.step_count);
        }
        return (uint16_t)(state.printer.sensors_latch | (state.printer.sensors_enable ? 0x0100u : 0));
    case 0x040:
        return state.printer.head_data;
    case 0x042:
        return state.printer.motor;
    case 0x044:
        return state.printer.head_ctrl;
    case 0x010: {
        uint16_t v = (uint16_t)control_latch_byte(0) | ((uint16_t)control_latch_byte(1) << 8);
        state.debug_ctrl_reads[0]++; state.debug_ctrl_last[0] = v;
        return v;
    }
    case 0x012: {
        uint16_t v = (uint16_t)control_latch_byte(2) | ((uint16_t)control_latch_byte(3) << 8);
        state.debug_ctrl_reads[1]++; state.debug_ctrl_last[1] = v;
        return v;
    }
    case 0x014: {
        uint16_t v = (uint16_t)control_latch_byte(4) | ((uint16_t)control_latch_byte(5) << 8);
        state.debug_ctrl_reads[2]++; state.debug_ctrl_last[2] = v;
        return v;
    }
    case 0x050: {
        uint16_t dx = sign_extend12((uint16_t)state.mouse_dx);
        uint16_t buttons = 0;
        if (!state.mouse_left) buttons |= 0x1000u;   /* inverted: 1 = unpressed */
        if (!state.mouse_right) buttons |= 0x4000u;
        state.mouse_dx = 0;
        return buttons | dx;
    }
    case 0x052: {
        uint16_t dy = sign_extend12((uint16_t)state.mouse_dy);
        state.mouse_dy = 0;
        return dy;
    }
    case 0x054:
        return state.control_out & 0x3Fu;
    default:
        LOOPY_DEBUG_PRINTF("[IO] unmapped read16 %08X\n", addr);
        return 0;
    }
}

uint32_t loopy_io_reg_read32(uint32_t addr) {
    uint32_t hi = loopy_io_reg_read16(addr);
    uint32_t lo = loopy_io_reg_read16(addr + 2);
    return (hi << 16) | lo;
}

void loopy_io_reg_write8(uint32_t addr, uint8_t value) {
    /* The 0x04 VDP mirror is byte-accessed by some software.  Treat byte
     * writes to CONTROL_OUT as writes to the meaningful low 6 bits; do not
     * read-modify-write because CONTROL_MOUSE reads clear movement counters. */
    switch (addr & 0xFFFu) {
    case 0x054:
    case 0x055:
        set_direct_control_out(value);
        break;
    default:
        LOOPY_DEBUG_PRINTF("[IO] unmapped write8 %08X: %02X\n", addr & 0xFFFu, value);
        break;
    }
}

void loopy_io_reg_write16(uint32_t addr, uint16_t value) {
    addr &= 0xFFFu;
    switch (addr) {
    case 0x000:
        state.printer.analog_ctrl = value & 0x3Fu;
        state.printer.adc_value = printer_adc_for_mux((value >> 3) & 7u);
        break;
    case 0x020:
        break;
    case 0x030:
        /* Sensor register is read-only from the CPU perspective.  Keep writes harmless. */
        break;
    case 0x040:
        printer_write_head_data(&state.printer, value);
        break;
    case 0x042:
        printer_write_motor(&state.printer, value);
        break;
    case 0x044:
        printer_write_head_ctrl(&state.printer, value);
        break;
    case 0x054:
        set_direct_control_out((uint8_t)value);
        break;
    default:
        LOOPY_DEBUG_PRINTF("[IO] unmapped write16 %08X: %04X\n", addr, value);
        break;
    }
}

void loopy_io_reg_write32(uint32_t addr, uint32_t value) {
    loopy_io_reg_write16(addr, (uint16_t)(value >> 16));
    loopy_io_reg_write16(addr + 2, (uint16_t)value);
}

void loopy_io_update_pad(int key_info, bool pressed) {
    if (pressed) state.pad_buttons |= (uint16_t)key_info;
    else state.pad_buttons &= (uint16_t)~key_info;
}

void loopy_io_set_controller_mode(bool matrix_mode, bool mouse_counter_enable) {
    int old_matrix = state.matrix_mode;
    state.matrix_mode = matrix_mode ? 1 : 0;
    state.mouse_counter_enable = mouse_counter_enable ? 1 : 0;
    if (!old_matrix && state.matrix_mode) {
        memset(state.control_latch, 0, sizeof(state.control_latch));
    }
}

void loopy_io_matrix_scan_vcount(uint16_t vcount) {
    if (!state.matrix_mode || state.mouse_connected) return;
    unsigned vc = (unsigned)(vcount & 0x1FFu);
    if (vc <= 0x00Cu) state.control_latch[0] = pad_matrix_row(0);
    else if (vc >= 0x020u && vc <= 0x02Cu) state.control_latch[1] = pad_matrix_row(1);
    else if (vc >= 0x040u && vc <= 0x04Cu) state.control_latch[2] = pad_matrix_row(2);
    else if (vc >= 0x060u && vc <= 0x06Cu) state.control_latch[3] = pad_matrix_row(3);
    else if (vc >= 0x080u && vc <= 0x08Cu) state.control_latch[4] = pad_matrix_row(4);
    else if (vc >= 0x0A0u && vc <= 0x0ACu) state.control_latch[5] = pad_matrix_row(5);
}

void loopy_io_controller_debug_reset(void) {
    memset(state.debug_ctrl_reads, 0, sizeof(state.debug_ctrl_reads));
    memset(state.debug_ctrl_last, 0, sizeof(state.debug_ctrl_last));
}

void loopy_io_controller_debug_get(unsigned index, unsigned *reads, uint16_t *last_value) {
    if (index >= 3) {
        if (reads) *reads = 0;
        if (last_value) *last_value = 0;
        return;
    }
    if (reads) *reads = state.debug_ctrl_reads[index];
    if (last_value) *last_value = state.debug_ctrl_last[index];
}

void loopy_io_set_mouse_connected(bool connected) {
    state.mouse_connected = connected ? 1 : 0;
    if (!state.mouse_connected) {
        state.mouse_dx = 0;
        state.mouse_dy = 0;
        state.mouse_quad_x = 0;
        state.mouse_quad_y = 0;
    }
}

void loopy_io_set_mouse_button(int button, bool pressed) {
    if (button == MOUSE_LEFT) state.mouse_left = pressed ? 1 : 0;
    if (button == MOUSE_RIGHT) state.mouse_right = pressed ? 1 : 0;
}

static void advance_quadrature(uint8_t *phase, int amount) {
    static const uint8_t gray[4] = {0, 1, 3, 2};
    int step = amount > 0 ? 1 : -1;
    int count = amount > 0 ? amount : -amount;
    unsigned pos = 0;
    for (unsigned i = 0; i < 4; i++) if (gray[i] == (*phase & 3u)) pos = i;
    for (int i = 0; i < count; i++) pos = (unsigned)((int)pos + step + 4) & 3u;
    *phase = gray[pos];
}

void loopy_io_add_mouse_delta(int dx, int dy) {
    if (!state.mouse_connected) return;

    /* Keep raw encoder pins plausible for games that inspect CONTROL_IN. */
    if (dx) advance_quadrature(&state.mouse_quad_x, dx);
    if (dy) advance_quadrature(&state.mouse_quad_y, -dy);

    if (!state.mouse_counter_enable) return;
    state.mouse_dx = clamp_mouse_delta((int)state.mouse_dx + dx);
    state.mouse_dy = clamp_mouse_delta((int)state.mouse_dy - dy); /* Loopy positive Y is up. */
}

void loopy_io_trigger_adc(void) {
    uint16_t mux = (uint16_t)((state.printer.analog_ctrl >> 3) & 7u);
    state.printer.adc_value = printer_adc_for_mux(mux);
}

void loopy_io_trigger_printer_sensors(void) {
    state.printer.sensors_enable = 1;
    state.printer.sensor_triggers++;
    printer_update_sensor_latch(&state.printer);
    if (state.printer.trace && (state.printer.sensor_triggers <= 32u || (state.printer.sensor_triggers & 0xFFu) == 0)) {
        printf("[Printer] PSEN trigger #%u latch=%04X step=%u\n", state.printer.sensor_triggers, state.printer.sensors_latch, state.printer.step_count);
    }
}

void loopy_io_printer_write_source_image(const uint16_t *rgb555, uint16_t width, uint16_t height, const char *source) {
    LoopyPrinterState *p = &state.printer;
    if (!rgb555 || width == 0 || height == 0) return;
    p->source_image_seen = 1;
#ifdef LOOPY_WASM_FRONTEND
    size_t pixels = (size_t)width * (size_t)height;
    if (!pixels || pixels > (1024u * 4096u)) return;
    uint16_t *copy = (uint16_t *)malloc(pixels * sizeof(uint16_t));
    if (!copy) return;
    memcpy(copy, rgb555, pixels * sizeof(uint16_t));
    printer_clear_pending_image(p);
    p->pending_rgb555 = copy;
    p->pending_width = width;
    p->pending_height = height;
    p->pending_serial++;
    (void)source;
#else
    char path[1024];
    printer_make_unique_png_path(p, path, sizeof(path));
    if (printer_write_rgb555_png(path, rgb555, width, height)) {
        printf("[Printer] Wrote %s from BIOS print source image (%ux%u%s%s)\n",
               path, (unsigned)width, (unsigned)height, source ? ", " : "", source ? source : "");
    } else {
        fprintf(stderr, "[Printer] Failed to write BIOS print source image %s\n", path);
    }
#endif
}

void loopy_io_printer_frame_snapshot(const uint16_t *rgb555, uint16_t width, uint16_t height) {
    LoopyPrinterState *p = &state.printer;
    if (!rgb555 || width == 0 || height == 0) return;
    uint16_t copy_w = width < PRINTER_CAPTURE_WIDTH ? width : PRINTER_CAPTURE_WIDTH;
    uint16_t copy_h = height < PRINTER_CAPTURE_HEIGHT ? height : PRINTER_CAPTURE_HEIGHT;
    for (uint16_t y = 0; y < PRINTER_CAPTURE_HEIGHT; y++) {
        uint16_t *dst = &p->latest_frame_rgb555[(uint32_t)y * PRINTER_CAPTURE_WIDTH];
        if (y < copy_h) {
            memcpy(dst, &rgb555[(uint32_t)y * width], (size_t)copy_w * sizeof(uint16_t));
            if (copy_w < PRINTER_CAPTURE_WIDTH) {
                for (uint16_t x = copy_w; x < PRINTER_CAPTURE_WIDTH; x++) dst[x] = 0x7FFFu;
            }
        } else {
            for (uint16_t x = 0; x < PRINTER_CAPTURE_WIDTH; x++) dst[x] = 0x7FFFu;
        }
    }
    p->latest_frame_valid = 1;
}

void loopy_io_printer_capture_scanline(uint16_t y, const uint16_t *rgb555, uint16_t width, uint8_t source_format) {
    LoopyPrinterState *p = &state.printer;
    if (!rgb555 || y >= PRINTER_CAPTURE_HEIGHT || width == 0) return;

    /* VDP scanline capture is the documented source for sticker image data.
     * PRINT_HEAD_DATA is a post-processed/serialized thermal stream whose exact
     * format is still unknown, so keep these capture lines as the primary
     * user-facing printer output whenever printing support is enabled. */
    uint16_t n = width;
    if (n > PRINTER_CAPTURE_WIDTH) n = PRINTER_CAPTURE_WIDTH;
    memcpy(&p->capture_rgb555[(uint32_t)y * PRINTER_CAPTURE_WIDTH], rgb555, (size_t)n * sizeof(uint16_t));
    if (n < PRINTER_CAPTURE_WIDTH) {
        memset(&p->capture_rgb555[(uint32_t)y * PRINTER_CAPTURE_WIDTH + n], 0xFF,
               (size_t)(PRINTER_CAPTURE_WIDTH - n) * sizeof(uint16_t));
    }
    if (!p->capture_valid[y]) p->capture_line_count++;
    p->capture_valid[y] = 1;
    if (p->capture_first_y > y) p->capture_first_y = y;
    if (p->capture_last_y < y) p->capture_last_y = y;
    p->capture_events++;
    p->capture_last_format = source_format;
    p->active = 1;
    if (p->trace && (p->capture_events <= 16u || y == 0u || y == 239u || (p->capture_events & 0x3Fu) == 0)) {
        printf("[Printer] CAPTURE line #%u y=%u fmt=%u valid=%u\n",
               p->capture_events, (unsigned)y, (unsigned)source_format, p->capture_line_count);
    }
}

void loopy_io_printer_set_output_dir(const char *path) {
    if (!path || !*path) { state.printer.output_dir[0] = '\0'; return; }
    snprintf(state.printer.output_dir, sizeof(state.printer.output_dir), "%s", path);
}

void loopy_io_printer_set_trace(bool enabled) {
    state.printer.trace = enabled ? 1 : 0;
}


int loopy_io_printer_has_pending_image(void) { return state.printer.pending_rgb555 != NULL; }
uint32_t loopy_io_printer_pending_width(void) { return state.printer.pending_width; }
uint32_t loopy_io_printer_pending_height(void) { return state.printer.pending_height; }
const uint16_t *loopy_io_printer_pending_pixels(void) { return state.printer.pending_rgb555; }
void loopy_io_printer_clear_pending_image(void) { printer_clear_pending_image(&state.printer); }

typedef struct LoopyIOStateBlob {
    uint16_t pad_buttons;
    int matrix_mode;
    int mouse_counter_enable;
    int mouse_connected;
    int mouse_left;
    int mouse_right;
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_quad_x;
    uint8_t mouse_quad_y;
    uint8_t control_out;
    uint8_t control_latch[6];
    uint16_t analog_ctrl;
    uint16_t adc_value;
    uint16_t sensors_latch;
    int sensors_enable;
    uint16_t head_data;
    uint16_t head_ctrl;
    uint16_t motor;
    uint8_t last_motor_phase;
    int8_t last_motor_index;
    int32_t motor_position;
    unsigned step_count;
    unsigned print_index;
} LoopyIOStateBlob;

uint32_t loopy_io_state_blob_size(void) { return (uint32_t)sizeof(LoopyIOStateBlob); }

void loopy_io_get_state_blob(void *dst, uint32_t size) {
    if (!dst || size != sizeof(LoopyIOStateBlob)) return;
    LoopyIOStateBlob b;
    memset(&b, 0, sizeof(b));
    b.pad_buttons = state.pad_buttons;
    b.matrix_mode = state.matrix_mode;
    b.mouse_counter_enable = state.mouse_counter_enable;
    b.mouse_connected = state.mouse_connected;
    b.mouse_left = state.mouse_left;
    b.mouse_right = state.mouse_right;
    b.mouse_dx = state.mouse_dx;
    b.mouse_dy = state.mouse_dy;
    b.mouse_quad_x = state.mouse_quad_x;
    b.mouse_quad_y = state.mouse_quad_y;
    b.control_out = state.control_out;
    memcpy(b.control_latch, state.control_latch, sizeof(b.control_latch));
    b.analog_ctrl = state.printer.analog_ctrl;
    b.adc_value = state.printer.adc_value;
    b.sensors_latch = state.printer.sensors_latch;
    b.sensors_enable = state.printer.sensors_enable;
    b.head_data = state.printer.head_data;
    b.head_ctrl = state.printer.head_ctrl;
    b.motor = state.printer.motor;
    b.last_motor_phase = state.printer.last_motor_phase;
    b.last_motor_index = state.printer.last_motor_index;
    b.motor_position = state.printer.motor_position;
    b.step_count = state.printer.step_count;
    b.print_index = state.printer.print_index;
    memcpy(dst, &b, sizeof(b));
}

void loopy_io_set_state_blob(const void *src, uint32_t size) {
    if (!src || size != sizeof(LoopyIOStateBlob)) return;
    LoopyIOStateBlob b;
    memcpy(&b, src, sizeof(b));
    free(state.printer.word_pool);
    free(state.printer.lines);
    state.printer.word_pool = NULL;
    state.printer.lines = NULL;
    state.printer.word_capacity = 0;
    state.printer.line_capacity = 0;
    state.printer.word_count = 0;
    state.printer.line_count = 0;
    state.printer.current_count = 0;
    state.printer.max_words_per_line = 0;
    state.printer.active = 0;
    state.printer.dirty = 0;

    state.pad_buttons = b.pad_buttons;
    state.matrix_mode = b.matrix_mode;
    state.mouse_counter_enable = b.mouse_counter_enable;
    state.mouse_connected = b.mouse_connected;
    state.mouse_left = b.mouse_left;
    state.mouse_right = b.mouse_right;
    state.mouse_dx = b.mouse_dx;
    state.mouse_dy = b.mouse_dy;
    state.mouse_quad_x = b.mouse_quad_x;
    state.mouse_quad_y = b.mouse_quad_y;
    state.control_out = b.control_out;
    memcpy(state.control_latch, b.control_latch, sizeof(state.control_latch));
    state.printer.analog_ctrl = b.analog_ctrl;
    state.printer.adc_value = b.adc_value;
    state.printer.sensors_latch = b.sensors_latch;
    state.printer.sensors_enable = b.sensors_enable;
    state.printer.head_data = b.head_data;
    state.printer.head_ctrl = b.head_ctrl;
    state.printer.motor = b.motor;
    state.printer.last_motor_phase = b.last_motor_phase;
    state.printer.last_motor_index = b.last_motor_index;
    state.printer.motor_position = b.motor_position;
    state.printer.step_count = b.step_count;
    state.printer.print_index = b.print_index;
}
