#include "core/cart_meta.h"

#include <ctype.h>
#include <string.h>

static int ascii_ci_match_at(const uint8_t *data, size_t size, size_t pos, const char *needle) {
    size_t n = strlen(needle);
    if (pos > size || n > size - pos) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = data[pos + i];
        unsigned char b = (unsigned char)needle[i];
        if (tolower(a) != tolower(b)) return 0;
    }
    return 1;
}

static int ascii_ci_contains(const uint8_t *data, size_t size, const char *needle) {
    if (!data || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    if (size < n) return 0;
    for (size_t i = 0; i <= size - n; i++) {
        if (ascii_ci_match_at(data, size, i, needle)) return 1;
    }
    return 0;
}

int loopy_cart_rom_is_wanwan(const uint8_t *rom, size_t size) {
    if (!rom || size < 0x100u) return 0;

    /* Wanwan's cartridge image contains its English title string near the
       early ROM metadata/code area, before the body of asset data.  Use the
       ROM contents rather than the host filename so the OKI warning is not
       shown for unrelated cartridges. */
    size_t scan = size < 0x10000u ? size : 0x10000u;
    if (ascii_ci_contains(rom, scan, "WAN WAN STORY")) return 1;
    if (ascii_ci_contains(rom, scan, "WANWAN STORY")) return 1;
    if (ascii_ci_contains(rom, scan, "XK-501")) return 1;
    if (ascii_ci_contains(rom, scan, "WANWAN AIJOU")) return 1;
    return 0;
}
