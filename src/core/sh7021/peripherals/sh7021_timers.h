#ifndef LOOPY_SH7021_TIMERS_H
#define LOOPY_SH7021_TIMERS_H
#include <stdint.h>
void sh7021_ocpm_timer_initialize(void);
uint8_t sh7021_ocpm_timer_read8(uint32_t addr);
uint16_t sh7021_ocpm_timer_read16(uint32_t addr);
void sh7021_ocpm_timer_write8(uint32_t addr, uint8_t value);
void sh7021_ocpm_timer_write16(uint32_t addr, uint16_t value);
uint32_t sh7021_ocpm_timer_state_blob_size(void);
void sh7021_ocpm_timer_get_state_blob(void *dst, uint32_t size);
void sh7021_ocpm_timer_set_state_blob(const void *src, uint32_t size);

#endif
