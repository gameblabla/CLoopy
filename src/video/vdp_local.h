#ifndef LOOPY_VDP_LOCAL_H
#define LOOPY_VDP_LOCAL_H
#include "video/video.h"
#include <stdint.h>


typedef struct VDPBitmapRegs {
    uint16_t scrollx, scrolly, screenx, screeny, w, clipx, h, buffer_ctrl;
    uint8_t buffered_color;
} VDPBitmapRegs;

typedef struct VDP {
    uint16_t *bg_output[2];
    uint16_t *bitmap_output[4];
    uint16_t *obj_output[2];
    uint16_t *screen_output[2];
    uint16_t *display_output;
    int frame_ended;
    int visible_scanlines;
    uint8_t screens[2][VIDEO_DISPLAY_WIDTH];
    uint8_t bitmap_linebuf[4][VIDEO_DISPLAY_WIDTH];
    uint8_t bitmap_linebuf_valid[4];
    uint8_t bitmap[VIDEO_BITMAP_VRAM_SIZE];
    uint8_t tile[VIDEO_TILE_VRAM_SIZE];
    uint8_t oam[VIDEO_OAM_SIZE];
    uint8_t palette[VIDEO_PALETTE_SIZE];
    uint8_t capture_buffer[VIDEO_CAPTURE_SIZE];
    struct { int use_pal, extra_scanlines, unk, mouse_scan, pad_scan, unk2; } mode;
    uint16_t hcount;
    uint16_t vcount;
    uint16_t bus_latch;
    uint64_t line_start_timestamp;
    struct { int irq1_enable, irq1_source; } sync_irq_ctrl;
    int capture_enable;
    VDPBitmapRegs bitmap_regs[4];
    uint16_t bitmap_ctrl;
    uint16_t bitmap_palsel;
    struct { int shared_maps, map_size, bg0_8bit, tile_size0, tile_size1; } bg_ctrl;
    uint16_t bg_scrollx[2];
    uint16_t bg_scrolly[2];
    uint16_t bg_palsel[2];
    uint16_t tilebase;
    struct { int id_offs, tile_index_offs[2], is_8bit; } obj_ctrl;
    uint16_t obj_palsel[2];
    uint16_t dispmode;
    struct { int bg_enable[2], bitmap_enable[4], obj_enable[2], bitmap_screen_mode[2], obj_screen_mode[2]; } layer_ctrl;
    struct { int prio_mode, screen_b_backdrop_only, output_screen_b, output_screen_a, blend_mode; } color_prio;
    uint16_t backdrops[2];
    struct { int scanline, format; } capture_ctrl;
    struct { int irq0_enable, nmi_enable, use_vcmp, irq0_enable2; } cmp_irq_ctrl;
    uint16_t irq0_hcmp;
    uint16_t irq0_vcmp;
    int irq2_source;
    int irq2_enable_a;
    int irq2_enable_b;
    int irq2_enable_c;
    uint16_t bitmap_mem_ctrl;
    uint16_t dma_mask;
    uint16_t dma_value;
} VDP;

extern VDP vdp;

#endif
