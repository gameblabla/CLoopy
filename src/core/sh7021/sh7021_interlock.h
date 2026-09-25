#ifndef LOOPY_SH7021_INTERLOCK_H
#define LOOPY_SH7021_INTERLOCK_H
#include <stdint.h>

/* SH-1 one-cycle load/use interlock helpers.  Only instructions that actually
   load a GPR from memory are classified as producers. */
uint16_t sh7021_interlock_gpr_read_mask(uint16_t opcode);
int sh7021_interlock_loaded_gpr(uint16_t opcode);
int sh7021_interlock_stall_cycles(uint8_t load_valid, uint8_t load_reg, uint16_t next_opcode);

#endif
