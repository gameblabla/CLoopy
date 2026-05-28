#include "core/timing.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

typedef struct RegisteredFunc {
    char *name;
    TimingEventFunc func;
} RegisteredFunc;

typedef struct Event {
    int64_t exec_time;
    uint64_t param;
    TimingEventFunc func;
    int64_t id;
} Event;

typedef struct Timer {
    int64_t timestamp;
    int64_t next_event_id;
    int32_t slice_length;
    int32_t *cycles_left;
    Event *events;
    size_t event_count;
    size_t event_capacity;
    TimingTimerFunc func;
    int id;
    bool in_slice;
} Timer;

typedef struct TimingState {
    Timer *cur_timer;
    RegisteredFunc *funcs;
    size_t func_count;
    size_t func_capacity;
    Timer timers[TIMING_NUM_TIMERS];
} TimingState;

static TimingState state;

static char *timing_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static int event_greater(const Event *l, const Event *r) {
    return l->exec_time > r->exec_time;
}

static void event_swap(Event *a, Event *b) {
    Event t = *a; *a = *b; *b = t;
}

static void heap_push(Timer *timer, Event ev) {
    if (timer->event_count == timer->event_capacity) {
        size_t nc = timer->event_capacity ? timer->event_capacity * 2 : 16;
        Event *ne = (Event *)realloc(timer->events, nc * sizeof(Event));
        assert(ne);
        timer->events = ne;
        timer->event_capacity = nc;
    }
    size_t i = timer->event_count++;
    timer->events[i] = ev;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!event_greater(&timer->events[p], &timer->events[i])) break;
        event_swap(&timer->events[p], &timer->events[i]);
        i = p;
    }
}

static Event heap_pop(Timer *timer) {
    assert(timer->event_count > 0);
    Event ret = timer->events[0];
    timer->events[0] = timer->events[--timer->event_count];
    size_t i = 0;
    for (;;) {
        size_t l = i * 2 + 1;
        size_t r = l + 1;
        size_t s = i;
        if (l < timer->event_count && event_greater(&timer->events[s], &timer->events[l])) s = l;
        if (r < timer->event_count && event_greater(&timer->events[s], &timer->events[r])) s = r;
        if (s == i) break;
        event_swap(&timer->events[i], &timer->events[s]);
        i = s;
    }
    return ret;
}

static void heapify(Timer *timer) {
    if (timer->event_count < 2) return;
    for (ptrdiff_t i = (ptrdiff_t)(timer->event_count / 2); i >= 0; --i) {
        size_t p = (size_t)i;
        for (;;) {
            size_t l = p * 2 + 1, r = l + 1, s = p;
            if (l < timer->event_count && event_greater(&timer->events[s], &timer->events[l])) s = l;
            if (r < timer->event_count && event_greater(&timer->events[s], &timer->events[r])) s = r;
            if (s == p) break;
            event_swap(&timer->events[p], &timer->events[s]);
            p = s;
        }
        if (i == 0) break;
    }
}

static int32_t timer_get_cycles_left(Timer *timer) { return *timer->cycles_left; }
static void timer_set_cycles_left(Timer *timer, int32_t sched_cycles) { *timer->cycles_left = sched_cycles; }
static int64_t timer_get_timestamp(Timer *timer) {
    int64_t result = timer->timestamp;
    if (timer->in_slice) result += timer->slice_length - (int64_t)timer_get_cycles_left(timer);
    return result;
}

static Timer *get_timer(int id) {
    if (id < 0) return state.cur_timer;
    assert(id < TIMING_NUM_TIMERS);
    return &state.timers[id];
}

static void process_events(void) {
    Timer *timer = state.cur_timer;
    int32_t cycles_executed = timer->slice_length - timer_get_cycles_left(timer);
    timer->timestamp += cycles_executed;
    timer->slice_length = 0;
    timer_set_cycles_left(timer, 0);
    timer->in_slice = false;
    while (timer->event_count && timer->events[0].exec_time <= timer_get_timestamp(timer)) {
        Event ev = heap_pop(timer);
        int cycles_late = (int)(timer->timestamp - ev.exec_time);
        ev.func(ev.param, cycles_late);
    }
}

static void set_cur_timer(int id, int32_t slice) {
    assert(id >= 0);
    Timer *timer = get_timer(id);
    timer->slice_length = slice;
    timer_set_cycles_left(timer, slice);
    timer->in_slice = true;
    state.cur_timer = timer;
}

void timing_initialize(void) {
    memset(&state, 0, sizeof(state));
    for (int i = 0; i < TIMING_NUM_TIMERS; i++) state.timers[i].id = i;
}

void timing_shutdown(void) {
    for (size_t i = 0; i < state.func_count; i++) free(state.funcs[i].name);
    free(state.funcs);
    for (int i = 0; i < TIMING_NUM_TIMERS; i++) free(state.timers[i].events);
    memset(&state, 0, sizeof(state));
}

void timing_register_timer(TimingTimerId id, int32_t *cycle_count, TimingTimerFunc func) {
    assert(!state.cur_timer);
    assert(cycle_count);
    assert(id < TIMING_NUM_TIMERS);
    state.timers[id].cycles_left = cycle_count;
    state.timers[id].id = id;
    state.timers[id].func = func;
}

TimingFuncHandle timing_register_func(const char *name, TimingEventFunc func) {
    if (state.func_count == state.func_capacity) {
        size_t nc = state.func_capacity ? state.func_capacity * 2 : 32;
        RegisteredFunc *nf = (RegisteredFunc *)realloc(state.funcs, nc * sizeof(RegisteredFunc));
        assert(nf);
        state.funcs = nf;
        state.func_capacity = nc;
    }
    state.funcs[state.func_count].name = timing_strdup(name ? name : "");
    state.funcs[state.func_count].func = func;
    TimingFuncHandle handle = { (int)state.func_count };
    state.func_count++;
    return handle;
}

TimingEventHandle timing_add_event(TimingFuncHandle func, TimingUnitCycle cycles, uint64_t param, int core) {
    assert(timing_func_handle_is_valid(func));
    Timer *timer = get_timer(core);
    RegisteredFunc *reg_func = &state.funcs[func.value];
    Event ev;
    ev.func = reg_func->func;
    ev.param = param;
    ev.id = (timer->next_event_id << 8) | timer->id;
    timer->next_event_id++;
    int64_t raw_cycles = (int64_t)cycles;
    ev.exec_time = timer_get_timestamp(timer) + raw_cycles;
    int32_t raw_cycles_left = timer_get_cycles_left(timer);
    if (timer->in_slice && raw_cycles < raw_cycles_left && timer == state.cur_timer) {
        timer->slice_length -= raw_cycles_left - (int32_t)raw_cycles;
        timer_set_cycles_left(timer, (int32_t)raw_cycles);
    }
    heap_push(timer, ev);
    TimingEventHandle handle = { ev.id };
    return handle;
}

void timing_cancel_event(TimingEventHandle *ev) {
    assert(ev && timing_event_handle_is_valid(*ev));
    Timer *timer = get_timer(timing_event_handle_get_timer_id(*ev));
    int found = 0;
    for (size_t i = 0; i < timer->event_count; i++) {
        if (timer->events[i].id == ev->value) {
            timer->events[i] = timer->events[--timer->event_count];
            heapify(timer);
            found = 1;
            break;
        }
    }
    assert(found);
    ev->value = -1;
}

void timing_process_slice(int id, int32_t slice) {
    set_cur_timer(id, slice);
    state.cur_timer->func();
    process_events();
}

int64_t timing_calc_slice_length(int id) {
    Timer *timer = get_timer(id);
    if (!timer->event_count) return TIMING_MAX_SLICE_LENGTH;
    int64_t next_event_delta = timer->events[0].exec_time - timer_get_timestamp(timer);
    return next_event_delta < TIMING_MAX_SLICE_LENGTH ? next_event_delta : TIMING_MAX_SLICE_LENGTH;
}

int64_t timing_get_timestamp(int id) {
    Timer *timer = get_timer(id);
    return timer_get_timestamp(timer);
}

TimingUnitCycle timing_convert_frequency(int64_t num, int64_t freq) {
    int64_t max_value = TIMING_MAX_TIMESTAMP / freq;
    if (num / freq > max_value) return TIMING_MAX_TIMESTAMP;
    if (num > max_value) return (num / freq) * TIMING_F_CPU;
    return num * TIMING_F_CPU / freq;
}

TimingUnitCycle timing_convert_cpu(int64_t cycles) { return timing_convert_frequency(cycles, TIMING_F_CPU); }

typedef struct TimingSaveHeader { uint32_t magic; uint32_t version; int32_t cur_timer_id; uint32_t timer_count; } TimingSaveHeader;
typedef struct TimerSaveHeader { int64_t timestamp; int64_t next_event_id; int32_t slice_length; uint32_t event_count; int32_t in_slice; } TimerSaveHeader;
typedef struct EventSave { int64_t exec_time; uint64_t param; int32_t func_index; int64_t id; } EventSave;

static int timing_func_index(TimingEventFunc func) {
    for (size_t i = 0; i < state.func_count; i++) if (state.funcs[i].func == func) return (int)i;
    return -1;
}

int timing_save_state(FILE *file) {
    if (!file) return -1;
    TimingSaveHeader h;
    h.magic = 0x54494D47u; /* TIMG */
    h.version = 1;
    h.cur_timer_id = state.cur_timer ? state.cur_timer->id : -1;
    h.timer_count = TIMING_NUM_TIMERS;
    if (fwrite(&h, sizeof(h), 1, file) != 1) return -1;
    for (int i = 0; i < TIMING_NUM_TIMERS; i++) {
        Timer *t = &state.timers[i];
        TimerSaveHeader th;
        th.timestamp = t->timestamp;
        th.next_event_id = t->next_event_id;
        th.slice_length = t->slice_length;
        th.event_count = (uint32_t)t->event_count;
        th.in_slice = t->in_slice ? 1 : 0;
        if (fwrite(&th, sizeof(th), 1, file) != 1) return -1;
        for (size_t e = 0; e < t->event_count; e++) {
            EventSave es;
            es.exec_time = t->events[e].exec_time;
            es.param = t->events[e].param;
            es.func_index = timing_func_index(t->events[e].func);
            es.id = t->events[e].id;
            if (es.func_index < 0) return -1;
            if (fwrite(&es, sizeof(es), 1, file) != 1) return -1;
        }
    }
    return 0;
}

int timing_load_state(FILE *file) {
    if (!file) return -1;
    TimingSaveHeader h;
    if (fread(&h, sizeof(h), 1, file) != 1) return -1;
    if (h.magic != 0x54494D47u || h.version != 1 || h.timer_count != TIMING_NUM_TIMERS) return -1;
    for (int i = 0; i < TIMING_NUM_TIMERS; i++) {
        TimerSaveHeader th;
        if (fread(&th, sizeof(th), 1, file) != 1) return -1;
        Timer *t = &state.timers[i];
        t->timestamp = th.timestamp;
        t->next_event_id = th.next_event_id;
        t->slice_length = th.slice_length;
        t->in_slice = th.in_slice ? true : false;
        if (t->event_capacity < th.event_count) {
            Event *ne = (Event *)realloc(t->events, (size_t)th.event_count * sizeof(Event));
            if (!ne && th.event_count) return -1;
            t->events = ne;
            t->event_capacity = th.event_count;
        }
        t->event_count = 0;
        for (size_t e = 0; e < th.event_count; e++) {
            EventSave es;
            if (fread(&es, sizeof(es), 1, file) != 1) return -1;
            if (es.func_index < 0 || (size_t)es.func_index >= state.func_count) continue;
            size_t dst = t->event_count++;
            t->events[dst].exec_time = es.exec_time;
            t->events[dst].param = es.param;
            t->events[dst].func = state.funcs[es.func_index].func;
            t->events[dst].id = es.id;
        }
        heapify(t);
    }
    state.cur_timer = (h.cur_timer_id >= 0 && h.cur_timer_id < TIMING_NUM_TIMERS) ? &state.timers[h.cur_timer_id] : NULL;
    return 0;
}


uint32_t timing_state_blob_size(void) {
    uint64_t total = sizeof(TimingSaveHeader);
    for (int i = 0; i < TIMING_NUM_TIMERS; i++) {
        total += sizeof(TimerSaveHeader);
        total += (uint64_t)state.timers[i].event_count * sizeof(EventSave);
    }
    return total > 0xFFFFFFFFu ? 0u : (uint32_t)total;
}

static int timing_blob_write(uint8_t **p, uint8_t *end, const void *src, size_t n) {
    if (*p > end || (size_t)(end - *p) < n) return -1;
    if (n) memcpy(*p, src, n);
    *p += n;
    return 0;
}

static int timing_blob_read(const uint8_t **p, const uint8_t *end, void *dst, size_t n) {
    if (*p > end || (size_t)(end - *p) < n) return -1;
    if (n) memcpy(dst, *p, n);
    *p += n;
    return 0;
}

int timing_get_state_blob(void *dst, uint32_t size) {
    if (!dst) return -1;
    uint32_t need = timing_state_blob_size();
    if (!need || size != need) return -1;
    uint8_t *p = (uint8_t *)dst;
    uint8_t *end = p + size;
    TimingSaveHeader h;
    h.magic = 0x54494D47u;
    h.version = 1;
    h.cur_timer_id = state.cur_timer ? state.cur_timer->id : -1;
    h.timer_count = TIMING_NUM_TIMERS;
    if (timing_blob_write(&p, end, &h, sizeof(h)) != 0) return -1;
    for (int i = 0; i < TIMING_NUM_TIMERS; i++) {
        Timer *t = &state.timers[i];
        TimerSaveHeader th;
        th.timestamp = t->timestamp;
        th.next_event_id = t->next_event_id;
        th.slice_length = t->slice_length;
        th.event_count = (uint32_t)t->event_count;
        th.in_slice = t->in_slice ? 1 : 0;
        if (timing_blob_write(&p, end, &th, sizeof(th)) != 0) return -1;
        for (size_t e = 0; e < t->event_count; e++) {
            EventSave es;
            es.exec_time = t->events[e].exec_time;
            es.param = t->events[e].param;
            es.func_index = timing_func_index(t->events[e].func);
            es.id = t->events[e].id;
            if (es.func_index < 0) return -1;
            if (timing_blob_write(&p, end, &es, sizeof(es)) != 0) return -1;
        }
    }
    return p == end ? 0 : -1;
}

int timing_set_state_blob(const void *src, uint32_t size) {
    if (!src || size < sizeof(TimingSaveHeader)) return -1;
    const uint8_t *p = (const uint8_t *)src;
    const uint8_t *end = p + size;
    TimingSaveHeader h;
    if (timing_blob_read(&p, end, &h, sizeof(h)) != 0) return -1;
    if (h.magic != 0x54494D47u || h.version != 1 || h.timer_count != TIMING_NUM_TIMERS) return -1;
    for (int i = 0; i < TIMING_NUM_TIMERS; i++) {
        TimerSaveHeader th;
        if (timing_blob_read(&p, end, &th, sizeof(th)) != 0) return -1;
        Timer *t = &state.timers[i];
        t->timestamp = th.timestamp;
        t->next_event_id = th.next_event_id;
        t->slice_length = th.slice_length;
        t->in_slice = th.in_slice ? true : false;
        if (t->event_capacity < th.event_count) {
            Event *ne = (Event *)realloc(t->events, (size_t)th.event_count * sizeof(Event));
            if (!ne && th.event_count) return -1;
            t->events = ne;
            t->event_capacity = th.event_count;
        }
        t->event_count = 0;
        for (size_t e = 0; e < th.event_count; e++) {
            EventSave es;
            if (timing_blob_read(&p, end, &es, sizeof(es)) != 0) return -1;
            if (es.func_index < 0 || (size_t)es.func_index >= state.func_count) continue;
            size_t dst = t->event_count++;
            t->events[dst].exec_time = es.exec_time;
            t->events[dst].param = es.param;
            t->events[dst].func = state.funcs[es.func_index].func;
            t->events[dst].id = es.id;
        }
        heapify(t);
    }
    if (p != end) return -1;
    state.cur_timer = (h.cur_timer_id >= 0 && h.cur_timer_id < TIMING_NUM_TIMERS) ? &state.timers[h.cur_timer_id] : NULL;
    return 0;
}
