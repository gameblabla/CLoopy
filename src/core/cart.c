#include "core/cart.h"
#include "core/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CartState {
    ByteBuffer rom;
    ByteBuffer sram;
    char *sram_file_path;
} CartState;

static CartState state;

static void commit_sram(void) {
    if (!state.sram_file_path || !state.sram.data) return;
    FILE *file = fopen(state.sram_file_path, "wb");
    if (!file) return;
    fwrite(state.sram.data, 1, state.sram.size, file);
    fclose(file);
}

void cart_initialize(const ConfigCartInfo *info) {
    byte_buffer_free(&state.rom);
    byte_buffer_free(&state.sram);
    free(state.sram_file_path);
    memset(&state, 0, sizeof(state));

    byte_buffer_copy(&state.rom, &info->rom);
    byte_buffer_copy(&state.sram, &info->sram);
    state.sram_file_path = info->sram_file_path ? loopy_strdup(info->sram_file_path) : NULL;

    if (state.rom.size & 0xFFFu) {
        size_t new_size = (state.rom.size + 0xFFFu) & ~(size_t)0xFFFu;
        byte_buffer_resize(&state.rom, new_size, 0xFF);
    }
    if (state.sram.size & 0xFFFu) {
        size_t new_size = (state.sram.size + 0xFFFu) & ~(size_t)0xFFFu;
        byte_buffer_resize(&state.sram, new_size, 0xFF);
    }
    memory_map_sh7021_pagetable(state.rom.data, CART_ROM_START, (uint32_t)state.rom.size);
    memory_map_sh7021_pagetable(state.sram.data, CART_SRAM_START, (uint32_t)state.sram.size);
}

void cart_shutdown(void) {
    commit_sram();
    byte_buffer_free(&state.rom);
    byte_buffer_free(&state.sram);
    free(state.sram_file_path);
    memset(&state, 0, sizeof(state));
}

void cart_sram_commit_check(void) {
    static int frame_count = 0;
    frame_count++;
    if (frame_count < 60) return;
    frame_count = 0;
    commit_sram();
}

void *cart_get_sram_data(void) { return state.sram.data; }
uint32_t cart_get_sram_size(void) { return (uint32_t)state.sram.size; }

uint32_t cart_state_blob_size(void) { return (uint32_t)state.sram.size; }
void cart_get_state_blob(void *dst, uint32_t size) { if (dst && state.sram.data && size == state.sram.size) memcpy(dst, state.sram.data, state.sram.size); }
void cart_set_state_blob(const void *src, uint32_t size) { if (src && state.sram.data && size == state.sram.size) memcpy(state.sram.data, src, state.sram.size); }
