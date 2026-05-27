#ifndef LOOPY_SH7021_SERIAL_H
#define LOOPY_SH7021_SERIAL_H
#include <stdint.h>

typedef void (*SerialTxCallback)(uint8_t value);
void sh7021_ocpm_serial_initialize(void);
uint8_t sh7021_ocpm_serial_read8(uint32_t addr);
void sh7021_ocpm_serial_write8(uint32_t addr, uint8_t value);
void sh7021_ocpm_serial_set_tx_callback(int port, SerialTxCallback callback);

uint32_t sh7021_ocpm_serial_state_blob_size(void);
void sh7021_ocpm_serial_get_state_blob(void *dst, uint32_t size);
void sh7021_ocpm_serial_set_state_blob(const void *src, uint32_t size);

#endif
