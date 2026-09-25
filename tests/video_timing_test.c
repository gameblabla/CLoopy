#include "video/video_timing.h"
#include <stdio.h>
#include <stdlib.h>

static void expecti(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d expected %d\n", name, got, want);
        exit(1);
    }
}

int main(void) {
    int line = video_timing_cpu_cycles_per_ntsc_line();
    expecti("NTSC line cycles", line, 1019);
    expecti("H=-84 origin", video_timing_hcount_to_cpu_cycles(-84), 0);
    expecti("H=0 active start", video_timing_hcount_to_cpu_cycles(0), 252);
    expecti("H=257 right edge", video_timing_hcount_to_cpu_cycles(257), line - 1);
    expecti("H below range", video_timing_hcount_to_cpu_cycles(-85), -1);
    expecti("H above range", video_timing_hcount_to_cpu_cycles(258), -1);

    expecti("counter at origin", video_timing_cpu_cycles_to_hcount(0), -84);
    expecti("counter before active", video_timing_cpu_cycles_to_hcount(251), -1);
    expecti("counter at active start", video_timing_cpu_cycles_to_hcount(252), 0);
    expecti("counter at final CPU cycle", video_timing_cpu_cycles_to_hcount(line - 1), 256);
    expecti("counter wraps at line", video_timing_cpu_cycles_to_hcount(line), -84);

    if (!(video_timing_hcount_to_cpu_cycles(0) < video_timing_hcount_to_cpu_cycles(257))) {
        fprintf(stderr, "HSYNC release must precede assertion within a raster line\n");
        return 1;
    }

    puts("video_timing_test: OK");
    return 0;
}
