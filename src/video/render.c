#include <assert.h>
#include <string.h>
#include "common/bswp.h"
#include "core/loopy_io.h"
#include "video/render.h"
#include "video/vdp_local.h"

typedef struct TilemapInfo
{
	int width;
	int height;
	uint32_t bg1_start;
	uint32_t data_start;
} TilemapInfo;

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int sign9(uint16_t value)
{
	value &= 0x1FF;
	return (value & 0x100) ? ((int)value - 0x200) : (int)value;
}

static uint16_t read_palette(uint8_t value)
{
	uint16_t color;
	memcpy(&color, vdp.palette + (value * 2), 2);
	return common_bswp16(color);
}

static uint16_t read_screen(int index, int x)
{
	uint8_t pal_color = vdp.screens[index][x];
	if (!pal_color || (index == 1 && vdp.color_prio.screen_b_backdrop_only))
	{
		return vdp.backdrops[index];
	}

	return read_palette(pal_color);
}

static void write_screen(int index, int x, uint8_t value)
{
	x &= 0x1FF;
	if (x < VIDEO_DISPLAY_WIDTH)
	{
		vdp.screens[index][x] = value;
	}
}

static void write_color(uint16_t * buffer, int x, int y, uint16_t value)
{
	x &= 0x1FF;

	//Layer output is always 240 lines long, even in 224-line mode
	//This just centers the picture for 224-line mode
	if (!vdp.mode.extra_scanlines)
	{
		y += 8;
	}

	if (x < VIDEO_DISPLAY_WIDTH)
	{
		buffer[x + (y * VIDEO_DISPLAY_WIDTH)] = value;
	}
}

static void fill_224_mode_vertical_borders(void)
{
	/* In 224-line mode the active 256x224 picture is centered inside
	   the 240-line video field.  The eight lines above and below are
	   outside the active render area and are colored by screen A's
	   backdrop; they must not retain stale framebuffer contents. */
	if (vdp.mode.extra_scanlines)
	{
		return;
	}

	const uint16_t color = vdp.backdrops[0];
	for (int y = 0; y < 8; y++)
	{
		for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
		{
			vdp.display_output[y * VIDEO_DISPLAY_WIDTH + x] = color;
			vdp.screen_output[0][y * VIDEO_DISPLAY_WIDTH + x] = color;
			vdp.screen_output[1][y * VIDEO_DISPLAY_WIDTH + x] = color;
		}
	}

	for (int y = VIDEO_DISPLAY_HEIGHT - 8; y < VIDEO_DISPLAY_HEIGHT; y++)
	{
		for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
		{
			vdp.display_output[y * VIDEO_DISPLAY_WIDTH + x] = color;
			vdp.screen_output[0][y * VIDEO_DISPLAY_WIDTH + x] = color;
			vdp.screen_output[1][y * VIDEO_DISPLAY_WIDTH + x] = color;
		}
	}
}

static void write_pal_color(uint16_t * buffer, int x, int y, uint8_t pal_index)
{
	uint16_t color = read_palette(pal_index);
	write_color(buffer, x, y, color);
}

static int get_bg_tile_size(int index)
{
	int tile_size = (index == 0) ? vdp.bg_ctrl.tile_size0 : vdp.bg_ctrl.tile_size1;
	switch (tile_size)
	{
	case 0x00:
		tile_size = 8;
		break;
	case 0x01:
		tile_size = 16;
		break;
	case 0x02:
		tile_size = 32;
		break;
	case 0x03:
		tile_size = 64;
		break;
	default:
		assert(0);
	}
	return tile_size;
}

static void get_tilemap_info(TilemapInfo *info)
{
	switch (vdp.bg_ctrl.map_size)
	{
	case 0x00:
		info->width = 64;
		info->height = 64;
		break;
	case 0x01:
		info->width = 64;
		info->height = 32;
		break;
	case 0x02:
		info->width = 32;
		info->height = 64;
		break;
	case 0x03:
		info->width = 32;
		info->height = 32;
		break;
	default:
		assert(0);
	}

	info->data_start = (info->width * info->height) << 1;
	if (vdp.bg_ctrl.shared_maps)
	{
		info->bg1_start = 0;
	}
	else
	{
		info->bg1_start = info->data_start;
		info->data_start <<= 1;
	}
}

static void draw_bg(int index, int screen_y)
{
	if (!vdp.layer_ctrl.bg_enable[index])
	{
		return;
	}

	bool is_8bit = index == 0 && vdp.bg_ctrl.bg0_8bit;
	int tile_size = get_bg_tile_size(index);
	int tile_size_mask = tile_size - 1;

	TilemapInfo tilemap;
	get_tilemap_info(&tilemap);

	uint32_t map_start = (index == 1) ? tilemap.bg1_start : 0;;

	for (int screen_x = 0; screen_x < VIDEO_DISPLAY_WIDTH; screen_x++)
	{
		int x = (screen_x + vdp.bg_scrollx[index]) & ((tilemap.width * tile_size) - 1);
		int y = (screen_y + vdp.bg_scrolly[index]) & ((tilemap.height * tile_size) - 1);

		uint16_t map_offs = (x / tile_size) + ((y / tile_size) * tilemap.width);

		uint16_t descriptor;
		memcpy(&descriptor, &vdp.tile[map_start + (map_offs << 1)], 2);
		descriptor = common_bswp16(descriptor);

		uint16_t tile_index = descriptor & 0x7FF;
		int screen_index = (descriptor >> 11) & 0x1;
		int pal_descriptor = (descriptor >> 12) & 0x3;
		bool x_flip = (descriptor >> 14) & 0x1;
		bool y_flip = descriptor >> 15;

		int tile_x = x & tile_size_mask;
		if (x_flip)
		{
			tile_x = tile_size_mask - tile_x;
		}

		int tile_y = y & tile_size_mask;
		if (y_flip)
		{
			tile_y = tile_size_mask - tile_y;
		}

		tile_index += tile_y & ~0x7;
		tile_index += tile_x >> 3;
		uint32_t offs = (tile_x & 0x7) + ((tile_y & 0x7) * 0x08) + (tile_index << 6);

		uint8_t tile_data;
		if (is_8bit)
		{
			tile_data = vdp.tile[(tilemap.data_start + offs) & 0xFFFF];
		}
		else
		{
			offs >>= 1;
			offs += vdp.tilebase << 9;
			tile_data = vdp.tile[(tilemap.data_start + offs) & 0xFFFF];
			if (tile_x & 0x1)
			{
				tile_data &= 0xF;
			}
			else
			{
				tile_data >>= 4;
			}
		}

		//0 is transparent, no matter if it's 4-bit or 8-bit
		if (!tile_data)
		{
			continue;
		}

		uint8_t output = tile_data;
		if (!is_8bit)
		{
			uint16_t palsel = vdp.bg_palsel[index];
			int pal = (palsel >> (pal_descriptor * 4)) & 0xF;
			output |= pal << 4;
		}
		
		write_pal_color(vdp.bg_output[index], screen_x, screen_y, output);
		write_screen(screen_index, screen_x, output);
	}
}

typedef struct BitmapLayout
{
	bool is_8bit;
	bool split_y;
	bool split_x;
	int vram_width;
	int vram_height;
} BitmapLayout;

static BitmapLayout get_bitmap_layout(void)
{
	BitmapLayout layout = {0};
	/* VDP.BM_CTRL has five documented layouts. Invalid modes 5, 6 and 7
	   alias modes 0, 1 and 1 respectively on hardware. */
	switch (vdp.bitmap_ctrl & 0x7)
	{
	case 0x00:
	case 0x05:
		layout.is_8bit = true;
		layout.split_y = true;
		layout.vram_width = 256;
		layout.vram_height = 256;
		break;
	case 0x01:
	case 0x06:
	case 0x07:
		layout.is_8bit = true;
		layout.vram_width = 256;
		layout.vram_height = 512;
		break;
	case 0x02:
		layout.is_8bit = false;
		layout.split_y = true;
		layout.vram_width = 512;
		layout.vram_height = 256;
		break;
	case 0x03:
		layout.is_8bit = false;
		layout.split_x = true;
		layout.vram_width = 256;
		layout.vram_height = 512;
		break;
	case 0x04:
		layout.is_8bit = false;
		layout.vram_width = 512;
		layout.vram_height = 512;
		break;
	}
	return layout;
}

static uint8_t read_bitmap_pixel(const BitmapLayout *layout, const VDPBitmapRegs *regs, int layer_x, int layer_y)
{
	int width_mask = layout->vram_width - 1;
	int height_mask = layout->vram_height - 1;
	int data_x = (regs->scrollx + layer_x) & width_mask;
	int data_y = (regs->scrolly + layer_y) & height_mask;

	uint32_t split_pixel_offset = 0;
	if (layout->split_y)
	{
		split_pixel_offset = (uint32_t)(regs->scrolly & 0x100) * (uint32_t)layout->vram_width;
	}
	if (layout->split_x)
	{
		split_pixel_offset = (uint32_t)(regs->scrollx & 0x100) * (uint32_t)layout->vram_height;
	}

	uint32_t addr = split_pixel_offset + (uint32_t)data_x + ((uint32_t)data_y * (uint32_t)layout->vram_width);
	if (layout->is_8bit)
	{
		return vdp.bitmap[addr & 0x1FFFF];
	}

	addr >>= 1;
	uint8_t data = vdp.bitmap[addr & 0x1FFFF];
	return (data_x & 0x1) ? (uint8_t)(data & 0x0F) : (uint8_t)(data >> 4);
}

static void prepare_bitmap_line(int index, int y)
{
	memset(vdp.bitmap_linebuf[index], 0, VIDEO_DISPLAY_WIDTH);
	vdp.bitmap_linebuf_valid[index] = 0;

	if (!vdp.layer_ctrl.bitmap_enable[index])
	{
		return;
	}

	VDPBitmapRegs* regs = &vdp.bitmap_regs[index];
	int screen_y0 = sign9(regs->screeny);
	int layer_y = y - screen_y0;
	int end_y = regs->h & 0xFF;
	if (layer_y < 0 || layer_y > end_y || layer_y >= 256)
	{
		return;
	}

	BitmapLayout layout = get_bitmap_layout();
	uint8_t latch_max = layout.is_8bit ? 0xFF : 0x0F;
	uint8_t latch_threshold = (uint8_t)(regs->buffer_ctrl & (layout.is_8bit ? 0xFF : 0x0F));
	int latch_end = (regs->w & 0xFF) + 1;
	if (latch_end > 255) latch_end = 255;

	/* Hardware fetches one full 256-pixel bitmap line buffer independently of
	   on-screen X placement.  Color-latch processing is done on that buffer
	   before the line is emitted.  This keeps BM_POS signed without using the
	   old unsigned-position shortcut. */
	for (int layer_x = 0; layer_x < VIDEO_DISPLAY_WIDTH; layer_x++)
	{
		uint8_t data = read_bitmap_pixel(&layout, regs, layer_x, layer_y);

		if ((regs->buffer_ctrl & 0x100) && layer_x <= latch_end)
		{
			if (data == latch_max)
			{
				/* Documented hardware quirk: the last line-buffer pixel is not
				   replaced by the latched color. */
				if (layer_x != 0xFF)
				{
					data = regs->buffered_color;
				}
			}
			else if (data < latch_threshold)
			{
				regs->buffered_color = data;
			}
		}

		vdp.bitmap_linebuf[index][layer_x] = data;
	}
	vdp.bitmap_linebuf_valid[index] = 1;
}

static bool bitmap_layer_x_visible(const VDPBitmapRegs *regs, int layer_x)
{
	/* VDP.BM_WIDTH stores a layer-local inclusive window: STARTX in
	   bits 15..8 and ENDX in bits 7..0.  The hardware documentation
	   describes this as cropping the left and right edges to produce a
	   finite layer width; it is not a toroidal/window-wrap interval.
	   Retail code uses STARTX > ENDX as an empty mask while leaving the
	   bitmap layer enabled (Little Romance's BM1 uses STARTX=1, ENDX=0).
	   Treating that as a wraparound interval exposes one stale/prefetched
	   line at the top of the active area. */
	int clip_start = regs->clipx & 0xFF;
	int clip_end = regs->w & 0xFF;
	if (clip_start > clip_end)
	{
		return false;
	}
	return layer_x >= clip_start && layer_x <= clip_end;
}

static void draw_bitmap(int index, int y)
{
	if (!vdp.layer_ctrl.bitmap_enable[index] || !vdp.bitmap_linebuf_valid[index])
	{
		return;
	}

	VDPBitmapRegs* regs = &vdp.bitmap_regs[index];
	BitmapLayout layout = get_bitmap_layout();
	int screen_x0 = sign9(regs->screenx);
	int pair_index = index >> 1;
	int output_mode = vdp.layer_ctrl.bitmap_screen_mode[pair_index];

	for (int layer_x = 0; layer_x < VIDEO_DISPLAY_WIDTH; layer_x++)
	{
		uint8_t data = vdp.bitmap_linebuf[index][layer_x];
		if (!data || !bitmap_layer_x_visible(regs, layer_x))
		{
			continue;
		}

		uint8_t output = data;
		if (!layout.is_8bit)
		{
			int pal = (vdp.bitmap_palsel >> ((3 - index) * 4)) & 0xF;
			output |= pal << 4;
		}

		int screen_x = screen_x0 + layer_x;
		write_pal_color(vdp.bitmap_output[index], screen_x, y, output);
		if (output_mode & 0x1)
		{
			write_screen(1, screen_x, output);
		}
		if (output_mode & 0x2)
		{
			write_screen(0, screen_x, output);
		}
	}
}

static void draw_obj(int index, int screen_y)
{
	if (!vdp.layer_ctrl.obj_enable[index])
	{
		return;
	}

	//TODO: limit the maximum number of sprites per scanline

	//Tilemap info is only useful here to get the start of tile data
	TilemapInfo tilemap;
	get_tilemap_info(&tilemap);

	//OBJ #0 has highest priority, so the loop must be backwards
	for (int id = VIDEO_OBJ_COUNT - 1; id >= 0; id--)
	{
		int test_id = (id - vdp.obj_ctrl.id_offs) & 0xFF;
		if (index == 0 && test_id >= VIDEO_OBJ_COUNT)
		{
			continue;
		}

		if (index == 1 && test_id < VIDEO_OBJ_COUNT)
		{
			continue;
		}

		uint32_t descriptor;
		memcpy(&descriptor, vdp.oam + (id * 4), 4);
		descriptor = common_bswp32(descriptor);

		int tile_size = (descriptor >> 10) & 0x3;

		int obj_width = 0, obj_height = 0;
		switch (tile_size)
		{
		case 0x00:
			obj_width = 8;
			obj_height = 8;
			break;
		case 0x01:
			obj_width = 16;
			obj_height = 16;
			break;
		case 0x02:
			obj_width = 16;
			obj_height = 32;
			break;
		case 0x03:
			obj_width = 32;
			obj_height = 32;
			break;
		default:
			assert(0);
		}

		int start_y = (descriptor >> 16) & 0xFF;
		bool high_y = (descriptor >> 9) & 0x1;

		start_y |= high_y << 8;
		start_y = sign9((uint16_t)start_y);

		/* OAM positions are signed 9-bit screen coordinates.  They clip at
		   the active area edges; they do not wrap around the framebuffer.
		   The previous renderer kept the raw 9-bit value and wrapped with
		   &0x1FF, which let sprites parked just below the active area leak
		   into the first scanlines in Little Romance. */
		if (screen_y < start_y || screen_y >= start_y + obj_height)
		{
			continue;
		}

		int start_x = sign9((uint16_t)(descriptor & 0x1FF));
		int end_x = start_x + obj_width;
		if (end_x <= 0 || start_x >= VIDEO_DISPLAY_WIDTH)
		{
			continue;
		}

		int draw_x0 = start_x < 0 ? 0 : start_x;
		int draw_x1 = end_x > VIDEO_DISPLAY_WIDTH ? VIDEO_DISPLAY_WIDTH : end_x;
		for (int screen_x = draw_x0; screen_x < draw_x1; screen_x++)
		{

			bool x_flip = (descriptor >> 14) & 0x1;
			bool y_flip = (descriptor >> 15) & 0x1;

			int tile_x = (screen_x - start_x) & (obj_width - 1);
			if (x_flip)
			{
				tile_x = obj_width - 1 - tile_x;
			}

			int tile_y = (screen_y - start_y) & (obj_height - 1);
			if (y_flip)
			{
				tile_y = obj_height - 1 - tile_y;
			}

			int tile_index = descriptor >> 24;
			tile_index += tile_y & ~0x7;
			tile_index += tile_x >> 3;
			tile_index += vdp.obj_ctrl.tile_index_offs[index] << 8;
			uint32_t offs = (tile_x & 0x7) + ((tile_y & 0x7) * 0x08) + (tile_index << 6);

			uint8_t tile_data;
			if (vdp.obj_ctrl.is_8bit)
			{
				tile_data = vdp.tile[(tilemap.data_start + offs) & 0xFFFF];
			}
			else
			{
				offs >>= 1;
				offs += vdp.tilebase << 9;
				tile_data = vdp.tile[(tilemap.data_start + offs) & 0xFFFF];
				if (tile_x & 0x1)
				{
					tile_data &= 0xF;
				}
				else
				{
					tile_data >>= 4;
				}
			}

			if (!tile_data)
			{
				continue;
			}

			uint8_t output = tile_data;
			if (!vdp.obj_ctrl.is_8bit)
			{
				uint16_t palsel = vdp.obj_palsel[index];
				int pal_descriptor = (descriptor >> 12) & 0x3;
				int pal = (palsel >> (pal_descriptor * 4)) & 0xF;
				output |= pal << 4;
			}

			write_pal_color(vdp.obj_output[index], screen_x, screen_y, output);
			int output_mode = vdp.layer_ctrl.obj_screen_mode[index];
			if (output_mode & 0x1)
			{
				write_screen(1, screen_x, output);
			}

			if (output_mode & 0x2)
			{
				write_screen(0, screen_x, output);
			}
		}
	}
}

typedef enum RenderLayer
{
	RENDER_BG0,
	RENDER_BG1,
	RENDER_BM0,
	RENDER_BM1,
	RENDER_BM2,
	RENDER_BM3,
	RENDER_OBJ0,
	RENDER_OBJ1
} RenderLayer;

/* VDP.SCREEN_CTRL priority modes, compacted from the documented 12-slot
   table.  Entries are front-to-back; draw_layers() walks them in reverse. */
static const RenderLayer priority_order[16][8] =
{
	{ RENDER_OBJ0, RENDER_OBJ1, RENDER_BM0, RENDER_BM1, RENDER_BM2, RENDER_BM3, RENDER_BG0, RENDER_BG1 },
	{ RENDER_OBJ0, RENDER_OBJ1, RENDER_BM2, RENDER_BM3, RENDER_BM0, RENDER_BM1, RENDER_BG0, RENDER_BG1 },
	{ RENDER_OBJ0, RENDER_OBJ1, RENDER_BG0, RENDER_BM0, RENDER_BM1, RENDER_BM2, RENDER_BM3, RENDER_BG1 },
	{ RENDER_OBJ0, RENDER_OBJ1, RENDER_BG0, RENDER_BM2, RENDER_BM3, RENDER_BM0, RENDER_BM1, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BM0, RENDER_BM1, RENDER_OBJ0, RENDER_BM2, RENDER_BM3, RENDER_BG0, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BM2, RENDER_BM3, RENDER_OBJ0, RENDER_BM0, RENDER_BM1, RENDER_BG0, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BG0, RENDER_BM0, RENDER_BM1, RENDER_OBJ0, RENDER_BM2, RENDER_BM3, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BG0, RENDER_BM2, RENDER_BM3, RENDER_OBJ0, RENDER_BM0, RENDER_BM1, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BM0, RENDER_BM1, RENDER_BM2, RENDER_BM3, RENDER_OBJ0, RENDER_BG0, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BM2, RENDER_BM3, RENDER_BM0, RENDER_BM1, RENDER_OBJ0, RENDER_BG0, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BG0, RENDER_BM0, RENDER_BM1, RENDER_BM2, RENDER_BM3, RENDER_OBJ0, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BG0, RENDER_BM2, RENDER_BM3, RENDER_BM0, RENDER_BM1, RENDER_OBJ0, RENDER_BG1 },
	{ RENDER_OBJ1, RENDER_BM0, RENDER_BM1, RENDER_BM2, RENDER_BM3, RENDER_BG0, RENDER_BG1, RENDER_OBJ0 },
	{ RENDER_OBJ1, RENDER_BM2, RENDER_BM3, RENDER_BM0, RENDER_BM1, RENDER_BG0, RENDER_BG1, RENDER_OBJ0 },
	{ RENDER_OBJ1, RENDER_BG0, RENDER_BM0, RENDER_BM1, RENDER_BM2, RENDER_BM3, RENDER_BG1, RENDER_OBJ0 },
	{ RENDER_OBJ1, RENDER_BG0, RENDER_BM2, RENDER_BM3, RENDER_BM0, RENDER_BM1, RENDER_BG1, RENDER_OBJ0 },
};

static void draw_layer(RenderLayer layer, int y)
{
	switch (layer)
	{
	case RENDER_BG0: draw_bg(0, y); break;
	case RENDER_BG1: draw_bg(1, y); break;
	case RENDER_BM0: draw_bitmap(0, y); break;
	case RENDER_BM1: draw_bitmap(1, y); break;
	case RENDER_BM2: draw_bitmap(2, y); break;
	case RENDER_BM3: draw_bitmap(3, y); break;
	case RENDER_OBJ0: draw_obj(0, y); break;
	case RENDER_OBJ1: draw_obj(1, y); break;
	}
}

static void draw_layers(int y)
{
	const RenderLayer *order = priority_order[vdp.color_prio.prio_mode & 0xF];
	for (int i = 7; i >= 0; i--)
	{
		draw_layer(order[i], y);
	}
}

static void draw_color_math(int y, bool half)
{
	for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
	{
		uint16_t input_a = 0, input_b = 0;
		if (vdp.color_prio.output_screen_a)
		{
			input_a = read_screen(0, x);
		}

		if (vdp.color_prio.output_screen_b)
		{
			input_b = read_screen(1, x);
		}

		int a_r = (input_a >> 10) & 0x1F;
		int a_g = (input_a >> 5) & 0x1F;
		int a_b = input_a & 0x1F;

		int b_r = (input_b >> 10) & 0x1F;
		int b_g = (input_b >> 5) & 0x1F;
		int b_b = input_b & 0x1F;

		int out_r, out_g, out_b;

		if (vdp.color_prio.blend_mode)
		{
			//Subtractive blending
			out_r = a_r - b_r;
			out_g = a_g - b_g;
			out_b = a_b - b_b;
		}
		else
		{
			//Additive blending
			out_r = a_r + b_r;
			out_g = a_g + b_g;
			out_b = a_b + b_b;
		}

		if (half)
		{
			out_r >>= 1;
			out_g >>= 1;
			out_b >>= 1;
		}

		out_r = clamp_int(out_r, 0, 0x1F);
		out_g = clamp_int(out_g, 0, 0x1F);
		out_b = clamp_int(out_b, 0, 0x1F);

		uint16_t output = (out_r << 10) | (out_g << 5) | out_b;
		write_color(vdp.display_output, x, y, output);
	}
}

static void draw_screen_overlay(int y, bool screen_b_prio)
{
	for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
	{
		uint16_t output;
		if (screen_b_prio)
		{
			/* Mode 4: screen B over screen A.  Screen B's backdrop is
			   transparent in this mode; when SBCOL is set, B outputs only its
			   backdrop, so it must not be used as an opaque top layer. */
			output = vdp.color_prio.output_screen_a ? read_screen(0, x) : 0;
			if (vdp.color_prio.output_screen_b &&
				!vdp.color_prio.screen_b_backdrop_only &&
				vdp.screens[1][x])
			{
				output = read_screen(1, x);
			}
		}
		else
		{
			/* Mode 5: screen A over screen B, with screen A's backdrop
			   transparent.  Screen B remains a normal bottom screen. */
			output = vdp.color_prio.output_screen_b ? read_screen(1, x) : 0;
			if (vdp.color_prio.output_screen_a && vdp.screens[0][x])
			{
				output = read_screen(0, x);
			}
		}

		write_color(vdp.display_output, x, y, output);
	}
}

static void draw_single_screen(int y, int screen)
{
	bool enabled = screen ? vdp.color_prio.output_screen_b : vdp.color_prio.output_screen_a;
	for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
	{
		write_color(vdp.display_output, x, y, enabled ? read_screen(screen, x) : 0);
	}
}

static uint16_t average_rgb555(uint16_t a, uint16_t b)
{
	int ar = (a >> 10) & 0x1F;
	int ag = (a >> 5) & 0x1F;
	int ab = a & 0x1F;
	int br = (b >> 10) & 0x1F;
	int bg = (b >> 5) & 0x1F;
	int bb = b & 0x1F;
	return (uint16_t)((((ar + br) >> 1) << 10) | (((ag + bg) >> 1) << 5) | ((ab + bb) >> 1));
}

static void draw_hires_approx(int y)
{
	/* Hardware interleaves screen A and B at half-pixel width.  The renderer is
	   256 pixels wide, so approximate the 512-pixel output by resolving each
	   pair into a single RGB555 sample instead of falling into the invalid-mode
	   assert path. */
	for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
	{
		uint16_t input_a = vdp.color_prio.output_screen_a ? read_screen(0, x) : 0;
		uint16_t input_b = vdp.color_prio.output_screen_b ? read_screen(1, x) : 0;
		write_color(vdp.display_output, x, y, average_rgb555(input_a, input_b));
	}
}

static void draw_black_scanline(int y)
{
	for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
	{
		write_color(vdp.display_output, x, y, 0);
	}
}

static void write_capture_rgb555(int x, uint16_t color)
{
	uint16_t be = common_bswp16(color);
	memcpy(&vdp.capture_buffer[x * 2], &be, sizeof(be));
}

static void display_capture(int y)
{
    const uint16_t *printer_line = NULL;
	switch (vdp.capture_ctrl.format)
	{
	case 0x00:
		// Capture blended output as raw RGB555.
		for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
		{
			write_capture_rgb555(x, vdp.display_output[y * VIDEO_DISPLAY_WIDTH + x]);
		}
		printer_line = &vdp.display_output[y * VIDEO_DISPLAY_WIDTH];
		break;
	case 0x01:
		// Capture screen A as raw RGB555.
		for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
		{
			write_capture_rgb555(x, vdp.screen_output[0][y * VIDEO_DISPLAY_WIDTH + x]);
		}
		printer_line = &vdp.screen_output[0][y * VIDEO_DISPLAY_WIDTH];
		break;
	case 0x02:
	case 0x03:
		// Capture screen A before applying the palette.
		memcpy(vdp.capture_buffer, vdp.screens[0], VIDEO_DISPLAY_WIDTH * sizeof(uint8_t));
		memset(vdp.capture_buffer + VIDEO_DISPLAY_WIDTH, 0, VIDEO_CAPTURE_SIZE - VIDEO_DISPLAY_WIDTH);
		printer_line = &vdp.screen_output[0][y * VIDEO_DISPLAY_WIDTH];
		break;
	default:
		break;
	}

    if (printer_line) {
        loopy_io_printer_capture_scanline((uint16_t)y, printer_line, VIDEO_DISPLAY_WIDTH,
                                          (uint8_t)vdp.capture_ctrl.format);
    }
}

void video_renderer_draw_scanline(int y)
{
	if (y == 0)
	{
		fill_224_mode_vertical_borders();
	}

	//Set both screens to the backdrop color
	memset(vdp.screens, 0, sizeof(vdp.screens));

	for (int i = 0; i < 4; i++)
	{
		prepare_bitmap_line(i, y);
	}

	draw_layers(y);

	//Fetch the screen colors
	for (int x = 0; x < VIDEO_DISPLAY_WIDTH; x++)
	{
		uint16_t color = read_screen(0, x);
		write_color(vdp.screen_output[0], x, y, color);

		color = read_screen(1, x);
		write_color(vdp.screen_output[1], x, y, color);
	}

	//Draw the screens to the display output buffer
	switch (vdp.dispmode)
	{
	case 0x00:
		draw_color_math(y, false);
		break;
	case 0x01:
		draw_color_math(y, true);
		break;
	case 0x02:
		draw_single_screen(y, 0);
		break;
	case 0x03:
		draw_hires_approx(y);
		break;
	case 0x04:
		draw_screen_overlay(y, true);
		break;
	case 0x05:
		draw_screen_overlay(y, false);
		break;
	default:
		draw_black_scanline(y);
		break;
	}

	if (vdp.capture_enable && y == vdp.capture_ctrl.scanline)
	{
		display_capture(y);
		vdp.capture_enable = false;
	}
}
