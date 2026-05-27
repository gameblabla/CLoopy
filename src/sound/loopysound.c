/*
 * C11 port of Kasami's Loopy uPD937 sound implementation.
 * Original C++ implementation: src/sound/loopysound.cpp in LoopyMSE.
 */
#include "sound/loopysound.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TUNING 442.0f
#define MIX_LEVEL 0.7f
#define FILTER_ENABLE 1
#define FILTER_CUTOFF 8247.0f
#define FILTER_RESONANCE 1.67f
#define HC_RATETABLE 0x1000
#define HC_VOLTABLE 0x1400
#define HC_PITCHTABLE 0x1600
#define HC_INSTDESC 0x2200
#define HC_KEYMAPS 0x3DA0
#define HC_NUM_BANKS 1
#define CLK2_MUL 15625
#define CLK2_DIVP 128
#define MIDI_QUEUE_CAPACITY 2048

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }

typedef struct UPD937_VoiceState {
    int channel, note;
    bool active, sustained;
    int pitch;
    int volume, volume_target, volume_rate_mul, volume_rate_div, volume_rate_counter;
    bool volume_down;
    int volume_env, volume_env_step, volume_env_delay;
    int pitch_env, pitch_env_step, pitch_env_delay, pitch_env_value, pitch_env_rate, pitch_env_target;
    int sample_start, sample_end, sample_loop, sample_ptr, sample_fract, sample_last_val;
    bool sample_new;
} UPD937_VoiceState;

typedef struct UPD937_ChannelState {
    bool midi_enabled;
    bool mute;
    int first_voice, voice_count;
    bool sustain;
    int instrument;
    int partials_offset;
    int keymap_no;
    bool layered;
    int bend_offset, bend_value;
    int allocate_next;
} UPD937_ChannelState;

typedef struct UPD937_Core {
    uint8_t *rom;
    int rom_mask;
    uint32_t ptr_partials;
    uint32_t ptr_pitchenv;
    uint32_t ptr_volenv;
    uint32_t ptr_sampdesc;
    uint32_t ptr_demosong;
    uint32_t ptr_pitchtable;
    uint32_t ptr_instdesc;
    uint32_t ptr_keymaps;
    uint32_t ptr_ratetable;
    uint32_t ptr_voltable;
    UPD937_VoiceState voices[32];
    UPD937_ChannelState channels[32];
    int volume_slider[2];
    int clk2_counter;
    int delay_update_phase;
    uint32_t sample_count;
    float synthesis_rate;
    int midi_status;
    int midi_running_status;
    char midi_param_bytes[8];
    int midi_param_count;
    bool midi_in_sysex;
} UPD937_Core;

typedef struct BiquadStereoFilter {
    float fs, fc, q;
    bool hp;
    float a1, a2, b0, b1, b2;
    float x1[2], x2[2], y1[2], y2[2];
} BiquadStereoFilter;

struct LoopySoundEngine {
    UPD937_Core synth;
    BiquadStereoFilter filter_tone;
    BiquadStereoFilter filter_block_dc;
    bool has_filter_tone;
    bool has_filter_block_dc;
    float mix_level;
    float out_rate;
    float synth_rate;
    int buffer_size;
    int raw_samples[2];
    float current_sample[2];
    float last_sample[2];
    float mix_sample[2];
    float interpolation_step;
    int out_sample_count;
    int time_reference_samples;
    bool has_time_reference;
    int buttons_last;
    int channel_config_state;
    bool in_demo;
    char midi_queue_bytes[MIDI_QUEUE_CAPACITY];
    int midi_queue_timestamps[MIDI_QUEUE_CAPACITY];
    int queue_write, queue_read;
    bool midi_overflowed;
};

static void upd_prog_chg(UPD937_Core *s, int channel, int prog);
static void upd_set_channel_configuration(UPD937_Core *s, bool multi, bool all);
static void upd_reset_channels(UPD937_Core *s, bool clear_program);
static void upd_update_volume_envelopes(UPD937_Core *s);
static void upd_update_pitch_envelopes(UPD937_Core *s);
static int upd_get_free_voice(UPD937_Core *s, int c);
static void upd_note_on(UPD937_Core *s, int channel, int note);
static void upd_note_off(UPD937_Core *s, int channel, int note);
static void upd_pitch_bend(UPD937_Core *s, int channel, int bend_byte);
static void upd_control_chg_sustain(UPD937_Core *s, int channel, bool sustain);
static int upd_midi_prog_to_bank(int prog, int bank_select);
static void engine_handle_midi_event(LoopySoundEngine *e);

static int upd_read_rom_8(const UPD937_Core *s, int offset) {
    return s->rom[offset & s->rom_mask] & 0xFF;
}

static int upd_read_rom_16(const UPD937_Core *s, int offset) {
    return ((s->rom[(offset + 1) & s->rom_mask] & 0xFF) << 8) | (s->rom[offset & s->rom_mask] & 0xFF);
}

static int upd_read_rom_24(const UPD937_Core *s, int offset) {
    return ((s->rom[(offset + 2) & s->rom_mask] & 0xFF) << 16) |
           ((s->rom[(offset + 1) & s->rom_mask] & 0xFF) << 8) |
           (s->rom[offset & s->rom_mask] & 0xFF);
}

static bool upd_init(UPD937_Core *s, const uint8_t *rom_in, size_t rom_size, float synthesis_rate) {
    memset(s, 0, sizeof(*s));
    s->ptr_pitchtable = HC_PITCHTABLE;
    s->ptr_instdesc = HC_INSTDESC;
    s->ptr_keymaps = HC_KEYMAPS;
    s->ptr_ratetable = HC_RATETABLE;
    s->ptr_voltable = HC_VOLTABLE;
    int padded = 1;
    while ((size_t)padded < rom_size) padded <<= 1;
    s->rom = (uint8_t *)calloc((size_t)padded, 1);
    if (!s->rom) return false;
    memcpy(s->rom, rom_in, rom_size);
    s->rom_mask = padded - 1;
    s->ptr_partials = (uint32_t)upd_read_rom_16(s, 0) * 32u;
    s->ptr_pitchenv = (uint32_t)upd_read_rom_16(s, 2) * 32u;
    s->ptr_volenv   = (uint32_t)upd_read_rom_16(s, 4) * 32u;
    s->ptr_sampdesc = (uint32_t)upd_read_rom_16(s, 6) * 32u;
    s->ptr_demosong = (uint32_t)upd_read_rom_16(s, 8) * 32u;
    for (int c = 0; c < 4; c++) upd_prog_chg(s, c, 0);
    upd_set_channel_configuration(s, false, false);
    s->synthesis_rate = synthesis_rate;
    s->volume_slider[0] = 4;
    s->volume_slider[1] = 4;
    return true;
}

static void upd_destroy(UPD937_Core *s) {
    if (!s) return;
    free(s->rom);
    s->rom = NULL;
    s->rom_mask = 0;
}

static void upd_update_sample(UPD937_Core *s) {
    if ((s->sample_count % 384u) == 0) upd_update_volume_envelopes(s);
    int clk2_div = (int)roundf(CLK2_DIVP * s->synthesis_rate);
    s->clk2_counter += CLK2_MUL;
    if (s->clk2_counter >= clk2_div) {
        upd_update_pitch_envelopes(s);
        s->clk2_counter -= clk2_div;
    }
    for (int v = 0; v < 32; v++) {
        UPD937_VoiceState *vo = &s->voices[v];
        vo->volume_rate_counter++;
        if (vo->volume_rate_counter >= vo->volume_rate_div) {
            vo->volume_rate_counter = 0;
            if (vo->volume_down) {
                vo->volume = clampi(maxi(vo->volume_target, vo->volume - vo->volume_rate_mul), 0, 65535);
            } else {
                vo->volume = clampi(mini(vo->volume_target, vo->volume + vo->volume_rate_mul), 0, 65535);
            }
        }
        if (vo->volume > 0) {
            int pitch_relative = vo->pitch;
            pitch_relative += vo->pitch_env_value / 16;
            pitch_relative += s->channels[vo->channel].bend_offset;
            vo->sample_fract += upd_read_rom_16(s, (int)s->ptr_pitchtable + pitch_relative * 2);
            if (vo->sample_fract >= 0x8000) {
                vo->sample_fract -= 0x8000;
                vo->sample_last_val = (upd_read_rom_16(s, vo->sample_ptr * 2) >> 4) - 0x800;
                vo->sample_ptr++;
            }
            if (vo->sample_ptr > vo->sample_end) vo->sample_ptr = vo->sample_loop;
        }
    }
    s->sample_count++;
}

static void upd_gen_sample(UPD937_Core *s, int out[2]) {
    static const int volume_slider_levels[5] = {0, 2048, 2580, 3251, 4096};
    upd_update_sample(s);
    for (int lr = 0; lr <= 1; lr++) {
        int accum = 0;
        for (int v = 0; v < 32; v += 2) {
            UPD937_VoiceState *vo = &s->voices[v + lr];
            UPD937_ChannelState *ch = &s->channels[vo->channel];
            if (vo->volume == 0 || ch->mute) continue;
            int sample = vo->sample_last_val;
            int sb = (upd_read_rom_16(s, vo->sample_ptr * 2) >> 4) - 0x800;
            int sd = ((sb - sample) * vo->sample_fract) / 0x8000;
            sample += sd;
            sample = (sample * vo->volume) / 65536;
            if (vo->channel > 0) {
                sample = (sample * volume_slider_levels[s->volume_slider[vo->channel == 3 ? 1 : 0]]) / 4096;
            }
            accum += sample;
        }
        out[lr] = clampi(accum, -32767, 32767);
    }
}

static void upd_set_channel_configuration(UPD937_Core *s, bool multi, bool all) {
    if (multi) {
        s->channels[0].first_voice = 0;
        s->channels[0].voice_count = 12;
        s->channels[1].first_voice = 12;
        s->channels[1].voice_count = 8;
        s->channels[2].first_voice = 20;
        s->channels[2].voice_count = 4;
        s->channels[3].first_voice = 24;
        s->channels[3].voice_count = 8;
        s->channels[0].midi_enabled = true;
        s->channels[1].midi_enabled = true;
        s->channels[2].midi_enabled = true;
        s->channels[3].midi_enabled = all;
    } else {
        s->channels[0].first_voice = 0;
        s->channels[0].voice_count = 24;
        s->channels[0].midi_enabled = true;
        s->channels[1].midi_enabled = false;
        s->channels[2].midi_enabled = false;
        s->channels[3].midi_enabled = false;
        s->channels[1].voice_count = 0;
        s->channels[2].voice_count = 0;
        s->channels[3].voice_count = 0;
    }
    for (int v = 0; v < 32; v++) s->voices[v].channel = 0;
    for (int c = 1; c < 4; c++) {
        for (int v = 0; v < s->channels[c].voice_count; v++) {
            s->voices[s->channels[c].first_voice + v].channel = c;
        }
    }
}

static void upd_set_volume_slider(UPD937_Core *s, int group, int slider) {
    group = clampi(group, 0, 1);
    slider = clampi(slider, 0, 4);
    s->volume_slider[group] = slider;
}

static void upd_set_channel_muted(UPD937_Core *s, int channel, bool mute) {
    if (channel >= 0 && channel < 32) s->channels[channel].mute = mute;
}

static void upd_reset_channels(UPD937_Core *s, bool clear_program) {
    int p = clear_program ? 0 : 128;
    upd_prog_chg(s, 0, p);
    upd_prog_chg(s, 1, p);
    upd_prog_chg(s, 2, p);
    upd_prog_chg(s, 3, p);
}

static void upd_process_midi_now(UPD937_Core *s, char midi_byte) {
    int m = midi_byte & 0xFF;
    if (m >= 0x80) {
        if (m == 0xF0 && !s->midi_in_sysex) s->midi_in_sysex = true;
        if (m == 0xF7 && s->midi_in_sysex) s->midi_in_sysex = false;
        if (m < 0xF8) {
            s->midi_status = m;
            s->midi_running_status = (m < 0xF0) ? m : 0;
            s->midi_param_count = 0;
        }
    } else {
        if (s->midi_param_count >= (int)sizeof(s->midi_param_bytes) || s->midi_status == 0) return;
        s->midi_param_bytes[s->midi_param_count++] = (char)(m & 0x7F);
        if (s->midi_in_sysex) return;
        int status_hi = s->midi_status >> 4;
        if (status_hi == 0xF) {
            return;
        }
        int channel = s->midi_status & 0x0F;
        int message_size = (status_hi == 0xC || status_hi == 0xD) ? 1 : 2;
        if (s->midi_param_count >= message_size && !s->midi_in_sysex) {
            if (channel >= 0 && channel < 32 && s->channels[channel].midi_enabled) {
                switch (status_hi) {
                case 0x8:
                    upd_note_off(s, channel, s->midi_param_bytes[0]);
                    break;
                case 0x9:
                    if (s->midi_param_bytes[1] > 0) upd_note_on(s, channel, s->midi_param_bytes[0]);
                    else upd_note_off(s, channel, s->midi_param_bytes[0]);
                    break;
                case 0xB:
                    if (s->midi_param_bytes[0] == 0x40) {
                        upd_control_chg_sustain(s, channel, s->midi_param_bytes[1] >= 0x40);
                    }
                    break;
                case 0xC:
                    upd_prog_chg(s, channel, s->midi_param_bytes[0]);
                    break;
                case 0xE:
                    upd_pitch_bend(s, channel, (s->midi_param_bytes[1] << 1) | (s->midi_param_bytes[1] >> 6));
                    break;
                default:
                    break;
                }
            }
            s->midi_param_count = 0;
            s->midi_status = s->midi_running_status;
        }
    }
}

static void upd_update_volume_envelopes(UPD937_Core *s) {
    s->delay_update_phase = (s->delay_update_phase + 1) & 1;
    for (int v = 0; v < 32; v++) {
        UPD937_VoiceState *vo = &s->voices[v];
        bool changed = false;
        if (vo->volume_env_delay > 0) {
            if (s->delay_update_phase == 0) vo->volume_env_delay--;
            if (vo->volume_env_delay > 0) continue;
            else if (vo->active) changed = true;
        }
        if (vo->volume_env_step < 16 && vo->volume > 0 && !vo->active) {
            vo->volume_env_step |= 16;
            changed = true;
        } else {
            if ((vo->volume <= vo->volume_target && vo->volume_down) ||
                (vo->volume >= vo->volume_target && !vo->volume_down)) {
                if (vo->volume_target > 0 && vo->volume_rate_mul != 0) {
                    vo->volume_env_step = ((vo->volume_env_step + 1) & 15) + (vo->volume_env_step & 16);
                    changed = true;
                }
            }
        }
        bool already_reset = false;
        while (changed) {
            changed = false;
            int env_rate = upd_read_rom_8(s, (int)s->ptr_volenv + vo->volume_env * 64 + vo->volume_env_step * 2 + 0);
            int env_target = upd_read_rom_8(s, (int)s->ptr_volenv + vo->volume_env * 64 + vo->volume_env_step * 2 + 1);
            bool env_down = env_rate >= 128;
            env_rate &= 127;
            int env_volume_target = upd_read_rom_16(s, (int)s->ptr_voltable + env_target * 2);
            vo->volume_down = env_down;
            if (env_rate == 127) {
                vo->volume_rate_mul = 0xFFFF;
                vo->volume_rate_div = 1;
            } else if (env_rate == 0 && env_down) {
                vo->volume_rate_mul = 0;
                vo->volume_rate_div = 1;
            } else if (env_volume_target == 0 && !env_down && !already_reset) {
                vo->volume_env_step &= 16;
                already_reset = true;
                changed = true;
            } else {
                env_rate = (env_rate * 2) + 2;
                vo->volume_rate_mul = upd_read_rom_16(s, (int)s->ptr_ratetable + env_rate * 4 + 0);
                vo->volume_rate_div = upd_read_rom_8(s, (int)s->ptr_ratetable + env_rate * 4 + 2) + 1;
            }
            vo->volume_target = env_volume_target;
        }
    }
}

static void upd_update_pitch_envelopes(UPD937_Core *s) {
    for (int v = 0; v < 32; v++) {
        UPD937_VoiceState *vo = &s->voices[v];
        if (vo->volume == 0) continue;
        bool changed = false;
        if (vo->pitch_env_delay > 0) {
            vo->pitch_env_delay--;
            if (vo->pitch_env_delay > 0) continue;
            else changed = true;
        }
        if (vo->pitch_env_rate != 0) {
            vo->pitch_env_value += vo->pitch_env_rate;
            bool reached_target = false;
            if (vo->pitch_env_rate > 0) reached_target = vo->pitch_env_value >= vo->pitch_env_target;
            else reached_target = vo->pitch_env_value <= vo->pitch_env_target;
            if (reached_target) {
                vo->pitch_env_value = vo->pitch_env_target;
                vo->pitch_env_step++;
                if (vo->pitch_env_step >= 8) vo->pitch_env_step = 1;
                changed = true;
            }
        }
        bool already_looped = false;
        while (changed && vo->pitch_env_step < 8) {
            changed = false;
            int env_rate = upd_read_rom_16(s, (int)s->ptr_pitchenv + vo->pitch_env * 32 + vo->pitch_env_step * 4 + 0);
            int env_target = upd_read_rom_16(s, (int)s->ptr_pitchenv + vo->pitch_env * 32 + vo->pitch_env_step * 4 + 2);
            bool loop_flag = (env_rate & 0x2000) > 0;
            bool env_down = (env_rate & 0x1000) > 0;
            env_rate &= 0xFFF;
            if (loop_flag) {
                vo->pitch_env_step = env_rate & 7;
                changed = !already_looped;
                already_looped = true;
            } else {
                vo->pitch_env_rate = env_rate * (env_down ? -1 : 1);
                vo->pitch_env_target += env_target * (env_down ? -16 : 16);
            }
        }
    }
}

static int upd_get_free_voice(UPD937_Core *s, int c) {
    UPD937_ChannelState *ch = &s->channels[c];
    int ret = ch->first_voice + ch->allocate_next;
    for (int i = 0; i < ch->voice_count; i++) {
        if (!s->voices[ret].active) break;
        ch->allocate_next++;
        if (ch->allocate_next >= ch->voice_count) ch->allocate_next = 0;
        ret = ch->first_voice + ch->allocate_next;
    }
    ch->allocate_next++;
    if (ch->allocate_next >= ch->voice_count) ch->allocate_next = 0;
    return ret;
}

static void upd_note_on(UPD937_Core *s, int channel, int note) {
    if (channel < 0 || channel > 3) return;
    UPD937_ChannelState *ch = &s->channels[channel];
    note &= 127;
    int note_ranged = note;
    while (note_ranged < 36) note_ranged += 12;
    while (note_ranged > 96) note_ranged -= 12;
    int partial_addr = ch->partials_offset;
    int voices_per_note = ch->layered ? 4 : 2;
    int keymap_byte = (note_ranged - 36) / 2;
    int keymap_shift = ((note_ranged - 36) & 1) * 4;
    int keymap_val = (upd_read_rom_8(s, (int)s->ptr_keymaps + ch->keymap_no * 32 + keymap_byte) >> keymap_shift) & 0xF;
    partial_addr += keymap_val * voices_per_note * 3;
    partial_addr *= 2;
    for (int vn = 0; vn < voices_per_note; vn++) {
        UPD937_VoiceState *vo = &s->voices[upd_get_free_voice(s, channel)];
        vo->pitch_env = upd_read_rom_16(s, (int)s->ptr_partials + partial_addr + 0);
        vo->volume_env = upd_read_rom_16(s, (int)s->ptr_partials + partial_addr + 2);
        int sample_descriptor = upd_read_rom_16(s, (int)s->ptr_partials + partial_addr + 4);
        vo->sample_start = upd_read_rom_24(s, (int)s->ptr_sampdesc + sample_descriptor * 10 + 1);
        vo->sample_end = upd_read_rom_24(s, (int)s->ptr_sampdesc + sample_descriptor * 10 + 4);
        vo->sample_loop = upd_read_rom_24(s, (int)s->ptr_sampdesc + sample_descriptor * 10 + 7);
        vo->sample_ptr = vo->sample_start;
        vo->sample_fract = 0;
        vo->sample_last_val = 0;
        vo->note = note;
        int sample_note = upd_read_rom_8(s, (int)s->ptr_sampdesc + sample_descriptor * 10);
        if (sample_note > 0) vo->pitch = (note_ranged - sample_note) * 32;
        else vo->pitch = 0x200;
        vo->volume = 0;
        vo->volume_target = 0;
        vo->volume_rate_mul = 0;
        vo->volume_rate_div = 1;
        vo->volume_down = false;
        vo->volume_env_delay = 0;
        vo->volume_env_step = 0;
        int env_rate = upd_read_rom_8(s, (int)s->ptr_volenv + vo->volume_env * 64 + 0);
        int env_target = upd_read_rom_8(s, (int)s->ptr_volenv + vo->volume_env * 64 + 1);
        if (env_target == 0) {
            vo->volume_env_delay = env_rate + 1;
            vo->volume_env_step = 1;
        } else {
            vo->volume_down = env_rate >= 128;
            env_rate &= 127;
            vo->volume_target = upd_read_rom_16(s, (int)s->ptr_voltable + env_target * 2);
            if (env_rate == 127) {
                vo->volume_rate_mul = 0xFFFF;
                vo->volume_rate_div = 1;
            } else {
                env_rate = (env_rate * 2) + 2;
                vo->volume_rate_mul = upd_read_rom_16(s, (int)s->ptr_ratetable + env_rate * 4 + 0);
                vo->volume_rate_div = upd_read_rom_8(s, (int)s->ptr_ratetable + env_rate * 4 + 2) + 1;
            }
        }
        int pitch_initial = upd_read_rom_16(s, (int)s->ptr_pitchenv + vo->pitch_env * 32 + 0);
        pitch_initial = (pitch_initial & 0xFFF) * ((pitch_initial >= 0x1000) ? -1 : 1);
        vo->pitch_env_value = pitch_initial * 16;
        vo->pitch_env_target = pitch_initial * 16;
        vo->pitch_env_rate = 0;
        vo->pitch_env_delay = upd_read_rom_16(s, (int)s->ptr_pitchenv + vo->pitch_env * 32 + 2) + 1;
        vo->pitch_env_step = 1;
        vo->active = true;
        vo->sustained = false;
        partial_addr += 6;
    }
}

static void upd_note_off(UPD937_Core *s, int channel, int note) {
    if (channel < 0 || channel > 3) return;
    UPD937_ChannelState *ch = &s->channels[channel];
    note &= 127;
    int voices_per_note = ch->layered ? 4 : 2;
    for (int v = ch->first_voice; v < ch->first_voice + ch->voice_count; v += voices_per_note) {
        UPD937_VoiceState *vo = &s->voices[v];
        if (vo->note == note && vo->active && !vo->sustained) {
            for (int i = 0; i < voices_per_note; i++) {
                if (ch->sustain) s->voices[v + i].sustained = true;
                else s->voices[v + i].active = false;
            }
            break;
        }
    }
}

static void upd_prog_chg(UPD937_Core *s, int channel, int prog) {
    if (channel < 0 || channel > 3) return;
    UPD937_ChannelState *ch = &s->channels[channel];
    for (int v = ch->first_voice; v < ch->first_voice + ch->voice_count; v++) {
        s->voices[v].active = false;
        s->voices[v].sustained = false;
        s->voices[v].volume_rate_mul = (s->voices[v].volume + 511) / 512;
        s->voices[v].volume_rate_div = 1;
        s->voices[v].volume_target = 0;
        s->voices[v].volume_down = true;
        s->voices[v].volume_env_step = 16;
    }
    ch->allocate_next = 0;
    if (prog < 0 || prog > 109) return;
    prog = upd_midi_prog_to_bank(prog, 0);
    ch->instrument = prog;
    ch->partials_offset = upd_read_rom_16(s, (int)s->ptr_instdesc + prog * 4 + 0);
    ch->keymap_no = upd_read_rom_8(s, (int)s->ptr_instdesc + prog * 4 + 2);
    int flags = upd_read_rom_8(s, (int)s->ptr_instdesc + prog * 4 + 3);
    ch->layered = (flags & 0x10) > 0;
}

static void upd_pitch_bend(UPD937_Core *s, int channel, int bend_byte) {
    if (channel < 0 || channel > 3) return;
    UPD937_ChannelState *ch = &s->channels[channel];
    ch->bend_value = bend_byte - 128;
    ch->bend_offset = upd_read_rom_8(s, (int)s->ptr_ratetable + bend_byte * 4 + 3) - 128;
}

static void upd_control_chg_sustain(UPD937_Core *s, int channel, bool sustain) {
    if (channel < 0 || channel > 3) return;
    UPD937_ChannelState *ch = &s->channels[channel];
    ch->sustain = sustain;
    if (!sustain) {
        for (int i = ch->first_voice; i < ch->first_voice + ch->voice_count; i++) {
            if (s->voices[i].sustained) {
                s->voices[i].sustained = false;
                s->voices[i].active = false;
            }
        }
    }
}

static int upd_midi_prog_to_bank(int prog, int bank_select) {
    if (prog < 10) return prog + (bank_select * 10);
    return prog - 10 + bank_select * 100 + HC_NUM_BANKS * 10;
}

static void filter_update_coefficients(BiquadStereoFilter *f) {
    const float PI = 3.14159265358979323846f;
    float K = tanf(PI * f->fc / f->fs);
    float W = K * K;
    float alpha = 1.0f + (K / f->q) + W;
    f->a1 = 2.0f * (W - 1.0f) / alpha;
    f->a2 = (1.0f - (K / f->q) + W) / alpha;
    if (f->hp) {
        f->b0 = 1.0f / alpha;
        f->b2 = f->b0;
        f->b1 = -2.0f * f->b0;
    } else {
        f->b0 = W / alpha;
        f->b2 = f->b0;
        f->b1 = 2.0f * f->b0;
    }
}

static void filter_reset(BiquadStereoFilter *f) {
    for (int c = 0; c < 2; c++) f->x1[c] = f->x2[c] = f->y1[c] = f->y2[c] = 0.0f;
}

static void filter_init(BiquadStereoFilter *f, float fs, float fc, float q, bool hp) {
    memset(f, 0, sizeof(*f));
    filter_reset(f);
    f->fs = fs;
    f->fc = fc;
    f->q = q;
    f->hp = hp;
    filter_update_coefficients(f);
}

static void filter_process(BiquadStereoFilter *f, float sample[2]) {
    for (int c = 0; c < 2; c++) {
        float x0 = sample[c];
        float y0 = f->b0 * x0 + f->b1 * f->x1[c] + f->b2 * f->x2[c] - f->a1 * f->y1[c] - f->a2 * f->y2[c];
        f->x2[c] = f->x1[c];
        f->x1[c] = x0;
        f->y2[c] = f->y1[c];
        f->y1[c] = y0;
        sample[c] = y0;
    }
}

LoopySoundEngine *loopy_sound_engine_create(const uint8_t *rom, size_t rom_size, float out_rate, int buffer_size) {
    if (!rom || rom_size == 0) return NULL;
    LoopySoundEngine *e = (LoopySoundEngine *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->out_rate = out_rate;
    e->synth_rate = TUNING * 192.0f;
    e->mix_level = MIX_LEVEL;
    e->buffer_size = buffer_size;
    printf("[Sound] Init uPD937 core: synth rate %.01f, out rate %.01f, buffer size %d\n", e->synth_rate, out_rate, buffer_size);
    if (!upd_init(&e->synth, rom, rom_size, e->synth_rate)) {
        free(e);
        return NULL;
    }
#if FILTER_ENABLE
    printf("[Sound] Init filters\n");
    filter_init(&e->filter_tone, e->synth_rate, FILTER_CUTOFF, FILTER_RESONANCE, false);
    filter_init(&e->filter_block_dc, out_rate, 20.0f, 0.7f, true);
    e->has_filter_tone = true;
    e->has_filter_block_dc = true;
#endif
    return e;
}

void loopy_sound_engine_destroy(LoopySoundEngine *e) {
    if (!e) return;
    upd_destroy(&e->synth);
    free(e);
}

void loopy_sound_engine_gen_sample(LoopySoundEngine *e, float out[2]) {
    if (!e) { out[0] = out[1] = 0.0f; return; }
    if ((e->out_sample_count & 63) == 0) engine_handle_midi_event(e);
    e->interpolation_step += e->synth_rate / e->out_rate;
    while (e->interpolation_step >= 1.0f) {
        e->last_sample[0] = e->current_sample[0];
        e->last_sample[1] = e->current_sample[1];
        upd_gen_sample(&e->synth, e->raw_samples);
        e->current_sample[0] = e->raw_samples[0] / 32768.0f;
        e->current_sample[1] = e->raw_samples[1] / 32768.0f;
        if (e->has_filter_tone) filter_process(&e->filter_tone, e->current_sample);
        e->interpolation_step -= 1.0f;
    }
    e->mix_sample[0] = (e->last_sample[0] + (e->current_sample[0] - e->last_sample[0]) * e->interpolation_step) * 6.8f * e->mix_level;
    e->mix_sample[1] = (e->last_sample[1] + (e->current_sample[1] - e->last_sample[1]) * e->interpolation_step) * 6.8f * e->mix_level;
    if (e->has_filter_block_dc) filter_process(&e->filter_block_dc, e->mix_sample);
    out[0] = clampf(e->mix_sample[0], -1.0f, 1.0f);
    out[1] = clampf(e->mix_sample[1], -1.0f, 1.0f);
    e->out_sample_count++;
}

void loopy_sound_engine_set_channel_muted(LoopySoundEngine *e, int channel, bool mute) {
    if (!e) return;
    upd_set_channel_muted(&e->synth, channel, mute);
}

void loopy_sound_engine_time_reference(LoopySoundEngine *e, float delta) {
    if (!e) return;
    e->has_time_reference = true;
    if (delta > 0.0f) {
        int delta_samples = (int)floorf(delta * e->out_rate);
        e->time_reference_samples += delta_samples;
    }
    int clamp_range = 2 * e->buffer_size;
    e->time_reference_samples = clampi(e->time_reference_samples, e->out_sample_count, e->out_sample_count + clamp_range);
    e->time_reference_samples += (e->out_sample_count + e->buffer_size - e->time_reference_samples + 32) >> 6;
}

void loopy_sound_engine_set_control_register(LoopySoundEngine *e, int creg) {
    if (!e) return;
    creg &= 0xFFF;
    int vol_sw_0 = (creg >> 6) & 7;
    int vol_sw_1 = (creg >> 9) & 7;
    if ((vol_sw_0 & 1) > 0) upd_set_volume_slider(&e->synth, 0, 2);
    else if ((vol_sw_0 & 2) > 0) upd_set_volume_slider(&e->synth, 0, 3);
    else if ((vol_sw_0 & 4) > 0) upd_set_volume_slider(&e->synth, 0, 4);
    if ((vol_sw_1 & 1) > 0) upd_set_volume_slider(&e->synth, 1, 2);
    else if ((vol_sw_1 & 2) > 0) upd_set_volume_slider(&e->synth, 1, 3);
    else if ((vol_sw_1 & 4) > 0) upd_set_volume_slider(&e->synth, 1, 4);
    int buttons = creg & 63;
    int buttons_pushed = buttons & (~e->buttons_last);
    e->buttons_last = buttons;
    if ((buttons_pushed & 16) > 0) {
        e->channel_config_state = 0;
        upd_set_channel_configuration(&e->synth, false, false);
        upd_reset_channels(&e->synth, true);
    }
    if ((buttons_pushed & 1) > 0) {
        e->in_demo = !e->in_demo;
        if (e->in_demo) upd_reset_channels(&e->synth, false);
    }
    if ((buttons_pushed & 32) > 0 && e->channel_config_state == 0) {
        e->channel_config_state = 1;
        upd_set_channel_configuration(&e->synth, false, false);
        upd_reset_channels(&e->synth, true);
    }
    if ((buttons_pushed & 8) > 0) {
        /* External rhythm button; not implemented in original either. */
    }
    if ((buttons_pushed & 4) > 0 && (e->channel_config_state == 1 || e->channel_config_state == 3)) {
        upd_set_channel_configuration(&e->synth, true, true);
        upd_reset_channels(&e->synth, false);
        e->channel_config_state = 4;
    }
    if ((buttons_pushed & 2) > 0 && e->channel_config_state == 1) {
        upd_set_channel_configuration(&e->synth, true, false);
        upd_reset_channels(&e->synth, false);
        e->channel_config_state = 3;
    }
}

bool loopy_sound_engine_midi_in(LoopySoundEngine *e, char b) {
    if (!e) return true;
    if (e->in_demo || e->channel_config_state == 0) return true;
    int timestamp = e->time_reference_samples;
    if ((e->queue_write + 1) % MIDI_QUEUE_CAPACITY == e->queue_read) {
        if (!e->midi_overflowed) printf("[Sound] MIDI queue overflow, increase queue capacity or send smaller groups more often.\n");
        e->midi_overflowed = true;
        return false;
    }
    e->midi_overflowed = false;
    e->midi_queue_bytes[e->queue_write] = b;
    e->midi_queue_timestamps[e->queue_write] = timestamp;
    e->queue_write = (e->queue_write + 1) % MIDI_QUEUE_CAPACITY;
    return true;
}

static void engine_handle_midi_event(LoopySoundEngine *e) {
    while (e->queue_write != e->queue_read) {
        int event_time = e->midi_queue_timestamps[e->queue_read];
        int time_diff = event_time - e->out_sample_count;
        if (e->has_time_reference && time_diff > 0) break;
        char event_byte = e->midi_queue_bytes[e->queue_read];
        e->queue_read = (e->queue_read + 1) % MIDI_QUEUE_CAPACITY;
        upd_process_midi_now(&e->synth, event_byte);
    }
}
uint32_t loopy_sound_engine_state_blob_size(void) { return (uint32_t)sizeof(LoopySoundEngine); }

void loopy_sound_engine_get_state_blob(LoopySoundEngine *engine, void *dst, uint32_t size) {
    if (!engine || !dst || size != sizeof(LoopySoundEngine)) return;
    memcpy(dst, engine, sizeof(LoopySoundEngine));
}

void loopy_sound_engine_set_state_blob(LoopySoundEngine *engine, const void *src, uint32_t size) {
    if (!engine || !src || size != sizeof(LoopySoundEngine)) return;
    uint8_t *rom = engine->synth.rom;
    int rom_mask = engine->synth.rom_mask;
    memcpy(engine, src, sizeof(LoopySoundEngine));
    engine->synth.rom = rom;
    engine->synth.rom_mask = rom_mask;
}

