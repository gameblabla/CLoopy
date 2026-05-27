#ifndef LOOPY_IO_H
#define LOOPY_IO_H
#include <stdbool.h>
#include <stdint.h>

#define LOOPY_IO_BASE_ADDR 0x0405D000u
#define LOOPY_IO_END_ADDR 0x0405E000u

void loopy_io_initialize(void);
void loopy_io_shutdown(void);
uint8_t loopy_io_reg_read8(uint32_t addr);
uint16_t loopy_io_reg_read16(uint32_t addr);
uint32_t loopy_io_reg_read32(uint32_t addr);
void loopy_io_reg_write8(uint32_t addr, uint8_t value);
void loopy_io_reg_write16(uint32_t addr, uint16_t value);
void loopy_io_reg_write32(uint32_t addr, uint32_t value);
void loopy_io_update_pad(int key_info, bool pressed);
void loopy_io_set_controller_mode(bool matrix_mode, bool mouse_counter_enable);
void loopy_io_matrix_scan_vcount(uint16_t vcount);
void loopy_io_controller_debug_reset(void);
void loopy_io_controller_debug_get(unsigned index, unsigned *reads, uint16_t *last_value);
void loopy_io_set_mouse_connected(bool connected);
void loopy_io_set_mouse_button(int button, bool pressed);
void loopy_io_add_mouse_delta(int dx, int dy);
void loopy_io_trigger_adc(void);
void loopy_io_trigger_printer_sensors(void);
void loopy_io_printer_capture_scanline(uint16_t y, const uint16_t *rgb555, uint16_t width, uint8_t source_format);
void loopy_io_printer_frame_snapshot(const uint16_t *rgb555, uint16_t width, uint16_t height);
void loopy_io_printer_write_source_image(const uint16_t *rgb555, uint16_t width, uint16_t height, const char *source);
void loopy_io_printer_set_output_dir(const char *path);
void loopy_io_printer_set_trace(bool enabled);
int loopy_io_printer_has_pending_image(void);
uint32_t loopy_io_printer_pending_width(void);
uint32_t loopy_io_printer_pending_height(void);
const uint16_t *loopy_io_printer_pending_pixels(void);
void loopy_io_printer_clear_pending_image(void);
void loopy_io_get_state_blob(void *dst, uint32_t size);
void loopy_io_set_state_blob(const void *src, uint32_t size);
uint32_t loopy_io_state_blob_size(void);

#endif
