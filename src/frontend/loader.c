#include "frontend/loader.h"
#include "common/bswp.h"
#include "core/cart_meta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *remove_extension_alloc(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (dot && slash && dot < slash) dot = NULL;
    size_t n = dot ? (size_t)(dot - path) : strlen(path);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

void loopy_config_free(ConfigSystemInfo *config) {
    if (!config) return;
    byte_buffer_free(&config->cart.rom);
    byte_buffer_free(&config->cart.sram);
    free(config->cart.sram_file_path);
    byte_buffer_free(&config->bios_rom);
    byte_buffer_free(&config->sound_rom);
    byte_buffer_free(&config->oki_adpcm_rom);
    memset(config, 0, sizeof(*config));
}

int loopy_parse_common_args(int argc, char **argv, LoopyLaunchInfo *out, int default_frames) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->frames = default_frames;
    out->wanwan_replacement_pcm_enabled = 1;
    out->y4m_start = 1;
    out->y4m_step = 1;
    out->lr_hold_left_start = 1;
    out->lr_mouse_dx = -2;
    out->cpu_prof_top = 20;
    if (argc >= 3 && strcmp(argv[1], "--replay-cmdlist") == 0) {
        out->replay_cmdlist_path = argv[2];
        int argi = 3;
        while (argi < argc) {
            if (strcmp(argv[argi], "--frames") == 0 && argi + 1 < argc) {
                out->frames = atoi(argv[argi + 1]);
                out->frames_set = 1;
                argi += 2;
            } else if (strcmp(argv[argi], "--printer-output-dir") == 0 && argi + 1 < argc) {
                out->printer_output_dir = argv[argi + 1];
                argi += 2;
            } else if (strcmp(argv[argi], "--printer-trace") == 0) {
                out->printer_trace = 1;
                argi++;
            } else {
                fprintf(stderr, "Unknown argument: %s\n", argv[argi]);
                return -1;
            }
        }
        if (out->frames_set && out->frames < 1) out->frames = 1;
        return 0;
    }
    if (argc < 3) return -1;
    out->cart_path = argv[1];
    out->bios_path = argv[2];
    int argi = 3;
    if (argi < argc && strncmp(argv[argi], "--", 2) != 0) out->sound_path = argv[argi++];
    if (argi < argc && strncmp(argv[argi], "--", 2) != 0) out->oki_adpcm_path = argv[argi++];
    while (argi < argc) {
        if (strcmp(argv[argi], "--mcp") == 0) {
            out->mcp = 1;
            argi++;
        } else if (strcmp(argv[argi], "--list-regions") == 0) {
            out->list_regions = 1;
            argi++;
        } else if (strcmp(argv[argi], "--disasm") == 0 && argi + 1 < argc) {
            out->disasm_spec = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--dump") == 0 && argi + 2 < argc) {
            if (out->dump_count >= LOOPY_MAX_DUMPS) {
                fprintf(stderr, "Too many --dump requests (max %d)\n", LOOPY_MAX_DUMPS);
                return -1;
            }
            out->dumps[out->dump_count].region = argv[argi + 1];
            out->dumps[out->dump_count].path = argv[argi + 2];
            out->dump_count++;
            argi += 3;
        } else if (strcmp(argv[argi], "--bus-prof") == 0) {
            out->bus_prof = 1;
            argi++;
        } else if (strcmp(argv[argi], "--bus-prof-per-frame") == 0) {
            out->bus_prof = 1;
            out->bus_prof_per_frame = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cpu-prof") == 0) {
            out->cpu_prof = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cpu-prof-top") == 0 && argi + 1 < argc) {
            out->cpu_prof = 1;
            out->cpu_prof_top = atoi(argv[argi + 1]);
            if (out->cpu_prof_top < 1) out->cpu_prof_top = 20;
            argi += 2;
        } else if (strcmp(argv[argi], "--bios-trace") == 0) {
            out->bios_trace = 1;
            argi++;
        } else if (strcmp(argv[argi], "--frames") == 0 && argi + 1 < argc) {
            out->frames = atoi(argv[argi + 1]);
            out->frames_set = 1;
            argi += 2;
        } else if (strcmp(argv[argi], "--headless") == 0) {
            out->headless = 1;
            argi++;
        } else if (strcmp(argv[argi], "--record-cmdlist") == 0 && argi + 1 < argc) {
            out->record_cmdlist_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--record-y4m") == 0 && argi + 1 < argc) {
            out->record_y4m_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--record-wav") == 0 && argi + 1 < argc) {
            out->record_wav_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--load-state") == 0 && argi + 1 < argc) {
            out->load_state_path = argv[argi + 1];
            argi += 2;
        } else if ((strcmp(argv[argi], "--oki-rom") == 0 || strcmp(argv[argi], "--oki-adpcm-rom") == 0) && argi + 1 < argc) {
            out->oki_adpcm_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--wanwan-oki-montage-wav") == 0 && argi + 1 < argc) {
            out->wanwan_oki_montage_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--no-wanwan-internal-pcm") == 0 ||
                   strcmp(argv[argi], "--disable-wanwan-internal-pcm") == 0 ||
                   strcmp(argv[argi], "--no-wanwan-replacement-pcm") == 0) {
            out->wanwan_replacement_pcm_enabled = 0;
            argi++;
        } else if (strcmp(argv[argi], "--wanwan-internal-pcm") == 0 ||
                   strcmp(argv[argi], "--enable-wanwan-internal-pcm") == 0 ||
                   strcmp(argv[argi], "--wanwan-replacement-pcm") == 0) {
            out->wanwan_replacement_pcm_enabled = 1;
            argi++;
        } else if (strcmp(argv[argi], "--no-idle-skip") == 0) {
            out->idle_skip_mode = LOOPY_IDLE_SKIP_OFF;
            argi++;
        } else if (strcmp(argv[argi], "--idle-skip") == 0) {
            out->idle_skip_mode = LOOPY_IDLE_SKIP_ON;
            argi++;
        } else if (strcmp(argv[argi], "--y4m-start") == 0 && argi + 1 < argc) {
            out->y4m_start = atoi(argv[argi + 1]);
            if (out->y4m_start < 1) out->y4m_start = 1;
            argi += 2;
        } else if (strcmp(argv[argi], "--y4m-step") == 0 && argi + 1 < argc) {
            out->y4m_step = atoi(argv[argi + 1]);
            if (out->y4m_step < 1) out->y4m_step = 1;
            argi += 2;
        } else if (strcmp(argv[argi], "--auto-cascadefx") == 0) {
            out->auto_cascadefx = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cascadefx-watch") == 0) {
            out->cascadefx_watch = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cascadefx-prof") == 0) {
            out->cascadefx_prof = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cascadefx-dpad-test") == 0) {
            out->cascadefx_dpad_test = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cascadefx-dpad-prehold-test") == 0) {
            out->cascadefx_dpad_prehhold_test = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cascadefx-dpad-tap-test") == 0) {
            out->cascadefx_dpad_tap_test = 1;
            argi++;
        } else if (strcmp(argv[argi], "--cascadefx-pad-trace") == 0) {
            out->cascadefx_pad_trace = 1;
            argi++;
        } else if (strcmp(argv[argi], "--auto-anarch") == 0) {
            out->auto_anarch = 1;
            argi++;
        } else if (strcmp(argv[argi], "--anarch-prof") == 0) {
            out->anarch_prof = 1;
            argi++;
        } else if (strcmp(argv[argi], "--lr-hold-left") == 0) {
            out->lr_hold_left = 1;
            argi++;
        } else if (strcmp(argv[argi], "--lr-hold-left-start") == 0 && argi + 1 < argc) {
            out->lr_hold_left_start = atoi(argv[argi + 1]);
            if (out->lr_hold_left_start < 1) out->lr_hold_left_start = 1;
            argi += 2;
        } else if (strcmp(argv[argi], "--lr-mouse-dx") == 0 && argi + 1 < argc) {
            out->lr_mouse_dx = atoi(argv[argi + 1]);
            argi += 2;
        } else if (strcmp(argv[argi], "--printer-output-dir") == 0 && argi + 1 < argc) {
            out->printer_output_dir = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--printer-trace") == 0) {
            out->printer_trace = 1;
            argi++;
        } else if (strcmp(argv[argi], "--mouse") == 0) {
            out->input_device = 1;
            out->input_device_set = 1;
            argi++;
        } else if (strcmp(argv[argi], "--gamepad") == 0) {
            out->input_device = 0;
            out->input_device_set = 1;
            argi++;
        } else if (strcmp(argv[argi], "--device") == 0 && argi + 1 < argc) {
            if (strcmp(argv[argi + 1], "mouse") == 0) {
                out->input_device = 1;
                out->input_device_set = 1;
            } else if (strcmp(argv[argi + 1], "gamepad") == 0 || strcmp(argv[argi + 1], "pad") == 0) {
                out->input_device = 0;
                out->input_device_set = 1;
            } else {
                fprintf(stderr, "Unknown input device: %s\n", argv[argi + 1]);
                return -1;
            }
            argi += 2;
        } else if (strncmp(argv[argi], "--", 2) != 0) {
            /* Accept remaining bare ROM paths as positional arguments even if
             * they appear after frontend options.  This keeps SDL3/headless
             * tolerant of the documented form:
             *   <game ROM> <BIOS> [sound BIOS] [OKI ADPCM ROM]
             * and prevents the optional OKI ROM from being rejected as an
             * unknown argument. */
            if (!out->sound_path) {
                out->sound_path = argv[argi++];
            } else if (!out->oki_adpcm_path) {
                out->oki_adpcm_path = argv[argi++];
            } else {
                fprintf(stderr, "Unknown argument: %s\n", argv[argi]);
                return -1;
            }
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[argi]);
            return -1;
        }
    }
    if ((out->frames_set || default_frames > 0) && out->frames < 1) out->frames = 1;
    return 0;
}

int loopy_load_config(const LoopyLaunchInfo *launch, ConfigSystemInfo *config) {
    if (!launch || !config || !launch->cart_path || !launch->bios_path) return -1;
    memset(config, 0, sizeof(*config));

    if (byte_buffer_load_file(&config->cart.rom, launch->cart_path) != 0) {
        fprintf(stderr, "Failed to open %s\n", launch->cart_path);
        loopy_config_free(config);
        return -1;
    }
    if (byte_buffer_load_file(&config->bios_rom, launch->bios_path) != 0) {
        fprintf(stderr, "Failed to open %s\n", launch->bios_path);
        loopy_config_free(config);
        return -1;
    }
    if (launch->sound_path && byte_buffer_load_file(&config->sound_rom, launch->sound_path) != 0) {
        fprintf(stderr, "Failed to open %s\n", launch->sound_path);
        loopy_config_free(config);
        return -1;
    }
    if (launch->oki_adpcm_path && byte_buffer_load_file(&config->oki_adpcm_rom, launch->oki_adpcm_path) != 0) {
        fprintf(stderr, "Failed to open %s\n", launch->oki_adpcm_path);
        loopy_config_free(config);
        return -1;
    }

    if (config->cart.rom.size < 0x18) {
        fprintf(stderr, "ROM is too small to contain a Loopy header\n");
        loopy_config_free(config);
        return -1;
    }
    if (config->bios_rom.size < 0x8000) {
        fprintf(stderr, "BIOS is too small; loopy_bios.bin is mandatory\n");
        loopy_config_free(config);
        return -1;
    }

    config->cart_is_wanwan = loopy_cart_rom_is_wanwan(config->cart.rom.data, config->cart.rom.size);
    config->cart_is_official = loopy_cart_rom_is_official(config->cart.rom.data, config->cart.rom.size);
    config->wanwan_replacement_pcm_enabled = launch->wanwan_replacement_pcm_enabled ? 1 : 0;
    config->idle_skip_mode = launch->idle_skip_mode;

    uint32_t sram_start, sram_end;
    memcpy(&sram_start, config->cart.rom.data + 0x10, 4);
    memcpy(&sram_end, config->cart.rom.data + 0x14, 4);
    uint32_t sram_size = common_bswp32(sram_end) - common_bswp32(sram_start) + 1;

    char *base = remove_extension_alloc(launch->cart_path);
    if (!base) { loopy_config_free(config); return -1; }
    size_t sav_len = strlen(base) + 5;
    config->cart.sram_file_path = (char *)malloc(sav_len);
    if (!config->cart.sram_file_path) { free(base); loopy_config_free(config); return -1; }
    snprintf(config->cart.sram_file_path, sav_len, "%s.sav", base);

    if (byte_buffer_load_file(&config->cart.sram, config->cart.sram_file_path) != 0) {
        printf("Warning: SRAM not found\n");
        byte_buffer_init(&config->cart.sram);
    } else {
        printf("Successfully found SRAM\n");
    }
    if (byte_buffer_resize(&config->cart.sram, sram_size, 0xFF) != 0) {
        free(base);
        loopy_config_free(config);
        return -1;
    }
    free(base);
    return 0;
}

char *loopy_make_state_path(const char *cart_path, int slot) {
    char *base = remove_extension_alloc(cart_path ? cart_path : "loopy");
    if (!base) return NULL;
    char suffix[32];
    snprintf(suffix, sizeof(suffix), ".ls%d", slot);
    size_t n = strlen(base) + strlen(suffix) + 1;
    char *out = (char *)malloc(n);
    if (!out) { free(base); return NULL; }
    snprintf(out, n, "%s%s", base, suffix);
    free(base);
    return out;
}
