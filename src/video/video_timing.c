#include "video/video_timing.h"
#include "core/timing.h"

int video_timing_cpu_cycles_per_ntsc_line(void)
{
    return VIDEO_NTSC_LINE_CYCLES;
}

static int hcount_delta_to_cpu_cycles_ceil(int hcount_delta)
{
    const int64_t den = VIDEO_NTSC_LINE_CLOCKS;
    const int64_t num = 4LL * VIDEO_NTSC_LINE_CYCLES;
    return (int)(((int64_t)hcount_delta * num + den - 1) / den);
}

int video_timing_hcount_to_cpu_cycles(int signed_h)
{
    if (signed_h < VIDEO_NTSC_H_MIN || signed_h > VIDEO_NTSC_H_ACTIVE_END) return -1;
    int cycles = hcount_delta_to_cpu_cycles_ceil(signed_h - VIDEO_NTSC_H_MIN);
    int line_cycles = video_timing_cpu_cycles_per_ntsc_line();
    /* H=257 is the final VDP edge of the line and falls between the last CPU
       cycle and the line-boundary event.  Clamp it to that last cycle so the
       assertion still precedes the next line's H=-84 origin. */
    if (cycles >= line_cycles && signed_h == VIDEO_NTSC_H_ACTIVE_END) cycles = line_cycles - 1;
    return (cycles >= 0 && cycles < line_cycles) ? cycles : -1;
}

int video_timing_cpu_cycles_to_hcount(int64_t elapsed_cpu)
{
    int line_cycles = video_timing_cpu_cycles_per_ntsc_line();
    if (line_cycles <= 0) return VIDEO_NTSC_H_MIN;
    if (elapsed_cpu < 0) elapsed_cpu = 0;
    if (elapsed_cpu >= line_cycles) elapsed_cpu %= line_cycles;

    int64_t vdp_clock = (elapsed_cpu * VIDEO_NTSC_LINE_CLOCKS) / line_cycles;
    int h = VIDEO_NTSC_H_MIN + (int)(vdp_clock / 4);
    if (h > VIDEO_NTSC_H_ACTIVE_END) h = VIDEO_NTSC_H_ACTIVE_END;
    return h;
}
