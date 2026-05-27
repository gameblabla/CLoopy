#include "frontend/cmdlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *path = "build/tests/cmdlist_roundtrip.lpcl";
    LoopyCmdListWriter writer;
    uint16_t pixels[LOOPY_CMDLIST_WIDTH * LOOPY_CMDLIST_HEIGHT];
    unsigned char state[16];
    for (uint32_t i = 0; i < LOOPY_CMDLIST_WIDTH * LOOPY_CMDLIST_HEIGHT; i++) pixels[i] = (uint16_t)(i & 0x7FFFu);
    for (size_t i = 0; i < sizeof(state); i++) state[i] = (unsigned char)(0xA0u + i);
    if (loopy_cmdlist_writer_open(&writer, path, LOOPY_CMDLIST_WIDTH, LOOPY_CMDLIST_HEIGHT) != 0) return 1;
    if (loopy_cmdlist_writer_write_frame(&writer, 123u, state, (uint32_t)sizeof(state), pixels) != 0) return 2;
    if (loopy_cmdlist_writer_close(&writer) != 0) return 3;

    LoopyCmdListReader reader;
    if (loopy_cmdlist_reader_open(&reader, path) != 0) return 4;
    if (reader.frame_count != 1u || reader.width != LOOPY_CMDLIST_WIDTH || reader.height != LOOPY_CMDLIST_HEIGHT) return 5;
    if (reader.frames[0].frame_index != 123u || reader.frames[0].state_size != sizeof(state)) return 6;
    uint16_t *out_pixels = (uint16_t *)calloc(LOOPY_CMDLIST_WIDTH * LOOPY_CMDLIST_HEIGHT, sizeof(uint16_t));
    unsigned char out_state[16];
    if (!out_pixels) return 7;
    if (loopy_cmdlist_reader_read_framebuffer(&reader, 0, out_pixels, LOOPY_CMDLIST_WIDTH * LOOPY_CMDLIST_HEIGHT) != 0) return 8;
    if (loopy_cmdlist_reader_read_state(&reader, 0, out_state, (uint32_t)sizeof(out_state)) != 0) return 9;
    int ok = memcmp(out_pixels, pixels, sizeof(pixels)) == 0 && memcmp(out_state, state, sizeof(state)) == 0;
    free(out_pixels);
    loopy_cmdlist_reader_close(&reader);
    if (!ok) return 10;
    puts("cmdlist_roundtrip_test: OK");
    return 0;
}
