#ifndef LOOPY_SH7021_DMAC_H
#define LOOPY_SH7021_DMAC_H
#include <stdint.h>
#include <stdbool.h>

typedef enum DREQ {
    DREQ_External,
    DREQ_Reserved,
    DREQ_External2,
    DREQ_External3,
    DREQ_RXI0,
    DREQ_TXI0,
    DREQ_RXI1,
    DREQ_TXI1,
    DREQ_IMIA0,
    DREQ_IMIA1,
    DREQ_IMIA2,
    DREQ_IMIA3,
    DREQ_Auto,
    DREQ_Reserved2,
    DREQ_Reserved3,
    DREQ_Reserved4,
    DREQ_NumDreq
} DREQ;

void sh7021_ocpm_dmac_initialize(void);
void sh7021_ocpm_dmac_send_dreq(DREQ dreq);
void sh7021_ocpm_dmac_clear_dreq(DREQ dreq);
void sh7021_ocpm_dmac_pulse_dreq(DREQ dreq);
uint8_t sh7021_ocpm_dmac_read8(uint32_t addr);
uint16_t sh7021_ocpm_dmac_read16(uint32_t addr);
uint32_t sh7021_ocpm_dmac_read32(uint32_t addr);
void sh7021_ocpm_dmac_write8(uint32_t addr, uint8_t value);
void sh7021_ocpm_dmac_write16(uint32_t addr, uint16_t value);
void sh7021_ocpm_dmac_write32(uint32_t addr, uint32_t value);

uint32_t sh7021_ocpm_dmac_state_blob_size(void);
void sh7021_ocpm_dmac_get_state_blob(void *dst, uint32_t size);
void sh7021_ocpm_dmac_set_state_blob(const void *src, uint32_t size);

#endif
