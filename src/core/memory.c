#include "core/memory.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SH7021_PAGETABLE_SIZE ((1u << 28) / 4096u)
#define SH7021_REGION_SIZE (1u << 24)

typedef struct MemoryState {
    uint8_t **sh7021_pagetable;
    uint8_t bios[MEMORY_BIOS_SIZE];
    uint8_t ram[MEMORY_RAM_SIZE];
} MemoryState;

static MemoryState *state;

static void map_pagetable(uint8_t **table, uint8_t *data, uint32_t start, uint32_t size) {
    start >>= 12;
    size >>= 12;
    for (uint32_t i = 0; i < size; i++) {
        table[start + i] = data + (i << 12);
    }
}

void memory_initialize(const ByteBuffer *bios_rom) {
    state = (MemoryState *)calloc(1, sizeof(MemoryState));
    assert(state);
    assert(bios_rom && bios_rom->data && bios_rom->size >= MEMORY_BIOS_SIZE);
    memcpy(state->bios, bios_rom->data, MEMORY_BIOS_SIZE);
    state->sh7021_pagetable = (uint8_t **)calloc(SH7021_PAGETABLE_SIZE, sizeof(uint8_t *));
    assert(state->sh7021_pagetable);
    memory_map_sh7021_pagetable(state->bios, MEMORY_BIOS_START, MEMORY_BIOS_SIZE);
    for (uint32_t i = 0; i < SH7021_REGION_SIZE; i += MEMORY_RAM_SIZE) {
        memory_map_sh7021_pagetable(state->ram, MEMORY_RAM_START + i, MEMORY_RAM_SIZE);
    }
}

void memory_shutdown(void) {
    if (!state) return;
    free(state->sh7021_pagetable);
    free(state);
    state = NULL;
}

void memory_map_sh7021_pagetable(uint8_t *data, uint32_t start, uint32_t size) {
    assert(state && state->sh7021_pagetable);
    map_pagetable(state->sh7021_pagetable, data, start, size);
}

uint8_t **memory_get_sh7021_pagetable(void) {
    assert(state);
    return state->sh7021_pagetable;
}

uint32_t memory_state_blob_size(void) { return MEMORY_RAM_SIZE; }
void memory_get_state_blob(void *dst, uint32_t size) { if (state && dst && size == MEMORY_RAM_SIZE) memcpy(dst, state->ram, MEMORY_RAM_SIZE); }
void memory_set_state_blob(const void *src, uint32_t size) { if (state && src && size == MEMORY_RAM_SIZE) memcpy(state->ram, src, MEMORY_RAM_SIZE); }
