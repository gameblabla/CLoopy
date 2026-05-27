#ifndef LOOPY_SOUND_H
#define LOOPY_SOUND_H
#include <stdbool.h>
#include <stdint.h>
#include "core/config.h"

#define SOUND_TARGET_SAMPLE_RATE 48000
#define SOUND_TARGET_BUFFER_SIZE 2048
#define SOUND_TIMEREF_FREQUENCY 100
#define SOUND_TIMEREF_ENABLE (SOUND_TIMEREF_FREQUENCY > (SOUND_TARGET_SAMPLE_RATE / SOUND_TARGET_BUFFER_SIZE))
#define SOUND_MUTE_FADE_MS 20
#define SOUND_CTRL_START 0x04080000u
#define SOUND_CTRL_END 0x040A0000u

void sound_initialize(const ByteBuffer *sound_rom);
void sound_shutdown(void);
uint8_t sound_ctrl_read8(uint32_t addr);
uint16_t sound_ctrl_read16(uint32_t addr);
uint32_t sound_ctrl_read32(uint32_t addr);
void sound_ctrl_write8(uint32_t addr, uint8_t value);
void sound_ctrl_write16(uint32_t addr, uint16_t value);
void sound_ctrl_write32(uint32_t addr, uint32_t value);
void sound_midi_byte_in(uint8_t value);
void sound_set_mute(bool mute_in);
void sound_set_paused(bool pause_in);
void sound_wasm_generate_i16(int16_t *out, int frames);
uint32_t sound_state_blob_size(void);
void sound_get_state_blob(void *dst, uint32_t size);
void sound_set_state_blob(const void *src, uint32_t size);

#endif
