#ifndef LOOPY_SYSTEM_H
#define LOOPY_SYSTEM_H
#include <stdint.h>
#include "core/config.h"
void system_initialize(const ConfigSystemInfo *config);
void system_shutdown(void);
void system_run(void);
uint16_t *system_get_display_output(void);
int system_save_state(const char *path);
int system_load_state(const char *path);
uint32_t system_state_blob_size(void);
int system_save_state_to_buffer(void *dst, uint32_t size);
int system_load_state_from_buffer(const void *src, uint32_t size);
#endif
