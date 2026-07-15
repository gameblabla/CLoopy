#ifndef LOOPY_SH7021_LOCAL_H
#define LOOPY_SH7021_LOCAL_H
#include <stdint.h>

typedef struct SH7021CPU {
    uint32_t gpr[16];
    uint32_t pc;
    uint32_t pr;
    uint32_t macl, mach;
    uint32_t gbr, vbr;
    uint32_t sr;
    uint32_t ea;
    uint32_t m_delay;
    uint32_t current_opcode_pc;
    uint8_t in_delay_slot;
    uint8_t sleep_mode;
    int32_t cycles_left;
    int pending_irq_prio;
    int pending_irq_vector;
    int irq_delay;
    uint8_t **pagetable;
} SH7021CPU;

extern SH7021CPU sh7021;

/* Idle-loop detection state.  Deliberately kept out of SH7021CPU: the savestate
   blob is a raw copy of that struct, and this is pure detection state that is
   rebuilt from scratch every timeslice, so it must neither grow the blob nor be
   restored from one.  sh7021_bus.c sets these; sh7021_run() consumes them. */
extern uint8_t sh7021_idle_wrote_mem;
extern uint8_t sh7021_idle_unsafe_read;

void sh7021_assert_irq(int vector_id, int prio);
void sh7021_irq_check(void);
void sh7021_block_irq_next(void);
int sh7021_service_pending_irq(void);
void sh7021_raise_exception(int vector_id);
void sh7021_raise_slot_illegal(void);
void sh7021_set_pc(uint32_t new_pc);
void sh7021_set_sr(uint32_t new_sr);

#endif
