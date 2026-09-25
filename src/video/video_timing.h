#ifndef LOOPY_VIDEO_TIMING_H
#define LOOPY_VIDEO_TIMING_H
#include <stdint.h>

/* Hardware timing uses the standalone test ROM's full -84..257 compare range.
   Timinglab R3 measures about 296.39 HCOUNT units per 1000 CPU cycles. */
#define VIDEO_NTSC_H_MIN (-84)
#define VIDEO_NTSC_H_ACTIVE_START 0
#define VIDEO_NTSC_H_ACTIVE_END 257
#define VIDEO_NTSC_LINE_CLOCKS 1363
#define VIDEO_NTSC_LINE_CYCLES 1019

int video_timing_cpu_cycles_per_ntsc_line(void);
int video_timing_hcount_to_cpu_cycles(int signed_h);
int video_timing_cpu_cycles_to_hcount(int64_t elapsed_cpu);

#endif
