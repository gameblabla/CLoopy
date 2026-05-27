#ifndef LOOPY_LOOPYSOUND_H
#define LOOPY_LOOPYSOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LoopySoundEngine LoopySoundEngine;

LoopySoundEngine *loopy_sound_engine_create(const uint8_t *rom, size_t rom_size, float out_rate, int buffer_size);
void loopy_sound_engine_destroy(LoopySoundEngine *engine);
void loopy_sound_engine_gen_sample(LoopySoundEngine *engine, float out[2]);
void loopy_sound_engine_set_channel_muted(LoopySoundEngine *engine, int channel, bool mute);
void loopy_sound_engine_time_reference(LoopySoundEngine *engine, float delta);
void loopy_sound_engine_set_control_register(LoopySoundEngine *engine, int creg);
bool loopy_sound_engine_midi_in(LoopySoundEngine *engine, char b);
uint32_t loopy_sound_engine_state_blob_size(void);
void loopy_sound_engine_get_state_blob(LoopySoundEngine *engine, void *dst, uint32_t size);
void loopy_sound_engine_set_state_blob(LoopySoundEngine *engine, const void *src, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
