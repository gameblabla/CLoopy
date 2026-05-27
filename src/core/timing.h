#ifndef LOOPY_TIMING_H
#define LOOPY_TIMING_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TIMING_F_CPU (16 * 1000 * 1000)
#define TIMING_MAX_SLICE_LENGTH 512LL
#define TIMING_MAX_TIMESTAMP INT64_MAX

typedef enum TimingTimerId {
    TIMING_CPU_TIMER,
    TIMING_NUM_TIMERS,
    TIMING_INVALID_TIMER
} TimingTimerId;

typedef int64_t TimingUnitCycle;
typedef void (*TimingTimerFunc)(void);
typedef void (*TimingEventFunc)(uint64_t param, int cycles_late);

typedef struct TimingFuncHandle { int value; } TimingFuncHandle;
typedef struct TimingEventHandle { int64_t value; } TimingEventHandle;

static inline TimingFuncHandle timing_invalid_func_handle(void) { TimingFuncHandle h = { -1 }; return h; }
static inline TimingEventHandle timing_invalid_event_handle(void) { TimingEventHandle h = { -1 }; return h; }
static inline bool timing_func_handle_is_valid(TimingFuncHandle h) { return h.value >= 0; }
static inline bool timing_event_handle_is_valid(TimingEventHandle h) { return h.value >= 0; }
static inline int timing_event_handle_get_timer_id(TimingEventHandle h) { return (int)(h.value & 0xFF); }
static inline int64_t timing_event_handle_get_ev_id(TimingEventHandle h) { return h.value >> 8; }

void timing_initialize(void);
void timing_shutdown(void);
void timing_register_timer(TimingTimerId id, int32_t *cycle_count, TimingTimerFunc func);
TimingFuncHandle timing_register_func(const char *name, TimingEventFunc func);
TimingEventHandle timing_add_event(TimingFuncHandle func, TimingUnitCycle cycles, uint64_t param, int core);
void timing_cancel_event(TimingEventHandle *handle);
void timing_process_slice(int id, int32_t slice);
int64_t timing_calc_slice_length(int id);
int64_t timing_get_timestamp(int id);
TimingUnitCycle timing_convert_cpu(int64_t cycles);
TimingUnitCycle timing_convert_frequency(int64_t num, int64_t freq);
int timing_save_state(FILE *file);
int timing_load_state(FILE *file);
uint32_t timing_state_blob_size(void);
int timing_get_state_blob(void *dst, uint32_t size);
int timing_set_state_blob(const void *src, uint32_t size);

#endif
