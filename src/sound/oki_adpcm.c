/*
 * OKI MSM6653A / MSM6376-style ADPCM support for CLoopy.
 *
 * This implementation is based on MAME's OKI MSM6376/MSM6650 driver,
 * particularly its ADPCM step/index tables, difference table generation,
 * phrase block playback, channel handling, NAR/BUSY/ST/CH behavior, and
 * attenuation handling:
 *
 *   MAME src/devices/sound/okim6376.cpp
 *   MAME src/devices/sound/okim6376.h
 *   license: BSD-3-Clause
 *   copyright-holders: Mirko Buffoni, James Wallace
 *
 * Adapted to C11 and CLoopy's Wanwan Aijou Monogatari cartridge-audio
 * path.  MAME itself is not linked into this emulator.
 */
#include "sound/oki_adpcm.h"
#include "sound/wanwan_oki_bank.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OKI_VOICES 2
#define OKI_COMMANDS 128
#define OKI_CLOCK_HZ 4096000.0f
#define OKI_ADPCM_RATE (OKI_CLOCK_HZ / 64.0f / 8.0f)
#define OKI_MAX_OUTPUT 32768.0f

static const int index_shift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static int diff_lookup[49 * 16];
static int tables_ready;
static uint8_t *external_oki_rom;
static uint32_t external_oki_rom_size;

static void compute_tables(void) {
    if (tables_ready) return;
    static const int nbl2bit[16][4] = {
        { 1, 0, 0, 0}, { 1, 0, 0, 1}, { 1, 0, 1, 0}, { 1, 0, 1, 1},
        { 1, 1, 0, 0}, { 1, 1, 0, 1}, { 1, 1, 1, 0}, { 1, 1, 1, 1},
        {-1, 0, 0, 0}, {-1, 0, 0, 1}, {-1, 0, 1, 0}, {-1, 0, 1, 1},
        {-1, 1, 0, 0}, {-1, 1, 0, 1}, {-1, 1, 1, 0}, {-1, 1, 1, 1}
    };
    for (int step = 0; step <= 48; step++) {
        int stepval = (int)floor(16.0 * pow(11.0 / 10.0, (double)step));
        for (int nib = 0; nib < 16; nib++) {
            diff_lookup[step * 16 + nib] = nbl2bit[nib][0] *
                (stepval * nbl2bit[nib][1] +
                 stepval / 2 * nbl2bit[nib][2] +
                 stepval / 4 * nbl2bit[nib][3] +
                 stepval / 8);
        }
    }
    tables_ready = 1;
}

typedef struct OkiVoice {
    uint8_t playing;
    uint32_t base_offset;
    uint32_t sample;
    uint32_t count;
    uint32_t volume;
    int32_t signal;
    int32_t step;
} OkiVoice;

struct OkiAdpcm {
    uint8_t *rom;
    uint32_t rom_size;
    uint8_t owns_rom;
    OkiVoice voice[OKI_VOICES];
    int32_t command[OKI_VOICES];
    uint8_t stage[OKI_VOICES];
    int32_t latch;
    uint8_t channel;
    uint8_t nar;
    int32_t nar_samples;
    uint8_t busy;
    uint8_t ch;
    uint8_t st;
    uint8_t st_pulses;
    uint8_t ch_update;
    uint8_t st_update;
    uint8_t reset_pin;
    float output_rate;
    float adpcm_rate;
    float resample_phase;
    int16_t last_sample;
};

static void voice_reset(OkiVoice *v) {
    compute_tables();
    v->signal = -2;
    v->step = 0;
}

static int16_t voice_clock(OkiVoice *v, uint8_t nibble) {
    v->signal += diff_lookup[v->step * 16 + (nibble & 15)];
    if (v->signal > 2047) v->signal = 2047;
    else if (v->signal < -2048) v->signal = -2048;
    v->step += index_shift[nibble & 7];
    if (v->step > 48) v->step = 48;
    else if (v->step < 0) v->step = 0;
    return (int16_t)v->signal;
}

static uint8_t rom_read(const OkiAdpcm *o, uint32_t addr) {
    if (!o || !o->rom || !o->rom_size || addr >= o->rom_size) return 0;
    return o->rom[addr];
}

static uint32_t get_start_position(const OkiAdpcm *o, int channel) {
    uint32_t command = (uint32_t)(o->command[channel] & 0x7f);
    uint32_t base = command * 4u;
    uint32_t pos = ((uint32_t)rom_read(o, base + 0) << 16) |
                   ((uint32_t)rom_read(o, base + 1) << 8) |
                    (uint32_t)rom_read(o, base + 2);
    return pos & 0x1fffffu;
}

static void oki_process(OkiAdpcm *o, int channel, int command) {
    if (!o) return;
    /* Wanwan sends 0x60 as a prefix/control strobe before sample commands.
     * It should not be resolved through the phrase table and must not stop
     * the currently playing phrase when its table entry is empty. */
    if (command == 0x60) return;
    if (command != -1 && command != 0) {
        OkiVoice *v = &o->voice[channel];
        uint32_t start = get_start_position(o, channel);
        if (start == 0 || start >= o->rom_size) {
            v->playing = 0;
        } else if (!v->playing) {
            v->playing = 1;
            v->base_offset = start;
            v->sample = 0;
            v->count = 0;
            voice_reset(v);
            if (v->volume == 0) v->volume = 0x20;
        } else {
            /* The MSM6653A-457 contains complete phrase effects in its mask
             * ROM.  Wanwan's command stream can strobe while a phrase is still
             * busy; queuing that as a replay makes the .ls0 dog bark timing
             * drift and can add an extra bark.  Treat in-flight retriggers as
             * ignored until hardware evidence shows otherwise. */
        }
    } else if (command == 0) {
        for (int i = 0; i < OKI_VOICES; i++) o->voice[i].playing = 0;
    }
}

static int16_t generate_voice(OkiAdpcm *o, int channel) {
    OkiVoice *v = &o->voice[channel];
    if (!v->playing) {
        if (o->stage[channel]) {
            o->stage[channel] = 0;
            oki_process(o, channel, o->command[channel]);
        }
        return 0;
    }

    if (v->count == 0) {
        v->count = (uint32_t)(rom_read(o, v->base_offset + v->sample / 2u) & 0x7fu) << 1;
        if (v->count == 0) {
            v->playing = 0;
            if (o->stage[channel]) {
                o->stage[channel] = 0;
                oki_process(o, channel, o->command[channel]);
            }
            return 0;
        }
        v->sample += 2;
    }

    uint8_t byte = rom_read(o, v->base_offset + v->sample / 2u);
    uint8_t nibble = (uint8_t)(byte >> (((v->sample & 1u) << 2) ^ 4));
    int32_t sample = voice_clock(v, nibble) * (int32_t)v->volume / 2;
    v->sample++;
    v->count--;
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    return (int16_t)sample;
}

static uint8_t *copy_rom_data(const void *data, uint32_t size, uint32_t *out_size) {
    if (!data || !size || size > 8u * 1024u * 1024u) return NULL;
    uint8_t *rom = (uint8_t *)malloc((size_t)size);
    if (!rom) return NULL;
    memcpy(rom, data, (size_t)size);
    if (out_size) *out_size = size;
    return rom;
}

static uint8_t *make_replacement_wanwan_rom(uint32_t *out_size) {
    if (!wanwan_oki_rom_size) return NULL;
    uint8_t *rom = copy_rom_data(wanwan_oki_rom, wanwan_oki_rom_size, out_size);
    if (!rom) return NULL;
    fprintf(stderr,
            "[Sound] WARNING: Wanwan Aijou Monogatari uses an MSM6653A-457 cartridge ADPCM sample ROM. No user OKI ROM was supplied, so the freely licensed replacement bank is being used (%u bytes). Pass an OKI ROM as the fourth positional argument after loopy_sound.bin, or use --oki-rom <file>, to override.\n",
            (unsigned)wanwan_oki_rom_size);
    return rom;
}

int oki_adpcm_set_external_rom_data(const void *data, uint32_t size) {
    uint8_t *copy = copy_rom_data(data, size, NULL);
    if (!copy) return 0;
    free(external_oki_rom);
    external_oki_rom = copy;
    external_oki_rom_size = size;
    return 1;
}

void oki_adpcm_clear_external_rom_data(void) {
    free(external_oki_rom);
    external_oki_rom = NULL;
    external_oki_rom_size = 0;
}

static uint8_t *copy_legacy_external_rom(uint32_t *out_size) {
    uint8_t *rom = copy_rom_data(external_oki_rom, external_oki_rom_size, out_size);
    if (rom) fprintf(stderr, "[Sound] Loaded user-supplied OKI ADPCM sample ROM from frontend storage (%u bytes)\n", (unsigned)external_oki_rom_size);
    return rom;
}

OkiAdpcm *oki_adpcm_create(float output_rate, const void *external_rom, uint32_t external_rom_size, int use_wanwan_replacement) {
    OkiAdpcm *o = (OkiAdpcm *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->rom = copy_rom_data(external_rom, external_rom_size, &o->rom_size);
    if (o->rom) {
        fprintf(stderr, "[Sound] Loaded user-supplied OKI ADPCM sample ROM (%u bytes)\n", (unsigned)o->rom_size);
    }
    if (!o->rom) o->rom = copy_legacy_external_rom(&o->rom_size);
    if (!o->rom && use_wanwan_replacement) o->rom = make_replacement_wanwan_rom(&o->rom_size);
    o->owns_rom = o->rom ? 1u : 0u;
    o->output_rate = output_rate > 1000.0f ? output_rate : 48000.0f;
    o->adpcm_rate = OKI_ADPCM_RATE;
    oki_adpcm_reset(o);
    return o;
}

void oki_adpcm_destroy(OkiAdpcm *o) {
    if (!o) return;
    if (o->owns_rom) free(o->rom);
    free(o);
}

void oki_adpcm_reset(OkiAdpcm *o) {
    if (!o) return;
    for (int i = 0; i < OKI_VOICES; i++) {
        o->voice[i].playing = 0;
        o->voice[i].volume = 0x20;
        voice_reset(&o->voice[i]);
        o->command[i] = -1;
        o->stage[i] = 0;
    }
    o->latch = 0;
    o->channel = 0;
    o->nar = 1;
    o->nar_samples = 0;
    o->busy = 1;
    o->ch = 1;
    o->st = 1;
    o->st_pulses = 0;
    o->ch_update = 0;
    o->st_update = 0;
    o->reset_pin = 0;
    o->resample_phase = 0.0f;
    o->last_sample = 0;
}

void oki_adpcm_write_data(OkiAdpcm *o, uint8_t data) {
    if (!o) return;
    o->latch = data & 0x7f;
}

void oki_adpcm_set_ch(OkiAdpcm *o, int state) {
    if (!o) return;
    state = state ? 1 : 0;
    o->ch_update = 0;
    if (o->ch != state) {
        o->ch = (uint8_t)state;
        o->ch_update = 1;
    }
    if (!o->ch && o->ch_update) {
        o->st_pulses = 0;
        o->channel = 1;
        if (o->voice[0].playing && o->st) {
            o->command[1] = o->command[0];
            o->voice[1].volume = 0x10;
        }
    }
    if (o->ch && o->ch_update) {
        o->stage[1] = 0;
        oki_process(o, 1, o->command[1]);
        o->channel = 0;
    }
}

void oki_adpcm_set_st(OkiAdpcm *o, int state) {
    if (!o) return;
    state = state ? 1 : 0;
    o->st_update = 0;
    if (o->st == state) return;
    o->st = (uint8_t)state;
    o->st_update = 1;
    if (o->channel == 1 && !o->st) {
        OkiVoice *v = &o->voice[o->channel];
        o->st_pulses++;
        if (o->st_pulses > 3) o->st_pulses = 3;
        static const uint32_t volume_table[3] = { 0x20, 0x10, 0x08 };
        v->volume = volume_table[o->st_pulses - 1u];
    }
    if (o->st) {
        o->command[o->channel] = o->latch;
        if (o->channel == 0) {
            if (o->nar) {
                o->stage[0] = 0;
                oki_process(o, 0, o->command[0]);
                o->nar = 0;
                o->nar_samples = (int)(o->adpcm_rate * 0.000375f);
                if (o->nar_samples < 1) o->nar_samples = 1;
            }
        } else {
            o->stage[1] = 0;
            oki_process(o, 1, o->command[1]);
        }
    }
}

void oki_adpcm_set_reset(OkiAdpcm *o, int state) {
    if (!o) return;
    state = state ? 1 : 0;
    if (state && !o->reset_pin) oki_adpcm_reset(o);
    o->reset_pin = (uint8_t)state;
}

int oki_adpcm_nar(const OkiAdpcm *o) { return !o || o->nar ? 1 : 0; }
int oki_adpcm_busy(const OkiAdpcm *o) {
    if (!o) return 1;
    return (o->voice[0].playing || o->voice[1].playing) ? 0 : 1;
}

float oki_adpcm_generate(OkiAdpcm *o) {
    if (!o) return 0.0f;
    if (!o->nar && o->nar_samples > 0) {
        o->nar_samples--;
        if (o->nar_samples <= 0) o->nar = 1;
    }
    float step = o->adpcm_rate / o->output_rate;
    o->resample_phase += step;
    while (o->resample_phase >= 1.0f) {
        int32_t s = (int32_t)generate_voice(o, 0) + (int32_t)generate_voice(o, 1);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        o->last_sample = (int16_t)s;
        o->resample_phase -= 1.0f;
    }
    return (float)o->last_sample / OKI_MAX_OUTPUT;
}

void oki_adpcm_debug_play_command(OkiAdpcm *o, uint8_t command) {
    if (!o) return;
    int channel = 0;
    o->command[channel] = (int32_t)(command & 0x7f);
    o->stage[channel] = 0;
    o->nar = 1;
    o->voice[channel].volume = 0x20;
    oki_process(o, channel, o->command[channel]);
}

int oki_adpcm_debug_active(const OkiAdpcm *o) {
    if (!o) return 0;
    for (int i = 0; i < OKI_VOICES; i++) {
        if (o->voice[i].playing || o->stage[i]) return 1;
    }
    return 0;
}

typedef struct OkiAdpcmStateBlob {
    uint32_t magic;
    uint32_t version;
    OkiVoice voice[OKI_VOICES];
    int32_t command[OKI_VOICES];
    uint8_t stage[OKI_VOICES];
    int32_t latch;
    uint8_t channel, nar, busy, ch, st, st_pulses, reset_pin;
    int32_t nar_samples;
    float resample_phase;
    int16_t last_sample;
} OkiAdpcmStateBlob;

uint32_t oki_adpcm_state_blob_size(void) { return (uint32_t)sizeof(OkiAdpcmStateBlob); }

void oki_adpcm_get_state_blob(const OkiAdpcm *o, void *dst, uint32_t size) {
    if (!o || !dst || size != sizeof(OkiAdpcmStateBlob)) return;
    OkiAdpcmStateBlob b;
    memset(&b, 0, sizeof(b));
    b.magic = 0x4F4B4931u;
    b.version = 1;
    memcpy(b.voice, o->voice, sizeof(b.voice));
    memcpy(b.command, o->command, sizeof(b.command));
    memcpy(b.stage, o->stage, sizeof(b.stage));
    b.latch = o->latch;
    b.channel = o->channel;
    b.nar = o->nar;
    b.busy = o->busy;
    b.ch = o->ch;
    b.st = o->st;
    b.st_pulses = o->st_pulses;
    b.reset_pin = o->reset_pin;
    b.nar_samples = o->nar_samples;
    b.resample_phase = o->resample_phase;
    b.last_sample = o->last_sample;
    memcpy(dst, &b, sizeof(b));
}

void oki_adpcm_set_state_blob(OkiAdpcm *o, const void *src, uint32_t size) {
    if (!o || !src || size != sizeof(OkiAdpcmStateBlob)) return;
    OkiAdpcmStateBlob b;
    memcpy(&b, src, sizeof(b));
    if (b.magic != 0x4F4B4931u || b.version != 1) return;
    memcpy(o->voice, b.voice, sizeof(o->voice));
    memcpy(o->command, b.command, sizeof(o->command));
    memcpy(o->stage, b.stage, sizeof(o->stage));
    o->latch = b.latch;
    o->channel = b.channel;
    o->nar = b.nar;
    o->busy = b.busy;
    o->ch = b.ch;
    o->st = b.st;
    o->st_pulses = b.st_pulses;
    o->reset_pin = b.reset_pin;
    o->nar_samples = b.nar_samples;
    o->resample_phase = b.resample_phase;
    o->last_sample = b.last_sample;
}
