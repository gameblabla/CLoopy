#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include "core/system.h"
#include "core/cart.h"
#include "core/loopy_io.h"
#include "core/timing.h"
#include "frontend/cmdlist.h"
#include "frontend/loader.h"
#include "input/input.h"
#include "sound/sound.h"
#include "video/video.h"
#include "video/render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE_W VIDEO_DISPLAY_WIDTH
#define BASE_H VIDEO_DISPLAY_HEIGHT
#define MAX_CONTROLLERS 4
#define OVERLAY_MS 1500u

typedef enum ScaleModeUI { SCALE_INTEGER_NEAREST = 0, SCALE_NEAREST, SCALE_LINEAR, SCALE_BICUBIC, SCALE_COUNT } ScaleModeUI;

typedef struct Binding {
    const char *name;
    PadButton pad;
    SDL_Keycode key;
    SDL_GamepadButton gamepad_button;
} Binding;

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *frame_tex;
    SDL_Texture *bicubic_tex;
    uint32_t *bicubic_pixels;
    int bicubic_w, bicubic_h;
    SDL_Gamepad *pads[MAX_CONTROLLERS];
    Binding bind[11];
    ScaleModeUI scale_mode;
    bool integer_scale;
    bool menu_open;
    int menu_index;
    bool remap_mode;
    int remap_index;
    int state_slot;
    uint64_t overlay_until;
    uint64_t menu_shortcuts_until;
    char message[160];
    const char *cart_path;
    bool running;
    bool axis_left, axis_right, axis_up, axis_down;
    bool fullscreen;
    bool mouse_left_down;
    bool mouse_capture;
    bool window_focused;
    bool mouse_abs_valid;
    float mouse_last_x;
    float mouse_last_y;
    float mouse_frac_x;
    float mouse_frac_y;
    ConfigSystemInfo *config;
    LoopyCmdListWriter cmd_writer;
    bool cmd_writer_open;
    uint32_t emu_frame;
    uint32_t frame_limit;
    bool fast_forward_hold;
    int fast_forward_level;
} App;

typedef struct ReplayApp {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *frame_tex;
    LoopyCmdListReader reader;
    uint16_t *pixels;
    uint32_t frame_index;
    bool running;
    bool playing;
    bool fullscreen;
    bool vdp_replay_initialized;
    bool rendered_from_state;
    uint64_t last_advance;
    char message[160];
    uint64_t overlay_until;
} ReplayApp;

static void set_message(App *app, const char *msg) {
    snprintf(app->message, sizeof(app->message), "%s", msg ? msg : "");
    app->overlay_until = SDL_GetTicks() + OVERLAY_MS;
}

static void set_message_ms(App *app, const char *msg, uint32_t ms) {
    snprintf(app->message, sizeof(app->message), "%s", msg ? msg : "");
    app->overlay_until = SDL_GetTicks() + ms;
}

static void reset_mouse_motion_accumulator(App *app) {
    app->mouse_abs_valid = false;
    app->mouse_last_x = 0.0f;
    app->mouse_last_y = 0.0f;
    app->mouse_frac_x = 0.0f;
    app->mouse_frac_y = 0.0f;
}

static void release_emulated_mouse_buttons(App *app) {
    (void)app;
    input_set_mouse_button(0, false);
    input_set_mouse_button(1, false);
}

static void feed_mouse_delta_float(App *app, float dx, float dy) {
    if (input_get_port_device() != INPUT_PORT_MOUSE) return;
    app->mouse_frac_x += dx;
    app->mouse_frac_y += dy;

    int ix = 0;
    int iy = 0;
    if (app->mouse_frac_x >= 1.0f || app->mouse_frac_x <= -1.0f) {
        ix = (int)(app->mouse_frac_x > 0.0f ? floorf(app->mouse_frac_x) : ceilf(app->mouse_frac_x));
        app->mouse_frac_x -= (float)ix;
    }
    if (app->mouse_frac_y >= 1.0f || app->mouse_frac_y <= -1.0f) {
        iy = (int)(app->mouse_frac_y > 0.0f ? floorf(app->mouse_frac_y) : ceilf(app->mouse_frac_y));
        app->mouse_frac_y -= (float)iy;
    }
    if (ix || iy) input_add_mouse_delta(ix, iy);
}

static bool should_capture_mouse(App *app) {
    return app->mouse_capture &&
           app->window_focused &&
           input_get_port_device() == INPUT_PORT_MOUSE &&
           !app->menu_open &&
           !app->remap_mode;
}

static void sync_mouse_capture(App *app, bool notify) {
    if (!app->window) return;
    bool want = should_capture_mouse(app);
    bool have = SDL_GetWindowRelativeMouseMode(app->window);
    if (have == want) {
        if (want) {
            SDL_SetWindowMouseGrab(app->window, true);
            SDL_CaptureMouse(true);
        }
        return;
    }

    reset_mouse_motion_accumulator(app);
    release_emulated_mouse_buttons(app);

    if (!SDL_SetWindowRelativeMouseMode(app->window, want)) {
        if (want) {
            app->mouse_capture = false;
            SDL_SetWindowMouseGrab(app->window, false);
            SDL_CaptureMouse(false);
            set_message(app, "Mouse capture failed");
        }
        return;
    }

    SDL_SetWindowMouseGrab(app->window, want);
    SDL_CaptureMouse(want);

    if (want) {
        SDL_HideCursor();
        float rx = 0.0f, ry = 0.0f;
        (void)SDL_GetRelativeMouseState(&rx, &ry); /* discard stale motion accumulated before capture */
        if (notify) set_message_ms(app, "Mouse captured - Esc menu, Ctrl+M release", 2600);
    } else {
        SDL_ShowCursor();
        if (notify && input_get_port_device() == INPUT_PORT_MOUSE && !app->menu_open && !app->remap_mode) {
            set_message(app, "Mouse released - Ctrl+M captures");
        }
    }
}

static void resume_game(App *app) {
    sound_set_paused(false);
    app->menu_open = false;
    app->remap_mode = false;
    sync_mouse_capture(app, true);
    if (!should_capture_mouse(app)) set_message(app, "Press Escape for menu");
}

static void set_default_bindings(App *app) {
    app->bind[0] = (Binding){"Start", PAD_START, SDLK_RETURN, SDL_GAMEPAD_BUTTON_START};
    app->bind[1] = (Binding){"A", PAD_A, 'z', SDL_GAMEPAD_BUTTON_SOUTH};
    app->bind[2] = (Binding){"B", PAD_B, 'x', SDL_GAMEPAD_BUTTON_EAST};
    app->bind[3] = (Binding){"C", PAD_C, 'a', SDL_GAMEPAD_BUTTON_WEST};
    app->bind[4] = (Binding){"D", PAD_D, 's', SDL_GAMEPAD_BUTTON_NORTH};
    app->bind[5] = (Binding){"L", PAD_L1, 'q', SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    app->bind[6] = (Binding){"R", PAD_R1, 'w', SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    app->bind[7] = (Binding){"Up", PAD_UP, SDLK_UP, SDL_GAMEPAD_BUTTON_DPAD_UP};
    app->bind[8] = (Binding){"Down", PAD_DOWN, SDLK_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN};
    app->bind[9] = (Binding){"Left", PAD_LEFT, SDLK_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT};
    app->bind[10] = (Binding){"Right", PAD_RIGHT, SDLK_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
}

static void apply_bindings(App *app) {
    input_clear_key_bindings();
    for (size_t i = 0; i < sizeof(app->bind)/sizeof(app->bind[0]); i++) input_add_key_binding((int)app->bind[i].key, app->bind[i].pad);
}

static const char *scale_name(ScaleModeUI m) {
    switch (m) {
    case SCALE_INTEGER_NEAREST: return "Integer nearest";
    case SCALE_NEAREST: return "Nearest";
    case SCALE_LINEAR: return "Linear";
    case SCALE_BICUBIC: return "Bicubic";
    default: return "Unknown";
    }
}

static int fast_forward_multiplier(const App *app) {
    static const int locked_speeds[] = { 1, 2, 4, 8 };
    int m = 1;
    if (app) {
        int level = app->fast_forward_level;
        if (level < 0) level = 0;
        if (level > 3) level = 3;
        m = locked_speeds[level];
        if (app->fast_forward_hold && m < 4) m = 4;
    }
    return m;
}

static void fast_forward_message(App *app) {
    char msg[64];
    int m = fast_forward_multiplier(app);
    if (m <= 1) snprintf(msg, sizeof(msg), "Fast forward off");
    else snprintf(msg, sizeof(msg), "Fast forward %dx", m);
    set_message(app, msg);
}

static void save_ui_config(App *app) {
    FILE *f = fopen("loopy_sdl3.cfg", "w");
    if (!f) return;
    fprintf(f, "scale_mode %d\ninteger_scale %d\nport_device %d\nmouse_capture %d\n", (int)app->scale_mode, app->integer_scale ? 1 : 0, (int)input_get_port_device(), app->mouse_capture ? 1 : 0);
    for (size_t i = 0; i < sizeof(app->bind)/sizeof(app->bind[0]); i++) fprintf(f, "key %s %u\n", app->bind[i].name, (unsigned)app->bind[i].key);
    fclose(f);
}

static bool load_ui_config(App *app) {
    FILE *f = fopen("loopy_sdl3.cfg", "r");
    if (!f) return false;
    char key[64], name[64];
    int value;
    while (fscanf(f, "%63s", key) == 1) {
        if (strcmp(key, "scale_mode") == 0 && fscanf(f, "%d", &value) == 1) {
            if (value >= 0 && value < SCALE_COUNT) app->scale_mode = (ScaleModeUI)value;
        } else if (strcmp(key, "integer_scale") == 0 && fscanf(f, "%d", &value) == 1) {
            app->integer_scale = value != 0;
        } else if (strcmp(key, "port_device") == 0 && fscanf(f, "%d", &value) == 1) {
            input_set_port_device(value == INPUT_PORT_MOUSE ? INPUT_PORT_MOUSE : INPUT_PORT_GAMEPAD);
        } else if (strcmp(key, "mouse_capture") == 0 && fscanf(f, "%d", &value) == 1) {
            app->mouse_capture = value != 0;
        } else if (strcmp(key, "key") == 0 && fscanf(f, "%63s %d", name, &value) == 2) {
            for (size_t i = 0; i < sizeof(app->bind)/sizeof(app->bind[0]); i++) if (strcmp(name, app->bind[i].name) == 0) app->bind[i].key = (SDL_Keycode)value;
        } else {
            char discard[256];
            (void)fgets(discard, sizeof(discard), f);
        }
    }
    fclose(f);
    return true;
}

static bool init_sdl(App *app) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL3 init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    app->window = SDL_CreateWindow("CLoopy SDL3", BASE_W * 3, BASE_H * 3, SDL_WINDOW_RESIZABLE);
    if (!app->window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return false; }
    app->renderer = SDL_CreateRenderer(app->window, NULL);
    if (!app->renderer) { fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError()); return false; }
    app->frame_tex = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB1555, SDL_TEXTUREACCESS_STREAMING, BASE_W, BASE_H);
    if (!app->frame_tex) { fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError()); return false; }
    SDL_SetTextureScaleMode(app->frame_tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(app->frame_tex, SDL_BLENDMODE_NONE);
    app->window_focused = true;
    return true;
}

static void close_sdl(App *app) {
    for (int i = 0; i < MAX_CONTROLLERS; i++) if (app->pads[i]) SDL_CloseGamepad(app->pads[i]);
    free(app->bicubic_pixels);
    if (app->bicubic_tex) SDL_DestroyTexture(app->bicubic_tex);
    if (app->frame_tex) SDL_DestroyTexture(app->frame_tex);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    SDL_Quit();
}

static SDL_FRect calc_viewport(App *app) {
    int ww = 0, wh = 0;
    SDL_GetRenderOutputSize(app->renderer, &ww, &wh);
    float scale_x = (float)ww / (float)BASE_W;
    float scale_y = (float)wh / (float)BASE_H;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    if (app->integer_scale || app->scale_mode == SCALE_INTEGER_NEAREST) {
        int iscale = (int)floorf(scale);
        if (iscale < 1) iscale = 1;
        scale = (float)iscale;
    }
    float w = BASE_W * scale;
    float h = BASE_H * scale;
    return (SDL_FRect){ floorf(((float)ww - w) * 0.5f), floorf(((float)wh - h) * 0.5f), floorf(w), floorf(h) };
}

static uint32_t rgb555_to_argb(uint16_t p) {
    uint32_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static float cubic(float v0, float v1, float v2, float v3, float t) {
    float p = (v3 - v2) - (v0 - v1);
    float q = (v0 - v1) - p;
    float r = v2 - v0;
    return p * t * t * t + q * t * t + r * t + v1;
}

static uint32_t sample_bicubic(const uint16_t *src, float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float tx = x - ix, ty = y - iy;
    float chan[3][4];
    for (int m = -1; m <= 2; m++) {
        int yy = iy + m; if (yy < 0) yy = 0; if (yy >= BASE_H) yy = BASE_H - 1;
        float row[3][4];
        for (int n = -1; n <= 2; n++) {
            int xx = ix + n; if (xx < 0) xx = 0; if (xx >= BASE_W) xx = BASE_W - 1;
            uint32_t c = rgb555_to_argb(src[yy * BASE_W + xx]);
            row[0][n+1] = (float)((c >> 16) & 0xFF);
            row[1][n+1] = (float)((c >> 8) & 0xFF);
            row[2][n+1] = (float)(c & 0xFF);
        }
        for (int c = 0; c < 3; c++) chan[c][m+1] = cubic(row[c][0], row[c][1], row[c][2], row[c][3], tx);
    }
    uint32_t out[3];
    for (int c = 0; c < 3; c++) {
        float v = cubic(chan[c][0], chan[c][1], chan[c][2], chan[c][3], ty);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out[c] = (uint32_t)(v + 0.5f);
    }
    return 0xFF000000u | (out[0] << 16) | (out[1] << 8) | out[2];
}

static bool ensure_bicubic_texture(App *app, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (app->bicubic_tex && app->bicubic_w == w && app->bicubic_h == h) return true;
    if (app->bicubic_tex) SDL_DestroyTexture(app->bicubic_tex);
    free(app->bicubic_pixels);
    app->bicubic_tex = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    app->bicubic_pixels = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    app->bicubic_w = w;
    app->bicubic_h = h;
    return app->bicubic_tex && app->bicubic_pixels;
}

static void render_pixels(App *app, const uint16_t *fb) {
    SDL_FRect vp = calc_viewport(app);
    SDL_SetRenderDrawColor(app->renderer, 8, 10, 18, 255);
    SDL_RenderClear(app->renderer);
    if (app->scale_mode == SCALE_BICUBIC) {
        int w = (int)vp.w, h = (int)vp.h;
        if (ensure_bicubic_texture(app, w, h)) {
            for (int y = 0; y < h; y++) {
                float sy = ((float)y + 0.5f) * (float)BASE_H / (float)h - 0.5f;
                for (int x = 0; x < w; x++) {
                    float sx = ((float)x + 0.5f) * (float)BASE_W / (float)w - 0.5f;
                    app->bicubic_pixels[y * w + x] = sample_bicubic(fb, sx, sy);
                }
            }
            SDL_UpdateTexture(app->bicubic_tex, NULL, app->bicubic_pixels, w * (int)sizeof(uint32_t));
            SDL_SetTextureScaleMode(app->bicubic_tex, SDL_SCALEMODE_NEAREST);
            SDL_RenderTexture(app->renderer, app->bicubic_tex, NULL, &vp);
        }
    } else {
        SDL_SetTextureScaleMode(app->frame_tex, app->scale_mode == SCALE_LINEAR ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_UpdateTexture(app->frame_tex, NULL, fb, BASE_W * (int)sizeof(uint16_t));
        SDL_RenderTexture(app->renderer, app->frame_tex, NULL, &vp);
    }
}

static void render_frame(App *app) {
    render_pixels(app, system_get_display_output());
}

static void draw_panel(App *app, float x, float y, float w, float h) {
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, 18, 23, 38, 222);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderDrawColor(app->renderer, 77, 151, 255, 255);
    SDL_RenderRect(app->renderer, &r);
}

static void draw_text(App *app, float x, float y, const char *text, bool strong) {
    SDL_SetRenderDrawColor(app->renderer, strong ? 245 : 198, strong ? 247 : 208, strong ? 255 : 228, 255);
    SDL_RenderDebugText(app->renderer, x, y, text);
}

static void draw_overlay(App *app) {
    uint64_t now = SDL_GetTicks();
    if (app->menu_open) return;
    if (now <= app->overlay_until && app->message[0]) {
        draw_panel(app, 16, 16, 420, 42);
        draw_text(app, 30, 30, app->message, true);
    }
}

#define MENU_ITEM_COUNT 13
#define REMAP_ITEM_COUNT 11
#define MENU_X 56.0f
#define MENU_Y 100.0f
#define MENU_W 392.0f
#define MENU_ROW_H 28.0f
#define MENU_ROW_DRAW_H 24.0f
#define MENU_PANEL_X 32.0f
#define MENU_PANEL_Y 32.0f
#define MENU_PANEL_W 460.0f
#define MENU_PANEL_H 452.0f
#define MENU_TOAST_X 516.0f
#define MENU_TOAST_Y 42.0f
#define MENU_TOAST_W 316.0f
#define MENU_TOAST_PAD_X 16.0f
#define MENU_TOAST_PAD_Y 16.0f
#define MENU_TOAST_TITLE_GAP 24.0f
#define MENU_TOAST_LINE_H 18.0f
#define REMAP_X 516.0f
#define REMAP_Y 164.0f
#define REMAP_W 288.0f
#define REMAP_ROW_H 23.0f
#define REMAP_ROW_DRAW_H 20.0f

static void open_menu(App *app) {
    sound_set_paused(true);
    app->menu_open = true;
    app->menu_shortcuts_until = SDL_GetTicks() + 3200u;
    release_emulated_mouse_buttons(app);
    reset_mouse_motion_accumulator(app);
    sync_mouse_capture(app, false);
}

static void draw_shortcuts_toast(App *app) {
    if (SDL_GetTicks() > app->menu_shortcuts_until) return;

    static const char *shortcut_lines[] = {
        "F1 Save   F2 Load",
        "F8 Fast-forward lock",
        "Hold Tab: temporary 4x",
        "F11 Fullscreen   F12 Reset",
        "Ctrl+M Mouse capture",
        "Esc Back / menu"
    };
    const int line_count = (int)(sizeof(shortcut_lines) / sizeof(shortcut_lines[0]));
    const float toast_h = MENU_TOAST_PAD_Y * 2.0f + MENU_TOAST_TITLE_GAP + MENU_TOAST_LINE_H * (float)line_count;

    draw_panel(app, MENU_TOAST_X, MENU_TOAST_Y, MENU_TOAST_W, toast_h);
    draw_text(app, MENU_TOAST_X + MENU_TOAST_PAD_X, MENU_TOAST_Y + MENU_TOAST_PAD_Y, "Shortcuts", true);
    for (int i = 0; i < line_count; i++) {
        draw_text(app,
                  MENU_TOAST_X + MENU_TOAST_PAD_X,
                  MENU_TOAST_Y + MENU_TOAST_PAD_Y + MENU_TOAST_TITLE_GAP + MENU_TOAST_LINE_H * (float)i,
                  shortcut_lines[i],
                  false);
    }
}

static void draw_menu(App *app) {
    if (!app->menu_open) return;
    static const char *items[] = {"Resume", "Save state", "Load state", "State slot", "Scaling", "Integer scale", "Port 1 device", "Mouse capture", "Remap controls", "Soft reset", "Cmd recording", "Fast forward", "Quit"};
    draw_panel(app, MENU_PANEL_X, MENU_PANEL_Y, MENU_PANEL_W, MENU_PANEL_H);
    draw_text(app, MENU_PANEL_X + 20.0f, MENU_PANEL_Y + 18.0f, "CLoopy", true);
    draw_text(app, MENU_PANEL_X + 20.0f, MENU_PANEL_Y + 38.0f, "SDL3 frontend", false);
    draw_text(app, MENU_PANEL_X + 20.0f, MENU_PANEL_Y + MENU_PANEL_H - 30.0f, "Click a row or use keyboard/gamepad. Esc closes.", false);

    char line[192];
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        SDL_FRect sel = { MENU_X, MENU_Y + i * MENU_ROW_H, MENU_W, MENU_ROW_DRAW_H };
        if (i == app->menu_index) {
            SDL_SetRenderDrawColor(app->renderer, 48, 104, 192, 210);
            SDL_RenderFillRect(app->renderer, &sel);
        }
        if (i == 3) snprintf(line, sizeof(line), "%s: %d", items[i], app->state_slot);
        else if (i == 4) snprintf(line, sizeof(line), "%s: %s", items[i], scale_name(app->scale_mode));
        else if (i == 5) snprintf(line, sizeof(line), "%s: %s", items[i], app->integer_scale ? "on" : "off");
        else if (i == 6) snprintf(line, sizeof(line), "%s: %s", items[i], input_get_port_device() == INPUT_PORT_MOUSE ? "Mouse" : "Gamepad");
        else if (i == 7) snprintf(line, sizeof(line), "%s: %s", items[i], app->mouse_capture ? "on" : "off");
        else if (i == 10) snprintf(line, sizeof(line), "%s: %s", items[i], app->cmd_writer_open ? "on" : "off");
        else if (i == 11) {
            int m = fast_forward_multiplier(app);
            snprintf(line, sizeof(line), "%s: %s", items[i], m <= 1 ? "off" : (m == 2 ? "2x" : (m == 4 ? "4x" : "8x")));
        } else snprintf(line, sizeof(line), "%s", items[i]);
        draw_text(app, MENU_X + 12.0f, MENU_Y + 5.0f + i * MENU_ROW_H, line, i == app->menu_index);
    }

    draw_shortcuts_toast(app);

    if (app->remap_mode) {
        draw_panel(app, 500.0f, 138.0f, 320.0f, 330.0f);
        draw_text(app, 520.0f, 158.0f, "Remap controls", true);
        for (int i = 0; i < REMAP_ITEM_COUNT; i++) {
            const char *kn = SDL_GetKeyName(app->bind[i].key);
            snprintf(line, sizeof(line), "%s: %s", app->bind[i].name, kn && kn[0] ? kn : "?");
            if (i == app->remap_index) {
                SDL_FRect rr = {REMAP_X, REMAP_Y + i * REMAP_ROW_H, REMAP_W, REMAP_ROW_DRAW_H};
                SDL_SetRenderDrawColor(app->renderer, 48, 104, 192, 210);
                SDL_RenderFillRect(app->renderer, &rr);
            }
            draw_text(app, REMAP_X + 10.0f, REMAP_Y + 4.0f + i * REMAP_ROW_H, line, i == app->remap_index);
        }
        draw_text(app, 520.0f, 434.0f, "Click a row, then press a key. Esc: back", false);
    }
}

static void record_current_cmdlist_frame(App *app) {
    if (!app->cmd_writer_open) return;
    uint32_t state_size = video_state_blob_size();
    void *state = malloc(state_size ? state_size : 1u);
    if (!state) {
        set_message(app, "Cmdlist state allocation failed");
        loopy_cmdlist_writer_close(&app->cmd_writer);
        app->cmd_writer_open = false;
        return;
    }
    video_get_state_blob(state, state_size);
    if (loopy_cmdlist_writer_write_frame(&app->cmd_writer, app->emu_frame, state, state_size, system_get_display_output()) != 0) {
        set_message(app, "Cmdlist write failed");
        loopy_cmdlist_writer_close(&app->cmd_writer);
        app->cmd_writer_open = false;
    }
    free(state);
}

static void quick_state(App *app, bool save) {
    char *path = loopy_make_state_path(app->cart_path, app->state_slot);
    if (!path) { set_message(app, "State path allocation failed"); return; }
    int r = save ? system_save_state(path) : system_load_state(path);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s state slot %d %s", save ? "Save" : "Load", app->state_slot, r == 0 ? "OK" : "failed");
    set_message(app, msg);
    free(path);
}

static void soft_reset(App *app) {
    if (!app->config) {
        set_message(app, "Soft reset unavailable");
        return;
    }
    InputPortDevice device = input_get_port_device();
    uint32_t sram_size = cart_state_blob_size();
    if (sram_size && app->config->cart.sram.data && app->config->cart.sram.size == sram_size) {
        cart_get_state_blob(app->config->cart.sram.data, sram_size);
    }
    system_shutdown();
    system_initialize(app->config);
    video_set_bmp_dump_enabled(false);
    sound_set_paused(false);
    input_set_port_device(device);
    apply_bindings(app);
    app->menu_open = false;
    app->remap_mode = false;
    sync_mouse_capture(app, false);
    set_message(app, "Soft reset");
}

static void toggle_fullscreen(App *app) {
    app->fullscreen = !app->fullscreen;
    if (!SDL_SetWindowFullscreen(app->window, app->fullscreen)) {
        app->fullscreen = !app->fullscreen;
        set_message(app, "Fullscreen toggle failed");
    } else {
        set_message(app, app->fullscreen ? "Fullscreen" : "Windowed");
    }
}

static int hit_test_menu(float x, float y) {
    if (x < MENU_X || x > MENU_X + MENU_W || y < MENU_Y) return -1;
    int idx = (int)((y - MENU_Y) / MENU_ROW_H);
    if (idx < 0 || idx >= MENU_ITEM_COUNT) return -1;
    if (y > MENU_Y + idx * MENU_ROW_H + MENU_ROW_DRAW_H) return -1;
    return idx;
}

static int hit_test_remap(float x, float y) {
    if (x < REMAP_X || x > REMAP_X + REMAP_W || y < REMAP_Y) return -1;
    int idx = (int)((y - REMAP_Y) / REMAP_ROW_H);
    if (idx < 0 || idx >= REMAP_ITEM_COUNT) return -1;
    if (y > REMAP_Y + idx * REMAP_ROW_H + REMAP_ROW_DRAW_H) return -1;
    return idx;
}

static void menu_activate(App *app) {
    switch (app->menu_index) {
    case 0:
        resume_game(app);
        break;
    case 1:
        quick_state(app, true);
        break;
    case 2:
        quick_state(app, false);
        break;
    case 3:
        app->state_slot = (app->state_slot + 1) % 10;
        break;
    case 4:
        app->scale_mode = (ScaleModeUI)((app->scale_mode + 1) % SCALE_COUNT);
        save_ui_config(app);
        break;
    case 5:
        app->integer_scale = !app->integer_scale;
        save_ui_config(app);
        break;
    case 6:
        input_set_port_device(input_get_port_device() == INPUT_PORT_MOUSE ? INPUT_PORT_GAMEPAD : INPUT_PORT_MOUSE);
        if (input_get_port_device() == INPUT_PORT_MOUSE) app->mouse_capture = true;
        sync_mouse_capture(app, false);
        save_ui_config(app);
        break;
    case 7:
        app->mouse_capture = !app->mouse_capture;
        sync_mouse_capture(app, false);
        save_ui_config(app);
        break;
    case 8:
        app->remap_mode = true;
        app->remap_index = 0;
        sync_mouse_capture(app, false);
        break;
    case 9:
        soft_reset(app);
        break;
    case 10:
        if (app->cmd_writer_open) set_message(app, "Cmd recording active");
        else set_message(app, "Use --record-cmdlist <file>");
        break;
    case 11:
        app->fast_forward_level = (app->fast_forward_level + 1) & 3;
        fast_forward_message(app);
        break;
    case 12:
        app->running = false;
        break;
    }
}

static void handle_keyboard(App *app, SDL_KeyboardEvent *key) {
    if (key->repeat) return;
    bool down = key->down;
    SDL_Keycode sym = key->key;
    if (app->remap_mode) {
        if (down && sym == SDLK_ESCAPE) { app->remap_mode = false; save_ui_config(app); apply_bindings(app); return; }
        if (down && sym == SDLK_UP) { if (app->remap_index > 0) app->remap_index--; return; }
        if (down && sym == SDLK_DOWN) { if (app->remap_index < REMAP_ITEM_COUNT - 1) app->remap_index++; return; }
        if (down && sym == SDLK_RETURN) { set_message(app, "Press a key for selected control"); return; }
        if (down) { app->bind[app->remap_index].key = sym; save_ui_config(app); apply_bindings(app); set_message(app, "Control remapped"); return; }
        return;
    }
    if (sym == SDLK_TAB) {
        app->fast_forward_hold = down;
        if (down) fast_forward_message(app);
        return;
    }
    if (down && sym == SDLK_F8) { app->fast_forward_level = (app->fast_forward_level + 1) & 3; fast_forward_message(app); return; }
    if (down && sym == SDLK_F11) { toggle_fullscreen(app); return; }
    if (down && sym == SDLK_F12) { soft_reset(app); return; }
    if (down && sym == SDLK_M && (key->mod & SDL_KMOD_CTRL)) {
        app->mouse_capture = !app->mouse_capture;
        sync_mouse_capture(app, true);
        save_ui_config(app);
        return;
    }
    if (down && sym == SDLK_ESCAPE) {
        if (app->menu_open) resume_game(app);
        else { open_menu(app); }
        return;
    }
    if (app->menu_open) {
        if (down && sym == SDLK_UP) { if (app->menu_index > 0) app->menu_index--; return; }
        if (down && sym == SDLK_DOWN) { if (app->menu_index < MENU_ITEM_COUNT - 1) app->menu_index++; return; }
        if (down && (sym == SDLK_LEFT || sym == SDLK_RIGHT)) { menu_activate(app); return; }
        if (down && sym == SDLK_RETURN) { menu_activate(app); return; }
        return;
    }
    if (down && sym == SDLK_F1) { quick_state(app, true); return; }
    if (down && sym == SDLK_F2) { quick_state(app, false); return; }
    if (down && sym == SDLK_F5) { app->state_slot = (app->state_slot + 9) % 10; set_message(app, "State slot changed"); return; }
    if (down && sym == SDLK_F6) { app->state_slot = (app->state_slot + 1) % 10; set_message(app, "State slot changed"); return; }
    if (down && sym == SDLK_F9) { app->scale_mode = (ScaleModeUI)((app->scale_mode + 1) % SCALE_COUNT); save_ui_config(app); set_message(app, scale_name(app->scale_mode)); return; }
    if (down && sym == SDLK_F10) { app->integer_scale = !app->integer_scale; save_ui_config(app); set_message(app, app->integer_scale ? "Integer scale on" : "Integer scale off"); return; }
    input_set_key_state((int)sym, down);
}

static void set_axis_button(bool *old_state, bool new_state, PadButton button) {
    if (*old_state != new_state) {
        *old_state = new_state;
        input_set_pad_button(button, new_state);
    }
}

static void handle_gamepad_axis(App *app, SDL_GamepadAxisEvent *axis) {
    const int dead = 12000;
    if (axis->axis == SDL_GAMEPAD_AXIS_LEFTX) {
        set_axis_button(&app->axis_left, axis->value < -dead, PAD_LEFT);
        set_axis_button(&app->axis_right, axis->value > dead, PAD_RIGHT);
    } else if (axis->axis == SDL_GAMEPAD_AXIS_LEFTY) {
        set_axis_button(&app->axis_up, axis->value < -dead, PAD_UP);
        set_axis_button(&app->axis_down, axis->value > dead, PAD_DOWN);
    }
}

static void handle_gamepad_button(App *app, SDL_GamepadButtonEvent *button) {
    bool down = button->down;
    for (int i = 0; i < 11; i++) if (app->bind[i].gamepad_button == (SDL_GamepadButton)button->button) input_set_pad_button(app->bind[i].pad, down);
    if (down && button->button == SDL_GAMEPAD_BUTTON_BACK) {
        if (app->menu_open) resume_game(app);
        else { open_menu(app); }
    }
    if (app->menu_open && down && button->button == SDL_GAMEPAD_BUTTON_SOUTH) menu_activate(app);
    if (app->menu_open && down && button->button == SDL_GAMEPAD_BUTTON_EAST) resume_game(app);
}

static void handle_mouse_button(App *app, SDL_MouseButtonEvent *button) {
    bool down = button->down;
    if (button->button == SDL_BUTTON_LEFT) {
        app->mouse_left_down = down;
        if (app->menu_open && down) {
            if (app->remap_mode) {
                int r = hit_test_remap(button->x, button->y);
                if (r >= 0) {
                    app->remap_index = r;
                    set_message(app, "Press a key for selected control");
                    return;
                }
            }
            int m = hit_test_menu(button->x, button->y);
            if (m >= 0) {
                app->menu_index = m;
                menu_activate(app);
                return;
            }
        }
    }
    if (!app->menu_open && input_get_port_device() == INPUT_PORT_MOUSE) {
        if (button->button == SDL_BUTTON_LEFT) input_set_mouse_button(0, down);
        if (button->button == SDL_BUTTON_RIGHT) input_set_mouse_button(1, down);
    }
}

static void handle_mouse_motion(App *app, SDL_MouseMotionEvent *motion) {
    if (app->menu_open) {
        app->mouse_abs_valid = false;
        if (app->remap_mode) {
            int r = hit_test_remap(motion->x, motion->y);
            if (r >= 0) app->remap_index = r;
        }
        int m = hit_test_menu(motion->x, motion->y);
        if (m >= 0) app->menu_index = m;
        return;
    }

    if (input_get_port_device() != INPUT_PORT_MOUSE) {
        app->mouse_abs_valid = false;
        return;
    }

    float dx = motion->xrel;
    float dy = motion->yrel;

    if (!SDL_GetWindowRelativeMouseMode(app->window)) {
        if (app->mouse_abs_valid) {
            dx = motion->x - app->mouse_last_x;
            dy = motion->y - app->mouse_last_y;
        } else if (dx == 0.0f && dy == 0.0f) {
            dx = 0.0f;
            dy = 0.0f;
        }
        app->mouse_last_x = motion->x;
        app->mouse_last_y = motion->y;
        app->mouse_abs_valid = true;
    }

    feed_mouse_delta_float(app, dx, dy);
}

static void poll_events(App *app) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: app->running = false; break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: handle_keyboard(app, &e.key); break;
        case SDL_EVENT_MOUSE_MOTION: handle_mouse_motion(app, &e.motion); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: handle_mouse_button(app, &e.button); break;
        case SDL_EVENT_GAMEPAD_ADDED:
            for (int i = 0; i < MAX_CONTROLLERS; i++) if (!app->pads[i]) { app->pads[i] = SDL_OpenGamepad(e.gdevice.which); break; }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: handle_gamepad_button(app, &e.gbutton); break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: handle_gamepad_axis(app, &e.gaxis); break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN: sound_set_mute(true); break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN: sound_set_mute(false); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            app->window_focused = false;
            release_emulated_mouse_buttons(app);
            reset_mouse_motion_accumulator(app);
            sync_mouse_capture(app, false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            app->window_focused = true;
            reset_mouse_motion_accumulator(app);
            sync_mouse_capture(app, false);
            break;
        default: break;
        }
    }
}


static SDL_FRect replay_calc_viewport(ReplayApp *app) {
    int ww = 0, wh = 0;
    SDL_GetRenderOutputSize(app->renderer, &ww, &wh);
    float scale_x = (float)ww / (float)BASE_W;
    float scale_y = (float)wh / (float)BASE_H;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    int iscale = (int)floorf(scale);
    if (iscale < 1) iscale = 1;
    scale = (float)iscale;
    float w = BASE_W * scale;
    float h = BASE_H * scale;
    return (SDL_FRect){ floorf(((float)ww - w) * 0.5f), floorf(((float)wh - h) * 0.5f), floorf(w), floorf(h) };
}

static bool replay_init_sdl(ReplayApp *app) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL3 init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    app->window = SDL_CreateWindow("CLoopy command-list replay", BASE_W * 3, BASE_H * 3, SDL_WINDOW_RESIZABLE);
    if (!app->window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return false; }
    app->renderer = SDL_CreateRenderer(app->window, NULL);
    if (!app->renderer) { fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError()); return false; }
    app->frame_tex = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB1555, SDL_TEXTUREACCESS_STREAMING, BASE_W, BASE_H);
    if (!app->frame_tex) { fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError()); return false; }
    SDL_SetTextureScaleMode(app->frame_tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(app->frame_tex, SDL_BLENDMODE_NONE);
    return true;
}

static void replay_close_sdl(ReplayApp *app) {
    if (app->vdp_replay_initialized) {
        video_shutdown();
        loopy_io_shutdown();
        timing_shutdown();
        app->vdp_replay_initialized = false;
    }
    if (app->frame_tex) SDL_DestroyTexture(app->frame_tex);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    SDL_Quit();
}

static void replay_set_message(ReplayApp *app, const char *msg) {
    snprintf(app->message, sizeof(app->message), "%s", msg ? msg : "");
    app->overlay_until = SDL_GetTicks() + 1800u;
}

static bool replay_init_vdp_renderer(ReplayApp *app) {
    if (app->vdp_replay_initialized) return true;
    timing_initialize();
    loopy_io_initialize();
    video_initialize();
    video_set_bmp_dump_enabled(false);
    app->vdp_replay_initialized = true;
    return true;
}

static bool replay_render_frame_from_state(ReplayApp *app, uint32_t frame) {
    if (frame >= app->reader.frame_count) return false;
    uint32_t state_size = app->reader.frames[frame].state_size;
    if (state_size != video_state_blob_size()) return false;
    void *state = malloc(state_size ? state_size : 1u);
    if (!state) return false;
    bool ok = false;
    if (loopy_cmdlist_reader_read_state(&app->reader, frame, state, state_size) == 0 &&
        replay_init_vdp_renderer(app)) {
        video_set_state_blob(state, state_size);
        video_start_frame();
        int lines = video_get_display_active_height();
        if (lines < 0) lines = 0;
        if (lines > BASE_H) lines = BASE_H;
        for (int y = 0; y < lines; y++) {
            video_renderer_draw_scanline(y);
        }
        memcpy(app->pixels, video_get_display_output(), (size_t)BASE_W * BASE_H * sizeof(uint16_t));
        ok = true;
    }
    free(state);
    return ok;
}

static void replay_load_frame(ReplayApp *app, uint32_t frame) {
    if (!app->reader.frame_count) return;
    if (frame >= app->reader.frame_count) frame = app->reader.frame_count - 1;
    app->rendered_from_state = false;
    if (replay_render_frame_from_state(app, frame)) {
        app->frame_index = frame;
        app->rendered_from_state = true;
        return;
    }
    if (loopy_cmdlist_reader_read_framebuffer(&app->reader, frame, app->pixels, BASE_W * BASE_H) == 0) {
        app->frame_index = frame;
    }
}

static void replay_toggle_fullscreen(ReplayApp *app) {
    app->fullscreen = !app->fullscreen;
    if (!SDL_SetWindowFullscreen(app->window, app->fullscreen)) {
        app->fullscreen = !app->fullscreen;
        replay_set_message(app, "Fullscreen toggle failed");
    }
}

static void replay_handle_key(ReplayApp *app, SDL_KeyboardEvent *key) {
    if (key->repeat || !key->down) return;
    SDL_Keycode sym = key->key;
    if (sym == SDLK_ESCAPE) { app->running = false; return; }
    if (sym == SDLK_F11) { replay_toggle_fullscreen(app); return; }
    if (sym == SDLK_SPACE) { app->playing = !app->playing; replay_set_message(app, app->playing ? "Replay playing" : "Replay paused"); return; }
    if (sym == SDLK_RIGHT || sym == SDLK_PERIOD) { app->playing = false; replay_load_frame(app, app->frame_index + 1); return; }
    if (sym == SDLK_LEFT || sym == SDLK_COMMA) { app->playing = false; replay_load_frame(app, app->frame_index ? app->frame_index - 1 : 0); return; }
    if (sym == SDLK_HOME) { app->playing = false; replay_load_frame(app, 0); return; }
    if (sym == SDLK_END) { app->playing = false; replay_load_frame(app, app->reader.frame_count ? app->reader.frame_count - 1 : 0); return; }
}

static void replay_poll_events(ReplayApp *app) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: app->running = false; break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: replay_handle_key(app, &e.key); break;
        default: break;
        }
    }
}

static void replay_render(ReplayApp *app) {
    SDL_FRect vp = replay_calc_viewport(app);
    SDL_SetRenderDrawColor(app->renderer, 8, 10, 18, 255);
    SDL_RenderClear(app->renderer);
    SDL_UpdateTexture(app->frame_tex, NULL, app->pixels, BASE_W * (int)sizeof(uint16_t));
    SDL_RenderTexture(app->renderer, app->frame_tex, NULL, &vp);

    char line[160];
    snprintf(line, sizeof(line), "Cmdlist replay  frame %u/%u  %s  %s", app->frame_index + 1, app->reader.frame_count, app->playing ? "playing" : "paused", app->rendered_from_state ? "state-render" : "stored-fb");
    draw_panel((App *)app, 16.0f, 16.0f, 480.0f, 66.0f);
    SDL_SetRenderDrawColor(app->renderer, 245, 247, 255, 255);
    SDL_RenderDebugText(app->renderer, 30.0f, 30.0f, line);
    SDL_SetRenderDrawColor(app->renderer, 198, 208, 228, 255);
    SDL_RenderDebugText(app->renderer, 30.0f, 50.0f, "Space play/pause   Left/Right step   Home/End jump   F11 fullscreen   Esc quit");
    if (SDL_GetTicks() <= app->overlay_until && app->message[0]) {
        draw_panel((App *)app, 16.0f, 96.0f, 320.0f, 42.0f);
        SDL_SetRenderDrawColor(app->renderer, 245, 247, 255, 255);
        SDL_RenderDebugText(app->renderer, 30.0f, 110.0f, app->message);
    }
}

static int run_cmdlist_replay(const char *path, uint32_t frame_limit) {
    ReplayApp app;
    memset(&app, 0, sizeof(app));
    if (loopy_cmdlist_reader_open(&app.reader, path) != 0) {
        fprintf(stderr, "Failed to open command list: %s\n", path ? path : "(null)");
        return 1;
    }
    if (app.reader.width != BASE_W || app.reader.height != BASE_H || !app.reader.frame_count) {
        fprintf(stderr, "Unsupported or empty command list: %s\n", path ? path : "(null)");
        loopy_cmdlist_reader_close(&app.reader);
        return 1;
    }
    app.pixels = (uint16_t *)calloc(BASE_W * BASE_H, sizeof(uint16_t));
    if (!app.pixels) {
        loopy_cmdlist_reader_close(&app.reader);
        return 1;
    }
    if (!replay_init_sdl(&app)) {
        replay_close_sdl(&app);
        loopy_cmdlist_reader_close(&app.reader);
        free(app.pixels);
        return 1;
    }
    app.running = true;
    app.playing = false;
    app.last_advance = SDL_GetTicks();
    replay_load_frame(&app, 0);
    replay_set_message(&app, "Command list loaded");

    uint32_t presented = 0;
    while (app.running) {
        uint64_t start = SDL_GetTicks();
        replay_poll_events(&app);
        if (app.playing && start - app.last_advance >= 16u) {
            uint32_t next = app.frame_index + 1;
            if (next >= app.reader.frame_count) {
                next = app.reader.frame_count - 1;
                app.playing = false;
            }
            replay_load_frame(&app, next);
            app.last_advance = start;
        }
        replay_render(&app);
        SDL_RenderPresent(app.renderer);
        presented++;
        if (frame_limit && presented >= frame_limit) app.running = false;
        uint64_t elapsed = SDL_GetTicks() - start;
        if (elapsed < 16) SDL_Delay(16 - (Uint32)elapsed);
    }

    replay_close_sdl(&app);
    loopy_cmdlist_reader_close(&app.reader);
    free(app.pixels);
    return 0;
}

int main(int argc, char **argv) {
    LoopyLaunchInfo launch;
    if (loopy_parse_common_args(argc, argv, &launch, 0) != 0) {
        printf("Args: <game ROM> <BIOS> [sound BIOS] [OKI ADPCM ROM] [--device gamepad|mouse] [--mouse|--gamepad] [--record-cmdlist file] [--frames N] [--oki-rom file] [--no-wanwan-internal-pcm]\n");
        //printf("Replay: --replay-cmdlist file [--frames N]\n"); 
        return 1;
    }
    if (launch.replay_cmdlist_path) {
        return run_cmdlist_replay(launch.replay_cmdlist_path, launch.frames_set ? (uint32_t)launch.frames : 0u);
    }

    ConfigSystemInfo config;
    if (loopy_load_config(&launch, &config) != 0) return 1;

    App app;
    memset(&app, 0, sizeof(app));
    app.scale_mode = SCALE_INTEGER_NEAREST;
    app.integer_scale = true;
    app.state_slot = 0;
    app.mouse_capture = true;
    app.window_focused = true;
    app.cart_path = launch.cart_path;
    app.config = &config;
    app.frame_limit = launch.frames_set ? (uint32_t)launch.frames : 0u;
    app.running = true;
    set_default_bindings(&app);

    if (!init_sdl(&app)) { close_sdl(&app); loopy_config_free(&config); return 1; }
    system_initialize(&config);
    video_set_bmp_dump_enabled(false);
    bool cfg_loaded = load_ui_config(&app);
    (void)cfg_loaded;
    if (launch.input_device_set) {
        input_set_port_device(launch.input_device ? INPUT_PORT_MOUSE : INPUT_PORT_GAMEPAD);
    } else {
        /* The controller device is a per-game hardware assumption, not a safe
         * global UI preference.  If a previous session saved mouse mode, normal
         * gamepad titles such as HARIHARI boot in direct mouse detection and
         * never see START/A in the matrix latches. */
        input_set_port_device(INPUT_PORT_GAMEPAD);
        app.mouse_capture = false;
    }
    apply_bindings(&app);
    sync_mouse_capture(&app, true);
    if (launch.record_cmdlist_path) {
        if (loopy_cmdlist_writer_open(&app.cmd_writer, launch.record_cmdlist_path, BASE_W, BASE_H) == 0) {
            app.cmd_writer_open = true;
            set_message(&app, "Command-list recording");
        } else {
            set_message(&app, "Command-list open failed");
        }
    }
    if (!should_capture_mouse(&app) && !app.message[0]) set_message(&app, "Press Escape for menu");

    while (app.running) {
        uint64_t frame_start = SDL_GetTicks();
        poll_events(&app);
        int ff_mult = fast_forward_multiplier(&app);
        if (!app.menu_open && !app.remap_mode) {
            for (int step = 0; step < ff_mult && app.running; step++) {
                system_run();
                record_current_cmdlist_frame(&app);
                app.emu_frame++;
                if (app.frame_limit && app.emu_frame >= app.frame_limit) app.running = false;
            }
        }
        render_frame(&app);
        draw_overlay(&app);
        draw_menu(&app);
        SDL_RenderPresent(app.renderer);
        uint64_t elapsed = SDL_GetTicks() - frame_start;
        if (fast_forward_multiplier(&app) <= 1 && elapsed < 16) SDL_Delay(16 - (Uint32)elapsed);
    }

    SDL_SetWindowRelativeMouseMode(app.window, false);
    SDL_SetWindowMouseGrab(app.window, false);
    SDL_CaptureMouse(false);
    SDL_ShowCursor();
    if (app.cmd_writer_open) {
        loopy_cmdlist_writer_close(&app.cmd_writer);
        app.cmd_writer_open = false;
    }
    save_ui_config(&app);
    system_shutdown();
    close_sdl(&app);
    loopy_config_free(&config);
    return 0;
}
