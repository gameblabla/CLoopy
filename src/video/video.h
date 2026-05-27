#ifndef LOOPY_VIDEO_H
#define LOOPY_VIDEO_H
#include <stdbool.h>
#include <stdint.h>


#define VIDEO_DISPLAY_WIDTH 0x100

//Output is always 240 lines tall, even in 224-line mode
#define VIDEO_DISPLAY_HEIGHT 0xF0

#define VIDEO_BITMAP_VRAM_START 0x04000000
#define VIDEO_BITMAP_VRAM_SIZE 0x20000
#define VIDEO_BITMAP_VRAM_END VIDEO_BITMAP_VRAM_START + VIDEO_BITMAP_VRAM_SIZE

#define VIDEO_TILE_VRAM_START 0x04040000
#define VIDEO_TILE_VRAM_SIZE 0x10000
#define VIDEO_TILE_VRAM_END VIDEO_TILE_VRAM_START + VIDEO_TILE_VRAM_SIZE

#define VIDEO_OAM_START 0x04050000
#define VIDEO_OAM_SIZE 0x200
#define VIDEO_OAM_END VIDEO_OAM_START + VIDEO_OAM_SIZE

#define VIDEO_PALETTE_START 0x04051000
#define VIDEO_PALETTE_SIZE 0x200
#define VIDEO_PALETTE_END VIDEO_PALETTE_START + VIDEO_PALETTE_SIZE

#define VIDEO_CAPTURE_START 0x04052000
#define VIDEO_CAPTURE_SIZE 0x200
#define VIDEO_CAPTURE_END VIDEO_CAPTURE_START + VIDEO_CAPTURE_SIZE

#define VIDEO_CTRL_REG_START 0x04058000
#define VIDEO_CTRL_REG_END 0x04059000

#define VIDEO_BITMAP_REG_START 0x04059000
#define VIDEO_BITMAP_REG_END 0x0405A000

#define VIDEO_BGOBJ_REG_START 0x0405A000
#define VIDEO_BGOBJ_REG_END 0x0405B000

#define VIDEO_DISPLAY_REG_START 0x0405B000
#define VIDEO_DISPLAY_REG_END 0x0405C000

#define VIDEO_IRQ_REG_START 0x0405C000
#define VIDEO_IRQ_REG_END 0x0405D000

#define VIDEO_DMA_CTRL_START 0x0405E000
#define VIDEO_DMA_CTRL_END 0x0405F000

#define VIDEO_DMA_START 0x0405F000
#define VIDEO_DMA_END 0x04060000

#define VIDEO_VDP_TRANSLATED_START 0x04000000u
#define VIDEO_VDP_TRANSLATED_END   0x04060000u

#define VIDEO_VDP_AREA_LOW_MIRROR  0x04000000u
#define VIDEO_VDP_AREA_NORMAL      0x0C000000u

#define VIDEO_OBJ_COUNT 128

void video_initialize();
void video_shutdown();

void video_start_frame();
bool video_check_frame_end();

uint16_t* video_get_display_output();
int video_get_display_active_height(void);
int video_get_display_active_y_offset(void);

void video_dump_for_serial();
uint32_t video_state_blob_size(void);
void video_get_state_blob(void *dst, uint32_t size);
void video_set_state_blob(const void *src, uint32_t size);
void video_set_bmp_dump_enabled(bool enabled);

bool video_bus_is_vdp_addr(uint32_t raw_addr, uint32_t translated_addr);
int video_bus_wait_cycles(uint32_t raw_addr, uint32_t translated_addr, int bytes, bool write);
uint8_t video_bus_read8(uint32_t raw_addr);
uint16_t video_bus_read16(uint32_t raw_addr);
uint32_t video_bus_read32(uint32_t raw_addr);
void video_bus_write8(uint32_t raw_addr, uint8_t value);
void video_bus_write16(uint32_t raw_addr, uint16_t value);
void video_bus_write32(uint32_t raw_addr, uint32_t value);

//TODO: should these MMIO accessors be moved to a different file?
uint8_t video_palette_read8(uint32_t addr);
uint16_t video_palette_read16(uint32_t addr);
uint32_t video_palette_read32(uint32_t addr);

void video_palette_write8(uint32_t addr, uint8_t value);
void video_palette_write16(uint32_t addr, uint16_t value);
void video_palette_write32(uint32_t addr, uint32_t value);

uint8_t video_oam_read8(uint32_t addr);
uint16_t video_oam_read16(uint32_t addr);
uint32_t video_oam_read32(uint32_t addr);

void video_oam_write8(uint32_t addr, uint8_t value);
void video_oam_write16(uint32_t addr, uint16_t value);
void video_oam_write32(uint32_t addr, uint32_t value);

uint8_t video_capture_read8(uint32_t addr);
uint16_t video_capture_read16(uint32_t addr);
uint32_t video_capture_read32(uint32_t addr);

void video_capture_write8(uint32_t addr, uint8_t value);
void video_capture_write16(uint32_t addr, uint16_t value);
void video_capture_write32(uint32_t addr, uint32_t value);

uint8_t video_ctrl_read8(uint32_t addr);
uint16_t video_ctrl_read16(uint32_t addr);
uint32_t video_ctrl_read32(uint32_t addr);

void video_ctrl_write8(uint32_t addr, uint8_t value);
void video_ctrl_write16(uint32_t addr, uint16_t value);
void video_ctrl_write32(uint32_t addr, uint32_t value);

uint8_t video_bitmap_reg_read8(uint32_t addr);
uint16_t video_bitmap_reg_read16(uint32_t addr);
uint32_t video_bitmap_reg_read32(uint32_t addr);

void video_bitmap_reg_write8(uint32_t addr, uint8_t value);
void video_bitmap_reg_write16(uint32_t addr, uint16_t value);
void video_bitmap_reg_write32(uint32_t addr, uint32_t value);

uint8_t video_bgobj_read8(uint32_t addr);
uint16_t video_bgobj_read16(uint32_t addr);
uint32_t video_bgobj_read32(uint32_t addr);

void video_bgobj_write8(uint32_t addr, uint8_t value);
void video_bgobj_write16(uint32_t addr, uint16_t value);
void video_bgobj_write32(uint32_t addr, uint32_t value);

uint8_t video_display_read8(uint32_t addr);
uint16_t video_display_read16(uint32_t addr);
uint32_t video_display_read32(uint32_t addr);

void video_display_write8(uint32_t addr, uint8_t value);
void video_display_write16(uint32_t addr, uint16_t value);
void video_display_write32(uint32_t addr, uint32_t value);

uint8_t video_irq_read8(uint32_t addr);
uint16_t video_irq_read16(uint32_t addr);
uint32_t video_irq_read32(uint32_t addr);

void video_irq_write8(uint32_t addr, uint8_t value);
void video_irq_write16(uint32_t addr, uint16_t value);
void video_irq_write32(uint32_t addr, uint32_t value);

uint8_t video_dma_ctrl_read8(uint32_t addr);
uint16_t video_dma_ctrl_read16(uint32_t addr);
uint32_t video_dma_ctrl_read32(uint32_t addr);

void video_dma_ctrl_write8(uint32_t addr, uint8_t value);
void video_dma_ctrl_write16(uint32_t addr, uint16_t value);
void video_dma_ctrl_write32(uint32_t addr, uint32_t value);

uint8_t video_dma_read8(uint32_t addr);
uint16_t video_dma_read16(uint32_t addr);
uint32_t video_dma_read32(uint32_t addr);

void video_dma_write8(uint32_t addr, uint8_t value);
void video_dma_write16(uint32_t addr, uint16_t value);
void video_dma_write32(uint32_t addr, uint32_t value);

#endif
