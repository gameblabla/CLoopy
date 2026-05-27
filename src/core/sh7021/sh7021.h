#ifndef LOOPY_SH7021_H
#include <stdint.h>
#define LOOPY_SH7021_H
void sh7021_initialize(void);
void sh7021_shutdown(void);
void sh7021_run(void);
uint32_t sh7021_state_blob_size(void);
void sh7021_get_state_blob(void *dst, uint32_t size);
void sh7021_set_state_blob(const void *src, uint32_t size);
#endif
