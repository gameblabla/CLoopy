/*
 * OKI MSM6653A / MSM6376-style ADPCM support for CLoopy.
 *
 * The ADPCM decoder tables, phrase playback model, and CPU-control
 * behavior are based on MAME's OKI MSM6376/MSM6650 sound device
 * implementation:
 *
 *   MAME src/devices/sound/okim6376.cpp
 *   MAME src/devices/sound/okim6376.h
 *   license: BSD-3-Clause
 *   copyright-holders: Mirko Buffoni, James Wallace
 *
 * This is a C11 adaptation integrated with CLoopy's cartridge audio path.
 */
#ifndef LOOPY_OKI_ADPCM_H
#define LOOPY_OKI_ADPCM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct OkiAdpcm OkiAdpcm;

OkiAdpcm *oki_adpcm_create(float output_rate, const void *external_rom, uint32_t external_rom_size, int use_wanwan_replacement);
int oki_adpcm_set_external_rom_data(const void *data, uint32_t size);
void oki_adpcm_clear_external_rom_data(void);
void oki_adpcm_destroy(OkiAdpcm *o);
void oki_adpcm_reset(OkiAdpcm *o);
void oki_adpcm_write_data(OkiAdpcm *o, uint8_t data);
void oki_adpcm_set_st(OkiAdpcm *o, int state);
void oki_adpcm_set_ch(OkiAdpcm *o, int state);
void oki_adpcm_set_reset(OkiAdpcm *o, int state);
int oki_adpcm_nar(const OkiAdpcm *o);
int oki_adpcm_busy(const OkiAdpcm *o);
float oki_adpcm_generate(OkiAdpcm *o);
void oki_adpcm_debug_play_command(OkiAdpcm *o, uint8_t command);
int oki_adpcm_debug_active(const OkiAdpcm *o);
uint32_t oki_adpcm_state_blob_size(void);
void oki_adpcm_get_state_blob(const OkiAdpcm *o, void *dst, uint32_t size);
void oki_adpcm_set_state_blob(OkiAdpcm *o, const void *src, uint32_t size);

#endif
