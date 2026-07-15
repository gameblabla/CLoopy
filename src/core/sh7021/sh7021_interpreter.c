/*
 * MAME-derived SH-1/SH7021 instruction executor for LoopyMSE.
 *
 * Based on MAME src/devices/cpu/sh/sh.cpp and sh7021.cpp from the
 * user-supplied mame-master archive. Original license: BSD-3-Clause.
 * Original copyright holders as listed by MAME include David Haywood,
 * Juergen Buchmueller, R. Belmont, and contributors.
 *
 * This file has been mechanically and manually adapted to C11 and to the
 * CLoopy memory/peripheral interface.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "core/sh7021/sh7021_bus.h"
#include "core/sh7021/sh7021_interpreter.h"
#include "core/sh7021/sh7021_local.h"

// This is a remanant from the SH-1 core in LoopyMSE. This is only to be used for testing or finding regressions, do not use
#define BUSY_LOOP_HACKS 0

#define REG_N ((opcode >> 8) & 15u)
#define REG_M ((opcode >> 4) & 15u)
#define SH_T 0x00000001u
#define SH_S 0x00000002u
#define SH_I 0x000000f0u
#define SH_Q 0x00000100u
#define SH_M 0x00000200u
#define SH_FLAGS (SH_M | SH_Q | SH_I | SH_S | SH_T)
#define SH7021_MAME_BIT(x, n) (((uint32_t)(x) >> (n)) & 1u)
#if defined(__GNUC__) || defined(__clang__)
#define SH7021_UNUSED_FN __attribute__((unused))
#else
#define SH7021_UNUSED_FN
#endif

static uint32_t sh7021_rotl32(uint32_t v, unsigned c) { return (v << c) | (v >> (32u - c)); }
static uint32_t sh7021_rotr32(uint32_t v, unsigned c) { return (v >> c) | (v << (32u - c)); }

static int32_t sh7021_mame_sext(uint32_t value, unsigned bits) {
    if (bits >= 32u) return (int32_t)value;
    const uint32_t sign = 1u << (bits - 1u);
    const uint32_t mask = (1u << bits) - 1u;
    value &= mask;
    return (int32_t)((value ^ sign) - sign);
}

/* SH-1/SH7021 only implements bits 9..0 of MACH.  When MACH is read or
 * stored, bit 9 is sign-extended through bits 31..10.  Keep the internal
 * register in that canonical SH-1 form after all MACH writes/updates. */
static uint32_t sh7021_mach_sh1_canonical(uint32_t value) {
    value &= 0x000003ffu;
    if (value & 0x00000200u)
        value |= 0xfffffc00u;
    return value;
}

static uint32_t sh7021_mach_sh1_read(void) {
    return sh7021_mach_sh1_canonical(sh7021.mach);
}

static void sh7021_mame_check_irq_after_sr_change(void) {
    sh7021_irq_check();
}

static void LDCSR(const uint16_t opcode);
static void LDCMSR(const uint16_t opcode);
static void RTE(void);
static void TRAPA(uint32_t i);
static void ILLEGAL(void);
static void execute_one_f000(uint16_t opcode);

static void ADD(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] += sh7021.gpr[m];
}

/*  code                 cycles  t-bit
 *  0111 nnnn iiii iiii  1       -
 *  ADD     #imm,Rn
 */
static void ADDI(uint32_t i, uint32_t n)
{
	sh7021.gpr[n] += sh7021_mame_sext(i, 8);
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 1110  1       carry
 *  ADDC    Rm,Rn
 */
static void ADDC(uint32_t m, uint32_t n)
{
	uint32_t tmp0, tmp1;

	tmp1 = sh7021.gpr[n] + sh7021.gpr[m];
	tmp0 = sh7021.gpr[n];
	sh7021.gpr[n] = tmp1 + (sh7021.sr & SH_T);
	if (tmp0 > tmp1)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
	if (tmp1 > sh7021.gpr[n])
		sh7021.sr |= SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 1111  1       overflow
 *  ADDV    Rm,Rn
 */
static void ADDV(uint32_t m, uint32_t n)
{
	int32_t dest = SH7021_MAME_BIT(sh7021.gpr[n], 31);
	int32_t src = SH7021_MAME_BIT(sh7021.gpr[m], 31);
	src += dest;

	sh7021.gpr[n] += sh7021.gpr[m];

	int32_t ans = SH7021_MAME_BIT(sh7021.gpr[n], 31);
	ans += dest;

	if (src != 1)
	{
		if (ans == 1)
			sh7021.sr |= SH_T;
		else
			sh7021.sr &= ~SH_T;
	}
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0010 nnnn mmmm 1001  1       -
 *  AND     Rm,Rn
 */
static void AND(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] &= sh7021.gpr[m];
}

/*  code                 cycles  t-bit
 *  1100 1001 iiii iiii  1       -
 *  AND     #imm,R0
 */
static void ANDI(uint32_t i)
{
	sh7021.gpr[0] &= i;
}

/*  code                 cycles  t-bit
 *  1100 1101 iiii iiii  1       -
 *  AND.B   #imm,@(R0,GBR)
 */
static void ANDM(uint32_t i)
{
	sh7021.ea = sh7021.gbr + sh7021.gpr[0];
	uint32_t temp = i & sh7021_bus_read8(sh7021.ea);
	sh7021_bus_write8(sh7021.ea, temp);
	sh7021.cycles_left -= 2;
}

/*  code                 cycles  t-bit
 *  1000 1011 dddd dddd  3/1     -
 *  BF      disp8
 */
static void BF(uint32_t d)
{
	if ((sh7021.sr & SH_T) == 0)
	{
		int32_t disp = sh7021_mame_sext(d, 8);
		sh7021.pc = sh7021.ea = sh7021.pc + disp * 2 + 2;
		sh7021.cycles_left -= 2;
	}
}

/*  code                 cycles  t-bit
 *  1000 1111 dddd dddd  3/1     -
 *  BFS     disp8
 */
static void SH7021_UNUSED_FN BFS(uint32_t d)
{
	if ((sh7021.sr & SH_T) == 0)
	{
		int32_t disp = sh7021_mame_sext(d, 8);
		sh7021.m_delay = sh7021.ea = sh7021.pc + disp * 2 + 2;
		sh7021.cycles_left--;
	}
}

/*  code                 cycles  t-bit
 *  1010 dddd dddd dddd  2       -
 *  BRA     disp12
 */
static void BRA(uint32_t d)
{
	int32_t disp = sh7021_mame_sext(d, 12);

#if BUSY_LOOP_HACKS
	if (disp == -2)
	{
		uint32_t next_opcode = sh7021_bus_read16(sh7021.pc & m_am);
		/* BRA  $
		 * NOP
		 */
		if (next_opcode == 0x0009)
			sh7021.cycles_left %= 3;   /* cycles for BRA $ and NOP taken (3) */
	}
#endif
	sh7021.m_delay = sh7021.ea = sh7021.pc + disp * 2 + 2;
	sh7021.cycles_left--;
}

/*  code                 cycles  t-bit
 *  0000 mmmm 0010 0011  2       -
 *  BRAF    Rm
 */
static void SH7021_UNUSED_FN BRAF(uint32_t m)
{
	sh7021.m_delay = sh7021.pc + sh7021.gpr[m] + 2;
	sh7021.cycles_left--;
}

/*  code                 cycles  t-bit
 *  1011 dddd dddd dddd  2       -
 *  BSR     disp12
 */
static void BSR(uint32_t d)
{
	int32_t disp = sh7021_mame_sext(d, 12);

	sh7021.pr = sh7021.pc + 2;
	sh7021.m_delay = sh7021.ea = sh7021.pc + disp * 2 + 2;
	sh7021.cycles_left--;
}

/*  code                 cycles  t-bit
 *  0000 mmmm 0000 0011  2       -
 *  BSRF    Rm
 */
static void SH7021_UNUSED_FN BSRF(uint32_t m)
{
	sh7021.pr = sh7021.pc + 2;
	sh7021.m_delay = sh7021.pc + sh7021.gpr[m] + 2;
	sh7021.cycles_left--;
}

/*  code                 cycles  t-bit
 *  1000 1001 dddd dddd  3/1     -
 *  BT      disp8
 */
static void BT(uint32_t d)
{
	if ((sh7021.sr & SH_T) != 0)
	{
		int32_t disp = sh7021_mame_sext(d, 8);
		sh7021.pc = sh7021.ea = sh7021.pc + disp * 2 + 2;
		sh7021.cycles_left -= 2;
	}
}

/*  code                 cycles  t-bit
 *  1000 1101 dddd dddd  2/1     -
 *  BTS     disp8
 */
static void SH7021_UNUSED_FN BTS(uint32_t d)
{
	if ((sh7021.sr & SH_T) != 0)
	{
		int32_t disp = sh7021_mame_sext(d, 8);
		sh7021.m_delay = sh7021.ea = sh7021.pc + disp * 2 + 2;
		sh7021.cycles_left--;
	}
}

/*  code                 cycles  t-bit
 *  0000 0000 0010 1000  1       -
 *  CLRMAC
 */
static void CLRMAC()
{
	sh7021.mach = sh7021_mach_sh1_canonical(0);
	sh7021.macl = 0;
}

/*  code                 cycles  t-bit
 *  0000 0000 0000 1000  1       -
 *  CLRT
 */
static void CLRT()
{
	sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0000  1       comparison result
 *  CMP_EQ  Rm,Rn
 */
static void CMPEQ(uint32_t m, uint32_t n)
{
	if (sh7021.gpr[n] == sh7021.gpr[m])
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0011  1       comparison result
 *  CMP_GE  Rm,Rn
 */
static void CMPGE(uint32_t m, uint32_t n)
{
	if ((int32_t)sh7021.gpr[n] >= (int32_t)sh7021.gpr[m])
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0111  1       comparison result
 *  CMP_GT  Rm,Rn
 */
static void CMPGT(uint32_t m, uint32_t n)
{
	if ((int32_t)sh7021.gpr[n] > (int32_t)sh7021.gpr[m])
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0110  1       comparison result
 *  CMP_HI  Rm,Rn
 */
static void CMPHI(uint32_t m, uint32_t n)
{
	if (sh7021.gpr[n] > sh7021.gpr[m])
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0010  1       comparison result
 *  CMP_HS  Rm,Rn
 */
static void CMPHS(uint32_t m, uint32_t n)
{
	if (sh7021.gpr[n] >= sh7021.gpr[m])
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0100 nnnn 0001 0101  1       comparison result
 *  CMP_PL  Rn
 */
static void CMPPL(uint32_t n)
{
	if ((int32_t)sh7021.gpr[n] > 0)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0100 nnnn 0001 0001  1       comparison result
 *  CMP_PZ  Rn
 */
static void CMPPZ(uint32_t n)
{
	if ((int32_t)sh7021.gpr[n] >= 0)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0010 nnnn mmmm 1100  1       comparison result
 * CMP_STR  Rm,Rn
 */
static void CMPSTR(uint32_t m, uint32_t n)
{
	uint32_t temp = sh7021.gpr[n] ^ sh7021.gpr[m];
	uint8_t upper_byte = (temp >> 24) & 0xff;
	uint8_t mid_upper_byte = (temp >> 16) & 0xff;
	uint8_t mid_lower_byte = (temp >> 8) & 0xff;
	uint8_t lower_byte = temp & 0xff;
	if (upper_byte && mid_upper_byte && mid_lower_byte && lower_byte)
		sh7021.sr &= ~SH_T;
	else
		sh7021.sr |= SH_T;
}

/*  code                 cycles  t-bit
 *  1000 1000 iiii iiii  1       comparison result
 *  CMP/EQ #imm,R0
 */
static void CMPIM(uint32_t i)
{
	uint32_t imm = (uint32_t)sh7021_mame_sext(i, 8);

	if (sh7021.gpr[0] == imm)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0010 nnnn mmmm 0111  1       calculation result
 *  DIV0S   Rm,Rn
 */
static void DIV0S(uint32_t m, uint32_t n)
{
	if (!SH7021_MAME_BIT(sh7021.gpr[n], 31))
		sh7021.sr &= ~SH_Q;
	else
		sh7021.sr |= SH_Q;

	if (!SH7021_MAME_BIT(sh7021.gpr[m], 31))
		sh7021.sr &= ~SH_M;
	else
		sh7021.sr |= SH_M;

	if (SH7021_MAME_BIT(sh7021.gpr[m] ^ sh7021.gpr[n], 31))
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  code                 cycles  t-bit
 *  0000 0000 0001 1001  1       0
 *  DIV0U
 */
static void DIV0U()
{
	sh7021.sr &= ~(SH_M | SH_Q | SH_T);
}

/*  code                 cycles  t-bit
 *  0011 nnnn mmmm 0100  1       calculation result
 *  DIV1 Rm,Rn
 */
static void DIV1(uint32_t m, uint32_t n)
{
	uint32_t old_q = sh7021.sr & SH_Q;
	if (0x80000000 & sh7021.gpr[n])
		sh7021.sr |= SH_Q;
	else
		sh7021.sr &= ~SH_Q;

	sh7021.gpr[n] = (sh7021.gpr[n] << 1) | (sh7021.sr & SH_T);

	if (!old_q)
	{
		if (!(sh7021.sr & SH_M))
		{
			uint32_t tmp = sh7021.gpr[n];
			sh7021.gpr[n] -= sh7021.gpr[m];
			if (!(sh7021.sr & SH_Q))
				if (sh7021.gpr[n] > tmp)
					sh7021.sr |= SH_Q;
				else
					sh7021.sr &= ~SH_Q;
			else
				if (sh7021.gpr[n] > tmp)
					sh7021.sr &= ~SH_Q;
				else
					sh7021.sr |= SH_Q;
		}
		else
		{
			uint32_t tmp = sh7021.gpr[n];
			sh7021.gpr[n] += sh7021.gpr[m];
			if (!(sh7021.sr & SH_Q))
			{
				if (sh7021.gpr[n] < tmp)
					sh7021.sr &= ~SH_Q;
				else
					sh7021.sr |= SH_Q;
			}
			else
			{
				if (sh7021.gpr[n] < tmp)
					sh7021.sr |= SH_Q;
				else
					sh7021.sr &= ~SH_Q;
			}
		}
	}
	else
	{
		if (!(sh7021.sr & SH_M))
		{
			uint32_t tmp = sh7021.gpr[n];
			sh7021.gpr[n] += sh7021.gpr[m];
			if (!(sh7021.sr & SH_Q))
				if (sh7021.gpr[n] < tmp)
					sh7021.sr |= SH_Q;
				else
					sh7021.sr &= ~SH_Q;
			else
				if (sh7021.gpr[n] < tmp)
					sh7021.sr &= ~SH_Q;
				else
					sh7021.sr |= SH_Q;
		}
		else
		{
			uint32_t tmp = sh7021.gpr[n];
			sh7021.gpr[n] -= sh7021.gpr[m];
			if (!(sh7021.sr & SH_Q))
				if (sh7021.gpr[n] > tmp)
					sh7021.sr &= ~SH_Q;
				else
					sh7021.sr |= SH_Q;
			else
				if (sh7021.gpr[n] > tmp)
					sh7021.sr |= SH_Q;
				else
					sh7021.sr &= ~SH_Q;
		}
	}

	uint32_t tmp = (sh7021.sr & (SH_Q | SH_M));
	if (tmp == 0 || tmp == 0x300) /* if Q == M set T else clear T */
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  DMULS.L Rm,Rn */
static void SH7021_UNUSED_FN DMULS(uint32_t m, uint32_t n)
{
	int32_t tempn = (int32_t)sh7021.gpr[n];
	int32_t tempm = (int32_t)sh7021.gpr[m];
	bool fnlml = (bool)SH7021_MAME_BIT(tempn ^ tempm, 31);

	if (tempn < 0)
		tempn = 0 - tempn;
	if (tempm < 0)
		tempm = 0 - tempm;

	uint32_t rn_l = (uint32_t)tempn & 0x0000ffff;
	uint32_t rn_h = (uint32_t)tempn >> 16;
	uint32_t rm_l = (uint32_t)tempm & 0x0000ffff;
	uint32_t rm_h = (uint32_t)tempm >> 16;

	uint32_t temp0 = rm_l * rn_l;
	uint32_t temp1 = rm_h * rn_l;
	uint32_t temp2 = rm_l * rn_h;
	uint32_t temp3 = rm_h * rn_h;

	uint32_t res2 = 0;
	uint32_t res1 = temp1 + temp2;
	if (res1 < temp1)
		res2 += 0x00010000;
	temp1 = res1 << 16;

	uint32_t res0 = temp0 + temp1;
	if (res0 < temp0)
		res2++;
	res2 = res2 + (res1 >> 16) + temp3;

	if (fnlml)
	{
		res2 = ~res2;
		if (res0 == 0)
			res2++;
		else
			res0 = (~res0) + 1;
	}

	sh7021.mach = res2;
	sh7021.macl = res0;
	sh7021.cycles_left--;
}

/*  DMULU.L Rm,Rn */
static void SH7021_UNUSED_FN DMULU(uint32_t m, uint32_t n)
{
	uint32_t rn_l = sh7021.gpr[n] & 0x0000ffff;
	uint32_t rn_h = sh7021.gpr[n] >> 16;
	uint32_t rm_l = sh7021.gpr[m] & 0x0000ffff;
	uint32_t rm_h = sh7021.gpr[m] >> 16;

	uint32_t temp0 = rm_l * rn_l;
	uint32_t temp1 = rm_h * rn_l;
	uint32_t temp2 = rm_l * rn_h;
	uint32_t temp3 = rm_h * rn_h;

	uint32_t res2 = 0;
	uint32_t res1 = temp1 + temp2;
	if (res1 < temp1)
		res2 += 0x00010000;

	temp1 = res1 << 16;
	uint32_t res0 = temp0 + temp1;
	if (res0 < temp0)
		res2++;

	res2 = res2 + (res1 >> 16) + temp3;

	sh7021.mach = res2;
	sh7021.macl = res0;
	sh7021.cycles_left--;
}

/*  DT      Rn */
static void SH7021_UNUSED_FN DT(uint32_t n)
{
	sh7021.gpr[n]--;
	if (sh7021.gpr[n] == 0)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
#if BUSY_LOOP_HACKS
	{
		uint32_t next_opcode = sh7021_bus_read16(sh7021.pc & AM);
		/* DT   Rn
		 * BF   $-2
		 */
		if (next_opcode == 0x8bfd)
		{
			while (sh7021.gpr[n] > 1 && sh7021.cycles_left > 4)
			{
				sh7021.gpr[n]--;
				sh7021.cycles_left -= 4;   /* cycles for DT (1) and BF taken (3) */
			}
		}
	}
#endif
}

/*  EXTS.B  Rm,Rn */
static void EXTSB(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = sh7021_mame_sext(sh7021.gpr[m], 8);
}

/*  EXTS.W  Rm,Rn */
static void EXTSW(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = sh7021_mame_sext(sh7021.gpr[m], 16);
}

/*  EXTU.B  Rm,Rn */
static void EXTUB(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = sh7021.gpr[m] & 0x000000ff;
}

/*  EXTU.W  Rm,Rn */
static void EXTUW(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = sh7021.gpr[m] & 0x0000ffff;
}

/*  JMP     @Rm */
static void JMP(uint32_t m)
{
	sh7021.m_delay = sh7021.ea = sh7021.gpr[m];
	/* SH-1 delayed branches, including JMP @Rm, consume two cycles total.
	 * The main executor accounts for the first cycle; charge the extra one here. */
	sh7021.cycles_left--;
}

/*  JSR     @Rm */
static void JSR(uint32_t m)
{
	sh7021.pr = sh7021.pc + 2;
	sh7021.m_delay = sh7021.ea = sh7021.gpr[m];
	sh7021.cycles_left--;
}

/*  LDC     Rm,GBR */
static void LDCGBR(uint32_t m)
{
	sh7021.gbr = sh7021.gpr[m];
	sh7021_block_irq_next();
}

/*  LDC     Rm,VBR */
static void LDCVBR(uint32_t m)
{
	sh7021.vbr = sh7021.gpr[m];
	sh7021_block_irq_next();
}

/*  LDC.L   @Rm+,GBR */
static void LDCMGBR(uint32_t m)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.gbr = sh7021_bus_read32(sh7021.ea);
	sh7021.gpr[m] += 4;
	sh7021.cycles_left -= 2;
	sh7021_block_irq_next();
}

/*  LDC.L   @Rm+,VBR */
static void LDCMVBR(uint32_t m)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.vbr = sh7021_bus_read32(sh7021.ea);
	sh7021.gpr[m] += 4;
	sh7021.cycles_left -= 2;
	sh7021_block_irq_next();
}

/*  LDS     Rm,MACH */
static void LDSMACH(uint32_t m)
{
	sh7021.mach = sh7021_mach_sh1_canonical(sh7021.gpr[m]);
	sh7021_block_irq_next();
}

/*  LDS     Rm,MACL */
static void LDSMACL(uint32_t m)
{
	sh7021.macl = sh7021.gpr[m];
	sh7021_block_irq_next();
}

/*  LDS     Rm,PR */
static void LDSPR(uint32_t m)
{
	sh7021.pr = sh7021.gpr[m];
	sh7021_block_irq_next();
}

/*  LDS.L   @Rm+,MACH */
static void LDSMMACH(uint32_t m)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.mach = sh7021_mach_sh1_canonical(sh7021_bus_read32(sh7021.ea));
	sh7021.gpr[m] += 4;
	sh7021_block_irq_next();
}

/*  LDS.L   @Rm+,MACL */
static void LDSMMACL(uint32_t m)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.macl = sh7021_bus_read32(sh7021.ea);
	sh7021.gpr[m] += 4;
	sh7021_block_irq_next();
}

/*  LDS.L   @Rm+,PR */
static void LDSMPR(uint32_t m)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.pr = sh7021_bus_read32(sh7021.ea);
	sh7021.gpr[m] += 4;
	sh7021_block_irq_next();
}

/*  MAC.L   @Rm+,@Rn+ */
static void SH7021_UNUSED_FN MAC_L(uint32_t m, uint32_t n)
{
	int32_t tempn = (int32_t)sh7021_bus_read32(sh7021.gpr[n]);
	sh7021.gpr[n] += 4;

	int32_t tempm = (int32_t)sh7021_bus_read32(sh7021.gpr[m]);
	sh7021.gpr[m] += 4;

	bool fnlml = SH7021_MAME_BIT(tempn ^ tempm, 31);

	if (tempn < 0)
		tempn = 0 - tempn;
	if (tempm < 0)
		tempm = 0 - tempm;

	uint32_t rn_l = (uint32_t)tempn & 0x0000ffff;
	uint32_t rn_h = (uint32_t)tempn >> 16;
	uint32_t rm_l = (uint32_t)tempm & 0x0000ffff;
	uint32_t rm_h = (uint32_t)tempm >> 16;

	uint32_t temp0 = rm_l * rn_l;
	uint32_t temp1 = rm_h * rn_l;
	uint32_t temp2 = rm_l * rn_h;
	uint32_t temp3 = rm_h * rn_h;

	uint32_t res2 = 0;
	uint32_t res1 = temp1 + temp2;
	if (res1 < temp1)
		res2 += 0x00010000;
	temp1 = res1 << 16;

	uint32_t res0 = temp0 + temp1;
	if (res0 < temp0)
		res2++;
	res2 = res2 + (res1 >> 16) + temp3;

	if (fnlml)
	{
		res2 = ~res2;
		if (res0 == 0)
			res2++;
		else
			res0 = (~res0) + 1;
	}

	if (sh7021.sr & SH_S)
	{
		res0 = sh7021.macl + res0;
		if (sh7021.macl > res0)
			res2++;
		res2 += sh7021.mach & 0x0000ffff;
		if ((int32_t)res2 < 0 && res2 < 0xffff8000)
		{
			res2 = 0x00008000;
			res0 = 0x00000000;
		}
		else if ((int32_t)res2 > 0 && res2 > 0x00007fff)
		{
			res2 = 0x00007fff;
			res0 = 0xffffffff;
		}
		sh7021.mach = res2;
		sh7021.macl = res0;
	}
	else
	{
		res0 = sh7021.macl + res0;
		if (sh7021.macl > res0)
			res2++;
		res2 += sh7021.mach;
		sh7021.mach = res2;
		sh7021.macl = res0;
	}
	sh7021.cycles_left -= 2;
}

/*  MAC.W   @Rm+,@Rn+ */
static void MAC_W(uint32_t m, uint32_t n)
{
	int32_t tempn = (int32_t)(int16_t)sh7021_bus_read16(sh7021.gpr[n]);
	sh7021.gpr[n] += 2;

	int32_t tempm = (int32_t)(int16_t)sh7021_bus_read16(sh7021.gpr[m]);
	sh7021.gpr[m] += 2;

	uint32_t templ = sh7021.macl;
	tempm *= tempn;

	int32_t dest = SH7021_MAME_BIT(sh7021.macl, 31);
	int32_t src = SH7021_MAME_BIT(tempm, 31) + dest;
	tempn = SH7021_MAME_BIT(tempm, 31) ? -1 : 0;

	sh7021.macl += tempm;

	int32_t ans = SH7021_MAME_BIT(sh7021.macl, 31) + dest;

	if (sh7021.sr & SH_S)
	{
		if (ans == 1)
		{
			if (src == 0 || src == 2)
				sh7021.mach = sh7021_mach_sh1_canonical(sh7021.mach | 0x00000001u);
			if (src == 0)
				sh7021.macl = 0x7fffffff;
			else if (src == 2)
				sh7021.macl = 0x80000000;
		}
	}
	else
	{
		sh7021.mach += tempn;
		if (templ > sh7021.macl)
			sh7021.mach += 1;
		sh7021.mach = sh7021_mach_sh1_canonical(sh7021.mach);
	}
	sh7021.cycles_left -= 2;
}

/*  MOV     Rm,Rn */
static void MOV(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = sh7021.gpr[m];
}

/*  MOV.B   Rm,@Rn */
static void MOVBS(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write8(sh7021.ea, sh7021.gpr[m] & 0x000000ff);
}

/*  MOV.W   Rm,@Rn */
static void MOVWS(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write16(sh7021.ea, sh7021.gpr[m] & 0x0000ffff);
}

/*  MOV.L   Rm,@Rn */
static void MOVLS(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write32(sh7021.ea, sh7021.gpr[m]);
}

/*  MOV.B   @Rm,Rn */
static void MOVBL(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(sh7021_bus_read8(sh7021.ea), 8);
}

/*  MOV.W   @Rm,Rn */
static void MOVWL(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(sh7021_bus_read16(sh7021.ea), 16);
}

/*  MOV.L   @Rm,Rn */
static void MOVLL(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[m];
	sh7021.gpr[n] = sh7021_bus_read32(sh7021.ea);
}

/*  MOV.B   Rm,@-Rn */
static void MOVBM(uint32_t m, uint32_t n)
{
	uint8_t data = (uint8_t)sh7021.gpr[m];

	sh7021.gpr[n] -= 1;
	sh7021_bus_write8(sh7021.gpr[n], data);
}

/*  MOV.W   Rm,@-Rn */
static void MOVWM(uint32_t m, uint32_t n)
{
	uint16_t data = (uint16_t)sh7021.gpr[m];

	sh7021.gpr[n] -= 2;
	sh7021_bus_write16(sh7021.gpr[n], data);
}

/*  MOV.L   Rm,@-Rn */
static void MOVLM(uint32_t m, uint32_t n)
{
	uint32_t data = sh7021.gpr[m];

	sh7021.gpr[n] -= 4;
	sh7021_bus_write32(sh7021.gpr[n], data);
}

/*  MOV.B   @Rm+,Rn */
static void MOVBP(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(sh7021_bus_read8(sh7021.gpr[m]), 8);
	if (n != m)
		sh7021.gpr[m] += 1;
}

/*  MOV.W   @Rm+,Rn */
static void MOVWP(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(sh7021_bus_read16(sh7021.gpr[m]), 16);
	if (n != m)
		sh7021.gpr[m] += 2;
}

/*  MOV.L   @Rm+,Rn */
static void MOVLP(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = sh7021_bus_read32(sh7021.gpr[m]);
	if (n != m)
		sh7021.gpr[m] += 4;
}

/*  MOV.B   Rm,@(R0,Rn) */
static void MOVBS0(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[n] + sh7021.gpr[0];
	sh7021_bus_write8(sh7021.ea, (uint8_t)sh7021.gpr[m]);
}

/*  MOV.W   Rm,@(R0,Rn) */
static void MOVWS0(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[n] + sh7021.gpr[0];
	sh7021_bus_write16(sh7021.ea, (uint16_t)sh7021.gpr[m]);
}

/*  MOV.L   Rm,@(R0,Rn) */
static void MOVLS0(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[n] + sh7021.gpr[0];
	sh7021_bus_write32(sh7021.ea, sh7021.gpr[m]);
}

/*  MOV.B   @(R0,Rm),Rn */
static void MOVBL0(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[m] + sh7021.gpr[0];
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(sh7021_bus_read8(sh7021.ea), 8);
}

/*  MOV.W   @(R0,Rm),Rn */
static void MOVWL0(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[m] + sh7021.gpr[0];
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(sh7021_bus_read16(sh7021.ea), 16);
}

/*  MOV.L   @(R0,Rm),Rn */
static void MOVLL0(uint32_t m, uint32_t n)
{
	sh7021.ea = sh7021.gpr[m] + sh7021.gpr[0];
	sh7021.gpr[n] = sh7021_bus_read32(sh7021.ea);
}

/*  MOV     #imm,Rn */
static void MOVI(uint32_t i, uint32_t n)
{
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(i, 8);
}

/*  MOV.W   @(disp8,PC),Rn */
static void MOVWI(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = sh7021.current_opcode_pc + 4u + disp * 2;
	sh7021.gpr[n] = (uint32_t)sh7021_mame_sext(sh7021_bus_read16(sh7021.ea), 16);
}

/*  MOV.L   @(disp8,PC),Rn */
static void MOVLI(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = ((sh7021.current_opcode_pc + 4u) & ~3u) + disp * 4;
	sh7021.gpr[n] = sh7021_bus_read32(sh7021.ea);
}

/*  MOV.B   @(disp8,GBR),R0 */
static void MOVBLG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = sh7021.gbr + disp;
	sh7021.gpr[0] = (uint32_t)sh7021_mame_sext(sh7021_bus_read8(sh7021.ea), 8);
}

/*  MOV.W   @(disp8,GBR),R0 */
static void MOVWLG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = sh7021.gbr + disp * 2;
	sh7021.gpr[0] = (int32_t)sh7021_mame_sext(sh7021_bus_read16(sh7021.ea), 16);
}

/*  MOV.L   @(disp8,GBR),R0 */
static void MOVLLG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = sh7021.gbr + disp * 4;
	sh7021.gpr[0] = sh7021_bus_read32(sh7021.ea);
}

/*  MOV.B   R0,@(disp8,GBR) */
static void MOVBSG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = sh7021.gbr + disp;
	sh7021_bus_write8(sh7021.ea, (uint8_t)sh7021.gpr[0]);
}

/*  MOV.W   R0,@(disp8,GBR) */
static void MOVWSG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = sh7021.gbr + disp * 2;
	sh7021_bus_write16(sh7021.ea, (uint16_t)sh7021.gpr[0]);
}

/*  MOV.L   R0,@(disp8,GBR) */
static void MOVLSG(uint32_t d)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = sh7021.gbr + disp * 4;
	sh7021_bus_write32(sh7021.ea, sh7021.gpr[0]);
}

/*  MOV.B   R0,@(disp4,Rn) */
static void MOVBS4(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	sh7021.ea = sh7021.gpr[n] + disp;
	sh7021_bus_write8(sh7021.ea, (uint8_t)sh7021.gpr[0]);
}

/*  MOV.W   R0,@(disp4,Rn) */
static void MOVWS4(uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	sh7021.ea = sh7021.gpr[n] + disp * 2;
	sh7021_bus_write16(sh7021.ea, (uint16_t)sh7021.gpr[0]);
}

/* MOV.L Rm,@(disp4,Rn) */
static void MOVLS4(uint32_t m, uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	sh7021.ea = sh7021.gpr[n] + disp * 4;
	sh7021_bus_write32(sh7021.ea, sh7021.gpr[m]);
}

/*  MOV.B   @(disp4,Rm),R0 */
static void MOVBL4(uint32_t m, uint32_t d)
{
	uint32_t disp = d & 0x0f;
	sh7021.ea = sh7021.gpr[m] + disp;
	sh7021.gpr[0] = (uint32_t)sh7021_mame_sext(sh7021_bus_read8(sh7021.ea), 8);
}

/*  MOV.W   @(disp4,Rm),R0 */
static void MOVWL4(uint32_t m, uint32_t d)
{
	uint32_t disp = d & 0x0f;
	sh7021.ea = sh7021.gpr[m] + disp * 2;
	sh7021.gpr[0] = (uint32_t)sh7021_mame_sext(sh7021_bus_read16(sh7021.ea), 16);
}

/*  MOV.L   @(disp4,Rm),Rn */
static void MOVLL4(uint32_t m, uint32_t d, uint32_t n)
{
	uint32_t disp = d & 0x0f;
	sh7021.ea = sh7021.gpr[m] + disp * 4;
	sh7021.gpr[n] = sh7021_bus_read32(sh7021.ea);
}

/*  MOVA    @(disp8,PC),R0 */
static void MOVA(uint32_t d)
{
	uint32_t disp = d & 0xff;
	sh7021.ea = ((sh7021.current_opcode_pc + 4u) & ~3u) + disp * 4;
	sh7021.gpr[0] = sh7021.ea;
}

/*  MOVT    Rn */
static void MOVT(uint32_t n)
{
	sh7021.gpr[n] = sh7021.sr & SH_T;
}

/*  MUL.L   Rm,Rn */
static void SH7021_UNUSED_FN MULL(uint32_t m, uint32_t n)
{
	sh7021.macl = sh7021.gpr[n] * sh7021.gpr[m];
	sh7021.cycles_left--;
}

/*  MULS    Rm,Rn */
static void MULS(uint32_t m, uint32_t n)
{
	sh7021.macl = (int16_t)sh7021.gpr[n] * (int16_t)sh7021.gpr[m];
	/* SH-1 16x16 multiply is a multi-cycle instruction; the dispatcher
	 * charges the base cycle, so add the second cycle here. */
	sh7021.cycles_left--;
}

/*  MULU    Rm,Rn */
static void MULU(uint32_t m, uint32_t n)
{
	sh7021.macl = (uint16_t)sh7021.gpr[n] * (uint16_t)sh7021.gpr[m];
	sh7021.cycles_left--;
}

/*  NEG     Rm,Rn */
static void NEG(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = 0 - sh7021.gpr[m];
}

/*  NEGC    Rm,Rn */
static void NEGC(uint32_t m, uint32_t n)
{
	uint32_t temp = sh7021.gpr[m];
	sh7021.gpr[n] = -temp - (sh7021.sr & SH_T);
	if (temp || (sh7021.sr & SH_T))
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  NOP */
static void NOP(void)
{
}

/*  NOT     Rm,Rn */
static void NOT(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = ~sh7021.gpr[m];
}

/*  OR      Rm,Rn */
static void OR(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] |= sh7021.gpr[m];
}

/*  OR      #imm,R0 */
static void ORI(uint32_t i)
{
	sh7021.gpr[0] |= i;
	sh7021.cycles_left -= 2;
}

/*  OR.B    #imm,@(R0,GBR) */
static void ORM(uint32_t i)
{
	sh7021.ea = sh7021.gbr + sh7021.gpr[0];
	sh7021_bus_write8(sh7021.ea, sh7021_bus_read8(sh7021.ea) | (uint8_t)i);
}

/*  ROTCL   Rn */
static void ROTCL(uint32_t n)
{
	uint32_t temp = (sh7021.gpr[n] >> 31) & SH_T;
	sh7021.gpr[n] = (sh7021.gpr[n] << 1) | (sh7021.sr & SH_T);
	sh7021.sr = (sh7021.sr & ~SH_T) | temp;
}

/*  ROTCR   Rn */
static void ROTCR(uint32_t n)
{
	uint32_t temp = (sh7021.sr & SH_T) << 31;
	if (sh7021.gpr[n] & SH_T)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
	sh7021.gpr[n] = (sh7021.gpr[n] >> 1) | temp;
}

/*  ROTL    Rn */
static void ROTL(uint32_t n)
{
	sh7021.sr = (sh7021.sr & ~SH_T) | ((sh7021.gpr[n] >> 31) & SH_T);
	sh7021.gpr[n] = sh7021_rotl32(sh7021.gpr[n], 1);
}

/*  ROTR    Rn */
static void ROTR(uint32_t n)
{
	sh7021.sr = (sh7021.sr & ~SH_T) | (sh7021.gpr[n] & SH_T);
	sh7021.gpr[n] = sh7021_rotr32(sh7021.gpr[n], 1);
}

/*  RTS */
static void RTS()
{
	sh7021.m_delay = sh7021.ea = sh7021.pr;
	sh7021.cycles_left--;
}

/*  SETT */
static void SETT()
{
	sh7021.sr |= SH_T;
}

/*  SHAL    Rn      (same as SHLL) */
static void SHAL(uint32_t n)
{
	sh7021.sr = (sh7021.sr & ~SH_T) | ((sh7021.gpr[n] >> 31) & SH_T);
	sh7021.gpr[n] <<= 1;
}

/*  SHAR    Rn */
static void SHAR(uint32_t n)
{
	sh7021.sr = (sh7021.sr & ~SH_T) | (sh7021.gpr[n] & SH_T);
	sh7021.gpr[n] = (uint32_t)((int32_t)sh7021.gpr[n] >> 1);
}

/*  SHLL    Rn      (same as SHAL) */
static void SHLL(uint32_t n)
{
	sh7021.sr = (sh7021.sr & ~SH_T) | ((sh7021.gpr[n] >> 31) & SH_T);
	sh7021.gpr[n] <<= 1;
}

/*  SHLL2   Rn */
static void SHLL2(uint32_t n)
{
	sh7021.gpr[n] <<= 2;
}

/*  SHLL8   Rn */
static void SHLL8(uint32_t n)
{
	sh7021.gpr[n] <<= 8;
}

/*  SHLL16  Rn */
static void SHLL16(uint32_t n)
{
	sh7021.gpr[n] <<= 16;
}

/*  SHLR    Rn */
static void SHLR(uint32_t n)
{
	sh7021.sr = (sh7021.sr & ~SH_T) | (sh7021.gpr[n] & SH_T);
	sh7021.gpr[n] >>= 1;
}

/*  SHLR2   Rn */
static void SHLR2(uint32_t n)
{
	sh7021.gpr[n] >>= 2;
}

/*  SHLR8   Rn */
static void SHLR8(uint32_t n)
{
	sh7021.gpr[n] >>= 8;
}

/*  SHLR16  Rn */
static void SHLR16(uint32_t n)
{
	sh7021.gpr[n] >>= 16;
}


/*  STC     SR,Rn */
static void STCSR(uint32_t n)
{
	sh7021.gpr[n] = sh7021.sr;
}

/*  STC     GBR,Rn */
static void STCGBR(uint32_t n)
{
	sh7021.gpr[n] = sh7021.gbr;
}

/*  STC     VBR,Rn */
static void STCVBR(uint32_t n)
{
	sh7021.gpr[n] = sh7021.vbr;
}

/*  STC.L   SR,@-Rn */
static void STCMSR(uint32_t n)
{
	sh7021.gpr[n] -= 4;
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write32(sh7021.ea, sh7021.sr);
	sh7021.cycles_left--;
}

/*  STC.L   GBR,@-Rn */
static void STCMGBR(uint32_t n)
{
	sh7021.gpr[n] -= 4;
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write32(sh7021.ea, sh7021.gbr);
	sh7021.cycles_left--;
}

/*  STC.L   VBR,@-Rn */
static void STCMVBR(uint32_t n)
{
	sh7021.gpr[n] -= 4;
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write32(sh7021.ea, sh7021.vbr);
	sh7021.cycles_left--;
}

/*  STS     MACH,Rn */
static void STSMACH(uint32_t n)
{
	sh7021.gpr[n] = sh7021_mach_sh1_read();
	sh7021_block_irq_next();
}

/*  STS     MACL,Rn */
static void STSMACL(uint32_t n)
{
	sh7021.gpr[n] = sh7021.macl;
	sh7021_block_irq_next();
}

/*  STS     PR,Rn */
static void STSPR(uint32_t n)
{
	sh7021.gpr[n] = sh7021.pr;
	sh7021_block_irq_next();
}

/*  STS.L   MACH,@-Rn */
static void STSMMACH(uint32_t n)
{
	sh7021.gpr[n] -= 4;
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write32(sh7021.ea, sh7021_mach_sh1_read());
	sh7021_block_irq_next();
}

/*  STS.L   MACL,@-Rn */
static void STSMMACL(uint32_t n)
{
	sh7021.gpr[n] -= 4;
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write32(sh7021.ea, sh7021.macl);
	sh7021_block_irq_next();
}

/*  STS.L   PR,@-Rn */
static void STSMPR(uint32_t n)
{
	sh7021.gpr[n] -= 4;
	sh7021.ea = sh7021.gpr[n];
	sh7021_bus_write32(sh7021.ea, sh7021.pr);
	sh7021_block_irq_next();
}

/*  SUB     Rm,Rn */
static void SUB(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] -= sh7021.gpr[m];
}

/*  SUBC    Rm,Rn */
static void SUBC(uint32_t m, uint32_t n)
{
	uint32_t tmp1 = sh7021.gpr[n] - sh7021.gpr[m];
	uint32_t tmp0 = sh7021.gpr[n];
	sh7021.gpr[n] = tmp1 - (sh7021.sr & SH_T);
	if (tmp0 < tmp1)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
	if (tmp1 < sh7021.gpr[n])
		sh7021.sr |= SH_T;
}

/*  SUBV    Rm,Rn */
static void SUBV(uint32_t m, uint32_t n)
{
	int32_t dest = SH7021_MAME_BIT(sh7021.gpr[n], 31);
	int32_t src = SH7021_MAME_BIT(sh7021.gpr[m], 31);
	src += dest;

	sh7021.gpr[n] -= sh7021.gpr[m];

	int32_t ans = SH7021_MAME_BIT(sh7021.gpr[n], 31);
	ans += dest;

	if (src == 1 && ans == 1)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  SWAP.B  Rm,Rn */
static void SWAPB(uint32_t m, uint32_t n)
{
	uint32_t temp = sh7021.gpr[m] & 0xffff0000;
	temp |= (sh7021.gpr[m] & 0x000000ff) << 8;
	sh7021.gpr[n] = (uint8_t)(sh7021.gpr[m] >> 8);
	sh7021.gpr[n] = sh7021.gpr[n] | temp;
}

/*  SWAP.W  Rm,Rn */
static void SWAPW(uint32_t m, uint32_t n)
{
	uint32_t temp = sh7021.gpr[m] >> 16;
	sh7021.gpr[n] = (sh7021.gpr[m] << 16) | temp;
}

/*  TAS.B   @Rn */
static void TAS(uint32_t n)
{
	sh7021.ea = sh7021.gpr[n];

	/* Bus Lock enable */
	uint32_t temp = sh7021_bus_read8(sh7021.ea);
	if (temp == 0)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
	temp |= 0x80;
	/* Bus Lock disable */
	sh7021_bus_write8(sh7021.ea, temp);
	sh7021.cycles_left -= 3;
}

/*  TST     Rm,Rn */
static void TST(uint32_t m, uint32_t n)
{
	if ((sh7021.gpr[n] & sh7021.gpr[m]) == 0)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  TST     #imm,R0 */
static void TSTI(uint32_t i)
{
	uint32_t imm = i & 0xff;

	if ((imm & sh7021.gpr[0]) == 0)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
}

/*  TST.B   #imm,@(R0,GBR) */
static void TSTM(uint32_t i)
{
	uint32_t imm = i & 0xff;

	sh7021.ea = sh7021.gbr + sh7021.gpr[0];
	if ((imm & sh7021_bus_read8(sh7021.ea)) == 0)
		sh7021.sr |= SH_T;
	else
		sh7021.sr &= ~SH_T;
	sh7021.cycles_left -= 2;
}

/*  XOR     Rm,Rn */
static void XOR(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] ^= sh7021.gpr[m];
}

/*  XOR     #imm,R0 */
static void XORI(uint32_t i)
{
	sh7021.gpr[0] ^= i & 0x000000ff;
}

/*  XOR.B   #imm,@(R0,GBR) */
static void XORM(uint32_t i)
{
	sh7021.ea = sh7021.gbr + sh7021.gpr[0];
	sh7021_bus_write8(sh7021.ea, sh7021_bus_read8(sh7021.ea) ^ (uint8_t)i);
	sh7021.cycles_left -= 2;
}

/*  XTRCT   Rm,Rn */
static void XTRCT(uint32_t m, uint32_t n)
{
	sh7021.gpr[n] = (sh7021.gpr[n] >> 16) | (sh7021.gpr[m] << 16);
}

/*  SLEEP */
static void SLEEP()
{
	/* 0 = normal mode */
	/* 1 = enters into power-down mode */
	/* 2 = go out the power-down mode after an exception */
	if (sh7021.sleep_mode != 2)
		sh7021.pc -= 2;
	sh7021.cycles_left -= 2;
	/* Wait_for_exception; */
	if (sh7021.sleep_mode == 0)
		sh7021.sleep_mode = 1;
	else if (sh7021.sleep_mode == 2)
		sh7021.sleep_mode = 0;
}

/* Common dispatch */

static void op0010(uint16_t opcode)
{
	switch (opcode & 15)
	{
	case  0: MOVBS(REG_M, REG_N);   break;
	case  1: MOVWS(REG_M, REG_N);   break;
	case  2: MOVLS(REG_M, REG_N);   break;
	case  3: ILLEGAL();             break;
	case  4: MOVBM(REG_M, REG_N);   break;
	case  5: MOVWM(REG_M, REG_N);   break;
	case  6: MOVLM(REG_M, REG_N);   break;
	case  7: DIV0S(REG_M, REG_N);   break;
	case  8: TST(REG_M, REG_N);     break;
	case  9: AND(REG_M, REG_N);     break;
	case 10: XOR(REG_M, REG_N);     break;
	case 11: OR(REG_M, REG_N);      break;
	case 12: CMPSTR(REG_M, REG_N);  break;
	case 13: XTRCT(REG_M, REG_N);   break;
	case 14: MULU(REG_M, REG_N);    break;
	case 15: MULS(REG_M, REG_N);    break;
	}
}

static void op0011(uint16_t opcode)
{
	switch (opcode & 15)
	{
	case  0: CMPEQ(REG_M, REG_N);   break;
	case  1: ILLEGAL();             break;
	case  2: CMPHS(REG_M, REG_N);   break;
	case  3: CMPGE(REG_M, REG_N);   break;
	case  4: DIV1(REG_M, REG_N);    break;
	case  5: ILLEGAL();             break; /* DMULU.L is SH-2 only */
	case  6: CMPHI(REG_M, REG_N);   break;
	case  7: CMPGT(REG_M, REG_N);   break;
	case  8: SUB(REG_M, REG_N);     break;
	case  9: ILLEGAL();             break;
	case 10: SUBC(REG_M, REG_N);    break;
	case 11: SUBV(REG_M, REG_N);    break;
	case 12: ADD(REG_M, REG_N);     break;
	case 13: ILLEGAL();             break; /* DMULS.L is SH-2 only */
	case 14: ADDC(REG_M, REG_N);    break;
	case 15: ADDV(REG_M, REG_N);    break;
	}
}

static void op0110(uint16_t opcode)
{
	switch (opcode & 15)
	{
	case  0: MOVBL(REG_M, REG_N);   break;
	case  1: MOVWL(REG_M, REG_N);   break;
	case  2: MOVLL(REG_M, REG_N);   break;
	case  3: MOV(REG_M, REG_N);     break;
	case  4: MOVBP(REG_M, REG_N);   break;
	case  5: MOVWP(REG_M, REG_N);   break;
	case  6: MOVLP(REG_M, REG_N);   break;
	case  7: NOT(REG_M, REG_N);     break;
	case  8: SWAPB(REG_M, REG_N);   break;
	case  9: SWAPW(REG_M, REG_N);   break;
	case 10: NEGC(REG_M, REG_N);    break;
	case 11: NEG(REG_M, REG_N);     break;
	case 12: EXTUB(REG_M, REG_N);   break;
	case 13: EXTUW(REG_M, REG_N);   break;
	case 14: EXTSB(REG_M, REG_N);   break;
	case 15: EXTSW(REG_M, REG_N);   break;
	}
}

static void op1000(uint16_t opcode)
{
	switch ((opcode >> 8) & 15)
	{
	case  0: MOVBS4(opcode & 0x0f, REG_M);  break;
	case  1: MOVWS4(opcode & 0x0f, REG_M);  break;
	case  2: ILLEGAL();                     break;
	case  3: ILLEGAL();                     break;
	case  4: MOVBL4(REG_M, opcode & 0x0f);  break;
	case  5: MOVWL4(REG_M, opcode & 0x0f);  break;
	case  6: ILLEGAL();                     break;
	case  7: ILLEGAL();                     break;
	case  8: CMPIM(opcode & 0xff);          break;
	case  9: BT(opcode & 0xff);             break;
	case 10: ILLEGAL();                     break;
	case 11: BF(opcode & 0xff);             break;
	case 12: ILLEGAL();                     break;
	case 13: ILLEGAL();                     break; /* BT/S is SH-2 only */
	case 14: ILLEGAL();                     break;
	case 15: ILLEGAL();                     break; /* BF/S is SH-2 only */
	}
}


static void op1100(uint16_t opcode)
{
	switch ((opcode >> 8) & 15)
	{
	case  0: MOVBSG(opcode & 0xff);     break;
	case  1: MOVWSG(opcode & 0xff);     break;
	case  2: MOVLSG(opcode & 0xff);     break;
	case  3: TRAPA(opcode & 0xff);      break; // sh7021/4 differ
	case  4: MOVBLG(opcode & 0xff);     break;
	case  5: MOVWLG(opcode & 0xff);     break;
	case  6: MOVLLG(opcode & 0xff);     break;
	case  7: MOVA(opcode & 0xff);       break;
	case  8: TSTI(opcode & 0xff);       break;
	case  9: ANDI(opcode & 0xff);       break;
	case 10: XORI(opcode & 0xff);       break;
	case 11: ORI(opcode & 0xff);        break;
	case 12: TSTM(opcode & 0xff);       break;
	case 13: ANDM(opcode & 0xff);       break;
	case 14: XORM(opcode & 0xff);       break;
	case 15: ORM(opcode & 0xff);        break;
	}
}

// SH4 cases fall through to here too
static void execute_one_0000(uint16_t opcode)
{
	// 04,05,06,07 always the same, 0c,0d,0e,0f always the same, other change based on upper bits

	switch (opcode & 0x3f)
	{
	case 0x00: ILLEGAL();               break;
	case 0x01: ILLEGAL();               break;
	case 0x02: STCSR(REG_N);            break;
	case 0x03: ILLEGAL();               break; /* BSRF is SH-2 only */
	case 0x04: MOVBS0(REG_M, REG_N);    break;
	case 0x05: MOVWS0(REG_M, REG_N);    break;
	case 0x06: MOVLS0(REG_M, REG_N);    break;
	case 0x07: ILLEGAL();               break; /* MUL.L is SH-2 only */
	case 0x08: CLRT();                  break;
	case 0x09: NOP();                   break;
	case 0x0a: STSMACH(REG_N);          break;
	case 0x0b: RTS();                   break;
	case 0x0c: MOVBL0(REG_M, REG_N);    break;
	case 0x0d: MOVWL0(REG_M, REG_N);    break;
	case 0x0e: MOVLL0(REG_M, REG_N);    break;
	case 0x0f: ILLEGAL();               break; /* MAC.L is SH-2 only */

	case 0x10: ILLEGAL();               break;
	case 0x11: ILLEGAL();               break;
	case 0x12: STCGBR(REG_N);           break;
	case 0x13: ILLEGAL();               break;
	case 0x14: MOVBS0(REG_M, REG_N);    break;
	case 0x15: MOVWS0(REG_M, REG_N);    break;
	case 0x16: MOVLS0(REG_M, REG_N);    break;
	case 0x17: ILLEGAL();               break; /* MUL.L is SH-2 only */
	case 0x18: SETT();                  break;
	case 0x19: DIV0U();                 break;
	case 0x1a: STSMACL(REG_N);          break;
	case 0x1b: SLEEP();                 break;
	case 0x1c: MOVBL0(REG_M, REG_N);    break;
	case 0x1d: MOVWL0(REG_M, REG_N);    break;
	case 0x1e: MOVLL0(REG_M, REG_N);    break;
	case 0x1f: ILLEGAL();               break; /* MAC.L is SH-2 only */

	case 0x20: ILLEGAL();               break;
	case 0x21: ILLEGAL();               break;
	case 0x22: STCVBR(REG_N);           break;
	case 0x23: ILLEGAL();               break; /* BRAF is SH-2 only */
	case 0x24: MOVBS0(REG_M, REG_N);    break;
	case 0x25: MOVWS0(REG_M, REG_N);    break;
	case 0x26: MOVLS0(REG_M, REG_N);    break;
	case 0x27: ILLEGAL();               break; /* MUL.L is SH-2 only */
	case 0x28: CLRMAC();                break;
	case 0x29: MOVT(REG_N);             break;
	case 0x2a: STSPR(REG_N);            break;
	case 0x2b: RTE();                   break;
	case 0x2c: MOVBL0(REG_M, REG_N);    break;
	case 0x2d: MOVWL0(REG_M, REG_N);    break;
	case 0x2e: MOVLL0(REG_M, REG_N);    break;
	case 0x2f: ILLEGAL();               break; /* MAC.L is SH-2 only */

	case 0x30: ILLEGAL();               break;
	case 0x31: ILLEGAL();               break;
	case 0x32: ILLEGAL();               break;
	case 0x33: ILLEGAL();               break;
	case 0x34: MOVBS0(REG_M, REG_N);    break;
	case 0x35: MOVWS0(REG_M, REG_N);    break;
	case 0x36: MOVLS0(REG_M, REG_N);    break;
	case 0x37: ILLEGAL();               break; /* MUL.L is SH-2 only */
	case 0x38: ILLEGAL();               break;
	case 0x39: ILLEGAL();               break;
	case 0x3a: ILLEGAL();               break;
	case 0x3b: ILLEGAL();               break;
	case 0x3c: MOVBL0(REG_M, REG_N);    break;
	case 0x3d: MOVWL0(REG_M, REG_N);    break;
	case 0x3e: MOVLL0(REG_M, REG_N);    break;
	case 0x3f: ILLEGAL();               break; /* MAC.L is SH-2 only */
	}
}

// SH4 cases fall through to here too
static void execute_one_4000(uint16_t opcode)
{
	// 0f always the same, others differ

	switch (opcode & 0x3f)
	{
	case 0x00: SHLL(REG_N);         break;
	case 0x01: SHLR(REG_N);         break;
	case 0x02: STSMMACH(REG_N);     break;
	case 0x03: STCMSR(REG_N);       break;
	case 0x04: ROTL(REG_N);         break;
	case 0x05: ROTR(REG_N);         break;
	case 0x06: LDSMMACH(REG_N);     break;
	case 0x07: LDCMSR(opcode);      break;
	case 0x08: SHLL2(REG_N);        break;
	case 0x09: SHLR2(REG_N);        break;
	case 0x0a: LDSMACH(REG_N);      break;
	case 0x0b: JSR(REG_N);          break;
	case 0x0c: ILLEGAL();           break;
	case 0x0d: ILLEGAL();           break;
	case 0x0e: LDCSR(opcode);       break;
	case 0x0f: MAC_W(REG_M, REG_N); break;

	case 0x10: ILLEGAL();           break; /* DT is SH-2 only */
	case 0x11: CMPPZ(REG_N);        break;
	case 0x12: STSMMACL(REG_N);     break;
	case 0x13: STCMGBR(REG_N);      break;
	case 0x14: ILLEGAL();           break;
	case 0x15: CMPPL(REG_N);        break;
	case 0x16: LDSMMACL(REG_N);     break;
	case 0x17: LDCMGBR(REG_N);      break;
	case 0x18: SHLL8(REG_N);        break;
	case 0x19: SHLR8(REG_N);        break;
	case 0x1a: LDSMACL(REG_N);      break;
	case 0x1b: TAS(REG_N);          break;
	case 0x1c: ILLEGAL();           break;
	case 0x1d: ILLEGAL();           break;
	case 0x1e: LDCGBR(REG_N);       break;
	case 0x1f: MAC_W(REG_M, REG_N); break;

	case 0x20: SHAL(REG_N);         break;
	case 0x21: SHAR(REG_N);         break;
	case 0x22: STSMPR(REG_N);       break;
	case 0x23: STCMVBR(REG_N);      break;
	case 0x24: ROTCL(REG_N);        break;
	case 0x25: ROTCR(REG_N);        break;
	case 0x26: LDSMPR(REG_N);       break;
	case 0x27: LDCMVBR(REG_N);      break;
	case 0x28: SHLL16(REG_N);       break;
	case 0x29: SHLR16(REG_N);       break;
	case 0x2a: LDSPR(REG_N);        break;
	case 0x2b: JMP(REG_N);          break;
	case 0x2c: ILLEGAL();           break;
	case 0x2d: ILLEGAL();           break;
	case 0x2e: LDCVBR(REG_N);       break;
	case 0x2f: MAC_W(REG_M, REG_N); break;

	case 0x30: ILLEGAL();           break;
	case 0x31: ILLEGAL();           break;
	case 0x32: ILLEGAL();           break;
	case 0x33: ILLEGAL();           break;
	case 0x34: ILLEGAL();           break;
	case 0x35: ILLEGAL();           break;
	case 0x36: ILLEGAL();           break;
	case 0x37: ILLEGAL();           break;
	case 0x38: ILLEGAL();           break;
	case 0x39: ILLEGAL();           break;
	case 0x3a: ILLEGAL();           break;
	case 0x3b: ILLEGAL();           break;
	case 0x3c: ILLEGAL();           break;
	case 0x3d: ILLEGAL();           break;
	case 0x3e: ILLEGAL();           break;
	case 0x3f: MAC_W(REG_M, REG_N); break;

	}
}

static void execute_one(const uint16_t opcode)
{
	switch ((opcode >> 12) & 15)
	{
		case  0: execute_one_0000(opcode);              break;
		case  1: MOVLS4(REG_M, opcode & 0xf, REG_N);    break;
		case  2: op0010(opcode);                        break;
		case  3: op0011(opcode);                        break;
		case  4: execute_one_4000(opcode);              break;
		case  5: MOVLL4(REG_M, opcode & 0x0f, REG_N);   break;
		case  6: op0110(opcode);                        break;
		case  7: ADDI(opcode & 0xff, REG_N);            break;
		case  8: op1000(opcode);                        break;
		case  9: MOVWI(opcode & 0xff, REG_N);           break;
		case 10: BRA(opcode & 0xfff);                   break;
		case 11: BSR(opcode & 0xfff);                   break;
		case 12: op1100(opcode);                        break;
		case 13: MOVLI(opcode & 0xff, REG_N);           break;
		case 14: MOVI(opcode & 0xff, REG_N);            break;
		case 15: execute_one_f000(opcode);              break;
	}
}
/* SH7021-family control/exception helpers adapted from MAME sh7021.cpp. */
static void LDCMSR(const uint16_t opcode) {
    const uint32_t rn = REG_N;
    sh7021.ea = sh7021.gpr[rn];
    sh7021_set_sr(sh7021_bus_read32(sh7021.ea) & SH_FLAGS);
    sh7021.gpr[rn] += 4;
    sh7021.cycles_left -= 2;
    sh7021_block_irq_next();
    sh7021_mame_check_irq_after_sr_change();
}

static void LDCSR(const uint16_t opcode) {
    sh7021_set_sr(sh7021.gpr[REG_N] & SH_FLAGS);
    sh7021_block_irq_next();
    sh7021_mame_check_irq_after_sr_change();
}

static void RTE(void) {
    sh7021.ea = sh7021.gpr[15];
    sh7021.m_delay = sh7021_bus_read32(sh7021.ea);
    sh7021.gpr[15] += 4;
    sh7021.ea = sh7021.gpr[15];
    /* RTE is a delayed branch.  Do not re-arbitrate interrupts until the
       return delay slot has executed and PC has landed at the restored target;
       otherwise BIOS timer handlers can be re-entered in the RTE slot and
       corrupt their PR/stack restore path. */
    sh7021.sr = sh7021_bus_read32(sh7021.ea) & SH_FLAGS;
    sh7021.gpr[15] += 4;
    sh7021.cycles_left -= 3;
}

static void TRAPA(uint32_t i) {
    const uint32_t imm = i & 0xffu;
    sh7021.ea = sh7021.vbr + imm * 4u;
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], sh7021.sr);
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], sh7021.pc);
    sh7021.pc = sh7021_bus_read32(sh7021.ea);
    sh7021.cycles_left -= 7;
}

static void ILLEGAL(void) {
    LOOPY_DEBUG_PRINTF("[SH7021/MAME] illegal opcode at %08X\n", sh7021.pc - 2u);
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], sh7021.sr);
    sh7021.gpr[15] -= 4;
    sh7021_bus_write32(sh7021.gpr[15], sh7021.pc - 2u);
    sh7021.pc = sh7021_bus_read32(sh7021.vbr + 4u * 4u);
    sh7021.cycles_left -= 5;
}

static void execute_one_f000(uint16_t opcode) {
    (void)opcode;
    ILLEGAL();
}

void sh7021_interpreter_run(uint16_t instr) {
    execute_one(instr);
}
