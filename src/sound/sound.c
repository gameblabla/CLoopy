#include "sound/sound.h"
#include "sound/loopysound.h"
#include "sound/oki_adpcm.h"
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
static OkiAdpcm *cart_adpcm;
static uint8_t last_exp_data;
static int sample_rate = SOUND_TARGET_SAMPLE_RATE;
static int buffer_size = SOUND_TARGET_BUFFER_SIZE;
static float volume_level = 1.0f;
static FILE *wav_file;
static uint32_t wav_data_bytes;
static double wav_frame_accum;

#if defined(LOOPY_SDL3_FRONTEND) || defined(LOOPY_WASM_FRONTEND)
static TimingFuncHandle timeref_func;
static TimingEventHandle timeref_ev;
#endif

#ifdef LOOPY_SDL3_FRONTEND
static SDL_AudioStream *audio_stream;
#endif

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

    int frames = float_count / 2;
    int p = 0;
    for (int i = 0; i < frames; i++) {
        update_volume_level();
        float l = 0.0f, r = 0.0f;
        if (sound_engine) {
            float tmp[2];
            loopy_sound_engine_gen_sample(sound_engine, tmp);
            l += tmp[0];
            r += tmp[1];
        }
        if (cart_adpcm) {
            float mono = oki_adpcm_generate(cart_adpcm) * 0.85f;
            l += mono;
            r += mono;
        }
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        buffer[p++] = l * volume_level;
        buffer[p++] = r * volume_level;
    }
    if (p < float_count) buffer[p] = 0.0f;
}

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

void sound_initialize(const ByteBuffer *sound_rom, const ByteBuffer *oki_adpcm_rom, int cart_is_wanwan) {
    sound_muted = false;
    sound_paused = false;
    volume_level = 1.0f;
#if defined(LOOPY_SDL3_FRONTEND) || defined(LOOPY_WASM_FRONTEND)
    timeref_func = timing_invalid_func_handle();
    timeref_ev = timing_invalid_event_handle();
#endif
    last_control_register = 0;
    last_exp_data = 0;
    sample_rate = SOUND_TARGET_SAMPLE_RATE;
    buffer_size = SOUND_TARGET_BUFFER_SIZE;
    const void *oki_data = (oki_adpcm_rom && oki_adpcm_rom->data && oki_adpcm_rom->size) ? oki_adpcm_rom->data : NULL;
    uint32_t oki_size = (oki_adpcm_rom && oki_adpcm_rom->data && oki_adpcm_rom->size && oki_adpcm_rom->size <= 0xFFFFFFFFu) ? (uint32_t)oki_adpcm_rom->size : 0u;
    cart_adpcm = oki_adpcm_create((float)sample_rate, oki_data, oki_size, cart_is_wanwan);
    if (!cart_adpcm) fprintf(stderr, "[Sound] Failed to initialize OKI ADPCM cart engine\n");
    sound_rom_present = sound_rom && sound_rom->data && sound_rom->size > 0;
    if (!sound_rom_present) {
        LOOPY_DEBUG_PRINTF("[Sound] no main sound ROM loaded; internal sound disabled, cart ADPCM remains enabled\n");
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
    sound_wav_close();
    if (sound_engine) {
        loopy_sound_engine_destroy(sound_engine);
        sound_engine = NULL;
    }
    if (cart_adpcm) {
        oki_adpcm_destroy(cart_adpcm);
        cart_adpcm = NULL;
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

void sound_generate_i16(int16_t *out, int frames) {
    if (!out || frames <= 0) return;
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
}

void sound_wasm_generate_i16(int16_t *out, int frames) {
    sound_generate_i16(out, frames);
}


uint8_t sound_exp_data_read8(uint32_t addr) {
    (void)addr;
    return last_exp_data;
}

uint16_t sound_exp_data_read16(uint32_t addr) {
    (void)addr;
    return (uint16_t)last_exp_data;
}

uint32_t sound_exp_data_read32(uint32_t addr) {
    (void)addr;
    return (uint32_t)last_exp_data << 24;
}

void sound_exp_data_write8(uint32_t addr, uint8_t value) {
    (void)addr;
    last_exp_data = value;
    if (cart_adpcm) oki_adpcm_write_data(cart_adpcm, value);
    LOOPY_DEBUG_PRINTF("[Sound] OKI latch %02X\n", value);
}

void sound_exp_data_write16(uint32_t addr, uint16_t value) {
    if (addr & 1u) sound_exp_data_write8(addr, (uint8_t)value);
    else sound_exp_data_write8(addr, (uint8_t)(value >> 8));
}

void sound_exp_data_write32(uint32_t addr, uint32_t value) {
    sound_exp_data_write8(addr, (uint8_t)(value >> 24));
}

uint16_t sound_cart_portb_read(uint16_t current_value) {
    if (cart_adpcm && oki_adpcm_nar(cart_adpcm)) current_value |= 0x0080u;
    else current_value &= (uint16_t)~0x0080u;
    return current_value;
}

void sound_cart_portb_write(uint16_t old_value, uint16_t new_value) {
    if (!cart_adpcm) return;
    (void)old_value;
    oki_adpcm_set_reset(cart_adpcm, (new_value & 0x0002u) ? 1 : 0);
    oki_adpcm_set_ch(cart_adpcm, (new_value & 0x0100u) ? 1 : 0);
    oki_adpcm_set_st(cart_adpcm, (new_value & 0x0400u) ? 1 : 0);
}

static void wav_write_u16(FILE *f, uint16_t v) {
    fputc((int)(v & 0xFFu), f);
    fputc((int)(v >> 8), f);
}

static void wav_write_u32(FILE *f, uint32_t v) {
    wav_write_u16(f, (uint16_t)v);
    wav_write_u16(f, (uint16_t)(v >> 16));
}

int sound_wav_open(const char *path) {
    if (!path || !*path) return -1;
    sound_wav_close();
    wav_file = fopen(path, "wb");
    if (!wav_file) return -1;
    wav_data_bytes = 0;
    wav_frame_accum = 0.0;
    fwrite("RIFF", 1, 4, wav_file);
    wav_write_u32(wav_file, 0);
    fwrite("WAVEfmt ", 1, 8, wav_file);
    wav_write_u32(wav_file, 16);
    wav_write_u16(wav_file, 1);
    wav_write_u16(wav_file, 2);
    wav_write_u32(wav_file, (uint32_t)sample_rate);
    wav_write_u32(wav_file, (uint32_t)sample_rate * 2u * 2u);
    wav_write_u16(wav_file, 4);
    wav_write_u16(wav_file, 16);
    fwrite("data", 1, 4, wav_file);
    wav_write_u32(wav_file, 0);
    return ferror(wav_file) ? -1 : 0;
}

void sound_wav_write_frame(void) {
    if (!wav_file) return;
    const double samples_per_frame = (double)SOUND_TARGET_SAMPLE_RATE * 10000.0 / 598261.0;
    wav_frame_accum += samples_per_frame;
    int frames = (int)wav_frame_accum;
    wav_frame_accum -= frames;
    if (frames <= 0) return;
    int16_t buffer[1024 * 2];
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > 1024) n = 1024;
        sound_generate_i16(buffer, n);
        size_t wrote = fwrite(buffer, sizeof(int16_t), (size_t)n * 2u, wav_file);
        wav_data_bytes += (uint32_t)(wrote * sizeof(int16_t));
        done += n;
    }
}

void sound_wav_close(void) {
    if (!wav_file) return;
    uint32_t riff_size = 36u + wav_data_bytes;
    fseek(wav_file, 4, SEEK_SET);
    wav_write_u32(wav_file, riff_size);
    fseek(wav_file, 40, SEEK_SET);
    wav_write_u32(wav_file, wav_data_bytes);
    fclose(wav_file);
    wav_file = NULL;
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
    uint32_t oki_size;
    uint16_t last_ctrl;
    uint8_t muted;
    uint8_t rom_present;
    uint8_t last_exp_data_blob;
    uint8_t reserved[3];
} SoundStateBlobHeader;

#define SOUND_STATE_MAGIC 0x534E4432u /* SND2 */
#define SOUND_STATE_VERSION 2u

uint32_t sound_state_blob_size(void) {
    uint32_t engine_size = sound_engine ? loopy_sound_engine_state_blob_size() : 0u;
    uint32_t oki_size = cart_adpcm ? oki_adpcm_state_blob_size() : 0u;
    return (uint32_t)sizeof(SoundStateBlobHeader) + engine_size + oki_size;
}

void sound_get_state_blob(void *dst, uint32_t size) {
    uint32_t engine_size = sound_engine ? loopy_sound_engine_state_blob_size() : 0u;
    uint32_t oki_size = cart_adpcm ? oki_adpcm_state_blob_size() : 0u;
    uint32_t need = (uint32_t)sizeof(SoundStateBlobHeader) + engine_size + oki_size;
    if (!dst || size != need) return;

    SoundStateBlobHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = SOUND_STATE_MAGIC;
    h.version = SOUND_STATE_VERSION;
    h.engine_size = engine_size;
    h.oki_size = oki_size;
    h.last_ctrl = last_control_register;
    h.muted = sound_muted ? 1u : 0u;
    h.rom_present = sound_rom_present ? 1u : 0u;
    h.last_exp_data_blob = last_exp_data;
    memcpy(dst, &h, sizeof(h));
    uint8_t *p = (uint8_t *)dst + sizeof(h);
    if (engine_size) {
        loopy_sound_engine_get_state_blob(sound_engine, p, engine_size);
        p += engine_size;
    }
    if (oki_size) oki_adpcm_get_state_blob(cart_adpcm, p, oki_size);
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
    if (size != sizeof(SoundStateBlobHeader) + h.engine_size + h.oki_size) return;

    sound_muted = h.muted != 0;
    sound_rom_present = h.rom_present != 0;
    last_control_register = h.last_ctrl;
    last_exp_data = h.last_exp_data_blob;
    const uint8_t *p = (const uint8_t *)src + sizeof(h);
    if (sound_engine && h.engine_size == loopy_sound_engine_state_blob_size()) {
        loopy_sound_engine_set_state_blob(sound_engine, p, h.engine_size);
    } else if (sound_engine) {
        loopy_sound_engine_set_control_register(sound_engine, last_control_register);
    }
    p += h.engine_size;
    if (cart_adpcm && h.oki_size == oki_adpcm_state_blob_size()) {
        oki_adpcm_set_state_blob(cart_adpcm, p, h.oki_size);
    }
}
