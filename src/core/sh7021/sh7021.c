#include "core/sh7021/sh7021.h"
#include "core/sh7021/sh7021_bus.h"
#include "core/sh7021/sh7021_interpreter.h"
#include "core/sh7021/sh7021_local.h"
#include "core/sh7021/peripherals/sh7021_bsc.h"
#include "core/sh7021/peripherals/sh7021_dmac.h"
#include "core/sh7021/peripherals/sh7021_intc.h"
#include "core/sh7021/peripherals/sh7021_serial.h"
#include "core/sh7021/peripherals/sh7021_timers.h"
#include "core/memory.h"
#include "core/loopy_io.h"
#include "core/timing.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

SH7021CPU sh7021;
static TimingFuncHandle irq_func;
static TimingEventHandle irq_ev;

static int ptr_in_known_space(uint32_t addr) {
    uint32_t area = addr & 0x0F000000u;
    return area == 0x09000000u || area == 0x0E000000u || area == 0x0C000000u || area == 0x04000000u || area == 0x0F000000u;
}

typedef struct BiosPrintImageSig {
    uint32_t hash;
    uint16_t width;
    uint16_t height;
    int64_t cpu_ts;
    uint8_t valid;
} BiosPrintImageSig;

static BiosPrintImageSig last_wrapper_print_image;
static BiosPrintImageSig last_parts_setup_image;
static uint32_t bios_print_last_hook_pc = 0xFFFFFFFFu;
static uint32_t bios_print_last_hook_data = 0;
static uint32_t bios_print_last_hook_stamp = 0;

static void sh7021_bios_print_reset_dedupe(void) {
    memset(&last_wrapper_print_image, 0, sizeof(last_wrapper_print_image));
    memset(&last_parts_setup_image, 0, sizeof(last_parts_setup_image));
    bios_print_last_hook_pc = 0xFFFFFFFFu;
    bios_print_last_hook_data = 0;
    bios_print_last_hook_stamp = 0;
}

static uint32_t bios_print_image_hash(const uint16_t *rgb555, uint32_t width, uint32_t height) {
    uint32_t h = 2166136261u;
    h ^= width; h *= 16777619u;
    h ^= height; h *= 16777619u;
    size_t pixels = (size_t)width * (size_t)height;
    for (size_t i = 0; i < pixels; i++) {
        uint16_t c = rgb555[i];
        h ^= (uint8_t)c; h *= 16777619u;
        h ^= (uint8_t)(c >> 8); h *= 16777619u;
    }
    return h ? h : 1u;
}

static int bios_print_source_is_parts(const char *source) {
    return source && strncmp(source, "bios_printParts", 15) == 0;
}

static int bios_print_source_setup_flag(const char *source) {
    if (!source) return -1;
    const char *p = strstr(source, "setup=");
    if (!p) return -1;
    p += 6;
    if (*p == '0') return 0;
    if (*p == '1') return 1;
    return -1;
}

static int bios_print_sig_matches(const BiosPrintImageSig *sig, uint32_t hash, uint16_t width, uint16_t height) {
    return sig && sig->valid && sig->width == width && sig->height == height && sig->hash == hash;
}

static void bios_print_store_sig(BiosPrintImageSig *sig, uint32_t hash, uint16_t width, uint16_t height, int64_t now) {
    if (!sig) return;
    sig->hash = hash;
    sig->width = width;
    sig->height = height;
    sig->cpu_ts = now;
    sig->valid = 1;
}

static void sh7021_bios_print_emit_source_image(const uint16_t *rgb555, uint16_t width, uint16_t height, const char *source) {
    if (!rgb555 || width == 0 || height == 0) return;
    uint32_t hash = bios_print_image_hash(rgb555, width, height);
    int64_t now = timing_get_timestamp(TIMING_CPU_TIMER);
    int is_parts = bios_print_source_is_parts(source);
    int setup = is_parts ? bios_print_source_setup_flag(source) : -1;

    if (is_parts && setup == 0 && last_parts_setup_image.valid &&
        last_parts_setup_image.width == width && last_parts_setup_image.height == height) {
        /* A setup=1 bios_printParts request can later re-enter bios_printParts
           with setup=0 while the same physical sticker job is still in
           progress.  That setup=0 entry is a continuation of the current
           printer transaction, not a new sticker request for the frontend to
           download or preview.  Do not require the source pixels to be stable:
           some games reuse or mutate the source buffers while the mechanical
           print routine is still running. */
        last_parts_setup_image.valid = 0;
        last_wrapper_print_image.valid = 0;
        return;
    }

    if (is_parts && last_wrapper_print_image.valid) {
        int64_t dt = now - last_wrapper_print_image.cpu_ts;
        if (dt >= 0 && dt < 1000000 && bios_print_sig_matches(&last_wrapper_print_image, hash, width, height)) {
            /* bios_print8bpp/bios_print15bpp/bios_printDirect are BIOS
               convenience wrappers which may call the real bios_printParts
               routine internally.  The wrapper and the inner call describe the
               same physical sticker request, so emitting both makes SDL3 and
               WASM look as if the printer ran twice. */
            last_wrapper_print_image.valid = 0;
            return;
        }
    }

    loopy_io_printer_write_source_image(rgb555, width, height, source);

    if (is_parts) {
        last_wrapper_print_image.valid = 0;
        if (setup == 1) bios_print_store_sig(&last_parts_setup_image, hash, width, height, now);
        else if (setup == 0) last_parts_setup_image.valid = 0;
    } else {
        bios_print_store_sig(&last_wrapper_print_image, hash, width, height, now);
    }
}

static void sh7021_bios_print_dump_15bpp(uint32_t data, uint32_t width, uint32_t height, const char *source) {
    if (!ptr_in_known_space(data) || width == 0 || height == 0 || width > 512u || height > 512u) return;
    uint16_t *img = (uint16_t *)malloc((size_t)width * height * sizeof(uint16_t));
    if (!img) return;
    int32_t saved_cycles = sh7021.cycles_left;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            img[(size_t)y * width + x] = (uint16_t)(sh7021_bus_read16(data + ((y * width + x) * 2u)) & 0x7FFFu);
        }
    }
    sh7021.cycles_left = saved_cycles;
    sh7021_bios_print_emit_source_image(img, (uint16_t)width, (uint16_t)height, source);
    free(img);
}

static void sh7021_bios_print_dump_8bpp(uint32_t data, uint32_t pal, uint32_t width, uint32_t height, const char *source) {
    if (!ptr_in_known_space(data) || !ptr_in_known_space(pal) || width == 0 || height == 0 || width > 512u || height > 512u) return;
    uint16_t *img = (uint16_t *)malloc((size_t)width * height * sizeof(uint16_t));
    if (!img) return;
    int32_t saved_cycles = sh7021.cycles_left;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t ix = sh7021_bus_read8(data + y * width + x);
            img[(size_t)y * width + x] = (uint16_t)(sh7021_bus_read16(pal + (uint32_t)ix * 2u) & 0x7FFFu);
        }
    }
    sh7021.cycles_left = saved_cycles;
    sh7021_bios_print_emit_source_image(img, (uint16_t)width, (uint16_t)height, source);
    free(img);
}


typedef struct PrintPartImage {
    uint16_t *rgb555;
    uint32_t width;
    uint32_t height;
    uint32_t src_width;
    uint32_t src_height;
    uint8_t fmt;
    uint32_t data;
    uint32_t pal;
} PrintPartImage;

static int decode_print_dim(uint32_t dim, uint32_t *width, uint32_t *height) {
    uint32_t w = dim & 0xFFFFu;
    uint32_t h = (dim >> 16) & 0xFFFFu;
    if (w == 0 || h == 0 || w > 512u || h > 512u) {
        uint32_t sw = (dim >> 16) & 0xFFFFu;
        uint32_t sh = dim & 0xFFFFu;
        if (sw != 0 && sh != 0 && sw <= 512u && sh <= 512u) { w = sw; h = sh; }
    }
    if (w == 0 || h == 0 || w > 512u || h > 512u) return 0;
    *width = w;
    *height = h;
    return 1;
}

static int sh7021_bios_print_decode_part(uint32_t data, uint32_t pal, uint32_t dim, uint8_t fmt, PrintPartImage *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    uint32_t src_w = 0, src_h = 0;
    if (!decode_print_dim(dim, &src_w, &src_h)) return 0;
    if (!ptr_in_known_space(data)) return 0;

    uint8_t type = fmt & 0x0Fu;
    if (type != 1u && type != 3u) {
        type = ptr_in_known_space(pal) ? 3u : 1u;
    }
    if (type == 3u && !ptr_in_known_space(pal)) return 0;

    uint32_t scale = (fmt & 0x10u) ? 2u : 1u;
    uint32_t dst_w = src_w * scale;
    uint32_t dst_h = src_h * scale;
    if (dst_w == 0 || dst_h == 0 || dst_w > 1024u || dst_h > 1024u) return 0;

    uint16_t *img = (uint16_t *)malloc((size_t)dst_w * dst_h * sizeof(uint16_t));
    if (!img) return 0;

    int32_t saved_cycles = sh7021.cycles_left;
    for (uint32_t y = 0; y < src_h; y++) {
        for (uint32_t x = 0; x < src_w; x++) {
            uint16_t c;
            if (type == 3u) {
                uint8_t ix = sh7021_bus_read8(data + y * src_w + x);
                c = (uint16_t)(sh7021_bus_read16(pal + (uint32_t)ix * 2u) & 0x7FFFu);
            } else {
                c = (uint16_t)(sh7021_bus_read16(data + ((y * src_w + x) * 2u)) & 0x7FFFu);
            }
            for (uint32_t yy = 0; yy < scale; yy++) {
                for (uint32_t xx = 0; xx < scale; xx++) {
                    img[(size_t)(y * scale + yy) * dst_w + (x * scale + xx)] = c;
                }
            }
        }
    }
    sh7021.cycles_left = saved_cycles;

    out->rgb555 = img;
    out->width = dst_w;
    out->height = dst_h;
    out->src_width = src_w;
    out->src_height = src_h;
    out->fmt = fmt;
    out->data = data;
    out->pal = pal;
    return 1;
}

static int printparts_stack_candidate(uint32_t sp, uint32_t stack_offset,
                                      uint32_t *unk5, uint32_t *fmts,
                                      uint32_t *numparts, uint32_t *setup) {
    if (!ptr_in_known_space(sp + stack_offset)) return 0;
    int32_t saved_cycles = sh7021.cycles_left;
    uint32_t a = sh7021_bus_read32(sp + stack_offset + 0u);
    uint32_t f = sh7021_bus_read32(sp + stack_offset + 4u);
    uint32_t n = sh7021_bus_read32(sp + stack_offset + 8u);
    uint32_t s = sh7021_bus_read32(sp + stack_offset + 12u);
    sh7021.cycles_left = saved_cycles;
    if (n == 0 || n > 32u || !ptr_in_known_space(f)) return 0;
    if (unk5) *unk5 = a;
    if (fmts) *fmts = f;
    if (numparts) *numparts = n;
    if (setup) *setup = s;
    return 1;
}

static void sh7021_bios_print_dump_parts(void) {
    /* bios_printParts(datas, pals, dims, unk4, unk5, fmts, numparts, setup)
       is the real public BIOS print interface.  Many games can bypass the
       8bpp/15bpp convenience wrappers, so the emulator must dump this image
       source instead of falling back to the displayed frame. */
    uint32_t datas = sh7021.gpr[4];
    uint32_t pals = sh7021.gpr[5];
    uint32_t dims = sh7021.gpr[6];
    uint32_t sp = sh7021.gpr[15];
    uint32_t unk5 = 0, fmts = 0, numparts = 0, setup = 0;
    if (!ptr_in_known_space(datas) || !ptr_in_known_space(dims)) return;
    if (!printparts_stack_candidate(sp, 0u, &unk5, &fmts, &numparts, &setup) &&
        !printparts_stack_candidate(sp, 4u, &unk5, &fmts, &numparts, &setup) &&
        !printparts_stack_candidate(sp, 8u, &unk5, &fmts, &numparts, &setup)) {
        return;
    }

    if (numparts > 16u) numparts = 16u;
    PrintPartImage parts[16];
    memset(parts, 0, sizeof(parts));
    uint32_t valid = 0;
    uint32_t max_w = 0, total_h = 0;

    int32_t saved_cycles = sh7021.cycles_left;
    for (uint32_t i = 0; i < numparts; i++) {
        uint32_t data = sh7021_bus_read32(datas + i * 4u);
        uint32_t pal = ptr_in_known_space(pals) ? sh7021_bus_read32(pals + i * 4u) : 0u;
        uint32_t dim = sh7021_bus_read32(dims + i * 4u);
        uint8_t fmt = sh7021_bus_read8(fmts + i);
        sh7021.cycles_left = saved_cycles;
        if (sh7021_bios_print_decode_part(data, pal, dim, fmt, &parts[valid])) {
            if (parts[valid].width > max_w) max_w = parts[valid].width;
            total_h += parts[valid].height;
            valid++;
        }
    }
    sh7021.cycles_left = saved_cycles;

    if (!valid || max_w == 0 || total_h == 0 || max_w > 1024u || total_h > 4096u) {
        for (uint32_t i = 0; i < valid; i++) free(parts[i].rgb555);
        return;
    }

    uint16_t *composite = (uint16_t *)malloc((size_t)max_w * total_h * sizeof(uint16_t));
    if (!composite) {
        for (uint32_t i = 0; i < valid; i++) free(parts[i].rgb555);
        return;
    }
    for (uint32_t i = 0; i < max_w * total_h; i++) composite[i] = 0x7FFFu;

    uint32_t yoff = 0;
    for (uint32_t i = 0; i < valid; i++) {
        uint32_t xoff = (max_w > parts[i].width) ? ((max_w - parts[i].width) / 2u) : 0u;
        for (uint32_t y = 0; y < parts[i].height; y++) {
            memcpy(&composite[(size_t)(yoff + y) * max_w + xoff],
                   &parts[i].rgb555[(size_t)y * parts[i].width],
                   (size_t)parts[i].width * sizeof(uint16_t));
        }
        yoff += parts[i].height;
    }

    char label[128];
    snprintf(label, sizeof(label), "bios_printParts parts=%u setup=%u", (unsigned)valid, (unsigned)(setup & 1u));
    sh7021_bios_print_emit_source_image(composite, (uint16_t)max_w, (uint16_t)total_h, label);

    /* Real hardware produces one sticker per BIOS print request.  Earlier
       emulator builds also dumped each printParts component as a diagnostic,
       which looked like duplicate printing in the SDL3 frontend.  Keep only
       the composed sticker image here. */

    free(composite);
    for (uint32_t i = 0; i < valid; i++) free(parts[i].rgb555);
}

static void sh7021_bios_print_hook(uint32_t pc) {
    uint32_t data = sh7021.gpr[4];
    uint32_t stamp = (uint32_t)(sh7021.cycles_left ^ sh7021.pc ^ sh7021.gpr[15]);
    /* Prevent accidental duplicate emission if a debug/reset path refetches the
       same BIOS entry without returning.  Repeated legitimate print calls with a
       changed data pointer or later stack/cycle state are still captured. */
    if (pc == bios_print_last_hook_pc && data == bios_print_last_hook_data && stamp == bios_print_last_hook_stamp) return;
    bios_print_last_hook_pc = pc;
    bios_print_last_hook_data = data;
    bios_print_last_hook_stamp = stamp;

    switch (pc) {
    case 0x000006D4u: /* bios_printParts(datas, pals, dims, unk4, unk5, fmts, numparts, setup). */
        sh7021_bios_print_dump_parts();
        break;
    case 0x0000101Cu: /* bios_print15bpp(ushort *data, bool setup), regular 256x224 seal. */
        sh7021_bios_print_dump_15bpp(sh7021.gpr[4], 256u, 224u, "bios_print15bpp");
        break;
    case 0x00001064u: /* bios_print8bpp(uchar *data, ushort *palette, bool setup), regular 256x224 seal. */
        sh7021_bios_print_dump_8bpp(sh7021.gpr[4], sh7021.gpr[5], 256u, 224u, "bios_print8bpp");
        break;
    case 0x00000FD6u: { /* bios_printDirect(data, pal, dim, ..., fmt, setup). */
        uint32_t dim = sh7021.gpr[6];
        uint32_t width = dim & 0xFFFFu;
        uint32_t height = (dim >> 16) & 0xFFFFu;
        if (!width || !height || width > 512u || height > 512u) { width = 256u; height = 224u; }
        /* fmt is the sixth parameter and may be stack-passed by the caller.  If
           palette is nonzero, 8bpp is the usual direct-print case; otherwise
           fall back to 15bpp. */
        if (ptr_in_known_space(sh7021.gpr[5])) sh7021_bios_print_dump_8bpp(sh7021.gpr[4], sh7021.gpr[5], width, height, "bios_printDirect/8bpp");
        else sh7021_bios_print_dump_15bpp(sh7021.gpr[4], width, height, "bios_printDirect/15bpp");
        break;
    }
    default:
        break;
    }
}

static bool can_exec_irq(int prio) {
    int imask = (sh7021.sr >> 4) & 0xF;
    return prio > imask;
}

static void handle_irq(uint64_t param, int cycles_late) {
    /* Hardware IRQ lines are sampled at instruction boundaries.  Timer events
       may still call this one-cycle scheduling shim; keep it as a request latch
       rather than entering the exception immediately in the middle of an
       instruction/delay-slot pair. */
    (void)cycles_late;
    sh7021.pending_irq_prio = (int)(param & 0xFF);
    sh7021.pending_irq_vector = (int)(param >> 8);
}

void sh7021_initialize(void) {
    memset(&sh7021, 0, sizeof(sh7021));
    sh7021_bios_print_reset_dedupe();
    sh7021.pagetable = memory_get_sh7021_pagetable();
    sh7021_set_pc(0x0E000480);
    timing_register_timer(TIMING_CPU_TIMER, &sh7021.cycles_left, sh7021_run);
    irq_func = timing_register_func("SH7021::handle_irq", handle_irq);
    irq_ev = timing_invalid_event_handle();
    (void)irq_ev;
    sh7021_ocpm_bsc_initialize();
    sh7021_ocpm_dmac_initialize();
    sh7021_ocpm_intc_initialize();
    sh7021_ocpm_serial_initialize();
    sh7021_ocpm_timer_initialize();
}

void sh7021_shutdown(void) { }

void sh7021_run(void) {
    while (sh7021.cycles_left > 0) {
        if (sh7021_service_pending_irq()) {
            continue;
        }
        uint32_t fetch_pc = sh7021.pc;
        sh7021_bios_print_hook(fetch_pc);
        uint16_t instr = sh7021_bus_fetch16(fetch_pc);
        sh7021.current_opcode_pc = fetch_pc;
        sh7021.in_delay_slot = 0;
        sh7021.pc = fetch_pc + 2;
        sh7021_interpreter_run(instr);
        sh7021.cycles_left--;

        if (sh7021.m_delay) {
            uint32_t target_pc = sh7021.m_delay;
            sh7021.m_delay = 0;
            uint32_t delay_pc = fetch_pc + 2;
            uint16_t delay_instr = sh7021_bus_fetch16(delay_pc);
            sh7021.current_opcode_pc = delay_pc;
            sh7021.in_delay_slot = 1;
            sh7021.pc = delay_pc + 2;
            sh7021_interpreter_run(delay_instr);
            sh7021.in_delay_slot = 0;
            sh7021.cycles_left--;
            if (sh7021.m_delay) {
                /* A control-transfer instruction in a delay slot raises the
                   SH-1 slot-illegal exception.  Do not land at either nested
                   target or the original branch target after taking it. */
                sh7021.m_delay = 0;
                sh7021_raise_slot_illegal();
            } else {
                sh7021.pc = target_pc;
                sh7021_bus_fetch_reset();
            }
        }
    }
}

void sh7021_assert_irq(int vector_id, int prio) {
    sh7021.pending_irq_vector = vector_id;
    sh7021.pending_irq_prio = prio;
    sh7021_irq_check();
}

void sh7021_irq_check(void) {
    /* Pending interrupt lines are sampled by sh7021_run() at instruction
       boundaries.  Do not enter via a separate timing event here; doing so can
       incorrectly re-enter across a delayed branch or after service has already
       raised SR.IMASK. */
    (void)irq_func;
}

void sh7021_block_irq_next(void) {
    sh7021.irq_delay = 1;
}

int sh7021_service_pending_irq(void) {
    if (sh7021.irq_delay > 0) {
        sh7021.irq_delay--;
        return 0;
    }
    if (!can_exec_irq(sh7021.pending_irq_prio)) return 0;
    int prio = sh7021.pending_irq_prio;
    int vector = sh7021.pending_irq_vector;
    if (prio < 0) prio = 0;
    if (prio > 15) prio = 15;
    sh7021_raise_exception(vector);
    sh7021.sr &= ~0xF0u;
    sh7021.sr |= (uint32_t)prio << 4;
    sh7021.pending_irq_prio = 0;
    return 1;
}

void sh7021_raise_exception(int vector_id) {
    assert(vector_id < 0x100);
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], sh7021.sr);
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], sh7021.pc);
    uint32_t vector_addr = sh7021.vbr + ((uint32_t)vector_id * 4u);
    uint32_t new_pc = sh7021_bus_read32(vector_addr);
    sh7021_set_pc(new_pc);
}

void sh7021_raise_slot_illegal(void) {
    uint32_t saved_pc = sh7021.current_opcode_pc;
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], sh7021.sr);
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], saved_pc);
    uint32_t new_pc = sh7021_bus_read32(sh7021.vbr + 6u * 4u);
    sh7021_set_pc(new_pc);
}

void sh7021_set_pc(uint32_t new_pc) { sh7021.pc = new_pc; sh7021.m_delay = 0; sh7021_bus_fetch_reset(); }
void sh7021_set_sr(uint32_t new_sr) { sh7021.sr = new_sr & 0x3F3u; sh7021_irq_check(); }

uint32_t sh7021_state_blob_size(void) { return (uint32_t)(sizeof(sh7021) - sizeof(sh7021.pagetable)); }
void sh7021_get_state_blob(void *dst, uint32_t size) {
    if (!dst || size != sh7021_state_blob_size()) return;
    SH7021CPU tmp = sh7021;
    tmp.pagetable = NULL;
    memcpy(dst, &tmp, size);
}
void sh7021_set_state_blob(const void *src, uint32_t size) {
    if (!src || size != sh7021_state_blob_size()) return;
    uint8_t **pt = memory_get_sh7021_pagetable();
    memcpy(&sh7021, src, size);
    sh7021.pagetable = pt;
}
