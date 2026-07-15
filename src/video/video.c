#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "common/bswp.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/loopy_io.h"
#include "video/render.h"
#include "video/vdp_local.h"
#include "video/video.h"

static TimingFuncHandle vcount_func, hsync_func, irq0_func, irq2_func;
static TimingEventHandle vcount_ev, hsync_ev, irq0_ev;
static bool bmp_dump_enabled = true;

VDP vdp;

#define LINES_PER_FRAME 263
#define VDP_NTSC_CLOCK 21477272LL
#define VDP_NTSC_LINE_CLOCKS 1365LL
#define VDP_NTSC_HACTIVE_CLOCKS 1029LL

static int cpu_cycles_per_ntsc_line(void)
{
    return (int)((VDP_NTSC_LINE_CLOCKS * (int64_t)TIMING_F_CPU + (VDP_NTSC_CLOCK / 2)) / VDP_NTSC_CLOCK);
}

static int vdp_clocks_to_cpu_cycles(int vdp_clocks)
{
    return (int)(((int64_t)vdp_clocks * (int64_t)TIMING_F_CPU + (VDP_NTSC_CLOCK / 2)) / VDP_NTSC_CLOCK);
}

static uint32_t translate_vdp_addr(uint32_t raw_addr)
{
    if ((raw_addr & 0x0F000000u) == VIDEO_VDP_AREA_LOW_MIRROR) {
        return (raw_addr & ~0xF8000000u);
    }
    if ((raw_addr & 0x0F000000u) == VIDEO_VDP_AREA_NORMAL) {
        return VIDEO_VDP_TRANSLATED_START | (raw_addr & 0x003FFFFFu);
    }
    return raw_addr & ~0xF8000000u;
}

static bool is_low_vdp_mirror(uint32_t raw_addr)
{
    return (raw_addr & 0x0F000000u) == VIDEO_VDP_AREA_LOW_MIRROR;
}

static bool irq0_line_matches(void)
{
    if (!(vdp.cmp_irq_ctrl.irq0_enable && vdp.cmp_irq_ctrl.irq0_enable2)) return false;
    if (!vdp.cmp_irq_ctrl.use_vcmp) return true;
    return (vdp.vcount & 0x1FFu) == (vdp.irq0_vcmp & 0x1FFu);
}

static int hcmp_to_line_cycles(uint16_t hcmp)
{
    int signed_h = (hcmp & 0x100) ? ((int)(hcmp & 0x1FF) - 0x200) : (int)(hcmp & 0x1FF);
    int vdp_clocks;
    if (signed_h >= 0) {
        if (signed_h > 257) return -1;
        vdp_clocks = signed_h * 4;
    } else {
        if (signed_h < -84 || signed_h > -1) return -1;
        vdp_clocks = (int)VDP_NTSC_HACTIVE_CLOCKS + ((signed_h + 84) * 4);
    }
    int cycles = vdp_clocks_to_cpu_cycles(vdp_clocks);
    int line_cycles = cpu_cycles_per_ntsc_line();
    if (cycles < 0 || cycles >= line_cycles) return -1;
    return cycles;
}

static void fire_irq0(uint64_t param, int cycles_late)
{
    (void)param;
    (void)cycles_late;
    if (irq0_line_matches()) {
        sh7021_ocpm_intc_assert_irq(IRQ_IRQ0, 0);
        sh7021_ocpm_intc_deassert_irq(IRQ_IRQ0);
    }
}

static void fire_irq2(uint64_t param, int cycles_late)
{
    (void)param;
    (void)cycles_late;
    if (vdp.irq2_enable_a && vdp.irq2_enable_b && vdp.irq2_enable_c) {
        sh7021_ocpm_intc_assert_irq(IRQ_IRQ2, 0);
        sh7021_ocpm_intc_deassert_irq(IRQ_IRQ2);
    }
}

static void schedule_irq0_for_line(int cycles_late)
{
    if (!irq0_line_matches()) return;
    int cycles = hcmp_to_line_cycles(vdp.irq0_hcmp);
    if (cycles < 0) return;
    cycles -= cycles_late;
    if (cycles < 1) cycles = 1;
    irq0_ev = timing_add_event(irq0_func, timing_convert_cpu(cycles), 0, TIMING_CPU_TIMER);
}

static void schedule_irq2_ready(int cycles)
{
    if (!(vdp.irq2_enable_a && vdp.irq2_enable_b && vdp.irq2_enable_c)) return;
    if (cycles < 1) cycles = 1;
    timing_add_event(irq2_func, timing_convert_cpu(cycles), 0, TIMING_CPU_TIMER);
}

typedef struct DumpHeader
{
	uint32_t addr;
	uint32_t length;
	uint32_t data_width;
} DumpHeader;

static void dump_bmp(const char *name, uint16_t *data)
{
    char path[512];
    snprintf(path, sizeof(path), "%s.bmp", name);
    FILE *bmp_file = fopen(path, "wb");
    if (!bmp_file) return;

    const int DATA_SIZE = (VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT * 2);
    const int DATA_OFFSET = 0x36;
    const int FILE_SIZE = DATA_OFFSET + DATA_SIZE;
    uint8_t header[0x36] = {
        'B', 'M',
        (uint8_t)(FILE_SIZE >> 0), (uint8_t)(FILE_SIZE >> 8), (uint8_t)(FILE_SIZE >> 16), (uint8_t)(FILE_SIZE >> 24),
        0, 0, 0, 0,
        DATA_OFFSET, 0, 0, 0,
        0x28, 0, 0, 0,
        (uint8_t)(VIDEO_DISPLAY_WIDTH >> 0), (uint8_t)(VIDEO_DISPLAY_WIDTH >> 8), 0, 0,
        (uint8_t)(VIDEO_DISPLAY_HEIGHT >> 0), (uint8_t)(VIDEO_DISPLAY_HEIGHT >> 8), 0, 0,
        1, 0,
        16, 0,
        0, 0, 0, 0,
        (uint8_t)(DATA_SIZE >> 0), (uint8_t)(DATA_SIZE >> 8), (uint8_t)(DATA_SIZE >> 16), (uint8_t)(DATA_SIZE >> 24),
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    fwrite(header, 1, sizeof(header), bmp_file);
    for (int y = 0; y < VIDEO_DISPLAY_HEIGHT; y++) {
        int flipped_y = VIDEO_DISPLAY_HEIGHT - y - 1;
        fwrite(data + flipped_y * VIDEO_DISPLAY_WIDTH, sizeof(uint16_t), VIDEO_DISPLAY_WIDTH, bmp_file);
    }
    fclose(bmp_file);
}

static void start_hsync(uint64_t param, int cycles_late)
{
	//IRQ1 is triggered on visible lines when in HSYNC mode
	if (vdp.sync_irq_ctrl.irq1_enable && vdp.sync_irq_ctrl.irq1_source == 1)
	{
		if(vdp.vcount < vdp.visible_scanlines)
		{
			IRQ irq_id = IRQ_IRQ1;
			sh7021_ocpm_intc_assert_irq(irq_id, 0);
			sh7021_ocpm_intc_deassert_irq(irq_id);
		}
	}
}

static void vsync_start()
{
	LOOPY_DEBUG_PRINTF("[Video] VSYNC start\n");

	//This is kinda weird, but when the VDP enters VSYNC, the total number of scanlines is subtracted from VCOUNT
	//Think of the VSYNC lines as being negative
	vdp.vcount = (vdp.vcount - LINES_PER_FRAME) & 0x1FF;
	vdp.frame_ended = true;

	//NMI is triggered on VSYNC
	if (vdp.cmp_irq_ctrl.nmi_enable)
	{
		//TODO: is there a cleaner way to do this?
		IRQ irq_id = IRQ_NMI;
		sh7021_ocpm_intc_assert_irq(irq_id, 0);
		sh7021_ocpm_intc_deassert_irq(irq_id);
	}

	//IRQ1 is triggered on VSYNC when in VSYNC mode
	if (vdp.sync_irq_ctrl.irq1_enable && (vdp.sync_irq_ctrl.irq1_source == 0))
	{
		IRQ irq_id = IRQ_IRQ1;
		sh7021_ocpm_intc_assert_irq(irq_id, 0);
		sh7021_ocpm_intc_deassert_irq(irq_id);
	}

	loopy_io_printer_frame_snapshot(vdp.display_output, VIDEO_DISPLAY_WIDTH, VIDEO_DISPLAY_HEIGHT);
	if (bmp_dump_enabled) dump_bmp("output_display", vdp.display_output);
	//dump_all_bmps();
	//dump_for_serial();
}

static void inc_vcount(uint64_t param, int cycles_late)
{
    vdp.line_start_timestamp = (uint64_t)timing_get_timestamp(TIMING_CPU_TIMER);
    loopy_io_matrix_scan_vcount(vdp.vcount);
	if (vdp.vcount < vdp.visible_scanlines)
	{
		video_renderer_draw_scanline(vdp.vcount);
	}

	vdp.vcount++;
	
	//Once we go past the visible region, enter VSYNC
	if (vdp.vcount == vdp.visible_scanlines)
	{
		vsync_start();
	}

	//At the end of VSYNC, wrap around to the start of the visible region
	const int VSYNC_END = 0x200;
	if (vdp.vcount == VSYNC_END)
	{
		LOOPY_DEBUG_PRINTF("[Video] VSYNC end\n");
		vdp.vcount = 0;
	}

    const int CYCLES_PER_LINE = cpu_cycles_per_ntsc_line();
    const int CYCLES_UNTIL_HSYNC = vdp_clocks_to_cpu_cycles((int)VDP_NTSC_HACTIVE_CLOCKS);

	TimingUnitCycle scanline_cycles = timing_convert_cpu(CYCLES_PER_LINE - cycles_late);
	vcount_ev = timing_add_event(vcount_func, scanline_cycles, 0, TIMING_CPU_TIMER);

	TimingUnitCycle hsync_cycles = timing_convert_cpu(CYCLES_UNTIL_HSYNC - cycles_late);
	hsync_ev = timing_add_event(hsync_func, hsync_cycles, 0, TIMING_CPU_TIMER);
    schedule_irq0_for_line(cycles_late);
}

static void dump_serial_region(FILE *dump, uint8_t* mem, uint32_t addr, uint32_t length)
{
	DumpHeader header;
	header.addr = common_bswp32(addr | (1 << 27)); //Make sure the address is 16-bit for the CPU
	header.length = common_bswp32(length);
	header.data_width = common_bswp32(2);

	fwrite(&header, 1, sizeof(header), dump);
	fwrite(mem, 1, length, dump);
}

void video_initialize()
{
	memset(&vdp, 0, sizeof(vdp));

	vdp.visible_scanlines = 0xE0;

	//Initialize output buffers
	for (int i = 0; i < 2; i++)
	{
		vdp.bg_output[i] = (uint16_t *)calloc(VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT, sizeof(uint16_t));
		vdp.obj_output[i] = (uint16_t *)calloc(VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT, sizeof(uint16_t));
		vdp.screen_output[i] = (uint16_t *)calloc(VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT, sizeof(uint16_t));
	}

	//Set all OBJs to invisible
	for (int i = 0; i < VIDEO_OAM_SIZE; i += 4)
	{
		video_oam_write32(i, 0x200);
	}

	for (int i = 0; i < 4; i++)
	{
		vdp.bitmap_output[i] = (uint16_t *)calloc(VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT, sizeof(uint16_t));
	}

	vdp.display_output = (uint16_t *)calloc(VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT, sizeof(uint16_t));

	vcount_func = timing_register_func("Video::inc_vcount", inc_vcount);
	hsync_func = timing_register_func("Video::start_hsync", start_hsync);
	irq0_func = timing_register_func("Video::fire_irq0", fire_irq0);
	irq2_func = timing_register_func("Video::fire_irq2", fire_irq2);
	irq0_ev = timing_invalid_event_handle();

	//Kickstart the VCOUNT event
	inc_vcount(0, 0);
}

void video_shutdown()
{
	for (int i = 0; i < 2; i++) {
		free(vdp.bg_output[i]);
		free(vdp.obj_output[i]);
		free(vdp.screen_output[i]);
	}
	for (int i = 0; i < 4; i++) free(vdp.bitmap_output[i]);
	free(vdp.display_output);
}

void video_start_frame()
{
	vdp.frame_ended = false;

	const size_t BUFFER_SIZE = VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT * sizeof(uint16_t);

	//Clear the output buffers
	for (int i = 0; i < 2; i++)
	{
		memset(vdp.bg_output[i], 0, BUFFER_SIZE);
		memset(vdp.obj_output[i], 0, BUFFER_SIZE);
		memset(vdp.bitmap_output[i], 0, BUFFER_SIZE);
		memset(vdp.bitmap_output[i + 2], 0, BUFFER_SIZE);
		memset(vdp.screen_output[i], 0, BUFFER_SIZE);
	}

	memset(vdp.display_output, 0, BUFFER_SIZE);
}

bool video_check_frame_end()
{
	return vdp.frame_ended;
}

uint16_t* video_get_display_output()
{
	return vdp.display_output;
}

int video_get_display_active_height(void)
{
	return vdp.mode.extra_scanlines ? VIDEO_DISPLAY_HEIGHT : 0xE0;
}

int video_get_display_active_y_offset(void)
{
	return vdp.mode.extra_scanlines ? 0 : 8;
}

void video_dump_for_serial()
{
	FILE *dump = fopen("emudump.bin", "wb");
	if (!dump) return;
	const char* MAGIC = "LPSTATE\0";

	fwrite(MAGIC, 1, 8, dump);

	dump_serial_region(dump, vdp.bitmap, VIDEO_BITMAP_VRAM_START, VIDEO_BITMAP_VRAM_SIZE);
	dump_serial_region(dump, vdp.tile, VIDEO_TILE_VRAM_START, VIDEO_TILE_VRAM_SIZE);
	dump_serial_region(dump, vdp.palette, VIDEO_PALETTE_START, VIDEO_PALETTE_SIZE);
	dump_serial_region(dump, vdp.oam, VIDEO_OAM_START, VIDEO_OAM_SIZE);

	//TODO: dump MMIO
	fclose(dump);
}


static uint8_t vdp_read8_from_read16(uint32_t addr, uint16_t (*read16fn)(uint32_t))
{
    uint16_t value = read16fn(addr & ~1u);
    return (addr & 1u) ? (uint8_t)(value & 0xFFu) : (uint8_t)(value >> 8);
}

static uint32_t vdp_read32_from_read16(uint32_t addr, uint16_t (*read16fn)(uint32_t))
{
    return ((uint32_t)read16fn(addr & ~1u) << 16) | read16fn((addr + 2u) & ~1u);
}

static void vdp_write8_via_write16(uint32_t addr, uint8_t value, uint16_t (*read16fn)(uint32_t), void (*write16fn)(uint32_t, uint16_t))
{
    uint32_t base = addr & ~1u;
    uint16_t cur = read16fn(base);
    if (addr & 1u) cur = (uint16_t)((cur & 0xFF00u) | value);
    else cur = (uint16_t)(((uint16_t)value << 8) | (cur & 0x00FFu));
    write16fn(base, cur);
}

static void vdp_write32_via_write16(uint32_t addr, uint32_t value, void (*write16fn)(uint32_t, uint16_t))
{
    write16fn(addr & ~1u, (uint16_t)(value >> 16));
    write16fn((addr + 2u) & ~1u, (uint16_t)value);
}

static uint16_t video_current_hcount(void)
{
    int64_t now = timing_get_timestamp(TIMING_CPU_TIMER);
    int64_t elapsed_cpu = now - (int64_t)vdp.line_start_timestamp;
    int line_cycles = cpu_cycles_per_ntsc_line();
    if (elapsed_cpu < 0) elapsed_cpu = 0;
    if (elapsed_cpu >= line_cycles) elapsed_cpu %= line_cycles;

    int64_t vdp_clock = (elapsed_cpu * VDP_NTSC_LINE_CLOCKS) / line_cycles;
    if (vdp_clock < 1028) {
        return (uint16_t)((vdp_clock / 4) & 0x1FF);
    }
    if (vdp_clock == 1028) {
        return 0x101;
    }
    int blank = (int)((vdp_clock - VDP_NTSC_HACTIVE_CLOCKS) / 4);
    if (blank < 0) blank = 0;
    if (blank > 83) blank = 83;
    return (uint16_t)((0x1AC + blank) & 0x1FF);
}

static uint16_t vdp_unmapped_read16(uint32_t addr)
{
    LOOPY_DEBUG_PRINTF("[Video] unmapped VDP read16 %08X -> latch %04X\n", addr, vdp.bus_latch);
    return vdp.bus_latch;
}

static void vdp_unmapped_write16(uint32_t addr, uint16_t value)
{
    LOOPY_DEBUG_PRINTF("[Video] unmapped VDP write16 %08X: %04X\n", addr, value);
    vdp.bus_latch = value;
}

static uint8_t video_bitmap_vram_read8(uint32_t addr)
{
    return vdp.bitmap[(addr - VIDEO_BITMAP_VRAM_START) & 0x1FFFFu];
}

static uint16_t video_bitmap_vram_read16(uint32_t addr)
{
    uint16_t value;
    uint32_t offs = (addr - VIDEO_BITMAP_VRAM_START) & 0x1FFFFu;
    memcpy(&value, &vdp.bitmap[offs], 2);
    return common_bswp16(value);
}

static uint32_t video_bitmap_vram_read32(uint32_t addr)
{
    return ((uint32_t)video_bitmap_vram_read16(addr & ~1u) << 16) |
           (uint32_t)video_bitmap_vram_read16((addr + 2u) & ~1u);
}

static void video_bitmap_vram_write8(uint32_t addr, uint8_t value)
{
    vdp.bitmap[(addr - VIDEO_BITMAP_VRAM_START) & 0x1FFFFu] = value;
}

static void video_bitmap_vram_write16(uint32_t addr, uint16_t value)
{
    uint32_t offs = (addr - VIDEO_BITMAP_VRAM_START) & 0x1FFFFu;
    value = common_bswp16(value);
    memcpy(&vdp.bitmap[offs], &value, 2);
}

static void video_bitmap_vram_write32(uint32_t addr, uint32_t value)
{
    video_bitmap_vram_write16(addr & ~1u, (uint16_t)(value >> 16));
    video_bitmap_vram_write16((addr + 2u) & ~1u, (uint16_t)value);
}

static uint8_t video_tile_vram_read8(uint32_t addr)
{
    return vdp.tile[(addr - VIDEO_TILE_VRAM_START) & 0xFFFFu];
}

static uint16_t video_tile_vram_read16(uint32_t addr)
{
    uint16_t value;
    uint32_t offs = (addr - VIDEO_TILE_VRAM_START) & 0xFFFFu;
    memcpy(&value, &vdp.tile[offs], 2);
    return common_bswp16(value);
}

static uint32_t video_tile_vram_read32(uint32_t addr)
{
    return ((uint32_t)video_tile_vram_read16(addr & ~1u) << 16) |
           (uint32_t)video_tile_vram_read16((addr + 2u) & ~1u);
}

static void video_tile_vram_write8(uint32_t addr, uint8_t value)
{
    vdp.tile[(addr - VIDEO_TILE_VRAM_START) & 0xFFFFu] = value;
}

static void video_tile_vram_write16(uint32_t addr, uint16_t value)
{
    uint32_t offs = (addr - VIDEO_TILE_VRAM_START) & 0xFFFFu;
    value = common_bswp16(value);
    memcpy(&vdp.tile[offs], &value, 2);
}

static void video_tile_vram_write32(uint32_t addr, uint32_t value)
{
    video_tile_vram_write16(addr & ~1u, (uint16_t)(value >> 16));
    video_tile_vram_write16((addr + 2u) & ~1u, (uint16_t)value);
}

bool video_bus_is_vdp_addr(uint32_t raw_addr, uint32_t translated_addr)
{
    uint32_t area = raw_addr & 0x0F000000u;
    if (area != VIDEO_VDP_AREA_NORMAL && area != VIDEO_VDP_AREA_LOW_MIRROR) return false;
    if (translated_addr >= LOOPY_IO_BASE_ADDR && translated_addr < LOOPY_IO_END_ADDR) return false;
    return translated_addr >= VIDEO_VDP_TRANSLATED_START && translated_addr < VIDEO_VDP_TRANSLATED_END;
}

static bool video_addr_is_bitmap_vram(uint32_t addr)
{
    return addr >= VIDEO_BITMAP_VRAM_START && addr < VIDEO_TILE_VRAM_START;
}

static bool video_addr_is_tile_vram(uint32_t addr)
{
    return addr >= VIDEO_TILE_VRAM_START && addr < VIDEO_TILE_VRAM_END;
}

static bool video_addr_is_memory(uint32_t addr)
{
    return video_addr_is_bitmap_vram(addr) || video_addr_is_tile_vram(addr) ||
           (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) ||
           (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) ||
           (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) ||
           (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END);
}

int video_bus_wait_cycles(uint32_t raw_addr, uint32_t translated_addr, int bytes, bool write)
{
    if (!video_bus_is_vdp_addr(raw_addr, translated_addr)) return 0;

    /* Loopy VDP accesses are bus transactions, not plain memory loads/stores.
       The hardware notes give bitmap VRAM timings measured by DRAM<->VRAM DMA:
         FBM=0: bitmap read 7 CPU cycles, bitmap write 4 CPU cycles
         FBM=1: bitmap read 6 CPU cycles, bitmap write 3 CPU cycles
       For non-bitmap VDP memory the documented baseline is 2+WAIT, with memory
       WAIT effectively about 2 CPU cycles.  Register accesses use 2+short WAIT,
       effectively about 3 CPU cycles.  Treat byte accesses to the normal 16-bit
       VDP map as a full word/RMW bus transaction; 32-bit accesses split into two. */
    int per_word;
    if (video_addr_is_bitmap_vram(translated_addr)) {
        bool fast = (vdp.bitmap_mem_ctrl & 0x1u) != 0;
        per_word = write ? (fast ? 3 : 4) : (fast ? 6 : 7);
    } else if (video_addr_is_memory(translated_addr)) {
        per_word = 4;
    } else {
        per_word = 3;
    }

    int transfers = (bytes == 4) ? 2 : 1;
    int cycles = per_word * transfers;
    cycles -= transfers; /* interpreter baseline one cycle per bus transfer */
    if (cycles < 0) cycles = 0;
    if (is_low_vdp_mirror(raw_addr)) cycles += bytes;
    return cycles;
}

static uint8_t vdp_route_read8(uint32_t addr)
{
    if (video_addr_is_bitmap_vram(addr)) return video_bitmap_vram_read8(addr);
    if (video_addr_is_tile_vram(addr)) return video_tile_vram_read8(addr);
    if (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) return video_palette_read8(addr);
    if (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) return video_oam_read8(addr);
    if (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) return video_capture_read8(addr);
    if (addr >= VIDEO_CTRL_REG_START && addr < VIDEO_CTRL_REG_END) return video_ctrl_read8(addr);
    if (addr >= VIDEO_BITMAP_REG_START && addr < VIDEO_BITMAP_REG_END) return video_bitmap_reg_read8(addr);
    if (addr >= VIDEO_BGOBJ_REG_START && addr < VIDEO_BGOBJ_REG_END) return video_bgobj_read8(addr);
    if (addr >= VIDEO_DISPLAY_REG_START && addr < VIDEO_DISPLAY_REG_END) return video_display_read8(addr);
    if (addr >= VIDEO_IRQ_REG_START && addr < VIDEO_IRQ_REG_END) return video_irq_read8(addr);
    if (addr >= VIDEO_DMA_CTRL_START && addr < VIDEO_DMA_CTRL_END) return video_dma_ctrl_read8(addr);
    if (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END) return video_dma_read8(addr);
    return vdp_read8_from_read16(addr, vdp_unmapped_read16);
}

static uint16_t vdp_route_read16(uint32_t addr)
{
    if (video_addr_is_bitmap_vram(addr)) return video_bitmap_vram_read16(addr);
    if (video_addr_is_tile_vram(addr)) return video_tile_vram_read16(addr);
    if (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) return video_palette_read16(addr);
    if (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) return video_oam_read16(addr);
    if (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) return video_capture_read16(addr);
    if (addr >= VIDEO_CTRL_REG_START && addr < VIDEO_CTRL_REG_END) return video_ctrl_read16(addr);
    if (addr >= VIDEO_BITMAP_REG_START && addr < VIDEO_BITMAP_REG_END) return video_bitmap_reg_read16(addr);
    if (addr >= VIDEO_BGOBJ_REG_START && addr < VIDEO_BGOBJ_REG_END) return video_bgobj_read16(addr);
    if (addr >= VIDEO_DISPLAY_REG_START && addr < VIDEO_DISPLAY_REG_END) return video_display_read16(addr);
    if (addr >= VIDEO_IRQ_REG_START && addr < VIDEO_IRQ_REG_END) return video_irq_read16(addr);
    if (addr >= VIDEO_DMA_CTRL_START && addr < VIDEO_DMA_CTRL_END) return video_dma_ctrl_read16(addr);
    if (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END) return video_dma_read16(addr);
    return vdp_unmapped_read16(addr);
}

static uint32_t vdp_route_read32(uint32_t addr)
{
    if (video_addr_is_bitmap_vram(addr)) return video_bitmap_vram_read32(addr);
    if (video_addr_is_tile_vram(addr)) return video_tile_vram_read32(addr);
    if (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) return video_palette_read32(addr);
    if (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) return video_oam_read32(addr);
    if (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) return video_capture_read32(addr);
    if (addr >= VIDEO_CTRL_REG_START && addr < VIDEO_CTRL_REG_END) return video_ctrl_read32(addr);
    if (addr >= VIDEO_BITMAP_REG_START && addr < VIDEO_BITMAP_REG_END) return video_bitmap_reg_read32(addr);
    if (addr >= VIDEO_BGOBJ_REG_START && addr < VIDEO_BGOBJ_REG_END) return video_bgobj_read32(addr);
    if (addr >= VIDEO_DISPLAY_REG_START && addr < VIDEO_DISPLAY_REG_END) return video_display_read32(addr);
    if (addr >= VIDEO_IRQ_REG_START && addr < VIDEO_IRQ_REG_END) return video_irq_read32(addr);
    if (addr >= VIDEO_DMA_CTRL_START && addr < VIDEO_DMA_CTRL_END) return video_dma_ctrl_read32(addr);
    if (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END) return video_dma_read32(addr);
    return ((uint32_t)vdp_unmapped_read16(addr & ~1u) << 16) | vdp_unmapped_read16((addr + 2u) & ~1u);
}

static void vdp_route_write8(uint32_t addr, uint8_t value)
{
    if (video_addr_is_bitmap_vram(addr)) { video_bitmap_vram_write8(addr, value); return; }
    if (video_addr_is_tile_vram(addr)) { video_tile_vram_write8(addr, value); return; }
    if (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) { video_palette_write8(addr, value); return; }
    if (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) { video_oam_write8(addr, value); return; }
    if (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) { video_capture_write8(addr, value); return; }
    if (addr >= VIDEO_CTRL_REG_START && addr < VIDEO_CTRL_REG_END) { video_ctrl_write8(addr, value); return; }
    if (addr >= VIDEO_BITMAP_REG_START && addr < VIDEO_BITMAP_REG_END) { video_bitmap_reg_write8(addr, value); return; }
    if (addr >= VIDEO_BGOBJ_REG_START && addr < VIDEO_BGOBJ_REG_END) { video_bgobj_write8(addr, value); return; }
    if (addr >= VIDEO_DISPLAY_REG_START && addr < VIDEO_DISPLAY_REG_END) { video_display_write8(addr, value); return; }
    if (addr >= VIDEO_IRQ_REG_START && addr < VIDEO_IRQ_REG_END) { video_irq_write8(addr, value); return; }
    if (addr >= VIDEO_DMA_CTRL_START && addr < VIDEO_DMA_CTRL_END) { video_dma_ctrl_write8(addr, value); return; }
    if (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END) { video_dma_write8(addr, value); return; }
    vdp_unmapped_write16(addr & ~1u, (uint16_t)value);
}

static void vdp_route_write16(uint32_t addr, uint16_t value)
{
    if (video_addr_is_bitmap_vram(addr)) { video_bitmap_vram_write16(addr, value); return; }
    if (video_addr_is_tile_vram(addr)) { video_tile_vram_write16(addr, value); return; }
    if (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) { video_palette_write16(addr, value); return; }
    if (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) { video_oam_write16(addr, value); return; }
    if (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) { video_capture_write16(addr, value); return; }
    if (addr >= VIDEO_CTRL_REG_START && addr < VIDEO_CTRL_REG_END) { video_ctrl_write16(addr, value); return; }
    if (addr >= VIDEO_BITMAP_REG_START && addr < VIDEO_BITMAP_REG_END) { video_bitmap_reg_write16(addr, value); return; }
    if (addr >= VIDEO_BGOBJ_REG_START && addr < VIDEO_BGOBJ_REG_END) { video_bgobj_write16(addr, value); return; }
    if (addr >= VIDEO_DISPLAY_REG_START && addr < VIDEO_DISPLAY_REG_END) { video_display_write16(addr, value); return; }
    if (addr >= VIDEO_IRQ_REG_START && addr < VIDEO_IRQ_REG_END) { video_irq_write16(addr, value); return; }
    if (addr >= VIDEO_DMA_CTRL_START && addr < VIDEO_DMA_CTRL_END) { video_dma_ctrl_write16(addr, value); return; }
    if (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END) { video_dma_write16(addr, value); return; }
    vdp_unmapped_write16(addr, value);
}

static void vdp_route_write32(uint32_t addr, uint32_t value)
{
    if (video_addr_is_bitmap_vram(addr)) { video_bitmap_vram_write32(addr, value); return; }
    if (video_addr_is_tile_vram(addr)) { video_tile_vram_write32(addr, value); return; }
    if (addr >= VIDEO_PALETTE_START && addr < VIDEO_PALETTE_END) { video_palette_write32(addr, value); return; }
    if (addr >= VIDEO_OAM_START && addr < VIDEO_OAM_END) { video_oam_write32(addr, value); return; }
    if (addr >= VIDEO_CAPTURE_START && addr < VIDEO_CAPTURE_END) { video_capture_write32(addr, value); return; }
    if (addr >= VIDEO_CTRL_REG_START && addr < VIDEO_CTRL_REG_END) { video_ctrl_write32(addr, value); return; }
    if (addr >= VIDEO_BITMAP_REG_START && addr < VIDEO_BITMAP_REG_END) { video_bitmap_reg_write32(addr, value); return; }
    if (addr >= VIDEO_BGOBJ_REG_START && addr < VIDEO_BGOBJ_REG_END) { video_bgobj_write32(addr, value); return; }
    if (addr >= VIDEO_DISPLAY_REG_START && addr < VIDEO_DISPLAY_REG_END) { video_display_write32(addr, value); return; }
    if (addr >= VIDEO_IRQ_REG_START && addr < VIDEO_IRQ_REG_END) { video_irq_write32(addr, value); return; }
    if (addr >= VIDEO_DMA_CTRL_START && addr < VIDEO_DMA_CTRL_END) { video_dma_ctrl_write32(addr, value); return; }
    if (addr >= VIDEO_DMA_START && addr < VIDEO_DMA_END) { video_dma_write32(addr, value); return; }
    vdp_unmapped_write16(addr & ~1u, (uint16_t)(value >> 16));
    vdp_unmapped_write16((addr + 2u) & ~1u, (uint16_t)value);
}

uint8_t video_bus_read8(uint32_t raw_addr)
{
    uint32_t addr = translate_vdp_addr(raw_addr);
    uint8_t result;
    if (is_low_vdp_mirror(raw_addr)) {
        uint16_t word = vdp_route_read16(addr & ~1u);
        vdp.bus_latch = word;
        result = (uint8_t)(word & 0x00FFu);
    } else {
        result = vdp_route_read8(addr);
        vdp.bus_latch = (uint16_t)((addr & 1u) ? ((vdp.bus_latch & 0xFF00u) | result) : (((uint16_t)result << 8) | (vdp.bus_latch & 0x00FFu)));
    }
    return result;
}

uint16_t video_bus_read16(uint32_t raw_addr)
{
    uint32_t addr = translate_vdp_addr(raw_addr);
    uint16_t result;
    if (is_low_vdp_mirror(raw_addr)) {
        result = (uint16_t)(((uint16_t)video_bus_read8(raw_addr) << 8) | video_bus_read8(raw_addr + 1u));
    } else {
        result = vdp_route_read16(addr & ~1u);
        vdp.bus_latch = result;
    }
    return result;
}

uint32_t video_bus_read32(uint32_t raw_addr)
{
    uint32_t addr = translate_vdp_addr(raw_addr);
    uint32_t result;
    if (is_low_vdp_mirror(raw_addr)) {
        result = ((uint32_t)video_bus_read16(raw_addr) << 16) | video_bus_read16(raw_addr + 2u);
    } else {
        result = vdp_route_read32(addr & ~1u);
        vdp.bus_latch = (uint16_t)result;
    }
    return result;
}

void video_bus_write8(uint32_t raw_addr, uint8_t value)
{
    uint32_t addr = translate_vdp_addr(raw_addr);
    if (is_low_vdp_mirror(raw_addr)) {
        uint16_t bus_value = (uint16_t)value; /* Upper byte is electrically undefined on the 8-bit mirror. */
        vdp_route_write16(addr & ~1u, bus_value);
        vdp.bus_latch = bus_value;
    } else {
        vdp_route_write8(addr, value);
        vdp.bus_latch = (uint16_t)((addr & 1u) ? ((vdp.bus_latch & 0xFF00u) | value) : (((uint16_t)value << 8) | (vdp.bus_latch & 0x00FFu)));
    }
}

void video_bus_write16(uint32_t raw_addr, uint16_t value)
{
    uint32_t addr = translate_vdp_addr(raw_addr);
    if (is_low_vdp_mirror(raw_addr)) {
        video_bus_write8(raw_addr, (uint8_t)(value >> 8));
        video_bus_write8(raw_addr + 1u, (uint8_t)value);
    } else {
        vdp_route_write16(addr & ~1u, value);
        vdp.bus_latch = value;
    }
}

void video_bus_write32(uint32_t raw_addr, uint32_t value)
{
    uint32_t addr = translate_vdp_addr(raw_addr);
    if (is_low_vdp_mirror(raw_addr)) {
        video_bus_write16(raw_addr, (uint16_t)(value >> 16));
        video_bus_write16(raw_addr + 2u, (uint16_t)value);
    } else {
        vdp_route_write32(addr & ~1u, value);
        vdp.bus_latch = (uint16_t)value;
    }
}

uint8_t video_palette_read8(uint32_t addr)
{
	return vdp.palette[addr & 0x1FF];
}

uint16_t video_palette_read16(uint32_t addr)
{
	uint16_t value;
	memcpy(&value, &vdp.palette[addr & 0x1FF], 2);
	return common_bswp16(value);
}

uint32_t video_palette_read32(uint32_t addr)
{
	uint32_t value;
	memcpy(&value, &vdp.palette[addr & 0x1FF], 4);
	return common_bswp32(value);
}

void video_palette_write8(uint32_t addr, uint8_t value)
{
	vdp.palette[addr & 0x1FF] = value;
}

void video_palette_write16(uint32_t addr, uint16_t value)
{
	value = common_bswp16(value);
	memcpy(&vdp.palette[addr & 0x1FF], &value, 2);
}

void video_palette_write32(uint32_t addr, uint32_t value)
{
	value = common_bswp32(value);
	memcpy(&vdp.palette[addr & 0x1FF], &value, 4);
}

uint8_t video_oam_read8(uint32_t addr)
{
	return vdp.oam[addr & 0x1FF];
}

uint16_t video_oam_read16(uint32_t addr)
{
	uint16_t value;
	memcpy(&value, &vdp.oam[addr & 0x1FF], 2);
	return common_bswp16(value);
}

uint32_t video_oam_read32(uint32_t addr)
{
	uint32_t value;
	memcpy(&value, &vdp.oam[addr & 0x1FF], 4);
	return common_bswp32(value);
}

void video_oam_write8(uint32_t addr, uint8_t value)
{
	vdp.oam[addr & 0x1FF] = value;
}

void video_oam_write16(uint32_t addr, uint16_t value)
{
	value = common_bswp16(value);
	memcpy(&vdp.oam[addr & 0x1FF], &value, 2);
}

void video_oam_write32(uint32_t addr, uint32_t value)
{
	value = common_bswp32(value);
	memcpy(&vdp.oam[addr & 0x1FF], &value, 4);
}

uint8_t video_capture_read8(uint32_t addr)
{
	return vdp.capture_buffer[addr & 0x1FF];
}

uint16_t video_capture_read16(uint32_t addr)
{
	addr &= 0x1FF;
	uint16_t value;
	memcpy(&value, &vdp.capture_buffer[addr], 2);
	return common_bswp16(value);
}

uint32_t video_capture_read32(uint32_t addr)
{
	addr &= 0x1FF;
	uint32_t value;
	memcpy(&value, &vdp.capture_buffer[addr], 4);
	return common_bswp32(value);
}

void video_capture_write8(uint32_t addr, uint8_t value)
{
	(void)addr;
	(void)value;
}

void video_capture_write16(uint32_t addr, uint16_t value)
{
	(void)addr;
	(void)value;
}

void video_capture_write32(uint32_t addr, uint32_t value)
{
	(void)addr;
	(void)value;
}

uint8_t video_bitmap_reg_read8(uint32_t addr)
{
	return vdp_read8_from_read16(addr, video_bitmap_reg_read16);
}

uint16_t video_bitmap_reg_read16(uint32_t addr)
{
	addr &= 0xFFF;
	
	int index = (addr >> 1) & 0x3;
	int reg = addr & ~0x7;

	switch (reg)
	{
	case 0x000:
		return vdp.bitmap_regs[index].scrollx;
	case 0x008:
		return vdp.bitmap_regs[index].scrolly;
	case 0x010:
		return vdp.bitmap_regs[index].screenx;
	case 0x018:
		return vdp.bitmap_regs[index].screeny;
	case 0x020:
		return vdp.bitmap_regs[index].w | (vdp.bitmap_regs[index].clipx << 8);
	case 0x028:
		return vdp.bitmap_regs[index].h;
	case 0x030:
		return vdp.bitmap_ctrl;
	case 0x040:
		return vdp.bitmap_palsel;
	case 0x050:
		return vdp.bitmap_regs[index].buffer_ctrl;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped read16 %04X\n", addr);
		return 0;
	}
}

uint32_t video_bitmap_reg_read32(uint32_t addr)
{
	return vdp_read32_from_read16(addr, video_bitmap_reg_read16);
}

void video_bitmap_reg_write8(uint32_t addr, uint8_t value)
{
	vdp_write8_via_write16(addr, value, video_bitmap_reg_read16, video_bitmap_reg_write16);
}

void video_bitmap_reg_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFF;

	int index = (addr >> 1) & 0x3;
	int reg = addr & ~0x7;

	switch (reg)
	{
	case 0x000:
		LOOPY_DEBUG_PRINTF("[Video] write BM%d_SCROLLX: %04X\n", index, value);
		vdp.bitmap_regs[index].scrollx = value & 0x1FF;
		break;
	case 0x008:
		LOOPY_DEBUG_PRINTF("[Video] write BM%d_SCROLLY: %04X\n", index, value);
		vdp.bitmap_regs[index].scrolly = value & 0x1FF;
		break;
	case 0x010:
		LOOPY_DEBUG_PRINTF("[Video] write BM%d_SCREENX: %04X\n", index, value);
		vdp.bitmap_regs[index].screenx = value & 0x1FF;
		break;
	case 0x018:
		LOOPY_DEBUG_PRINTF("[Video] write BM%d_SCREENY: %04X\n", index, value);
		vdp.bitmap_regs[index].screeny = value & 0x1FF;
		break;
	case 0x020:
		LOOPY_DEBUG_PRINTF("[Video] write BM%d_CLIPWIDTH: %04X\n", index, value);
		vdp.bitmap_regs[index].w = value & 0xFF;
		vdp.bitmap_regs[index].clipx = value >> 8;
		break;
	case 0x028:
		LOOPY_DEBUG_PRINTF("[Video] write BM%d_HEIGHT: %04X\n", index, value);
		vdp.bitmap_regs[index].h = value & 0xFF;
		break;
	case 0x030:
		LOOPY_DEBUG_PRINTF("[Video] write BM_CTRL: %04X\n", value);
		vdp.bitmap_ctrl = value;
		break;
	case 0x040:
		LOOPY_DEBUG_PRINTF("[Video] write BM_PALSEL: %04X\n", value);
		vdp.bitmap_palsel = value;
		break;
	case 0x050:
		LOOPY_DEBUG_PRINTF("[Video] write BM%d_BUFFER_CTRL: %04X\n", index, value);
		vdp.bitmap_regs[index].buffer_ctrl = value;
		break;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped write16 %04X: %04X\n", addr, value);
		break;
	}
}

void video_bitmap_reg_write32(uint32_t addr, uint32_t value)
{
	vdp_write32_via_write16(addr, value, video_bitmap_reg_write16);
}

uint8_t video_ctrl_read8(uint32_t addr)
{
	return vdp_read8_from_read16(addr, video_ctrl_read16);
}

uint16_t video_ctrl_read16(uint32_t addr)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
	{
		uint16_t result = vdp.mode.use_pal;
		result |= vdp.mode.extra_scanlines << 1;
		result |= vdp.mode.unk << 2;
		result |= vdp.mode.mouse_scan << 3;
		result |= vdp.mode.pad_scan << 4;
		result |= vdp.mode.unk2 << 5;
		return result;
	}
	case 0x002:
		return video_current_hcount();
	case 0x004:
		return vdp.vcount;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped read16 %04X\n", addr);
		return 0;
	}
}

uint32_t video_ctrl_read32(uint32_t addr)
{
	return vdp_read32_from_read16(addr, video_ctrl_read16);
}

void video_ctrl_write8(uint32_t addr, uint8_t value)
{
	vdp_write8_via_write16(addr, value, video_ctrl_read16, video_ctrl_write16);
}

void video_ctrl_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
		LOOPY_DEBUG_PRINTF("[Video] write MODE: %04X\n", value);
		vdp.mode.use_pal = value & 0x1;
		vdp.mode.extra_scanlines = (value >> 1) & 0x1;
		vdp.mode.unk = (value >> 2) & 0x1;
		vdp.mode.mouse_scan = (value >> 3) & 0x1;
		vdp.mode.pad_scan = (value >> 4) & 0x1;
		vdp.mode.unk2 = (value >> 5) & 0x1;
		loopy_io_set_controller_mode(vdp.mode.pad_scan != 0, vdp.mode.mouse_scan != 0);
		assert(!vdp.mode.use_pal);

		vdp.visible_scanlines = (vdp.mode.extra_scanlines) ? 0xF0 : 0xE0;
		break;
	case 0x006:
			if (value & 0x01)
			{
				vdp.capture_enable = true;
				if (vdp.irq2_source == 0) schedule_irq2_ready(cpu_cycles_per_ntsc_line());
			}
			if (value & 0x02)
			{
				loopy_io_trigger_adc();
				if (vdp.irq2_source == 1) schedule_irq2_ready(32);
			}
			if (value & 0x04)
			{
				loopy_io_trigger_printer_sensors();
			}

			if (value & ~0x07)
			{
				LOOPY_DEBUG_PRINTF("[Video] write trigger/control 006: %04X\n", value);
			}
			break;
		case 0x008:
		LOOPY_DEBUG_PRINTF("[Video] write SYNC_IRQ_CTRL: %04X\n", value);
		vdp.sync_irq_ctrl.irq1_enable = value & 0x1;
		vdp.sync_irq_ctrl.irq1_source = (value >> 1) & 0x1;
		break;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped write16 %04X: %04X\n", addr, value);
		break;
	}
}

void video_ctrl_write32(uint32_t addr, uint32_t value)
{
	vdp_write32_via_write16(addr, value, video_ctrl_write16);
}

uint8_t video_bgobj_read8(uint32_t addr)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x20:
		return vdp.tilebase;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped read16 %04X\n", addr);
		return 0;
	}
}

uint16_t video_bgobj_read16(uint32_t addr)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
	{
		uint16_t result = vdp.bg_ctrl.shared_maps;
		result |= vdp.bg_ctrl.map_size << 1;
		result |= vdp.bg_ctrl.bg0_8bit << 3;
		result |= vdp.bg_ctrl.tile_size1 << 4;
		result |= vdp.bg_ctrl.tile_size0 << 6;
		return result;
	}
	case 0x002:
		return vdp.bg_scrollx[0];
	case 0x004:
		return vdp.bg_scrolly[0];
	case 0x006:
		return vdp.bg_scrollx[1];
	case 0x008:
		return vdp.bg_scrolly[1];
	case 0x00A:
		return vdp.bg_palsel[0];
	case 0x00C:
		return vdp.bg_palsel[1];
	case 0x010:
	{
		uint16_t result = vdp.obj_ctrl.id_offs;
		result |= vdp.obj_ctrl.tile_index_offs[1] << 8;
		result |= vdp.obj_ctrl.tile_index_offs[0] << 11;
		result |= vdp.obj_ctrl.is_8bit << 14;
		return result;
	}
	case 0x012:
		return vdp.obj_palsel[0];
	case 0x014:
		return vdp.obj_palsel[1];
	case 0x020:
		return vdp.tilebase;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped read16 %04X\n", addr);
		return 0;
	}
}

uint32_t video_bgobj_read32(uint32_t addr)
{
	return vdp_read32_from_read16(addr, video_bgobj_read16);
}

void video_bgobj_write8(uint32_t addr, uint8_t value)
{
	vdp_write8_via_write16(addr, value, video_bgobj_read16, video_bgobj_write16);
}

void video_bgobj_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
		LOOPY_DEBUG_PRINTF("[Video] write BG_CTRL: %04X\n", value);
		vdp.bg_ctrl.shared_maps = value & 0x1;
		vdp.bg_ctrl.map_size = (value >> 1) & 0x3;
		vdp.bg_ctrl.bg0_8bit = (value >> 3) & 0x1;

		//Note the reversed order!
		vdp.bg_ctrl.tile_size1 = (value >> 4) & 0x3;
		vdp.bg_ctrl.tile_size0 = (value >> 6) & 0x3;
		break;
	case 0x002:
	case 0x006:
	{
		int index = (addr - 0x002) >> 2;
		LOOPY_DEBUG_PRINTF("[Video] write BG%d_SCROLLX: %04X\n", index, value);
		vdp.bg_scrollx[index] = value & 0xFFF;
		break;
	}
	case 0x004:
	case 0x008:
	{
		int index = (addr - 0x004) >> 2;
		LOOPY_DEBUG_PRINTF("[Video] write BG%d_SCROLLY: %04X\n", index, value);
		vdp.bg_scrolly[index] = value & 0xFFF;
		break;
	}
	case 0x00A:
	case 0x00C:
	{
		int index = (addr - 0x00A) >> 1;
		LOOPY_DEBUG_PRINTF("[Video] write BG%d_PALSEL: %04X\n", index, value);
		vdp.bg_palsel[index] = value;
		break;
	}
	case 0x010:
		LOOPY_DEBUG_PRINTF("[Video] write OBJ_CTRL: %04X\n", value);
		vdp.obj_ctrl.id_offs = value & 0xFF;

		//Note the reversed order!
		vdp.obj_ctrl.tile_index_offs[1] = (value >> 8) & 0x7;
		vdp.obj_ctrl.tile_index_offs[0] = (value >> 11) & 0x7;
		vdp.obj_ctrl.is_8bit = (value >> 14) & 0x1;
		break;
	case 0x012:
	case 0x014:
	{
		int index = (addr - 0x012) >> 1;
		LOOPY_DEBUG_PRINTF("[Video] write OBJ%d_PALSEL: %04X\n", index, value);
		vdp.obj_palsel[index] = value;
		break;
	}
	case 0x020:
		LOOPY_DEBUG_PRINTF("[Video] write TILEBASE: %04X\n", value);
		vdp.tilebase = value & 0xFF;
		break;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped write16 %04X: %04X\n", addr, value);
		break;
	}
}

void video_bgobj_write32(uint32_t addr, uint32_t value)
{
	vdp_write32_via_write16(addr, value, video_bgobj_write16);
}

uint8_t video_display_read8(uint32_t addr)
{
	return vdp_read8_from_read16(addr, video_display_read16);
}

uint16_t video_display_read16(uint32_t addr)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
		return vdp.dispmode;
	case 0x002:
	{
		uint16_t result = 0;

		for (int i = 0; i < 2; i++)
		{
			result |= vdp.layer_ctrl.bg_enable[i] << i;
			result |= vdp.layer_ctrl.obj_enable[i] << (i + 6);
		}

		for (int i = 0; i < 4; i++)
		{
			result |= vdp.layer_ctrl.bitmap_enable[i] << (i + 2);
		}

		result |= vdp.layer_ctrl.bitmap_screen_mode[0] << 8;
		result |= vdp.layer_ctrl.bitmap_screen_mode[1] << 10;
		result |= vdp.layer_ctrl.obj_screen_mode[0] << 12;
		result |= vdp.layer_ctrl.obj_screen_mode[1] << 14;
		return result;
	}
	case 0x004:
	{
		uint16_t result = vdp.color_prio.prio_mode;
		result |= vdp.color_prio.screen_b_backdrop_only << 4;
		result |= vdp.color_prio.output_screen_b << 5;
		result |= vdp.color_prio.output_screen_a << 6;
		result |= vdp.color_prio.blend_mode << 7;
		return result;
	}
	case 0x006:
		//Note the reversed order!
		return vdp.backdrops[1];
	case 0x008:
		return vdp.backdrops[0];
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped read16 %04X\n", addr);
		return 0;
	}
}

uint32_t video_display_read32(uint32_t addr)
{
	return vdp_read32_from_read16(addr, video_display_read16);
}

void video_display_write8(uint32_t addr, uint8_t value)
{
	vdp_write8_via_write16(addr, value, video_display_read16, video_display_write16);
}

void video_display_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
		vdp.dispmode = value & 0x7;
		LOOPY_DEBUG_PRINTF("[Video] write DISPMODE: %04X\n", value);
		break;
	case 0x002:
		for (int i = 0; i < 2; i++)
		{
			vdp.layer_ctrl.bg_enable[i] = (value >> i) & 0x1;
			vdp.layer_ctrl.obj_enable[i] = (value >> (i + 6)) & 0x1;
		}

		for (int i = 0; i < 4; i++)
		{
			vdp.layer_ctrl.bitmap_enable[i] = (value >> (i + 2)) & 0x1;
		}

		vdp.layer_ctrl.bitmap_screen_mode[0] = (value >> 8) & 0x3;
		vdp.layer_ctrl.bitmap_screen_mode[1] = (value >> 10) & 0x3;
		vdp.layer_ctrl.obj_screen_mode[0] = (value >> 12) & 0x3;
		vdp.layer_ctrl.obj_screen_mode[1] = value >> 14;
		LOOPY_DEBUG_PRINTF("[Video] write LAYER_CTRL: %04X\n", value);
		break;
	case 0x004:
		vdp.color_prio.prio_mode = value & 0xF;
		vdp.color_prio.screen_b_backdrop_only = (value >> 4) & 0x1;
		vdp.color_prio.output_screen_b = (value >> 5) & 0x1;
		vdp.color_prio.output_screen_a = (value >> 6) & 0x1;
		vdp.color_prio.blend_mode = (value >> 7) & 0x1;
		LOOPY_DEBUG_PRINTF("[Video] write COLORPRIO: %04X\n", value);
		break;
	case 0x006:
		//Note the reversed order!
		vdp.backdrops[1] = value;
		break;
	case 0x008:
		vdp.backdrops[0] = value;
		break;
	case 0x00A:
		vdp.capture_ctrl.scanline = value & 0xFF;
		vdp.capture_ctrl.format = (value >> 8) & 0x3;
		break;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped write16 %04X: %04X\n", addr, value);
		break;
	}
}

void video_display_write32(uint32_t addr, uint32_t value)
{
	vdp_write32_via_write16(addr, value, video_display_write16);
}

uint8_t video_irq_read8(uint32_t addr)
{
	return vdp_read8_from_read16(addr, video_irq_read16);
}

uint16_t video_irq_read16(uint32_t addr)
{
	addr &= 0xFFF;
	
	switch (addr)
	{
	case 0x002:
		return vdp.irq0_hcmp;
	case 0x004:
		return vdp.irq0_vcmp;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped read16 %04X\n", addr);
		return 0;
	}
}

uint32_t video_irq_read32(uint32_t addr)
{
	return vdp_read32_from_read16(addr, video_irq_read16);
}

void video_irq_write8(uint32_t addr, uint8_t value)
{
	vdp_write8_via_write16(addr, value, video_irq_read16, video_irq_write16);
}

void video_irq_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFF;

	switch (addr)
	{
	case 0x000:
		vdp.cmp_irq_ctrl.irq0_enable = (value >> 1) & 0x1;
		vdp.cmp_irq_ctrl.nmi_enable = (value >> 2) & 0x1;
		vdp.cmp_irq_ctrl.use_vcmp = (value >> 5) & 0x1;
		vdp.cmp_irq_ctrl.irq0_enable2 = (value >> 7) & 0x1;
		vdp.irq2_enable_a = value & 0x1;
		vdp.irq2_enable_b = (value >> 3) & 0x1;
		vdp.irq2_source = (value >> 4) & 0x1;
		vdp.irq2_enable_c = (value >> 6) & 0x1;
		LOOPY_DEBUG_PRINTF("[VDP] write CMP_IRQ_CTRL: %04X\n", value);
		break;
	case 0x002:
		vdp.irq0_hcmp = value & 0x1FF;
		break;
	case 0x004:
		vdp.irq0_vcmp = value & 0x1FF;
		break;
	}
}

void video_irq_write32(uint32_t addr, uint32_t value)
{
	vdp_write32_via_write16(addr, value, video_irq_write16);
}

uint8_t video_dma_ctrl_read8(uint32_t addr)
{
	return vdp_read8_from_read16(addr, video_dma_ctrl_read16);
}

uint16_t video_dma_ctrl_read16(uint32_t addr)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
		return vdp.bitmap_mem_ctrl;
	case 0x002:
		return vdp.dma_mask;
	case 0x004:
		return vdp.dma_value;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped read16 %04X\n", addr);
		return 0;
	}
}

uint32_t video_dma_ctrl_read32(uint32_t addr)
{
	return vdp_read32_from_read16(addr, video_dma_ctrl_read16);
}

void video_dma_ctrl_write8(uint32_t addr, uint8_t value)
{
	vdp_write8_via_write16(addr, value, video_dma_ctrl_read16, video_dma_ctrl_write16);
}

void video_dma_ctrl_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFF;
	switch (addr)
	{
	case 0x000:
		vdp.bitmap_mem_ctrl = value & 0x7;
		LOOPY_DEBUG_PRINTF("[Video] write BM_MEM_CTRL: %04X\n", value);
		break;
	case 0x002:
		//TODO: what does bit 8 do? Seems to have no effect in HW tests at this time
		vdp.dma_mask = value & 0x1FF;
		break;
	case 0x004:
		vdp.dma_value = value & 0xFF;
		break;
	default:
		LOOPY_DEBUG_PRINTF("[Video] unmapped write16 %04X: %04X\n", addr, value);
		break;
	}
}

void video_dma_ctrl_write32(uint32_t addr, uint32_t value)
{
	vdp_write32_via_write16(addr, value, video_dma_ctrl_write16);
}

uint8_t video_dma_read8(uint32_t addr)
{
	(void)addr;
	return 0;
}

uint16_t video_dma_read16(uint32_t addr)
{
	(void)addr;
	return 0;
}

uint32_t video_dma_read32(uint32_t addr)
{
	(void)addr;
	return 0;
}

void video_dma_write8(uint32_t addr, uint8_t value)
{
	(void)value;
	video_dma_write16(addr & ~1u, 0);
}

void video_dma_write16(uint32_t addr, uint16_t value)
{
	//Value written doesn't matter, it always triggers this
	//TODO: how long does this take? Is the CPU stalled?
	addr &= 0x3FF;

	int y = addr >> 1;
	for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
	{
		uint32_t addr = x + (y * VIDEO_DISPLAY_WIDTH);
		uint8_t data = vdp.bitmap[addr];
		data &= ~vdp.dma_mask;
		data |= vdp.dma_value & vdp.dma_mask;
		vdp.bitmap[addr] = data;
	}
}

void video_dma_write32(uint32_t addr, uint32_t value)
{
	(void)value;
	video_dma_write16(addr & ~1u, 0);
	video_dma_write16((addr + 2u) & ~1u, 0);
}


void video_set_bmp_dump_enabled(bool enabled) { bmp_dump_enabled = enabled; }

typedef struct VDPStateBlob {
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
} VDPStateBlob;

uint32_t video_state_blob_size(void) { return (uint32_t)sizeof(VDPStateBlob); }

void video_get_state_blob(void *dst, uint32_t size) {
    if (!dst || size != sizeof(VDPStateBlob)) return;
#ifdef LOOPY_WASM_FRONTEND
    static VDPStateBlob tmp;
    VDPStateBlob *b = &tmp;
#else
    VDPStateBlob tmp;
    VDPStateBlob *b = &tmp;
#endif
    memset(b, 0, sizeof(*b));
    b->frame_ended = vdp.frame_ended;
    b->visible_scanlines = vdp.visible_scanlines;
    memcpy(b->screens, vdp.screens, sizeof(b->screens));
    memcpy(b->bitmap_linebuf, vdp.bitmap_linebuf, sizeof(b->bitmap_linebuf));
    memcpy(b->bitmap_linebuf_valid, vdp.bitmap_linebuf_valid, sizeof(b->bitmap_linebuf_valid));
    memcpy(b->bitmap, vdp.bitmap, sizeof(b->bitmap));
    memcpy(b->tile, vdp.tile, sizeof(b->tile));
    memcpy(b->oam, vdp.oam, sizeof(b->oam));
    memcpy(b->palette, vdp.palette, sizeof(b->palette));
    memcpy(b->capture_buffer, vdp.capture_buffer, sizeof(b->capture_buffer));
    memcpy(&b->mode, &vdp.mode, sizeof(b->mode));
    b->hcount = vdp.hcount;
    b->vcount = vdp.vcount;
    b->bus_latch = vdp.bus_latch;
    b->line_start_timestamp = vdp.line_start_timestamp;
    memcpy(&b->sync_irq_ctrl, &vdp.sync_irq_ctrl, sizeof(b->sync_irq_ctrl));
    b->capture_enable = vdp.capture_enable;
    memcpy(b->bitmap_regs, vdp.bitmap_regs, sizeof(b->bitmap_regs));
    b->bitmap_ctrl = vdp.bitmap_ctrl;
    b->bitmap_palsel = vdp.bitmap_palsel;
    memcpy(&b->bg_ctrl, &vdp.bg_ctrl, sizeof(b->bg_ctrl));
    memcpy(b->bg_scrollx, vdp.bg_scrollx, sizeof(b->bg_scrollx));
    memcpy(b->bg_scrolly, vdp.bg_scrolly, sizeof(b->bg_scrolly));
    memcpy(b->bg_palsel, vdp.bg_palsel, sizeof(b->bg_palsel));
    b->tilebase = vdp.tilebase;
    memcpy(&b->obj_ctrl, &vdp.obj_ctrl, sizeof(b->obj_ctrl));
    memcpy(b->obj_palsel, vdp.obj_palsel, sizeof(b->obj_palsel));
    b->dispmode = vdp.dispmode;
    memcpy(&b->layer_ctrl, &vdp.layer_ctrl, sizeof(b->layer_ctrl));
    memcpy(&b->color_prio, &vdp.color_prio, sizeof(b->color_prio));
    memcpy(b->backdrops, vdp.backdrops, sizeof(b->backdrops));
    memcpy(&b->capture_ctrl, &vdp.capture_ctrl, sizeof(b->capture_ctrl));
    memcpy(&b->cmp_irq_ctrl, &vdp.cmp_irq_ctrl, sizeof(b->cmp_irq_ctrl));
    b->irq0_hcmp = vdp.irq0_hcmp;
    b->irq0_vcmp = vdp.irq0_vcmp;
    b->irq2_source = vdp.irq2_source;
    b->irq2_enable_a = vdp.irq2_enable_a;
    b->irq2_enable_b = vdp.irq2_enable_b;
    b->irq2_enable_c = vdp.irq2_enable_c;
    b->bitmap_mem_ctrl = vdp.bitmap_mem_ctrl;
    b->dma_mask = vdp.dma_mask;
    b->dma_value = vdp.dma_value;
    memcpy(dst, b, sizeof(*b));
}

#define VDP_STATE_BLOB_LEGACY_SIZE 198976u

void video_set_state_blob(const void *src, uint32_t size) {
    if (!src) return;
    uint8_t legacy_expand_storage[sizeof(VDPStateBlob)];
    if (size == VDP_STATE_BLOB_LEGACY_SIZE) {
        memset(legacy_expand_storage, 0, sizeof(legacy_expand_storage));
        const uint8_t *oldp = (const uint8_t *)src;
        uint8_t *newp = legacy_expand_storage;
        const size_t prefix = offsetof(VDPStateBlob, bitmap_linebuf);
        const size_t old_bitmap = prefix;
        const size_t new_bitmap = offsetof(VDPStateBlob, bitmap);
        memcpy(newp, oldp, prefix);
        memcpy(newp + new_bitmap, oldp + old_bitmap, VIDEO_BITMAP_VRAM_SIZE);
        memcpy(newp + new_bitmap + VIDEO_BITMAP_VRAM_SIZE,
               oldp + old_bitmap + VIDEO_BITMAP_VRAM_SIZE,
               VDP_STATE_BLOB_LEGACY_SIZE - old_bitmap - VIDEO_BITMAP_VRAM_SIZE);
        src = legacy_expand_storage;
        size = (uint32_t)sizeof(VDPStateBlob);
    }
    if (size != sizeof(VDPStateBlob)) return;
#ifdef LOOPY_WASM_FRONTEND
    static VDPStateBlob tmp;
    VDPStateBlob *b = &tmp;
#else
    VDPStateBlob tmp;
    VDPStateBlob *b = &tmp;
#endif
    memcpy(b, src, sizeof(*b));
    vdp.frame_ended = b->frame_ended;
    vdp.visible_scanlines = b->visible_scanlines;
    memcpy(vdp.screens, b->screens, sizeof(b->screens));
    memcpy(vdp.bitmap_linebuf, b->bitmap_linebuf, sizeof(b->bitmap_linebuf));
    memcpy(vdp.bitmap_linebuf_valid, b->bitmap_linebuf_valid, sizeof(b->bitmap_linebuf_valid));
    memcpy(vdp.bitmap, b->bitmap, sizeof(b->bitmap));
    memcpy(vdp.tile, b->tile, sizeof(b->tile));
    memcpy(vdp.oam, b->oam, sizeof(b->oam));
    memcpy(vdp.palette, b->palette, sizeof(b->palette));
    memcpy(vdp.capture_buffer, b->capture_buffer, sizeof(b->capture_buffer));
    memcpy(&vdp.mode, &b->mode, sizeof(vdp.mode));
    vdp.hcount = b->hcount;
    vdp.vcount = b->vcount;
    vdp.bus_latch = b->bus_latch;
    vdp.line_start_timestamp = b->line_start_timestamp;
    memcpy(&vdp.sync_irq_ctrl, &b->sync_irq_ctrl, sizeof(vdp.sync_irq_ctrl));
    vdp.capture_enable = b->capture_enable;
    memcpy(vdp.bitmap_regs, b->bitmap_regs, sizeof(b->bitmap_regs));
    vdp.bitmap_ctrl = b->bitmap_ctrl;
    vdp.bitmap_palsel = b->bitmap_palsel;
    memcpy(&vdp.bg_ctrl, &b->bg_ctrl, sizeof(vdp.bg_ctrl));
    memcpy(vdp.bg_scrollx, b->bg_scrollx, sizeof(b->bg_scrollx));
    memcpy(vdp.bg_scrolly, b->bg_scrolly, sizeof(b->bg_scrolly));
    memcpy(vdp.bg_palsel, b->bg_palsel, sizeof(b->bg_palsel));
    vdp.tilebase = b->tilebase;
    memcpy(&vdp.obj_ctrl, &b->obj_ctrl, sizeof(vdp.obj_ctrl));
    memcpy(vdp.obj_palsel, b->obj_palsel, sizeof(b->obj_palsel));
    vdp.dispmode = b->dispmode;
    memcpy(&vdp.layer_ctrl, &b->layer_ctrl, sizeof(vdp.layer_ctrl));
    memcpy(&vdp.color_prio, &b->color_prio, sizeof(vdp.color_prio));
    memcpy(vdp.backdrops, b->backdrops, sizeof(b->backdrops));
    memcpy(&vdp.capture_ctrl, &b->capture_ctrl, sizeof(vdp.capture_ctrl));
    memcpy(&vdp.cmp_irq_ctrl, &b->cmp_irq_ctrl, sizeof(vdp.cmp_irq_ctrl));
    vdp.irq0_hcmp = b->irq0_hcmp;
    vdp.irq0_vcmp = b->irq0_vcmp;
    vdp.irq2_source = b->irq2_source;
    vdp.irq2_enable_a = b->irq2_enable_a;
    vdp.irq2_enable_b = b->irq2_enable_b;
    vdp.irq2_enable_c = b->irq2_enable_c;
    vdp.bitmap_mem_ctrl = b->bitmap_mem_ctrl;
    vdp.dma_mask = b->dma_mask;
    vdp.dma_value = b->dma_value;
}

int video_debug_peek(uint32_t addr, int bytes, uint32_t *out_value)
{
    const uint8_t *base;
    uint32_t mask, offs;

    if (!out_value || bytes < 1 || bytes > 4) return 0;

    if (addr >= VIDEO_BITMAP_VRAM_START && addr < (uint32_t)(VIDEO_BITMAP_VRAM_END)) {
        base = vdp.bitmap; mask = 0x1FFFFu; offs = addr - VIDEO_BITMAP_VRAM_START;
    } else if (addr >= VIDEO_TILE_VRAM_START && addr < (uint32_t)(VIDEO_TILE_VRAM_END)) {
        base = vdp.tile; mask = 0xFFFFu; offs = addr - VIDEO_TILE_VRAM_START;
    } else if (addr >= VIDEO_OAM_START && addr < (uint32_t)(VIDEO_OAM_END)) {
        base = vdp.oam; mask = 0x1FFu; offs = addr - VIDEO_OAM_START;
    } else if (addr >= VIDEO_PALETTE_START && addr < (uint32_t)(VIDEO_PALETTE_END)) {
        base = vdp.palette; mask = 0x1FFu; offs = addr - VIDEO_PALETTE_START;
    } else if (addr >= VIDEO_CAPTURE_START && addr < (uint32_t)(VIDEO_CAPTURE_END)) {
        base = vdp.capture_buffer; mask = 0x1FFu; offs = addr - VIDEO_CAPTURE_START;
    } else {
        /* Registers are deliberately excluded.  Several are strobes or clear
           status on access, and reads of the control block are what the idle
           detector keys on, so there is no way to sample them for inspection
           without changing what the machine would do next. */
        return 0;
    }

    uint32_t value = 0;
    for (int i = 0; i < bytes; i++) value = (value << 8) | base[(offs + (uint32_t)i) & mask];
    *out_value = value;
    return 1;
}
