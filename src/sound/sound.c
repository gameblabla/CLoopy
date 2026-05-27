#include "sound/sound.h"
#include "sound/loopysound.h"
#include "core/timing.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LOOPY_SDL3_FRONTEND
#include <SDL3/SDL.h>
#endif

static bool sound_muted;
static bool sound_paused;
static bool sound_rom_present;
static uint16_t last_control_register;
static LoopySoundEngine *sound_engine;

#if defined(LOOPY_SDL3_FRONTEND) || defined(LOOPY_WASM_FRONTEND)
static TimingFuncHandle timeref_func;
static TimingEventHandle timeref_ev;
static int sample_rate = SOUND_TARGET_SAMPLE_RATE;
static int buffer_size = SOUND_TARGET_BUFFER_SIZE;
static float volume_level = 1.0f;
#endif

#ifdef LOOPY_SDL3_FRONTEND
static SDL_AudioStream *audio_stream;
#endif

#if defined(LOOPY_SDL3_FRONTEND) || defined(LOOPY_WASM_FRONTEND)
static void update_volume_level(void) {
    if (SOUND_MUTE_FADE_MS > 0) {
        float delta = 1000.0f / ((float)sample_rate * (float)SOUND_MUTE_FADE_MS);
        if (sound_muted) delta = -delta;
        volume_level += delta;
        if (volume_level < 0.0f) volume_level = 0.0f;
        if (volume_level > 1.0f) volume_level = 1.0f;
    } else {
        volume_level = sound_muted ? 0.0f : 1.0f;
    }
}

static void sound_generate(float *buffer, int float_count) {
    if (!buffer || float_count <= 0) return;
    if (sound_paused) {
        for (int i = 0; i < float_count; i++) buffer[i] = 0.0f;
        volume_level = 0.0f;
        return;
    }
    if (sound_engine) {
        float tmp[2];
        int p = 0;
        int frames = float_count / 2;
        for (int i = 0; i < frames; i++) {
            update_volume_level();
            loopy_sound_engine_gen_sample(sound_engine, tmp);
            buffer[p++] = tmp[0] * volume_level;
            buffer[p++] = tmp[1] * volume_level;
        }
        if (p < float_count) buffer[p] = 0.0f;
    } else {
        for (int i = 0; i < float_count; i++) buffer[i] = 0.0f;
    }
}
#endif

#ifdef LOOPY_SDL3_FRONTEND
static void SDLCALL sdl_audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    (void)userdata;
    (void)total_amount;
    if (additional_amount <= 0) return;
    int frames = additional_amount / ((int)sizeof(float) * 2);
    if (frames < 1) frames = 1;
    if (frames > 4096) frames = 4096;
    int float_count = frames * 2;
    float *tmp = (float *)malloc((size_t)float_count * sizeof(float));
    if (!tmp) return;
    sound_generate(tmp, float_count);
    SDL_PutAudioStreamData(stream, tmp, float_count * (int)sizeof(float));
    free(tmp);
}

static bool sdl_audio_initialize(const ByteBuffer *sound_rom) {
    if (!sound_rom || !sound_rom->data || !sound_rom->size) return false;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        printf("[Sound] SDL3 audio unavailable: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = SOUND_TARGET_SAMPLE_RATE;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    sample_rate = spec.freq;
    buffer_size = SOUND_TARGET_BUFFER_SIZE;
    sound_engine = loopy_sound_engine_create(sound_rom->data, sound_rom->size, (float)sample_rate, buffer_size);
    if (!sound_engine) {
        printf("[Sound] Failed to initialize uPD937 engine\n");
        return false;
    }
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_audio_callback, NULL);
    if (!audio_stream) {
        printf("[Sound] No SDL3 audio device available: %s\n", SDL_GetError());
        loopy_sound_engine_destroy(sound_engine);
        sound_engine = NULL;
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(audio_stream)) {
        printf("[Sound] Failed to resume SDL3 audio stream: %s\n", SDL_GetError());
    }
    printf("[Sound] SDL3 audio enabled: %d Hz, stereo float\n", sample_rate);
    return true;
}

static void sdl_audio_shutdown(void) {
    if (audio_stream) {
        SDL_DestroyAudioStream(audio_stream);
        audio_stream = NULL;
    }
}
#else
static void sdl_audio_shutdown(void) { }
#endif

#if defined(LOOPY_SDL3_FRONTEND) || defined(LOOPY_WASM_FRONTEND)
static void sound_timeref(uint64_t param, int cycles_late) {
    (void)param;
    const int cycles_per_timeref = TIMING_F_CPU / SOUND_TIMEREF_FREQUENCY;
    TimingUnitCycle timeref_cycles = timing_convert_cpu(cycles_per_timeref - cycles_late);
    timeref_ev = timing_add_event(timeref_func, timeref_cycles, 0, TIMING_CPU_TIMER);
    if (sound_engine) {
        const float timeref_period = 1.0f / (float)SOUND_TIMEREF_FREQUENCY;
        loopy_sound_engine_time_reference(sound_engine, timeref_period);
    }
}
#endif

void sound_initialize(const ByteBuffer *sound_rom) {
    sound_muted = false;
    sound_paused = false;
#if defined(LOOPY_SDL3_FRONTEND) || defined(LOOPY_WASM_FRONTEND)
    volume_level = 1.0f;
    timeref_func = timing_invalid_func_handle();
    timeref_ev = timing_invalid_event_handle();
#endif
    last_control_register = 0;
    sound_rom_present = sound_rom && sound_rom->data && sound_rom->size > 0;
    if (!sound_rom_present) {
        LOOPY_DEBUG_PRINTF("[Sound] no sound ROM loaded; audio disabled\n");
        return;
    }
#ifdef LOOPY_SDL3_FRONTEND
    if (sdl_audio_initialize(sound_rom)) {
        if (SOUND_TIMEREF_ENABLE) {
            printf("[Sound] Schedule timeref %d Hz\n", SOUND_TIMEREF_FREQUENCY);
            timeref_func = timing_register_func("Sound::timeref", sound_timeref);
            sound_timeref(0, 0);
        }
    }
#elif defined(LOOPY_WASM_FRONTEND)
    sample_rate = SOUND_TARGET_SAMPLE_RATE;
    buffer_size = SOUND_TARGET_BUFFER_SIZE;
    sound_engine = loopy_sound_engine_create(sound_rom->data, sound_rom->size, (float)sample_rate, buffer_size);
    if (sound_engine && SOUND_TIMEREF_ENABLE) {
        timeref_func = timing_register_func("Sound::timeref", sound_timeref);
        sound_timeref(0, 0);
    }
#else
    LOOPY_DEBUG_PRINTF("[Sound] sound ROM loaded (%zu bytes); headless build runs silent audio backend\n", sound_rom->size);
#endif
}

void sound_shutdown(void) {
    sdl_audio_shutdown();
    if (sound_engine) {
        loopy_sound_engine_destroy(sound_engine);
        sound_engine = NULL;
    }
}

uint8_t sound_ctrl_read8(uint32_t addr) {
    return (addr & 1u) ? (uint8_t)(last_control_register & 0xffu) : (uint8_t)(last_control_register >> 8);
}
uint16_t sound_ctrl_read16(uint32_t addr) { (void)addr; return last_control_register; }
uint32_t sound_ctrl_read32(uint32_t addr) { (void)addr; return (uint32_t)last_control_register << 16; }
void sound_ctrl_write8(uint32_t addr, uint8_t value) {
    uint16_t cur = last_control_register;
    if (addr & 1u) cur = (uint16_t)((cur & 0xff00u) | value);
    else cur = (uint16_t)(((uint16_t)value << 8) | (cur & 0x00ffu));
    sound_ctrl_write16(addr & ~1u, cur);
}
void sound_ctrl_write16(uint32_t addr, uint16_t value) {
    (void)addr;
    last_control_register = value & 0x0FFFu;
    if (sound_engine) loopy_sound_engine_set_control_register(sound_engine, last_control_register);
}
void sound_ctrl_write32(uint32_t addr, uint32_t value) {
    sound_ctrl_write16(addr & ~1u, (uint16_t)(value >> 16));
    sound_ctrl_write16((addr + 2u) & ~1u, (uint16_t)value);
}
void sound_midi_byte_in(uint8_t value) {
    if (sound_engine) loopy_sound_engine_midi_in(sound_engine, (char)value);
}

void sound_wasm_generate_i16(int16_t *out, int frames) {
    if (!out || frames <= 0) return;
#if defined(LOOPY_SDL3_FRONTEND) || defined(LOOPY_WASM_FRONTEND)
    float tmp[512 * 2];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > 512) n = 512;
        sound_generate(tmp, n * 2);
        for (int i = 0; i < n * 2; i++) {
            float v = tmp[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            out[done * 2 + i] = (int16_t)(v * 32767.0f);
        }
        done += n;
    }
#else
    for (int i = 0; i < frames * 2; i++) out[i] = 0;
#endif
}

void sound_set_mute(bool mute_in) {
#ifdef LOOPY_SDL3_FRONTEND
    if (audio_stream) SDL_LockAudioStream(audio_stream);
#endif
    sound_muted = mute_in;
#ifdef LOOPY_SDL3_FRONTEND
    if (audio_stream) SDL_UnlockAudioStream(audio_stream);
#endif
    LOOPY_DEBUG_PRINTF("[Sound] %s output\n", mute_in ? "Muted" : "Unmuted");
}

void sound_set_paused(bool pause_in) {
#ifdef LOOPY_SDL3_FRONTEND
    if (audio_stream) SDL_LockAudioStream(audio_stream);
#endif
    if (sound_paused != pause_in) {
        sound_paused = pause_in;
#ifdef LOOPY_SDL3_FRONTEND
        if (pause_in) volume_level = 0.0f;
        if (audio_stream) SDL_ClearAudioStream(audio_stream);
#endif
        LOOPY_DEBUG_PRINTF("[Sound] %s engine\n", pause_in ? "Paused" : "Resumed");
    }
#ifdef LOOPY_SDL3_FRONTEND
    if (audio_stream) SDL_UnlockAudioStream(audio_stream);
#endif
}

typedef struct LegacySoundStateBlob { bool muted; bool rom_present; uint16_t last_ctrl; } LegacySoundStateBlob;

typedef struct SoundStateBlobHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t engine_size;
    uint16_t last_ctrl;
    uint8_t muted;
    uint8_t rom_present;
    uint8_t reserved[4];
} SoundStateBlobHeader;

#define SOUND_STATE_MAGIC 0x534E4432u /* SND2 */
#define SOUND_STATE_VERSION 1u

uint32_t sound_state_blob_size(void) {
    uint32_t engine_size = sound_engine ? loopy_sound_engine_state_blob_size() : 0u;
    return (uint32_t)sizeof(SoundStateBlobHeader) + engine_size;
}

void sound_get_state_blob(void *dst, uint32_t size) {
    uint32_t engine_size = sound_engine ? loopy_sound_engine_state_blob_size() : 0u;
    uint32_t need = (uint32_t)sizeof(SoundStateBlobHeader) + engine_size;
    if (!dst || size != need) return;

    SoundStateBlobHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = SOUND_STATE_MAGIC;
    h.version = SOUND_STATE_VERSION;
    h.engine_size = engine_size;
    h.last_ctrl = last_control_register;
    h.muted = sound_muted ? 1u : 0u;
    h.rom_present = sound_rom_present ? 1u : 0u;
    memcpy(dst, &h, sizeof(h));
    if (engine_size) {
        loopy_sound_engine_get_state_blob(sound_engine, (uint8_t *)dst + sizeof(h), engine_size);
    }
}

void sound_set_state_blob(const void *src, uint32_t size) {
    if (!src || !size) return;

    if (size == sizeof(LegacySoundStateBlob)) {
        LegacySoundStateBlob b;
        memcpy(&b, src, sizeof(b));
        sound_muted = b.muted;
        sound_rom_present = b.rom_present;
        last_control_register = b.last_ctrl;
        if (sound_engine) loopy_sound_engine_set_control_register(sound_engine, last_control_register);
        return;
    }

    if (size < sizeof(SoundStateBlobHeader)) return;
    SoundStateBlobHeader h;
    memcpy(&h, src, sizeof(h));
    if (h.magic != SOUND_STATE_MAGIC || h.version != SOUND_STATE_VERSION) return;
    if (size != sizeof(SoundStateBlobHeader) + h.engine_size) return;

    sound_muted = h.muted != 0;
    sound_rom_present = h.rom_present != 0;
    last_control_register = h.last_ctrl;
    if (sound_engine && h.engine_size == loopy_sound_engine_state_blob_size()) {
        loopy_sound_engine_set_state_blob(sound_engine, (const uint8_t *)src + sizeof(h), h.engine_size);
    } else if (sound_engine) {
        loopy_sound_engine_set_control_register(sound_engine, last_control_register);
    }
}
