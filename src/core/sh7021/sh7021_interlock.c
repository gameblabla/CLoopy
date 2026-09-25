#include "core/sh7021/sh7021_interlock.h"

uint16_t sh7021_interlock_gpr_read_mask(uint16_t opcode) {
    uint32_t n = (opcode >> 8) & 15u;
    uint32_t m = (opcode >> 4) & 15u;
    uint16_t rn = (uint16_t)(1u << n);
    uint16_t rm = (uint16_t)(1u << m);

    switch (opcode >> 12) {
    case 0x0: {
        switch (opcode & 0x3fu) {
        case 0x04: case 0x05: case 0x06:
        case 0x14: case 0x15: case 0x16:
        case 0x24: case 0x25: case 0x26:
        case 0x34: case 0x35: case 0x36:
            return (uint16_t)(rm | rn | 1u); /* Rm,@(R0,Rn) */
        case 0x0c: case 0x0d: case 0x0e:
        case 0x1c: case 0x1d: case 0x1e:
        case 0x2c: case 0x2d: case 0x2e:
        case 0x3c: case 0x3d: case 0x3e:
            return (uint16_t)(rm | 1u);      /* @(R0,Rm),Rn */
        case 0x0b: return 0;                 /* RTS */
        case 0x2b: return (uint16_t)(1u << 15); /* RTE stack */
        default: return 0;
        }
    }
    case 0x1: return (uint16_t)(rm | rn);    /* MOV.L Rm,@(disp,Rn) */
    case 0x2: return (uint16_t)(rm | rn);    /* stores / two-register ALU */
    case 0x3: return (uint16_t)(rm | rn);    /* two-register ALU */
    case 0x4: {
        switch (opcode & 0x3fu) {
        case 0x02: case 0x03: case 0x06: case 0x07:
        case 0x0a: case 0x0b: case 0x0e:
        case 0x12: case 0x13: case 0x16: case 0x17:
        case 0x1a: case 0x1b: case 0x1e:
        case 0x22: case 0x23: case 0x26: case 0x27:
        case 0x2a: case 0x2b: case 0x2e:
            return rn;
        case 0x0f: case 0x1f: case 0x2f: case 0x3f:
            return (uint16_t)(rm | rn);      /* MAC.W @Rm+,@Rn+ */
        default:
            if ((opcode & 0x0fu) == 0x0u || (opcode & 0x0fu) == 0x1u ||
                (opcode & 0x0fu) == 0x4u || (opcode & 0x0fu) == 0x5u ||
                (opcode & 0x3fu) == 0x11u || (opcode & 0x3fu) == 0x15u ||
                (opcode & 0x3fu) == 0x20u || (opcode & 0x3fu) == 0x21u ||
                (opcode & 0x3fu) == 0x24u || (opcode & 0x3fu) == 0x25u ||
                (opcode & 0x3fu) == 0x28u || (opcode & 0x3fu) == 0x29u)
                return rn;
            return 0;
        }
    }
    case 0x5: return rm;                     /* MOV.L @(disp,Rm),Rn */
    case 0x6: return rm;                     /* load / MOV / unary Rm,Rn */
    case 0x7: return rn;                     /* ADD #imm,Rn */
    case 0x8:
        switch ((opcode >> 8) & 15u) {
        case 0: case 1: return (uint16_t)(rm | 1u); /* R0,@(disp,Rm) */
        case 4: case 5: return rm;                    /* @(disp,Rm),R0 */
        case 8: return 1u;                            /* CMP/EQ #imm,R0 */
        default: return 0;                            /* branches */
        }
    case 0x9: return 0;                       /* PC-relative load */
    case 0xa: case 0xb: return 0;              /* BRA / BSR */
    case 0xc:
        switch ((opcode >> 8) & 15u) {
        case 0: case 1: case 2:               /* R0,@(disp,GBR) */
        case 8: case 9: case 10: case 11:     /* immediate ALU R0 */
        case 12: case 13: case 14: case 15:   /* @(R0,GBR) */
            return 1u;
        case 3: return (uint16_t)(1u << 15);  /* TRAPA stack */
        default: return 0;
        }
    case 0xd: case 0xe: case 0xf: return 0;
    default: return 0;
    }
}

int sh7021_interlock_loaded_gpr(uint16_t opcode) {
    uint32_t n = (opcode >> 8) & 15u;
    switch (opcode >> 12) {
    case 0x0:
        switch (opcode & 0x3fu) {
        case 0x0c: case 0x0d: case 0x0e:
        case 0x1c: case 0x1d: case 0x1e:
        case 0x2c: case 0x2d: case 0x2e:
        case 0x3c: case 0x3d: case 0x3e:
            return (int)n;
        default: return -1;
        }
    case 0x5: return (int)n;
    case 0x6:
        switch (opcode & 15u) {
        case 0: case 1: case 2: case 4: case 5: case 6: return (int)n;
        default: return -1;
        }
    case 0x8:
        switch ((opcode >> 8) & 15u) {
        case 4: case 5: return 0;
        default: return -1;
        }
    case 0x9: return (int)n;
    case 0xc:
        switch ((opcode >> 8) & 15u) {
        case 4: case 5: case 6: return 0;
        default: return -1;
        }
    case 0xd: return (int)n; /* MOV.L @(disp,PC),Rn: memory load, not MOV #imm. */
    default: return -1;
    }
}

int sh7021_interlock_stall_cycles(uint8_t load_valid, uint8_t load_reg, uint16_t next_opcode) {
    if (!load_valid || load_reg >= 16u) return 0;
    return (sh7021_interlock_gpr_read_mask(next_opcode) & (uint16_t)(1u << load_reg)) ? 1 : 0;
}
