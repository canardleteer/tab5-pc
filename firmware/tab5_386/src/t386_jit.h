// A translator from x86 code to RISC-V (a JIT), in front of the fast path (t386_fast.h) and tiny386. Included after
// t386_fast.h when T386_JIT is defined; the includer provides the platform part (see "platform" below).
// Built on tiny386's internal definitions (Copyright (c) 2024-2025 Chunhui He, BSD-3-Clause: LICENSES/).
//
// Code runs as blocks: straight runs of x86 instructions (up to a jump or something the translator leaves alone),
// translated once to RISC-V and kept in a code cache, found by their linear address, CS base and mode. Blocks jump
// to each other directly once both exist. Real mode, virtual-8086 mode and protected mode are translated: without
// paging linear addresses are physical, so guest memory is one host array; with paging (JM_PAGED) accesses go through
// the fast path's TLB, and a block stays within one page. For whatever a block can't do, the fast path (and through it
// tiny386) runs the code.
//
// While translated code runs, the x86 registers live in RISC-V registers s1-s8 (callee-saved, so they survive calls
// into C); s0 holds the CPU, s9 guest RAM, s10 a byte per 4 KB page of the guest's physical address space (pagemap:
// 0 RAM; 1, 2 RAM whose writes must be checked because translated code lies there or begins just after it; 0x80
// not RAM, through tiny386's callbacks), s11 the instructions left to run before interrupts are looked at. Flags
// stay lazy as in tiny386 (cpu->cc); the last flag-setting instruction's operands are kept in t3-t5 (and a7) and
// written to cpu->cc only when code outside the block may read them, so CMP + Jcc becomes a compare-and-branch.
// An instruction without a translation of its own is run in place by the fast path (registers stored around it).
//
// Writes to RAM holding translated code invalidate the blocks there: translated code and the fast path check the
// pagemap, tiny386 calls T386_RAM_WRITE (vendor/patches/0005), ISA DMA T386_DMA_WRITE. A block that overwrites
// itself runs to its end first (as a 486's prefetch queue would).

// ---- platform: the includer defines
//   void *t386_jit_alloc(size_t)                 zeroed memory, executable (the code cache) and writable; NULL if none
//   void t386_jit_sync(void *p, size_t n)        make code written at p executable (caches)
//   T386_JIT_CACHE_SIZE                          bytes of code cache
//   size_t T386_JIT_ROOM(void), T386_JIT_CACHE_MAX  (optional) the bytes all of the translator's memory may take when it
//                                                starts: the code cache gets what its tables leave of them, at least
//                                                T386_JIT_CACHE_SIZE and at most T386_JIT_CACHE_MAX
//   u8 *T386_JIT_PAGEMAP(u8 fill)                (optional) the pagemap's 1 MB, its bytes from JIT_MAP_OWN on (pages
//                                                past 256 MB) holding fill and never written; NULL if none

bool t386_jit_enabled = true;   // (serial "jit 0|1" on the Tab5: the fast path alone, for comparing)

// The two biggest functions of the translator compiled for size: on the Tab5 its code runs from flash, and a
// translation spends most of its time waiting for it there (instructions per cycle 0.31). Smaller, they wait less:
// 47.4 -> 42.7 us a block (Linux starting; the whole translator for size, or -O2: 45.6, 44.7).
#if defined(__GNUC__) && !defined(__clang__)
#define JIT_SMALL __attribute__((optimize("Os")))
#else
#define JIT_SMALL
#endif

// ---------------------------------------------------------------------------------------------------------------
// RISC-V instructions

enum {
	X0 = 0, RA = 1, SP = 2, T0 = 5, T1 = 6, T2 = 7, S0 = 8, S1 = 9, A0 = 10, A1 = 11, A2 = 12, A3 = 13, A4 = 14,
	A5 = 15, A6 = 16, A7 = 17, S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23, S8 = 24, S9 = 25, S10 = 26,
	S11 = 27, T3 = 28, T4 = 29, T5 = 30, T6 = 31,
};
// x86 EAX ECX EDX EBX ESP EBP ESI EDI
static const u8 jg[8] = { S1, S2, S3, S4, S5, S6, S7, S8 };
#define R_CPU S0
#define R_MEM S9
#define R_MAP S10
#define R_BUDGET S11
// the pending flags (see above): dst, src1, src2, and CF / dst2 where the operation keeps it apart
#define F_DST T3
#define F_SRC1 T4
#define F_SRC2 T5
#define F_CF A7

static u32 *jp, *jlimit;   // where code is being emitted, and how far it may go
static bool joverflow;     // the block didn't fit: abandoned

static inline void E(u32 insn)
{
	if (jp < jlimit) *jp++ = insn;
	else joverflow = true;
}
#define RV_R(f7, rs2, rs1, f3, rd, op) ((u32) (f7) << 25 | (u32) (rs2) << 20 | (u32) (rs1) << 15 | (u32) (f3) << 12 | (u32) (rd) << 7 | (op))
#define RV_I(imm, rs1, f3, rd, op) (((u32) (imm) & 0xfff) << 20 | (u32) (rs1) << 15 | (u32) (f3) << 12 | (u32) (rd) << 7 | (op))
#define RV_S(imm, rs2, rs1, f3) ((((u32) (imm) >> 5) & 0x7f) << 25 | (u32) (rs2) << 20 | (u32) (rs1) << 15 | (u32) (f3) << 12 | ((u32) (imm) & 0x1f) << 7 | 0x23)
static inline u32 rv_b(int off, int rs2, int rs1, int f3)
{
	u32 o = off;
	return ((o >> 12) & 1) << 31 | ((o >> 5) & 0x3f) << 25 | (u32) rs2 << 20 | (u32) rs1 << 15 | (u32) f3 << 12 |
	       ((o >> 1) & 0xf) << 8 | ((o >> 11) & 1) << 7 | 0x63;
}
static inline u32 rv_jal(int off, int rd)
{
	u32 o = off;
	return ((o >> 20) & 1) << 31 | ((o >> 1) & 0x3ff) << 21 | ((o >> 11) & 1) << 20 | ((o >> 12) & 0xff) << 12 |
	       (u32) rd << 7 | 0x6f;
}

static inline int rv_jal_off(u32 insn)   // a JAL's offset
{
	u32 o = ((insn >> 31) & 1) << 20 | ((insn >> 21) & 0x3ff) << 1 | ((insn >> 20) & 1) << 11 | ((insn >> 12) & 0xff) << 12;
	return (s32) (o << 11) >> 11;
}

static inline void e_addi(int rd, int rs, int imm) { E(RV_I(imm, rs, 0, rd, 0x13)); }
static inline void e_slti(int rd, int rs, int imm) { E(RV_I(imm, rs, 2, rd, 0x13)); }
static inline void e_sltiu(int rd, int rs, int imm) { E(RV_I(imm, rs, 3, rd, 0x13)); }
static inline void e_xori(int rd, int rs, int imm) { E(RV_I(imm, rs, 4, rd, 0x13)); }
static inline void e_ori(int rd, int rs, int imm) { E(RV_I(imm, rs, 6, rd, 0x13)); }
static inline void e_andi(int rd, int rs, int imm) { E(RV_I(imm, rs, 7, rd, 0x13)); }
static inline void e_slli(int rd, int rs, int sh) { E(RV_I(sh & 31, rs, 1, rd, 0x13)); }
static inline void e_srli(int rd, int rs, int sh) { E(RV_I(sh & 31, rs, 5, rd, 0x13)); }
static inline void e_srai(int rd, int rs, int sh) { E(RV_I(0x400 | (sh & 31), rs, 5, rd, 0x13)); }
static inline void e_add(int rd, int a, int b) { E(RV_R(0, b, a, 0, rd, 0x33)); }
static inline void e_sub(int rd, int a, int b) { E(RV_R(0x20, b, a, 0, rd, 0x33)); }
static inline void e_sll(int rd, int a, int b) { E(RV_R(0, b, a, 1, rd, 0x33)); }
static inline void e_slt(int rd, int a, int b) { E(RV_R(0, b, a, 2, rd, 0x33)); }
static inline void e_sltu(int rd, int a, int b) { E(RV_R(0, b, a, 3, rd, 0x33)); }
static inline void e_xor(int rd, int a, int b) { E(RV_R(0, b, a, 4, rd, 0x33)); }
static inline void e_srl(int rd, int a, int b) { E(RV_R(0, b, a, 5, rd, 0x33)); }
static inline void e_sra(int rd, int a, int b) { E(RV_R(0x20, b, a, 5, rd, 0x33)); }
static inline void e_or(int rd, int a, int b) { E(RV_R(0, b, a, 6, rd, 0x33)); }
static inline void e_and(int rd, int a, int b) { E(RV_R(0, b, a, 7, rd, 0x33)); }
static inline void e_mul(int rd, int a, int b) { E(RV_R(1, b, a, 0, rd, 0x33)); }
static inline void e_mulh(int rd, int a, int b) { E(RV_R(1, b, a, 1, rd, 0x33)); }
static inline void e_mulhu(int rd, int a, int b) { E(RV_R(1, b, a, 3, rd, 0x33)); }
static inline void e_lui(int rd, u32 imm) { E((imm & 0xfffff000) | (u32) rd << 7 | 0x37); }
static inline void e_auipc(int rd, u32 imm) { E((imm & 0xfffff000) | (u32) rd << 7 | 0x17); }
static inline void e_lb(int rd, int rs, int off) { E(RV_I(off, rs, 0, rd, 0x03)); }
static inline void e_lh(int rd, int rs, int off) { E(RV_I(off, rs, 1, rd, 0x03)); }
static inline void e_lw(int rd, int rs, int off) { E(RV_I(off, rs, 2, rd, 0x03)); }
static inline void e_lbu(int rd, int rs, int off) { E(RV_I(off, rs, 4, rd, 0x03)); }
static inline void e_lhu(int rd, int rs, int off) { E(RV_I(off, rs, 5, rd, 0x03)); }
static inline void e_sb(int rs2, int rs1, int off) { E(RV_S(off, rs2, rs1, 0)); }
static inline void e_sh(int rs2, int rs1, int off) { E(RV_S(off, rs2, rs1, 1)); }
static inline void e_sw(int rs2, int rs1, int off) { E(RV_S(off, rs2, rs1, 2)); }
static inline void e_jalr(int rd, int rs, int off) { E(RV_I(off, rs, 0, rd, 0x67)); }
static inline void e_mv(int rd, int rs) { if (rd != rs) e_addi(rd, rs, 0); }
static inline void e_nop(void) { e_addi(X0, X0, 0); }

// rd = a 32-bit constant
static void e_li(int rd, u32 v)
{
	s32 lo = (s32) (v << 20) >> 20;   // the low 12 bits, sign-extended as addi takes them
	u32 hi = v - (u32) lo;
	if (hi == 0) {
		e_addi(rd, X0, lo);
	} else {
		e_lui(rd, hi);
		if (lo) e_addi(rd, rd, lo);
	}
}

// loads and stores at an absolute address (lui + offset)
static void e_lw_abs(int rd, const void *p)
{
	u32 a = (u32) (uintptr_t) p;
	s32 lo = (s32) (a << 20) >> 20;
	e_lui(rd, a - (u32) lo);
	e_lw(rd, rd, lo);
}
static void e_sw_abs(int rs, int tmp, const void *p)
{
	u32 a = (u32) (uintptr_t) p;
	s32 lo = (s32) (a << 20) >> 20;
	e_lui(tmp, a - (u32) lo);
	e_sw(rs, tmp, lo);
}

// branches and jumps to code not emitted yet: a label is the address of the instruction to patch
enum { BEQ = 0, BNE = 1, BLT = 4, BGE = 5, BLTU = 6, BGEU = 7 };
static u32 *e_branch(int f3, int a, int b)   // patched by j_bind
{
	u32 *at = jp;
	E(rv_b(0, b, a, f3));
	return at;
}
static u32 *e_jump(void)
{
	u32 *at = jp;
	E(rv_jal(0, X0));
	return at;
}
// point the branch or jump at to target (within reach: blocks are kept small)
static void j_bind(u32 *at, const u32 *target)
{
	if (joverflow || !at)
		return;
	int off = (int) ((const u8 *) target - (const u8 *) at);
	u32 insn = *at;
	if ((insn & 0x7f) == 0x63) {
		if (off < -4096 || off > 4094) { joverflow = true; return; }
		*at = rv_b(off, (insn >> 20) & 31, (insn >> 15) & 31, (insn >> 12) & 7);
	} else {
		*at = rv_jal(off, (insn >> 7) & 31);
	}
}
static void e_branch_to(int f3, int a, int b, const u32 *target)
{
	u32 *at = e_branch(f3, a, b);
	j_bind(at, target);
}

// a call to an absolute address, ra the link
static void e_call_abs(const void *fn)
{
	u32 t = (u32) (uintptr_t) fn;
	s32 lo = (s32) (t << 20) >> 20;
	e_lui(RA, t - (u32) lo);
	e_jalr(RA, RA, lo);
}

// a call to code in the cache (a shared routine): a jal if it is near enough
static void e_call_near(const u32 *target)
{
	int off = (int) ((const u8 *) target - (const u8 *) jp);
	if (off >= -(1 << 20) && off < (1 << 20)) E(rv_jal(off, RA));
	else e_call_abs(target);
}

// a call to a C function (absolute address) through t386_jit_thunk, which keeps every register but a0
void t386_jit_thunk(void);
static void e_call(const void *fn)
{
	u32 f = (u32) (uintptr_t) fn, t = (u32) (uintptr_t) t386_jit_thunk;
	s32 flo = (s32) (f << 20) >> 20, tlo = (s32) (t << 20) >> 20;
	e_lui(T6, f - (u32) flo);
	e_addi(T6, T6, flo);
	e_lui(RA, t - (u32) tlo);
	e_jalr(RA, RA, tlo);
}

// ---------------------------------------------------------------------------------------------------------------
// entering and leaving translated code (assembly; the CPU structure's layout is checked below)

_Static_assert(offsetof(CPUI386, gprx) == 0, "registers at the start of the CPU state");
_Static_assert(offsetof(CPUI386, next_ip) == 36, "next_ip");

int t386_jit_enter(CPUI386 *cpu, const void *code, int budget, u8 *mem, u8 *map);
void t386_jit_exit(void);
int t386_jit_budget_left;     // s11 when translated code was left
u32 *t386_jit_link_slot;      // a2 when it was left: the exit to link (EXIT_LINK)
#define jit_budget_left t386_jit_budget_left
#define jit_link_slot t386_jit_link_slot

__asm__(
	"	.text\n"
	"	.align 2\n"
	"	.globl t386_jit_enter\n"
	"	.type t386_jit_enter, @function\n"
	// a0 cpu, a1 code, a2 budget, a3 guest RAM, a4 pagemap
	"t386_jit_enter:\n"
	"	addi sp, sp, -64\n"
	"	sw ra, 60(sp)\n"
	"	sw s0, 56(sp)\n"
	"	sw s1, 52(sp)\n"
	"	sw s2, 48(sp)\n"
	"	sw s3, 44(sp)\n"
	"	sw s4, 40(sp)\n"
	"	sw s5, 36(sp)\n"
	"	sw s6, 32(sp)\n"
	"	sw s7, 28(sp)\n"
	"	sw s8, 24(sp)\n"
	"	sw s9, 20(sp)\n"
	"	sw s10, 16(sp)\n"
	"	sw s11, 12(sp)\n"
	"	mv s0, a0\n"
	"	lw s1, 0(s0)\n"
	"	lw s2, 4(s0)\n"
	"	lw s3, 8(s0)\n"
	"	lw s4, 12(s0)\n"
	"	lw s5, 16(s0)\n"
	"	lw s6, 20(s0)\n"
	"	lw s7, 24(s0)\n"
	"	lw s8, 28(s0)\n"
	"	mv s9, a3\n"
	"	mv s10, a4\n"
	"	mv s11, a2\n"
	"	jr a1\n"
	"	.size t386_jit_enter, .-t386_jit_enter\n"
	// from translated code: a0 the exit reason, a1 the next eip, a2 the exit to link (EXIT_LINK)
	"	.globl t386_jit_exit\n"
	"	.type t386_jit_exit, @function\n"
	"t386_jit_exit:\n"
	"	sw s1, 0(s0)\n"
	"	sw s2, 4(s0)\n"
	"	sw s3, 8(s0)\n"
	"	sw s4, 12(s0)\n"
	"	sw s5, 16(s0)\n"
	"	sw s6, 20(s0)\n"
	"	sw s7, 24(s0)\n"
	"	sw s8, 28(s0)\n"
	"	sw a1, 36(s0)\n"
	"	lui t0, %hi(t386_jit_budget_left)\n"
	"	sw s11, %lo(t386_jit_budget_left)(t0)\n"
	"	lui t0, %hi(t386_jit_link_slot)\n"
	"	sw a2, %lo(t386_jit_link_slot)(t0)\n"
	"	lw ra, 60(sp)\n"
	"	lw s0, 56(sp)\n"
	"	lw s1, 52(sp)\n"
	"	lw s2, 48(sp)\n"
	"	lw s3, 44(sp)\n"
	"	lw s4, 40(sp)\n"
	"	lw s5, 36(sp)\n"
	"	lw s6, 32(sp)\n"
	"	lw s7, 28(sp)\n"
	"	lw s8, 24(sp)\n"
	"	lw s9, 20(sp)\n"
	"	lw s10, 16(sp)\n"
	"	lw s11, 12(sp)\n"
	"	addi sp, sp, 64\n"
	"	ret\n"
	"	.size t386_jit_exit, .-t386_jit_exit\n"
	// a call into C from translated code: t6 the function, a0-a3 its arguments; returns a0, keeps the rest
	"	.globl t386_jit_thunk\n"
	"	.type t386_jit_thunk, @function\n"
	"t386_jit_thunk:\n"
	"	addi sp, sp, -64\n"
	"	sw ra, 60(sp)\n"
	"	sw t0, 56(sp)\n"
	"	sw t1, 52(sp)\n"
	"	sw t2, 48(sp)\n"
	"	sw t3, 44(sp)\n"
	"	sw t4, 40(sp)\n"
	"	sw t5, 36(sp)\n"
	"	sw a1, 32(sp)\n"
	"	sw a2, 28(sp)\n"
	"	sw a3, 24(sp)\n"
	"	sw a4, 20(sp)\n"
	"	sw a5, 16(sp)\n"
	"	sw a6, 12(sp)\n"
	"	sw a7, 8(sp)\n"
	"	jalr ra, t6\n"
	"	lw ra, 60(sp)\n"
	"	lw t0, 56(sp)\n"
	"	lw t1, 52(sp)\n"
	"	lw t2, 48(sp)\n"
	"	lw t3, 44(sp)\n"
	"	lw t4, 40(sp)\n"
	"	lw t5, 36(sp)\n"
	"	lw a1, 32(sp)\n"
	"	lw a2, 28(sp)\n"
	"	lw a3, 24(sp)\n"
	"	lw a4, 20(sp)\n"
	"	lw a5, 16(sp)\n"
	"	lw a6, 12(sp)\n"
	"	lw a7, 8(sp)\n"
	"	addi sp, sp, 64\n"
	"	ret\n"
	"	.size t386_jit_thunk, .-t386_jit_thunk\n"
	// the slow paths of guest memory accesses, shared by all blocks: t0 the linear address, t6 the value
	// (written, or read); every other register but ra kept
	"	.macro JSAVE\n"
	"	addi sp, sp, -64\n"
	"	sw ra, 60(sp)\n"
	"	sw t0, 56(sp)\n"
	"	sw t1, 52(sp)\n"
	"	sw t2, 48(sp)\n"
	"	sw t3, 44(sp)\n"
	"	sw t4, 40(sp)\n"
	"	sw t5, 36(sp)\n"
	"	sw a0, 32(sp)\n"
	"	sw a1, 28(sp)\n"
	"	sw a2, 24(sp)\n"
	"	sw a3, 20(sp)\n"
	"	sw a4, 16(sp)\n"
	"	sw a5, 12(sp)\n"
	"	sw a6, 8(sp)\n"
	"	sw a7, 4(sp)\n"
	"	.endm\n"
	"	.macro JRESTORE\n"
	"	lw ra, 60(sp)\n"
	"	lw t0, 56(sp)\n"
	"	lw t1, 52(sp)\n"
	"	lw t2, 48(sp)\n"
	"	lw t3, 44(sp)\n"
	"	lw t4, 40(sp)\n"
	"	lw t5, 36(sp)\n"
	"	lw a0, 32(sp)\n"
	"	lw a1, 28(sp)\n"
	"	lw a2, 24(sp)\n"
	"	lw a3, 20(sp)\n"
	"	lw a4, 16(sp)\n"
	"	lw a5, 12(sp)\n"
	"	lw a6, 8(sp)\n"
	"	lw a7, 4(sp)\n"
	"	addi sp, sp, 64\n"
	"	ret\n"
	"	.endm\n"
	"	.macro JENTRY name, size, common\n"
	"	.globl \\name\n"
	"\\name:\n"
	"	JSAVE\n"
	"	li a1, \\size\n"
	"	j \\common\n"
	"	.endm\n"
	"	JENTRY t386_jit_rd1, 1, 1f\n"
	"	JENTRY t386_jit_rd2, 2, 1f\n"
	"	JENTRY t386_jit_rd4, 4, 1f\n"
	"1:	mv a0, t0\n"
	"	call t386_jit_h_read\n"
	"	mv t6, a0\n"
	"	JRESTORE\n"
	"	JENTRY t386_jit_wr1, 1, 2f\n"
	"	JENTRY t386_jit_wr2, 2, 2f\n"
	"	JENTRY t386_jit_wr4, 4, 2f\n"
	"	JENTRY t386_jit_wrr1, 0x101, 2f\n"   // (0x100: the instruction can be done again: jit_h_write)
	"	JENTRY t386_jit_wrr2, 0x102, 2f\n"
	"	JENTRY t386_jit_wrr4, 0x104, 2f\n"
	"2:	mv a0, t0\n"
	"	mv a2, t6\n"
	"	call t386_jit_h_write\n"
	"	mv t6, a0\n"   // (nonzero: translated code overwritten)
	"	JRESTORE\n"
	// a dead block's guarded entry (jit_invalidate): lui ra / jalr ra here in place of its first two words
	"	.globl t386_jit_dead_exit\n"
	"t386_jit_dead_exit:\n"
	"	addi a2, ra, -8\n"
	"	li a0, 5\n"   // EXIT_DEAD
	"	j t386_jit_exit\n"
	// an exit for the interpreter (jt_stub_leave): lui ra / jalr ra here, then the x86 address, and for
	// t386_jit_leave_f the routine that writes out the pending flags (jit_fstore: it uses t6 only)
	"	.globl t386_jit_leave_f\n"
	"t386_jit_leave_f:\n"
	"	mv a2, ra\n"
	"	lw t6, 4(ra)\n"
	"	jalr ra, t6, 0\n"
	"	mv ra, a2\n"
	"	.globl t386_jit_leave\n"
	"t386_jit_leave:\n"
	"	lw a1, 0(ra)\n"
	"	li a0, 2\n"   // EXIT_INTERP
	"	j t386_jit_exit\n"
	"	JENTRY t386_jit_rmw1, 1, 3f\n"
	"	JENTRY t386_jit_rmw2, 2, 3f\n"
	"	JENTRY t386_jit_rmw4, 4, 3f\n"
	"3:	mv a0, t0\n"
	"	call t386_jit_h_rmw\n"
	"	mv t6, a0\n"
	"	JRESTORE\n"
	// with paging (t386_jit_h_p*, a1 nonzero for a fault): an access that would fault returns past the stub's
	// instructions after the call (a read's mv and jump, a write's jump), to its exit
	"	JENTRY t386_jit_prd1, 1, 4f\n"
	"	JENTRY t386_jit_prd2, 2, 4f\n"
	"	JENTRY t386_jit_prd4, 4, 4f\n"
	"4:	mv a0, t0\n"
	"	call t386_jit_h_pread\n"
	"	mv t6, a0\n"
	"	beqz a1, 7f\n"
	"	lw a0, 60(sp)\n"
	"	addi a0, a0, 8\n"
	"	sw a0, 60(sp)\n"
	"7:	JRESTORE\n"
	"	JENTRY t386_jit_pwr1, 0x101, 5f\n"   // (0x100: as t386_jit_wrr*)
	"	JENTRY t386_jit_pwr2, 0x102, 5f\n"
	"	JENTRY t386_jit_pwr4, 0x104, 5f\n"
	"	JENTRY t386_jit_pwn1, 1, 5f\n"
	"	JENTRY t386_jit_pwn2, 2, 5f\n"
	"	JENTRY t386_jit_pwn4, 4, 5f\n"
	"5:	mv a0, t0\n"
	"	mv a2, t6\n"
	"	call t386_jit_h_pwrite\n"
	"	beqz a1, 8f\n"
	"	lw a0, 60(sp)\n"
	"	addi a0, a0, 4\n"
	"	sw a0, 60(sp)\n"
	"8:	JRESTORE\n"
	"	JENTRY t386_jit_prmw1, 1, 6f\n"
	"	JENTRY t386_jit_prmw2, 2, 6f\n"
	"	JENTRY t386_jit_prmw4, 4, 6f\n"
	"6:	mv a0, t0\n"
	"	call t386_jit_h_prmw\n"
	"	mv t6, a0\n"
	"	JRESTORE\n"
	// ... from the compact stubs (jt_stub_compact): ra their data word. A read (called with its return in t6)
	// returns the value in t6, to the routine that moves it to its register; a write (the value in t6) goes on at
	// the stub's resume; an access that would fault leaves for the interpreter as jt_stub_leave's exit does
	"	.macro JREST\n"   // (JRESTORE but a6, without the return)
	"	lw ra, 60(sp)\n"
	"	lw t0, 56(sp)\n"
	"	lw t1, 52(sp)\n"
	"	lw t2, 48(sp)\n"
	"	lw t3, 44(sp)\n"
	"	lw t4, 40(sp)\n"
	"	lw t5, 36(sp)\n"
	"	lw a0, 32(sp)\n"
	"	lw a1, 28(sp)\n"
	"	lw a2, 24(sp)\n"
	"	lw a3, 20(sp)\n"
	"	lw a4, 16(sp)\n"
	"	lw a5, 12(sp)\n"
	"	lw a7, 4(sp)\n"
	"	addi sp, sp, 64\n"
	"	.endm\n"
	"	JENTRY t386_jit_cprd1, 1, .Ljc_rd\n"
	"	JENTRY t386_jit_cprd2, 2, .Ljc_rd\n"
	"	JENTRY t386_jit_cprd4, 4, .Ljc_rd\n"
	".Ljc_rd:\n"
	"	sw t6, 0(sp)\n"
	"	mv a0, t0\n"
	"	call t386_jit_h_pread\n"
	"	bnez a1, .Ljc_fault\n"
	"	mv t6, a0\n"
	"	lw a6, 0(sp)\n"
	"	JREST\n"
	"	jr a6\n"
	"	JENTRY t386_jit_cpwr1, 0x101, .Ljc_wr\n"   // (0x100: as t386_jit_wrr*)
	"	JENTRY t386_jit_cpwr2, 0x102, .Ljc_wr\n"
	"	JENTRY t386_jit_cpwr4, 0x104, .Ljc_wr\n"
	"	JENTRY t386_jit_cpwn1, 1, .Ljc_wr\n"
	"	JENTRY t386_jit_cpwn2, 2, .Ljc_wr\n"
	"	JENTRY t386_jit_cpwn4, 4, .Ljc_wr\n"
	".Ljc_wr:\n"
	"	mv a0, t0\n"
	"	mv a2, t6\n"
	"	call t386_jit_h_pwrite\n"
	"	bnez a1, .Ljc_fault\n"
	"	JREST\n"   // (on at the stub's resume: back from its data word by its low 13 bits, in words)
	"	lw a6, 0(ra)\n"
	"	slli a6, a6, 19\n"
	"	srli a6, a6, 17\n"
	"	sub a6, ra, a6\n"
	"	jr a6\n"
	// a push's (jit_r_push: called with its return in t1, ra the block's data word): back to it once written
	"	JENTRY t386_jit_cpsh2, 0x102, .Ljc_psh\n"
	"	JENTRY t386_jit_cpsh4, 0x104, .Ljc_psh\n"
	".Ljc_psh:\n"
	"	mv a0, t0\n"
	"	mv a2, t6\n"
	"	call t386_jit_h_pwrite\n"
	"	bnez a1, .Ljc_fault\n"
	"	JREST\n"
	"	jr t1\n"
	// a pop's (jit_r_pop: called with its return in t1, ra the block's data word): back with the value in t6
	"	JENTRY t386_jit_cpop2, 2, .Ljc_pop\n"
	"	JENTRY t386_jit_cpop4, 4, .Ljc_pop\n"
	".Ljc_pop:\n"
	"	mv a0, t0\n"
	"	call t386_jit_h_pread\n"
	"	bnez a1, .Ljc_fault\n"
	"	mv t6, a0\n"
	"	JREST\n"
	"	jr t1\n"
	".Ljc_fault:\n"
	"	lw a0, 60(sp)\n"
	"	call t386_jit_h_cfault\n"
	"	JREST\n"
	"	lui a6, %hi(t386_jit_cf_refund)\n"
	"	lw a6, %lo(t386_jit_cf_refund)(a6)\n"
	"	add s11, s11, a6\n"
	"	lui a6, %hi(t386_jit_cf_f)\n"
	"	lw t6, %lo(t386_jit_cf_f)(a6)\n"
	"	beqz t6, 1f\n"
	"	jalr ra, t6, 0\n"   // (the pending flags written out: jit_fstore, which uses t6 only)
	"1:	lui a1, %hi(t386_jit_cf_eip)\n"
	"	lw a1, %lo(t386_jit_cf_eip)(a1)\n"
	"	li a0, 2\n"   // EXIT_INTERP
	"	j t386_jit_exit\n"
	);
void t386_jit_rd1(void), t386_jit_rd2(void), t386_jit_rd4(void);
void t386_jit_wr1(void), t386_jit_wr2(void), t386_jit_wr4(void);
void t386_jit_wrr1(void), t386_jit_wrr2(void), t386_jit_wrr4(void);
void t386_jit_rmw1(void), t386_jit_rmw2(void), t386_jit_rmw4(void);
void t386_jit_prd1(void), t386_jit_prd2(void), t386_jit_prd4(void);
void t386_jit_pwr1(void), t386_jit_pwr2(void), t386_jit_pwr4(void);
void t386_jit_pwn1(void), t386_jit_pwn2(void), t386_jit_pwn4(void);
void t386_jit_prmw1(void), t386_jit_prmw2(void), t386_jit_prmw4(void);
void t386_jit_cprd1(void), t386_jit_cprd2(void), t386_jit_cprd4(void);
void t386_jit_cpwr1(void), t386_jit_cpwr2(void), t386_jit_cpwr4(void);
void t386_jit_cpwn1(void), t386_jit_cpwn2(void), t386_jit_cpwn4(void);
void t386_jit_cpsh2(void), t386_jit_cpsh4(void);
void t386_jit_cpop2(void), t386_jit_cpop4(void);
void t386_jit_dead_exit(void);
void t386_jit_leave(void), t386_jit_leave_f(void);

// why translated code was left (a0 at t386_jit_exit)
enum {
	EXIT_JUMP,     // to cpu->next_ip, found through the block table
	EXIT_LINK,     // a direct jump to cpu->next_ip whose exit (jit_link_slot) is to be linked to the block there
	EXIT_INTERP,   // the instruction at cpu->next_ip needs the interpreter
	EXIT_BUDGET,   // no instructions left for this step: at cpu->next_ip
	EXIT_STOP,     // the interpreter, run from inside a block, stopped (exception, halt, interrupt due, ...)
	EXIT_DEAD,     // a jump into a dead block's guarded entry (jit_invalidate): its code at jit_link_slot
};

// ---------------------------------------------------------------------------------------------------------------
// the code cache

// what a translation depends on besides its address and CS base
enum { JM_CODE16 = 1, JM_PROTECTED = 2, JM_STACK16 = 4, JM_IO = 8 /* IN and OUT need no permission check */,
       JM_PAGED = 16 /* paging: accesses through the fast path's TLB, the block within one page (jit_translate) */,
       JM_VM = 32 /* virtual-8086 mode: as real mode (no JM_PROTECTED), at CPL 3, never JM_IO (IN, OUT: the I/O
		     permission bitmap); INT, IRET, PUSHF, POPF, CLI, STI (IOPL-sensitive): the interpreter */,
       JM_FLAT = 64 /* protected mode with DS, ES and SS based at 0 (Linux, flat DOS extenders): their bases aren't
		       added. A block left when one gets another base: jit_h_ldseg, jit_after; links: same mode only */ };

typedef struct JitLink {        // an exit of some block jumping straight into this one
	u32 *slot;              // the exit (two words: jt_link_exit_cs)
	u32 *rt;                // ... and the routine it called before (jit_t_lexit, the copy it reached)
	struct JitLink *next;
} JitLink;

typedef struct JitBlock {
	uword lin, cs_base;     // translated from the code at linear address lin, CS based at cs_base
	uword phys;             // ... at physical address phys (= lin without paging): writes there invalidate it
	u8 mode;                // JM_*
	u8 dead;                // invalidated (its code stays until the cache is emptied)
	u8 checked;             // self-checking (self-modifying code: see jit_translate), not invalidated by writes
	u8 fails;               // ... and how often its first instruction was found changed
	u8 nsegs;               // protected mode: the segments it checks on entry aren't null (JIT_NULLSEGS)
	u8 guarded;             // dead, its entry a jump to t386_jit_dead_exit and the links into it kept (jit_revive)
	u8 crossing;            // with paging, its last instruction runs on into the next page, which (as found then)
	                        // lies after its page in RAM: jit_crossing_ok
	u16 nbytes;             // x86 code bytes from lin
	u32 born;               // cpu->cycle when it was translated
	u32 dyn[4];             // rewritten immediates it reads where they are (physical address << 3 | size): writes
	                        // to just those don't invalidate it
	u32 *code;
	u32 *xentry;            // with paging, the entry for jumps from other pages (jit_xentry), or NULL
	u32 xepoch, xmap;       // ... and what it checks: ftlb_flushes and the TLB when the dispatcher last found it
	struct JitBlock *hnext;     // in the table
	struct JitBlock *pnext[2];  // in the lists of the (up to two) pages its code lies on
	struct JitBlock *bnext;     // in its bucket's list (jit_buckets)
	JitLink *links;
	const u8 *src;              // the x86 bytes it was translated from (jit_src), or NULL
	struct JitBlock *gnext;     // dead: in the graveyard's list (jit_grave)
	u32 entry[2];               // guarded (dead, links into it kept): its first two words, as they were
} JitBlock;

#define JIT_HASH 32768   // (in PSRAM: the blocks are, and a chain walked is a cache miss a block)
#define JIT_MIN_BLOCKS 16384
#define JIT_CACHE_PER_BLOCK 256   // a block in the table per this many bytes of code cache (beyond JIT_MIN_BLOCKS:
                                  // blocks take 240 (Linux) to 400 bytes of code)
static int jit_max_blocks;            // (jit_init)
static JitBlock **jit_hash;           // [JIT_HASH]
static JitBlock *jit_blocks;          // [jit_max_blocks]
static int jit_nblocks;
static JitLink *jit_links;            // [jit_max_blocks * 2]
static int jit_nlinks;
static JitLink *jit_link_free;        // links into blocks that died, for others (jit_invalidate): with blocks
                                      // revived instead of made, the cache is seldom emptied, which frees them all
static u32 *jit_cache, *jit_cache_end, *jit_cache_free;
static size_t jit_cache_size;
static u32 *jit_cache_base;           // after the code made at the start (shared routines): blocks from here
static JitBlock **jit_page_blocks;    // per 4 KB page of RAM: the blocks translated from there
// Dead blocks (without paging) whose code could serve again: a program loaded where it was before, code a program
// patches and puts back (a RETF in a game's compiled scalers), FreeDOS's installer running a program a package. Each block
// keeps the x86 bytes it was translated from (jit_src, emptied with the cache); one found dead with the same
// address, CS base and mode, and those bytes in RAM again, is put back in place of a translation (jit_revive).
#define JIT_GRAVE 4096
#define JIT_SRC_BYTES (512 << 10)
static JitBlock **jit_grave;          // [JIT_GRAVE], by physical address
static u8 *jit_src, *jit_src_free;
static long jit_stat_revived;
static inline unsigned jit_grave_hash(uword phys) { return (phys ^ (phys >> 11)) & (JIT_GRAVE - 1); }
// Per 256 bytes of RAM: the blocks that start there. A write finds the blocks it hits in the few buckets from
// JIT_BLOCK_BYTES before it (a page's list can be long: a game's compiled sprite or wall scalers, each a block, that
// it writes into as it draws).
#define JIT_BUCKET_SHIFT 8
#define JIT_BLOCK_BYTES 1040   // a block's code is less than this (jit_translate: up to 1024 + one instruction)
static JitBlock **jit_buckets;
static u8 *jit_codelines;             // a bit per 64 bytes of RAM: translated code there (writes elsewhere in a
                                      // page with code go ahead: code and data often share pages)
// Self-modifying code: a block address whose translation keeps being overwritten soon after it was made is left
// to the interpreter for a while (loading programs overwrites many blocks, each once).
#define JIT_SMC 256
static struct JitSmc { uword lin; u32 count, last; } *jit_smc;   // [JIT_SMC] (PSRAM, as the tables below: internal RAM
								  // is what WiFi's DMA needs)
#define JIT_SMC_SOON 1000000          // overwritten within this many instructions of the last time
#define JIT_SMC_TIMES 4               // this many times in a row: interpreted
#define JIT_SMC_HOLD 20000000         // for this many instructions after the last time

// Immediates (and 32-bit displacements) a program rewrites as it runs (3D renderers' column and span drawers set
// their step so before each column, or their colormap's address and more once a frame):
// writes of up to 4 bytes that invalidated a block soon after it was translated, again (jit_invalidate). An
// instruction whose immediate is one of them is translated to read it where it is (jt_classify: the interpreter),
// and its block isn't invalidated by writes to it, as each would make it translated again.
#define JIT_DYNIMM 256
#define JIT_DYN_SOON 16000000         // (the same block overwritten twice within this many instructions: a renderer
                                      // may patch its drawers' displacements once a frame, ~2 M instructions apart)
static struct JitDynimm { uword phys; u8 len; } *jit_dynimm;   // [JIT_DYNIMM]
static inline unsigned jit_dynimm_hash(uword p) { return (p ^ (p >> 8)) & (JIT_DYNIMM - 1); }

// is the immediate of size bytes at physical address at one (or holds one)?
static bool jit_imm_dynamic(uword at, int size)
{
	for (int i = 0; i < size; i++) {
		unsigned h = jit_dynimm_hash(at + i);
		if (jit_dynimm[h].phys == at + i && i + jit_dynimm[h].len <= size) return true;
	}
	return false;
}

// is the write [addr, addr + len) within one of b's rewritten immediates?
static inline bool jit_dyn_covers(const JitBlock *b, uword addr, int len)
{
	for (int i = 0; i < 4 && b->dyn[i]; i++) {
		uword at = b->dyn[i] >> 3;
		if (addr >= at && addr + len <= at + (b->dyn[i] & 7)) return true;
	}
	return false;
}
static unsigned jit_generation;       // counts emptyings of the cache
// Writes found to invalidate nothing (the blocks there read those bytes where they run: dyn), until a block is next
// put in the buckets: a game's ray caster rewrites a Jcc for every ray (t386_code_write).
#define JIT_SAFEW 64
static struct JitSafew { uword addr; u32 added; int len; } jit_safew[JIT_SAFEW];
static u32 jit_added;                 // counts blocks put in the buckets
static long jit_stat_invalidated, jit_stat_exit[6], jit_stat_volatile;
static long long jit_stat_ins, jit_stat_bytes;   // (past 2^31 in a long session)
// the last invalidations (serial "jit inv"): the block, the write, where the writer was
static struct { uword lin, waddr, cs, eip; u16 nbytes; u8 wlen; } jit_inv_log[32];
static unsigned jit_inv_n;
static uword jit_waddr;
static int jit_wlen;
static uword jit_last_volatile;
uword jit_trace_waddr;
static uword jit_pages;               // pages of RAM
static bool jit_ready;
static CPUI386 *jit_cpu;

// t386_pagemap, a byte per 4 KB page of the address space: translated code's accesses go inline to RAM if it is 0
// (loads: if it is >= 0). PM_SLOW (the sign bit) sends both to the slow path: on its own, for RAM pages just before
// MMIO or the end of RAM (where an access of 2 or 4 bytes may run on into them); with PM_NOT_RAM, for those.
enum { PM_RAM = 0, PM_CODE = 1, PM_CODE_NEXT = 2, PM_NOT_RAM = 0x40, PM_SLOW = 0x80, PM_IO = PM_SLOW | PM_NOT_RAM };
#define JIT_MAP_OWN (1u << 16)   // pages that can be RAM (256 MB): beyond, PM_IO alone (see T386_JIT_PAGEMAP)
static inline bool pm_io(uword p) { return (t386_pagemap[p] & PM_IO) == PM_IO; }

static inline unsigned jit_hash_of(uword lin) { return (lin ^ (lin >> 14)) & (JIT_HASH - 1); }

// Where indirect jumps (RET, JMP and CALL through a register or memory) find their block from translated code:
// sets of two entries by linear address, the newer first; filled by the dispatcher (the entry there before goes
// second, the second one out); an invalidated block's entry is emptied. The set: the top bits of the address, its
// high bits folded into its low ones, times 2^32 / phi. (lin ^ lin >> 10, at first, put pairs of Linux's console
// code that run for every line in one slot, each pushing the other out: 34 thousand trips through the dispatcher a
// second. Then the product's top bits alone, one entry a slot, 1024 of them: lynx parsing a page, with Linux's
// own, missed ~20 thousand times a second, its three busiest targets in one slot. Two entries a set and the fold:
// QEMU's trace of that parse, 6.2% of its indirect jumps missing -> 0.8% with 512 sets (the 16 KB of before),
// 0.2% with 1024. But 1024 took 16 KB more internal RAM, and Wi-Fi, which takes what's left when it starts,
// then moved files ~13% slower.)
#define JIT_IJC_BITS 9   // (sets)
#define JIT_IJC (1 << JIT_IJC_BITS)
#define JIT_IJC_MUL 0x9e3779b1u
static struct JitIjc { uword lin, cs_base, mode; u32 *code; } jit_ijc[2 * JIT_IJC];
static inline unsigned jit_ijc_hash(uword lin) { return (u32) ((lin ^ lin >> 15) * JIT_IJC_MUL) >> (32 - JIT_IJC_BITS); }

// The TLB was flushed: the indirect jump cache's entries for paged code may have another mapping now (all go)
static void jit_ijc_unmap(void)
{
	memset(jit_ijc, 0xff, sizeof(jit_ijc));
}

static void jit_rt_restore(void);

#ifdef T386_TRACE
// (a debugging build, DEFS=-DT386_TRACE) the dispatcher's last steps, invalidations and emptyings, in
// a ring; frozen when the guest reboots or real mode runs past 1 MB + 64 KB; serial "trace" prints it
#define JIT_TRACE_N 1024
enum { TR_BLOCK, TR_COLD, TR_INTERP, TR_INVAL, TR_FLUSH, TR_CORRUPT };
// (on the Tab5 in memory a restart keeps: after a panic, the trace up to it, frozen by the boot's reset vector)
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define JIT_TRACE_KEEP __NOINIT_ATTR
#else
#define JIT_TRACE_KEEP
#endif
static JIT_TRACE_KEEP struct JitTrace { u32 a, b, c; u8 kind; } jit_trace[JIT_TRACE_N];
static JIT_TRACE_KEEP u32 jit_trace_n, jit_trace_magic;
static bool jit_trace_frozen;
static void jit_trace_boot(void)   // (jit_init) a trace from before a restart kept, frozen
{
	if (jit_trace_magic == 0x7a11b0a7) {
		jit_trace_frozen = true;
	} else {
		jit_trace_magic = 0x7a11b0a7;
		jit_trace_n = 0;
	}
}
static inline void jit_tr(u8 kind, u32 a, u32 b, u32 c)
{
	if (jit_trace_frozen) return;
	struct JitTrace *t = &jit_trace[jit_trace_n++ & (JIT_TRACE_N - 1)];
	t->kind = kind, t->a = a, t->b = b, t->c = c;
}
void t386_trace_report(void (*out)(const char *))
{
	static const char *const names[] = { "block", "cold", "interp", "inval", "flush", "changed" };
	char line[96];
	snprintf(line, sizeof line, "trace: %u steps, %s", (unsigned) jit_trace_n, jit_trace_frozen ? "frozen" : "running");
	out(line);
	const u32 end = jit_trace_n;   // (as it is now: the guest may go on adding while this is printed)
	for (u32 k = end > JIT_TRACE_N ? end - JIT_TRACE_N : 0; k < end; k++) {
		const struct JitTrace *t = &jit_trace[k & (JIT_TRACE_N - 1)];
		snprintf(line, sizeof line, "%5u %-6s %08x %08x %08x", (unsigned) (end - k), names[t->kind],
			 (unsigned) t->a, (unsigned) t->b, (unsigned) t->c);
		out(line);
	}
}
void t386_trace_thaw(void) { jit_trace_frozen = false; jit_trace_n = 0; }
#define JIT_TR(kind, a, b, c) jit_tr(kind, a, b, c)
// each block's code summed when it is made and when a jump in it is patched, and all of them checked now and then
// (jit_sums_check): code that changes after that, by whatever wrote there, freezes the trace; serial "trace" shows
// the block, its words then
static u32 *jit_sums;
static JitBlock *jit_bad_block;
static u32 jit_bad_words[48], jit_bad_sum, jit_bad_now;
static void jit_sum_set(JitBlock *b);
static void jit_sum_of(const u32 *p);
static void jit_sums_check(void);
#define JIT_SUM_SET(b) jit_sum_set(b)
#define JIT_SUM_OF(p) jit_sum_of(p)
void t386_trace_bad(void (*out)(const char *))
{
	char line[96];
	if (!jit_bad_block) { out("code sums: all as made"); return; }
	snprintf(line, sizeof line, "code sums: block %08x (code %p) %08x, made %08x", (unsigned) jit_bad_block->lin,
		 (void *) jit_bad_block->code, (unsigned) jit_bad_now, (unsigned) jit_bad_sum);
	out(line);
	for (int i = 0; i < 48; i += 6) {
		snprintf(line, sizeof line, "  %08x %08x %08x %08x %08x %08x", (unsigned) jit_bad_words[i],
			 (unsigned) jit_bad_words[i + 1], (unsigned) jit_bad_words[i + 2], (unsigned) jit_bad_words[i + 3],
			 (unsigned) jit_bad_words[i + 4], (unsigned) jit_bad_words[i + 5]);
		out(line);
	}
}
#else
#define JIT_TR(kind, a, b, c)
#define JIT_SUM_SET(b)
#define JIT_SUM_OF(p)
#endif

// Empty the cache: all translations go.
static void jit_flush(void)
{
	JIT_TR(TR_FLUSH, jit_generation, 0, 0);
	jit_rt_restore();
	jit_generation++;
	memset(jit_ijc, 0xff, sizeof(jit_ijc));
	if (jit_hash) memset(jit_hash, 0, sizeof(JitBlock *) * JIT_HASH);
	jit_nblocks = 0;
	jit_nlinks = 0;
	jit_link_free = NULL;
	jit_cache_free = jit_cache_base;
	if (jit_page_blocks) memset(jit_page_blocks, 0, jit_pages * sizeof(JitBlock *));
	if (jit_buckets) memset(jit_buckets, 0, (jit_pages << (12 - JIT_BUCKET_SHIFT)) * sizeof(JitBlock *));
	if (t386_pagemap) {
		for (uword p = 0; p < jit_pages; p++)
			t386_pagemap[p] &= ~(PM_CODE | PM_CODE_NEXT);
	}
	if (jit_codelines) memset(jit_codelines, 0, jit_pages * 8);
	if (jit_grave) memset(jit_grave, 0, sizeof(JitBlock *) * JIT_GRAVE);
	jit_src_free = jit_src;
}

static bool jit_lines_code(uword addr, int len);

// the buckets that blocks with bytes in [addr, end) can start in
static inline uword jit_first_bucket(uword addr)
{
	return (addr > JIT_BLOCK_BYTES ? addr - JIT_BLOCK_BYTES : 0) >> JIT_BUCKET_SHIFT;
}
static inline uword jit_last_bucket(uword end)
{
	uword last = (end - 1) >> JIT_BUCKET_SHIFT, nb = jit_pages << (12 - JIT_BUCKET_SHIFT);
	return last < nb ? last : nb - 1;
}

// is there translated code exactly in [addr, addr + len)? (the lines first, then the blocks there)
static bool jit_bytes_code(uword addr, int len)
{
	if (!jit_lines_code(addr, len)) return false;
	uword end = addr + len;
	for (uword k = jit_first_bucket(addr); k <= jit_last_bucket(end); k++)
		for (JitBlock *b = jit_buckets[k]; b; b = b->bnext)
			if (b->phys < end && b->phys + b->nbytes > addr) return true;
	return false;
}

// is the block at lin self-modifying (see jit_smc)?
static bool jit_volatile(uword lin)
{
	unsigned h = (lin ^ (lin >> 8)) & (JIT_SMC - 1);
	return jit_smc[h].lin == lin && jit_smc[h].count >= JIT_SMC_TIMES && (u32) jit_cpu->cycle - jit_smc[h].last < JIT_SMC_HOLD;
}

// Code is translated once it has been reached t386_jit_hot times (from the dispatcher; counted by physical address,
// in a table where others may share its count): a large protected-mode program runs much of its code a few times
// only, and translating it all filled the cache over and over. Until then the fast path runs it, JIT_COLD_RUN
// instructions at a time.
int t386_jit_hot = 8;   // (serial "jit hot N")
#define JIT_COLD_RUN 8
static u8 *jit_heat;   // [8192]
static bool jit_warm(uword phys, bool count)
{
	u8 *h = &jit_heat[(phys * 0x9e3779b1u) >> 19];
	if (*h >= t386_jit_hot) return true;
	if (count) (*h)++;
	return false;
}

// is there translated code in [addr, addr + len)?
static bool jit_lines_code(uword addr, int len)
{
	for (uword l = addr >> 6; l <= (addr + len - 1) >> 6; l++)
		if (l < jit_pages * 64 && (jit_codelines[l >> 3] >> (l & 7) & 1)) return true;
	return false;
}

static void jit_set_lines(uword addr, uword len)
{
	for (uword l = addr >> 6; l <= (addr + len - 1) >> 6; l++)
		if (l < jit_pages * 64) jit_codelines[l >> 3] |= 1 << (l & 7);
}

// page p's lines, from the blocks still there
static void jit_page_lines(uword p)
{
	memset(jit_codelines + p * 8, 0, 8);
	for (JitBlock *b = jit_page_blocks[p]; b; b = b->pnext[b->phys >> 12 == p ? 0 : 1]) {
		uword a = b->phys, e = b->phys + b->nbytes;
		if (a < p << 12) a = p << 12;
		if (e > (p + 1) << 12) e = (p + 1) << 12;
		jit_set_lines(a, e - a);
	}
}

static void jit_make_fstores(void);
static void jit_make_routines(void);
static void jit_rt_save(void);
static void jit_make_vga_writes(void);
static void jit_make_shared(void);
static u32 *jit_r_ijc, *jit_r_stop, *jit_r_interp, *jit_r_callc;   // the shared routines blocks jump to (jit_make_shared)
static u32 *jit_r_cf, *jit_r_cond;   // ... and call: CF, a condition, of flags not known in the block (jt_cf, jt_cond)
static u32 *jit_r_tlb[2][3], *jit_r_tlbs[6][2][3];   // (jit_r_tlbs: the segment's base added to t0 first)
static u32 *jit_t_prd[3], *jit_t_pwr[2][3], *jit_t_prmw[3], *jit_t_leave, *jit_t_leave_f;   // (trampolines: jit_tramp)
static u32 *jit_t_exit[5];   // leave with a reason, the x86 address in the word after the call (jt_exit)
static u32 *jit_t_lexit;     // ... EXIT_LINK, a linkable exit not linked yet (jt_link_exit_cs)
static u32 *jit_t_bexit;     // ... EXIT_BUDGET at the block's start (its stub, one word: the dispatcher finds the address)
static u32 *jit_t_cprd[3][32], *jit_t_cpwr[2][3][32];   // the compact stubs' routines, by size and register
static u32 *jit_r_push[2][2][9];   // with paging, a push (jt_push_call): 16-bit stack or not, 2 or 4 bytes, of a0, s1-s8
static u32 *jit_r_pop[2][2][18];   // ... a pop (jt_pop_call): into a0, a1, s1-s8, or (2 bytes) the low half of s1-s8

#ifndef T386_JIT_CACHE_MAX
#define T386_JIT_CACHE_MAX T386_JIT_CACHE_SIZE
#endif
// The code cache's size (in bytes, a multiple of 64 KB): T386_JIT_CACHE_SIZE, or with T386_JIT_ROOM what the room left
// by the tables holds, a JitBlock and two JitLinks in the table for each JIT_CACHE_PER_BLOCK bytes of code (the
// pagemap among the tables unless map_made)
static size_t jit_cache_bytes(bool map_made)
{
#ifdef T386_JIT_ROOM
	const size_t room = T386_JIT_ROOM();
	const size_t tables = sizeof(JitBlock *) * (JIT_HASH + jit_pages + (jit_pages << (12 - JIT_BUCKET_SHIFT))) +
			      (map_made ? 0 : 1 << 20) + jit_pages * 8 + sizeof(*jit_smc) * JIT_SMC +
			      sizeof(*jit_dynimm) * JIT_DYNIMM + 8192 + sizeof(JitBlock *) * JIT_GRAVE + JIT_SRC_BYTES;
	// (all of the tables jit_init makes: the room left after the cache is what Wi-Fi's buffers, uploads and the disks
	// need, and when PSRAM runs out they take internal RAM, which the card's DMA needs. Leaving out jit_src and
	// jit_grave, the room was 0.5 MB less: the web page's card reads failed for want of a DMA buffer.)
	const size_t per = JIT_CACHE_PER_BLOCK + sizeof(JitBlock) + 2 * sizeof(JitLink);
	size_t n = room > tables ? (room - tables) / per * JIT_CACHE_PER_BLOCK & ~(size_t) 0xffff : 0;
	if (n < T386_JIT_CACHE_SIZE) n = T386_JIT_CACHE_SIZE;
	if (n > T386_JIT_CACHE_MAX) n = T386_JIT_CACHE_MAX;
	return n;
#else
	(void) map_made;
	return T386_JIT_CACHE_SIZE;
#endif
}

static bool jit_init(CPUI386 *cpu)
{
	if (jit_ready) return true;
#ifdef T386_TRACE
	jit_trace_boot();
#endif
	jit_cpu = cpu;
	jit_pages = cpu->phys_mem_size >> 12;
	// the pagemap first (the platform's: the room left for the rest is measured after it)
	uword map_n = 1u << 20;   // (its bytes to set)
	u8 *map = NULL;
#ifdef T386_JIT_PAGEMAP
	if ((map = T386_JIT_PAGEMAP(PM_IO)))
		map_n = JIT_MAP_OWN;
#endif
	jit_cache_size = jit_cache_bytes(map != NULL);
	if (!map) map = (u8 *) t386_jit_alloc(1 << 20);
	jit_max_blocks = jit_cache_size / JIT_CACHE_PER_BLOCK > JIT_MIN_BLOCKS ? jit_cache_size / JIT_CACHE_PER_BLOCK
									       : JIT_MIN_BLOCKS;
	jit_blocks = (JitBlock *) t386_jit_alloc(sizeof(JitBlock) * jit_max_blocks);
	jit_hash = (JitBlock **) t386_jit_alloc(sizeof(JitBlock *) * JIT_HASH);
	jit_links = (JitLink *) t386_jit_alloc(sizeof(JitLink) * jit_max_blocks * 2);
	jit_page_blocks = (JitBlock **) t386_jit_alloc(sizeof(JitBlock *) * jit_pages);
	jit_buckets = (JitBlock **) t386_jit_alloc(sizeof(JitBlock *) * (jit_pages << (12 - JIT_BUCKET_SHIFT)));
	jit_codelines = (u8 *) t386_jit_alloc(jit_pages * 8);
	jit_smc = (struct JitSmc *) t386_jit_alloc(sizeof(*jit_smc) * JIT_SMC);
	jit_dynimm = (struct JitDynimm *) t386_jit_alloc(sizeof(*jit_dynimm) * JIT_DYNIMM);
	jit_heat = (u8 *) t386_jit_alloc(8192);
	jit_grave = (JitBlock **) t386_jit_alloc(sizeof(JitBlock *) * JIT_GRAVE);
	jit_src = (u8 *) t386_jit_alloc(JIT_SRC_BYTES);   // (without them, no block is revived)
	// the cache last, in what the tables left (smaller if no piece of memory is that big)
	while (!(jit_cache = (u32 *) t386_jit_alloc(jit_cache_size)) && jit_cache_size > T386_JIT_CACHE_SIZE)
		jit_cache_size -= 1 << 18;
	if (!jit_cache || !jit_blocks || !jit_hash || !jit_links || !jit_page_blocks || !jit_buckets || !map || !jit_codelines ||
	    !jit_smc || !jit_dynimm || !jit_heat) {
		fprintf(stderr, "jit: no memory, not used\n");
		t386_jit_enabled = false;
		return false;
	}
	jit_cache_end = jit_cache + jit_cache_size / 4;
	// not RAM: beyond it, and the VGA window (tiny386 keeps its own data in that part of the RAM array)
	for (uword p = 0; p < map_n; p++)
		map[p] = p >= jit_pages || (p >= 0xa0 && p < 0xc0) ? PM_IO : PM_RAM;
	for (uword p = 0; p + 1 < (1u << 20) && p < map_n; p++)   // the RAM page before VGA memory, and the last one
		if (map[p] == PM_RAM && map[p + 1] == PM_IO) map[p] = PM_SLOW;
	t386_pagemap = map;
	jp = jit_cache;
	jlimit = jit_cache_end;
	joverflow = false;
	memset(jit_dynimm, 0xff, sizeof(*jit_dynimm) * JIT_DYNIMM);
	jit_make_routines();
	jit_rt_save();
	jit_cache_base = jp;
	t386_jit_sync(jit_cache, (u8 *) jp - (u8 *) jit_cache);
	jit_flush();
	ftlb_on_flush = jit_ijc_unmap;
	jit_ready = true;
	return true;
}

// (phys: where lin is in RAM now; with paging the same linear address can hold other code)
static long jit_stat_finds, jit_stat_found, jit_stat_probes;   // lookups, those that found a block, blocks looked at
static JitBlock *jit_find(uword lin, uword cs_base, u8 mode, uword phys)
{
	jit_stat_finds++;
	JitBlock **head = &jit_hash[jit_hash_of(lin)];
	for (JitBlock **pp = head, *b; (b = *pp); pp = &b->hnext) {
		jit_stat_probes++;
		if (b->lin == lin && b->cs_base == cs_base && b->mode == mode && b->phys == phys && !b->dead) {
			jit_stat_found++;
			if (pp != head) {   // to the front: found first next time
				*pp = b->hnext;
				b->hnext = *head;
				*head = b;
			}
			return b;
		}
	}
	return NULL;
}

// Patch an exit (two instructions at slot) to jump to target.
// A linkable exit (two words: jal ra to jit_t_lexit, the x86 address) now a jump to target: a jal if it is near
// enough (the address stays), else auipc / jalr in both words
static void jit_patch_jump(u32 *slot, const u32 *target)
{
	int off = (int) ((const u8 *) target - (const u8 *) slot);
	if (off >= -(1 << 20) && off < (1 << 20)) {
		slot[0] = rv_jal(off, X0);
	} else {
		s32 lo = (s32) ((u32) off << 20) >> 20;
		slot[0] = ((u32) off - (u32) lo) | (u32) T6 << 7 | 0x17;   // auipc t6, hi
		slot[1] = RV_I(lo, T6, 0, X0, 0x67);                       // jalr x0, lo(t6)
	}
	t386_jit_sync(slot, 8);
	JIT_SUM_OF(slot);
}

// With paging, a direct jump into another page's block goes through that block's xentry, made here when it is first
// linked so: it goes on into the block if no TLB flush came since the dispatcher last found the block for its linear
// address (its physical page as then: an OS changes page tables, then empties the TLB) and the TLB is the one for the
// same privilege level; else it leaves for the dispatcher (EXIT_JUMP), which finds the block anew (and brings the
// two words up to date) or another one. The jump cache's entries hold as long, by the same reasoning (jit_ijc_unmap).
// (Before, these jumps all went through the dispatcher: some 30 thousand a second while Linux's console prints.)
// On by default. They were off for a while: Linux crashed the translator under qemu-riscv32 with them on, and the
// firmware stopped twice on the Tab5. But qemu crashed with them off too, and the stops came from the code sync
// (t386_i386.c, since fixed); since then Linux ran 12 boots with a mixed load with them on, no failure. Serial
// "jit xlink 0" turns them off.
#ifndef T386_JIT_XLINK
#define T386_JIT_XLINK 3
#endif
int t386_jit_xlink = T386_JIT_XLINK;   // (serial "jit xlink N") 1: links into other pages' blocks (jit_xentry); 2: blocks across a
                          // page's end (jit_crossing_ok)
static u32 *jit_xentry(JitBlock *b)
{
	if (!(t386_jit_xlink & 1)) return NULL;
#ifdef T386_TRACE
	return NULL;   // (the code sums take what follows a block for its own: jit_sums_check)
#endif
	if (b->xentry) return b->xentry;
	if (jit_cache_end - jit_cache_free < 4096 + (56 << 10) / 4 + 32) return NULL;   // (what jit_translate leaves free)
	u32 *x = jit_cache_free;
	jp = x;
	jlimit = x + 32;
	joverflow = false;
	b->xepoch = ftlb_flushes;
	b->xmap = (u32) (uintptr_t) ftlb;
	e_li(T6, (u32) (uintptr_t) &b->xepoch);
	e_lw(A0, T6, 0);
	e_lw(A1, T6, 4);
	u32 *other_tlb = e_branch(BNE, A1, R_MAP);
	e_li(A1, (u32) (uintptr_t) &ftlb_flushes);
	e_lw(A1, A1, 0);
	u32 *flushed = e_branch(BNE, A0, A1);
	{
		int off = (int) ((const u8 *) b->code - (const u8 *) jp);
		s32 lo = (s32) ((u32) off << 20) >> 20;
		e_auipc(T6, (u32) off - (u32) lo);
		e_jalr(X0, T6, lo);
	}
	j_bind(other_tlb, jp);
	j_bind(flushed, jp);
	e_li(A1, b->lin - b->cs_base);   // (the eip)
	e_li(A0, EXIT_JUMP);
	{
		u32 t = (u32) (uintptr_t) t386_jit_exit;
		s32 lo = (s32) (t << 20) >> 20;
		e_lui(T6, t - (u32) lo);
		e_jalr(X0, T6, lo);
	}
	if (joverflow) return NULL;
	jit_cache_free = jp;
	t386_jit_sync(x, (u8 *) jp - (u8 *) x);
	return b->xentry = x;
}

// A write over many blocks (a program loaded over another's code): their pages' lists and lines once, at its end,
// not once a block (each a walk of the page's list: a game's or FreeDOS's installer's pages hold hundreds)
#define JIT_BATCH_PAGES 16
static bool jit_batch;
static uword jit_batch_pages[JIT_BATCH_PAGES];
static int jit_batch_n;
static bool jit_batch_page(uword p)   // is page p (now) one whose dead blocks go at the write's end?
{
	if (!jit_batch) return false;
	for (int i = 0; i < jit_batch_n; i++)
		if (jit_batch_pages[i] == p) return true;
	if (jit_batch_n == JIT_BATCH_PAGES) return false;
	jit_batch_pages[jit_batch_n++] = p;
	return true;
}
static void jit_batch_end(void)
{
	for (int i = 0; i < jit_batch_n; i++) {
		const uword p = jit_batch_pages[i];
		for (JitBlock **q = &jit_page_blocks[p]; *q;) {
			const int k = (*q)->phys >> 12 == p ? 0 : 1;
			if ((*q)->dead) *q = (*q)->pnext[k]; else q = &(*q)->pnext[k];
		}
		if (!jit_page_blocks[p]) t386_pagemap[p] &= ~PM_CODE;
		jit_page_lines(p);
	}
	jit_batch = false;
	jit_batch_n = 0;
}

// Every jump into b back to its exit (a jal to jit_t_lexit, the x86 address), and its link for another
static void jit_unlink_into(JitBlock *b)
{
	for (JitLink *l = b->links, *next; l; l = next) {   // back to jit_t_lexit, and the link for another
		next = l->next;
		l->slot[0] = rv_jal((int) ((u8 *) l->rt - (u8 *) l->slot), RA);
		l->slot[1] = b->lin - b->cs_base;   // (its x86 address: overwritten by a far jump)
		t386_jit_sync(l->slot, 8);
		JIT_SUM_OF(l->slot);
		l->next = jit_link_free;
		jit_link_free = l;
	}
	b->links = NULL;
}

// Unlink every jump into b (they go back to their stubs) and take it out of the table and the page lists.
static void jit_invalidate(JitBlock *b)
{
	if (b->dead) return;
	b->dead = 1;
	jit_stat_invalidated++;
	{
		unsigned i = jit_inv_n++ & 31;
		jit_inv_log[i].lin = b->lin;
		jit_inv_log[i].nbytes = b->nbytes;
		jit_inv_log[i].waddr = jit_waddr;
		jit_inv_log[i].wlen = jit_wlen;
		jit_inv_log[i].cs = jit_cpu->seg[SEG_CS].sel;
		jit_inv_log[i].eip = jit_cpu->next_ip;
	}
	JIT_TR(TR_INVAL, b->lin, jit_waddr, jit_wlen << 16 | b->nbytes);
	{   // self-modifying?
		u32 now = jit_cpu->cycle;
		unsigned h = (b->phys ^ (b->phys >> 8)) & (JIT_SMC - 1);
		if (jit_smc[h].lin == b->phys && now - jit_smc[h].last < JIT_DYN_SOON && now - b->born < JIT_DYN_SOON &&
		    jit_wlen && jit_wlen <= 4) {   // (a write did it: jit_waddr; see jit_dynimm)
			unsigned d = jit_dynimm_hash(jit_waddr);
			jit_dynimm[d].phys = jit_waddr;
			jit_dynimm[d].len = jit_wlen;
		}
		if (jit_smc[h].lin == b->phys && now - jit_smc[h].last < JIT_SMC_SOON && now - b->born < JIT_SMC_SOON) {
			jit_smc[h].count++;
		} else {
			jit_smc[h].lin = b->phys;
			jit_smc[h].count = 1;
		}
		jit_smc[h].last = now;
	}
	if (b->src && !(b->mode & JM_PAGED) && !b->crossing) {   // (jit_revive)
		// its entry jumps out now, and the links into it stay for when it lives again (if another block takes
		// its place, they go: EXIT_DEAD). One cache sync instead of one for each link, and each again to relink.
		b->entry[0] = b->code[0];
		b->entry[1] = b->code[1];
		u32 t = (u32) (uintptr_t) t386_jit_dead_exit;
		s32 lo = (s32) (t << 20) >> 20;
		b->code[0] = (t - (u32) lo) | (u32) RA << 7 | 0x37;   // lui ra
		b->code[1] = RV_I(lo, RA, 0, RA, 0x67);               // jalr ra, lo(ra)
		t386_jit_sync(b->code, 8);
		JIT_SUM_OF(b->code);
		b->guarded = 1;
	} else {
		jit_unlink_into(b);
	}
	struct JitIjc *ij = &jit_ijc[2 * jit_ijc_hash(b->lin)];
	for (int k = 0; k < 2; k++)
		if (ij[k].code == b->code) ij[k].lin = -1;
	JitBlock **pp = &jit_hash[jit_hash_of(b->lin)];
	while (*pp && *pp != b) pp = &(*pp)->hnext;
	if (*pp) *pp = b->hnext;
	if (b->phys < (uword) jit_pages << 12) {
		pp = &jit_buckets[b->phys >> JIT_BUCKET_SHIFT];
		while (*pp && *pp != b) pp = &(*pp)->bnext;
		if (*pp) *pp = b->bnext;
	}
	uword p0 = b->phys >> 12, p1 = (b->phys + b->nbytes - 1) >> 12;
	if (!jit_batch_page(p0) || (p1 != p0 && !jit_batch_page(p1))) {   // (else at the write's end: jit_batch_end)
		for (int k = 0; k < 2; k++) {
			uword p = k ? p1 : p0;
			if (k && p1 == p0) break;
			JitBlock **q = &jit_page_blocks[p];
			while (*q && *q != b) q = &(*q)->pnext[(*q)->phys >> 12 == p ? 0 : 1];
			if (*q) *q = b->pnext[k];
			if (!jit_page_blocks[p]) t386_pagemap[p] &= ~PM_CODE;
			jit_page_lines(p);
		}
	}
	if (b->src) {   // (jit_revive)
		JitBlock **g = &jit_grave[jit_grave_hash(b->phys)];
		b->gnext = *g;
		*g = b;
	}
}

// A write of len bytes at physical address addr, to a page with translated code: the blocks it overlaps go.
void t386_code_write(uword addr, int len)
{
	if (!jit_ready || len <= 0 || addr >= (uword) jit_cpu->phys_mem_size || !jit_lines_code(addr, len)) return;
	jit_waddr = addr;
	jit_wlen = len;
	struct JitSafew *sw = &jit_safew[(addr ^ (addr >> 6)) & (JIT_SAFEW - 1)];
	if (sw->addr == addr && sw->len == len && sw->added == jit_added) { jit_wlen = 0; return; }
	uword end = addr + len;
	bool hit = false;
	jit_batch = len > 64;
	for (uword k = jit_first_bucket(addr); k <= jit_last_bucket(end); k++) {
		JitBlock *b = jit_buckets[k];
		while (b) {
			JitBlock *next = b->bnext;   // (invalidating takes b, and only b, out of the bucket)
			if (b->phys < end && b->phys + b->nbytes > addr && !jit_dyn_covers(b, addr, len)) {
				jit_invalidate(b);
				hit = true;
			}
			b = next;
		}
	}
	if (jit_batch) jit_batch_end();
	if (!hit) { sw->addr = addr; sw->len = len; sw->added = jit_added; }
	jit_wlen = 0;   // (invalidations from now on aren't this write's)
}

// ISA DMA (t386_i8257.c) and anything else outside the CPU writing guest RAM
void t386_ram_written(uword addr, int len)
{
	if (!t386_pagemap || len <= 0 || addr >= (uword) jit_cpu->phys_mem_size) return;
	uword last = addr + (uword) len - 1;
	if (last < addr || last >= (uword) jit_cpu->phys_mem_size) last = jit_cpu->phys_mem_size - 1;
	for (uword p = addr >> 12; p <= last >> 12; p++)   // every page it covers
		if (t386_pagemap[p] & (PM_CODE | PM_CODE_NEXT)) {
			t386_code_write(addr, last - addr + 1);
			return;
		}
}

// A new block's pages now hold code: their writes go through t386_code_write. The fast path's TLB may already
// let it write there: those entries go.
static void jit_mark_code_page(uword p, u8 how)
{
	if (p >= jit_pages) return;
	if (t386_pagemap[p] & how) return;   // (no new entries could have been made for it since)
	t386_pagemap[p] |= how;
	if (jit_cpu->cr0 & CR0_PG) {   // any entry may map the page: those that do, by their host address
		const uintptr_t host = (uintptr_t) jit_cpu->phys_mem + (p << 12);
		for (int k = 0; k < 2; k++)
			for (int i = 0; i < FTLB_N; i++) {
				struct ftlb_entry *e = &ftlb_both[k][i];
				if (e->wtag != (uword) -1 && (uintptr_t) (e->wtag << 12) + e->delta == host) e->wtag = -1;
			}
	} else {
		struct ftlb_entry *e = &ftlb[p & (FTLB_N - 1)];
		if (e->wtag == p) e->wtag = -1;
	}
}

static void jit_add_block(JitBlock *b)
{
	unsigned h = jit_hash_of(b->lin);
	b->hnext = jit_hash[h];
	jit_hash[h] = b;
	jit_added++;   // (jit_safew)
	b->bnext = jit_buckets[b->phys >> JIT_BUCKET_SHIFT];   // (writes are to physical addresses)
	jit_buckets[b->phys >> JIT_BUCKET_SHIFT] = b;
	uword p0 = b->phys >> 12, p1 = (b->phys + b->nbytes - 1) >> 12;
	b->pnext[0] = jit_page_blocks[p0];
	jit_page_blocks[p0] = b;
	jit_mark_code_page(p0, PM_CODE);
	if (p1 != p0) {
		b->pnext[1] = jit_page_blocks[p1];
		jit_page_blocks[p1] = b;
		jit_mark_code_page(p1, PM_CODE);
	}
	if ((b->phys & 0xfff) < 3 && p0)   // a write crossing into the block from the page before
		jit_mark_code_page(p0 - 1, PM_CODE_NEXT);
	jit_set_lines(b->phys, b->nbytes);
}

// ---------------------------------------------------------------------------------------------------------------
// decoding

typedef struct {
	uword eip;
	u8 len;
	u8 op;                  // the opcode byte (the second one after 0F)
	u8 two;                 // 0F xx
	u8 opsz16, adsz16, rep;
	s8 seg;                 // segment override, or -1
	u8 has_modrm, mod, reg, rm;
	u8 ends;                // the block ends with it
	u8 may_exit;            // it may leave the block before it runs (its memory accesses: not plain RAM, a fault)
	// a memory operand (mod != 3): base + (index << scale) + disp, default segment dseg; -1: none
	s8 base, index;
	u8 scale;
	s8 dseg;
	u32 disp;
	u32 imm, imm2;          // immediates (imm2: ENTER's second, a far pointer's selector)
	u8 imm_at, imm_sz;      // where the (first) immediate is in the instruction, its size (0: none)
	u8 imm_dyn;             // ... is rewritten by the program (jit_dynimm): read where it runs, not translated in
	u8 op_dyn;              // a short Jcc whose opcode the program rewrites (jit_dynimm): read where it runs
	u8 disp_at, disp_dyn;   // where a 32-bit displacement is (0: none), and the same for it (span drawers)
	u8 kind;                // K_*: how it is translated
	u8 fl;                  // FL_*: what it does with the flags
	u8 fl_live;             // its flags are read after it (pass 2)
} JIns;

// what an instruction does with the flags
enum {
	FL_NONE,      // nothing
	FL_WRITE,     // sets all of tiny386's lazy state, reading none of it
	FL_READ,      // reads (Jcc, SETcc, ...) or reads and changes (INC/DEC: CF; interpreted instructions: anything)
};

// opcode properties, one-byte opcodes: bit 0 modrm, bits 1-3 immediate: 1 imm8, 2 imm16/32 (operand size),
// 3 imm16, 4 imm16 + imm8, 5 far pointer, 6 offset (address size), 7 a near jump's rel16/32 (operand size)
#define P_M 1
#define P_I8 (1 << 1)
#define P_IV (2 << 1)
#define P_I16 (3 << 1)
#define P_I16I8 (4 << 1)
#define P_FAR (5 << 1)
#define P_MOFFS (6 << 1)
#define P_JV (7 << 1)
#define P_BAD 0x80
static const u8 jit_op1[256] = {
	/* 00 */ P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0, P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0,
	/* 10 */ P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0, P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0,
	/* 20 */ P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0, P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0,
	/* 30 */ P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0, P_M, P_M, P_M, P_M, P_I8, P_IV, 0, 0,
	/* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* 60 */ 0, 0, P_M, P_M, 0, 0, 0, 0, P_IV, P_M | P_IV, P_I8, P_M | P_I8, 0, 0, 0, 0,
	/* 70 */ P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8,
	/* 80 */ P_M | P_I8, P_M | P_IV, P_M | P_I8, P_M | P_I8, P_M, P_M, P_M, P_M, P_M, P_M, P_M, P_M, P_M, P_M, P_M, P_M,
	/* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, P_FAR, 0, 0, 0, 0, 0,
	/* a0 */ P_MOFFS, P_MOFFS, P_MOFFS, P_MOFFS, 0, 0, 0, 0, P_I8, P_IV, 0, 0, 0, 0, 0, 0,
	/* b0 */ P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_IV, P_IV, P_IV, P_IV, P_IV, P_IV, P_IV, P_IV,
	/* c0 */ P_M | P_I8, P_M | P_I8, P_I16, 0, P_M, P_M, P_M | P_I8, P_M | P_IV, P_I16I8, 0, P_I16, 0, 0, P_I8, 0, 0,
	/* d0 */ P_M, P_M, P_M, P_M, P_I8, P_I8, 0, 0, P_M, P_M, P_M, P_M, P_M, P_M, P_M, P_M,
	/* e0 */ P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_I8, P_IV, P_JV, P_FAR, P_I8, 0, 0, 0, 0,
	/* f0 */ 0, 0, 0, 0, 0, 0, P_M, P_M, 0, 0, 0, 0, 0, 0, P_M, P_M,
};

// two-byte opcodes the decoder knows (the rest end a block: tiny386 decodes them)
static u8 jit_op2(u8 op)
{
	if (op >= 0x80 && op <= 0x8f) return P_JV;
	if (op >= 0x90 && op <= 0x9f) return P_M;
	switch (op) {
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x20: case 0x21: case 0x22: case 0x23:
	case 0xa3: case 0xa5: case 0xab: case 0xad: case 0xaf: case 0xb0: case 0xb1: case 0xb2: case 0xb3:
	case 0xb4: case 0xb5: case 0xb6: case 0xb7: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
	case 0xc0: case 0xc1:
		return P_M;
	case 0xa4: case 0xac: case 0xba:
		return P_M | P_I8;
	case 0x06: case 0x08: case 0x09: case 0x31: case 0xa0: case 0xa1: case 0xa2: case 0xa8: case 0xa9:
	case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
		return 0;
	}
	return P_BAD;
}

// Decode the instruction at p (host address of guest code; at most max bytes readable), as tiny386 would.
// False if it can't be decoded here.
static bool jit_decode(const u8 *p, int max, bool code16, JIns *in)
{
	memset(in, 0, sizeof(*in));
	in->seg = -1;
	in->base = in->index = in->dseg = -1;
	in->opsz16 = in->adsz16 = code16;
	int n = 0;
	u8 b;
	for (;;) {
		if (n >= max || n >= 15) return false;
		b = p[n++];
		switch (b) {
		case 0x26: in->seg = SEG_ES; continue;
		case 0x2e: in->seg = SEG_CS; continue;
		case 0x36: in->seg = SEG_SS; continue;
		case 0x3e: in->seg = SEG_DS; continue;
		case 0x64: in->seg = SEG_FS; continue;
		case 0x65: in->seg = SEG_GS; continue;
		case 0x66: in->opsz16 = !code16; continue;
		case 0x67: in->adsz16 = !code16; continue;
		case 0xf3: in->rep = 1; continue;
		case 0xf2: in->rep = 2; continue;
		case 0xf0: continue;
		}
		break;
	}
	u8 prop;
	if (b == 0x0f) {
		if (n >= max) return false;
		in->two = 1;
		b = p[n++];
		prop = jit_op2(b);
	} else {
		prop = jit_op1[b];
	}
	if (prop & P_BAD) return false;
	in->op = b;
	if (prop & P_M) {
		if (n >= max) return false;
		u8 m = p[n++];
		in->has_modrm = 1;
		in->mod = m >> 6;
		in->reg = (m >> 3) & 7;
		in->rm = m & 7;
		if (in->two && b >= 0x20 && b <= 0x23)   // MOV to/from control and debug registers: the byte only
			in->mod = 3;
		if (in->mod != 3) {
			if (in->adsz16) {
				static const s8 b16[8] = { 3, 3, 5, 5, 6, 7, 5, 3 }, i16[8] = { 6, 7, 6, 7, -1, -1, -1, -1 };
				if (in->mod == 0 && in->rm == 6) {
					if (n + 2 > max) return false;
					in->disp = p[n] | p[n + 1] << 8;
					n += 2;
					in->dseg = SEG_DS;
				} else {
					in->base = b16[in->rm];
					in->index = i16[in->rm];
					in->dseg = (in->rm == 2 || in->rm == 3 || in->rm == 6) ? SEG_SS : SEG_DS;
					if (in->mod == 1) {
						if (n >= max) return false;
						in->disp = (u32) (s8) p[n++];
					} else if (in->mod == 2) {
						if (n + 2 > max) return false;
						in->disp = p[n] | p[n + 1] << 8;
						n += 2;
					}
				}
			} else {
				in->dseg = SEG_DS;
				if (in->rm == 4) {
					if (n >= max) return false;
					u8 sib = p[n++];
					int sb = sib & 7, si = (sib >> 3) & 7;
					if (sb == 5 && in->mod == 0) {
						if (n + 4 > max) return false;
						in->disp_at = n;
						in->disp = p[n] | p[n + 1] << 8 | p[n + 2] << 16 | (u32) p[n + 3] << 24;
						n += 4;
					} else {
						in->base = sb;
						if (sb == 4 || sb == 5) in->dseg = SEG_SS;
					}
					if (si != 4) {
						in->index = si;
						in->scale = sib >> 6;
					}
				} else if (in->rm == 5 && in->mod == 0) {
					if (n + 4 > max) return false;
					in->disp_at = n;
					in->disp = p[n] | p[n + 1] << 8 | p[n + 2] << 16 | (u32) p[n + 3] << 24;
					n += 4;
				} else {
					in->base = in->rm;
					if (in->rm == 5) in->dseg = SEG_SS;
				}
				if (in->mod == 1) {
					if (n >= max) return false;
					in->disp += (u32) (s8) p[n++];
				} else if (in->mod == 2) {
					if (n + 4 > max) return false;
					in->disp_at = n;
					in->disp += p[n] | p[n + 1] << 8 | p[n + 2] << 16 | (u32) p[n + 3] << 24;
					n += 4;
				}
			}
		}
	}
	// immediates; F6/F7 /0 /1 have one besides
	int imm = (prop >> 1) & 7;
	if (!in->two && (b == 0xf6 || b == 0xf7) && in->reg < 2) imm = b == 0xf6 ? 1 : 2;
	int sz = 0, sz2 = 0;
	switch (imm) {
	case 1: sz = 1; break;
	case 2: sz = in->opsz16 ? 2 : 4; break;
	case 3: sz = 2; break;
	case 4: sz = 2; sz2 = 1; break;
	case 5: sz = in->opsz16 ? 2 : 4; sz2 = 2; break;
	case 6: sz = in->adsz16 ? 2 : 4; break;
	case 7: sz = in->opsz16 ? 2 : 4; break;
	}
	if (n + sz + sz2 > max) return false;
	in->imm_at = n;
	in->imm_sz = sz;
	for (int i = 0; i < sz; i++) in->imm |= (u32) p[n + i] << (8 * i);
	n += sz;
	for (int i = 0; i < sz2; i++) in->imm2 |= (u32) p[n + i] << (8 * i);
	n += sz2;
	in->len = n;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// translating

// how an instruction is translated
enum {
	K_INTERP,     // run in place by the fast path
	K_REPSTR,     // REP MOVS, STOS, LODS: plain RAM, forwards, a page at a time (jt_repstr); else as K_INTERP
	K_REPCMP,     // REPE/REPNE CMPS, SCAS: the same way, by jit_h_repcmp (jt_repcmp)
	K_NOP, K_ALU, K_TEST, K_MOV, K_LEA, K_INCDEC, K_PUSH, K_POP, K_XCHG, K_CBW, K_CWD, K_MOVX, K_SHIFT, K_NOTNEG,
	K_SHXD, K_BT, K_IMUL, K_JCC, K_JMP, K_CALL, K_RET, K_LOOP, K_JMPI, K_CALLI, K_SETCC, K_MOVSEG, K_POPSEG, K_LDSEG, K_LEAVE,
	K_CLD, K_STD, K_ADCSBB, K_ROT, K_SHIFTCL, K_MULDIV, K_IN, K_OUT, K_STR, K_FLAGOP, K_PUSHA, K_POPA, K_XLAT,
	K_LAHF, K_SAHF, K_PUSHF, K_POPF, K_PMFAR, K_ROTCL, K_FPU,
	K_RCX,        // RCL, RCR by CL or by an immediate other than 1 (jit_h_rcx)
	K_BCD,        // DAA, DAS, AAA, AAS, AAM (not by 0), AAD (jit_h_bcd)
	K_LARLSL,     // LAR, LSL in protected mode (jit_h_larlsl: tiny386's larsl_helper)
	K_SHXD16,     // 16-bit SHLD, SHRD (jit_h_shxd16: tiny386's SHLD_helper, SHRD_helper)

	K_JMPF, K_CALLF, K_RETF, K_FARIND, K_INT, K_IRET,   // real mode
	K_ENTER, K_BSCAN, K_BSWAP, K_RDTSC, K_FWAIT, K_CMPXCHG, K_XADD,
};
// pending flags (see the top): the kind of the last flag-setting instruction whose result is in t3-t5, a7
enum {
	PF_NONE,      // the flags are in cpu->cc / cpu->flags
	PF_DEAD,      // overwritten before anything reads them: nothing kept
	PF_ADD, PF_SUB, PF_LOGIC, PF_INC, PF_DEC, PF_NEG, PF_SHL, PF_SHR, PF_SAR, PF_IMUL16, PF_IMUL32,
	PF_ADC, PF_SBB,   // F_CF: the carry in (tiny386's op is ADC/SBB with it, ADD/SUB without)
};

#define CPU_OFF(f) ((int) offsetof(CPUI386, f))
typedef __typeof__(((CPUI386 *) 0)->seg[0]) JSeg;
#define SEG_SEL_OFF(s) (CPU_OFF(seg) + (s) * (int) sizeof(JSeg))
#define SEG_BASE_OFF(s) (SEG_SEL_OFF(s) + (int) offsetof(JSeg, base))
#define SEG_LIMIT_OFF(s) (SEG_SEL_OFF(s) + (int) offsetof(JSeg, limit))
#define SEG_FLAGS_OFF(s) (SEG_SEL_OFF(s) + (int) offsetof(JSeg, flags))

typedef struct {
	u8 kind;            // STUB_*
	u8 size, reg;
	u32 *at;            // the branch or jump to it
	u32 *more[5];       // (STUB_EXIT, STUB_CHECK0: more branches to it)
	u32 *resume;        // where to go back to
	uword eip;          // an exit's x86 address
	u32 *slot;          // a linkable exit's two instructions
	u8 pf, pf_stored;   // the pending flags where it was made (an exit writes them out)
	int pf_bits;
	int refund;         // instructions of the block from the one it is made in (charged on entry, not done if it
	                    // leaves the block)
	u8 redo;            // a write's: the instruction can be done again from its start (nothing else changed yet)
	const void *fn;     // STUB_FCALL's helper, and its a1, a2 (-1: as they are)
	s32 a1, a2;
} JStub;
enum { STUB_READ, STUB_WRITE, STUB_EXIT, STUB_LINK, STUB_BUDGET, STUB_STOP, STUB_RMW, STUB_CHECK0, STUB_IRQ, STUB_FCALL };
static uword jit_check_fail_lin = -1;   // a self-checking block whose first instruction changed (STUB_CHECK0)

#define JIT_MAX_INS 48
static JIns jt_ins[JIT_MAX_INS];
#define JIT_MAX_STUBS (JIT_MAX_INS * 4 + 8)
static JStub *jt_stubs;   // [JIT_MAX_STUBS]
static int jt_nstubs;
static bool jt_stubs_full;   // a block needed more: it is translated again, shorter
static int jt_refund;        // for the stubs of the instruction being emitted
static u8 jt_mode;
static uword jt_cs_base;
static uword jt_phys_off;   // the block's physical address - its linear address
static uword jt_lin;   // the block's linear address
static int jit_interp_reason;   // jit_h_interp's verdict when it stops a block
static bool jit_exception;      // ... and an exception is to be delivered
// The segments null now (fast_nullsegs), which blocks check on entry against those they use: a byte of the CPU state,
// in the padding after code16 (tiny386 never writes it), so the check reads it from s0 (no address to build there)
#define JIT_NS_OFF (CPU_OFF(code16) + 1)
_Static_assert(offsetof(CPUI386, sp_mask) >= offsetof(CPUI386, code16) + 2, "a spare byte after code16");
#define JIT_NULLSEGS(cpu) (((u8 *) (cpu))[JIT_NS_OFF])

static int jt_pf, jt_pf_bits;
static bool jt_pf_stored;


static uword jt_cur_eip;   // the instruction being translated (an access's stub leaves at it on a fault)
static bool jt_no_redo;    // ... has moved (E)SP: a later write's stub can't send it to the interpreter to redo

static JStub *jt_stub(int kind)
{
	static JStub spare;   // (when they have run out: the block is thrown away)
	if (jt_nstubs == JIT_MAX_STUBS) jt_stubs_full = true;
	JStub *st = jt_stubs_full ? &spare : &jt_stubs[jt_nstubs++];
	memset(st, 0, sizeof(*st));
	st->kind = kind;
	st->pf = jt_pf;
	st->pf_bits = jt_pf_bits;
	st->pf_stored = jt_pf_stored;
	st->refund = jt_refund;
	st->eip = jt_cur_eip;
	st->redo = !jt_no_redo;
	return st;
}

// leave translated code: reason, next eip
static void jt_exit(int reason, uword eip)
{
	if (jit_t_exit[reason]) {   // a jal to the shared exit for the reason, the x86 address after it
		e_call_near(jit_t_exit[reason]);
		E(eip);
		return;
	}
	e_li(A1, eip);
	e_li(A0, reason);
	u32 t = (u32) (uintptr_t) t386_jit_exit;
	s32 lo = (s32) (t << 20) >> 20;
	e_lui(T6, t - (u32) lo);
	e_jalr(X0, T6, lo);
}

static void jt_ijc_exit(int csreg);

// an exit to another block at eip, linkable: two instructions, jumping to a stub until linked. With paging, one in
// another page goes through the indirect jump cache (links stay within a page). cs_base: CS's base there (a far JMP
// or CALL's new one: with the block's, a far call under a memory manager's paging looked up the target's offset in
// the old code segment, and ran whatever block was there)
static void jt_link_exit_cs(uword eip, uword cs_base)   // (the slot: jal ra to jit_t_lexit, the x86 address)
{
	if ((jt_mode & JM_PAGED) && (cs_base + eip) >> 12 != jt_lin >> 12) {
		e_li(A1, eip);
		if (cs_base == jt_cs_base) {
			jt_ijc_exit(-1);
		} else {
			e_li(A2, cs_base);
			jt_ijc_exit(A2);
		}
		return;
	}
	E(rv_jal((int) ((u8 *) jit_t_lexit - (u8 *) jp), RA));   // (the routines' copy is in reach: jit_translate)
	E(eip);
}

static void jt_link_exit(uword eip) { jt_link_exit_cs(eip, jt_cs_base); }

// ---- registers
static inline int jt_sext(int d, int s, int bits)
{
	if (bits == 32) { e_mv(d, s); return d; }
	e_slli(d, s, 32 - bits);
	e_srai(d, d, 32 - bits);
	return d;
}
static inline void jt_zext(int d, int s, int bits)
{
	if (bits == 32) e_mv(d, s);
	else if (bits == 8) e_andi(d, s, 255);
	else { e_slli(d, s, 16); e_srli(d, d, 16); }
}
// register r's value (bits wide) into d, zero-extended
static void jt_get_r(int d, int r, int bits)
{
	if (bits == 32) e_mv(d, jg[r]);
	else if (bits == 16) jt_zext(d, jg[r], 16);
	else if (r < 4) e_andi(d, jg[r], 255);
	else { e_srli(d, jg[r - 4], 8); e_andi(d, d, 255); }
}
// register r = the low bits of s (zext: s is zero-extended already)
static void jt_set_r(int r, int s, int bits, bool zext)
{
	if (bits == 32) {
		e_mv(jg[r], s);
	} else if (bits == 16) {
		e_lui(T6, 0xffff0000);
		e_and(jg[r], jg[r], T6);
		if (!zext) { e_slli(A6, s, 16); e_srli(A6, A6, 16); s = A6; }
		e_or(jg[r], jg[r], s);
	} else if (r < 4) {
		e_andi(jg[r], jg[r], -256);
		if (!zext) { e_andi(A6, s, 255); s = A6; }
		e_or(jg[r], jg[r], s);
	} else {
		int h = jg[r - 4];
		e_li(T6, 0xffff00ff);
		e_and(h, h, T6);
		e_andi(A6, s, 255);
		e_slli(A6, A6, 8);
		e_or(h, h, A6);
	}
}

// ---- memory
// t0 = the linear address of the instruction's memory operand, + extra (a far pointer's selector: after its offset,
// not wrapped to 16 bits with the address, as tiny386 reads it)
// t0 += segment seg's base; with paging, done by the TLB's lookup that comes next (jt_ptlb: jit_r_tlbs), two words
// fewer in the block
static int jt_seg_pending = -1;
static inline bool jt_based(int seg)   // is seg's base added? (JM_FLAT: not DS's, ES's, SS's)
{
	return !(jt_mode & JM_FLAT) || seg == SEG_CS || seg == SEG_FS || seg == SEG_GS;
}
static void jt_seg_base(int seg)
{
	if (jt_seg_pending >= 0) joverflow = true;   // (one not used: a mistake here; the block is not made)
	if (!jt_based(seg)) return;
	if (jt_mode & JM_PAGED) {
		jt_seg_pending = seg;
		return;
	}
	e_lw(T1, R_CPU, SEG_BASE_OFF(seg));
	e_add(T0, T0, T1);
}
static void jt_ea(const JIns *in, int extra)
{
	u32 disp = in->disp + extra;
	if (in->adsz16) {
		int from = X0;
		disp = in->disp;
		if (in->base >= 0 && in->index >= 0) { e_add(T0, jg[in->base], jg[in->index]); from = T0; }
		else if (in->base >= 0) from = jg[in->base];
		if (from == X0) e_li(T0, disp & 0xffff);   // an address alone: within 64 KB already
		else if (!(disp & 0xffff)) jt_zext(T0, from, 16);
		else {
			if ((s32) disp >= -2048 && (s32) disp < 2048) e_addi(T0, from, disp);
			else { e_li(T1, disp); e_add(T0, from, T1); }
			jt_zext(T0, T0, 16);
		}
		if (extra) e_addi(T0, T0, extra);
	} else {
		int from = -1;   // (T0, or the register that holds the sum so far)
		if (in->index >= 0) {
			if (in->scale) { e_slli(T0, jg[in->index], in->scale); from = T0; }
			else from = jg[in->index];
		}
		if (in->base >= 0) {
			if (from >= 0) { e_add(T0, from, jg[in->base]); from = T0; }
			else from = jg[in->base];
		}
		if (in->disp_dyn) {   // rewritten by the program: read where it is (as jt_imm)
			e_li(T1, jt_cs_base + in->eip + jt_phys_off + in->disp_at);
			e_add(T1, T1, R_MEM);
			e_lw(T1, T1, 0);
			if (extra) e_addi(T1, T1, extra);
			if (from < 0) e_mv(T0, T1);
			else e_add(T0, from, T1);
		}
		else if (from < 0) e_li(T0, disp);
		else if (!disp) e_mv(T0, from);   // (nothing when the sum is in t0 already)
		else if ((s32) disp >= -2048 && (s32) disp < 2048) e_addi(T0, from, disp);
		else { e_li(T1, disp); e_add(T0, from, T1); }
	}
	jt_seg_base(in->seg >= 0 ? in->seg : in->dseg);
}
static void jt_ea_linear(void)   // after jt_ea: t0 the linear address now (not only in the TLB's lookup)
{
	if (jt_seg_pending < 0) return;
	e_lw(T1, R_CPU, SEG_BASE_OFF(jt_seg_pending));
	e_add(T0, T0, T1);
	jt_seg_pending = -1;
}

// t0 = the linear address of the stack slot at (E)SP + d
static void jt_stack_ea(int d)
{
	e_addi(T0, jg[4], d);
	if (jt_mode & JM_STACK16) jt_zext(T0, T0, 16);
	jt_seg_base(SEG_SS);
}

// load bytes from linear address t0 into d (zero-extended). (An access that runs on into the next page is checked
// by its first page alone: the page before MMIO or the end of RAM is PM_SLOW, and so is the one before a page whose
// code starts in its first 3 bytes for writes, PM_CODE_NEXT.)
// With paging (JM_PAGED): t1 = the host address of the access at t0, through the fast path's TLB (R_MAP holds
// its address in such blocks), or to st: a miss, or (bytes > 1) an access running on into the next page. Uses t6
// and a6. (t0 the linear address after it: jit_r_tlbs add the segment's base. Called: in the blocks the lookups
// made the code longer, and slower, for all the call and the return they saved.)
static void jt_ptlb(JStub *st, int write, int bytes)
{
	const int k = bytes == 1 ? 0 : bytes == 2 ? 1 : 2, seg = jt_seg_pending;
	jt_seg_pending = -1;
	e_call_near(seg >= 0 ? jit_r_tlbs[seg][write][k] : jit_r_tlb[write][k]);
	st->at = e_branch(BEQ, T1, X0);
}

static void jt_load(int d, int bytes)
{
	JStub *st = jt_stub(STUB_READ);
	if (jt_mode & JM_PAGED) {
		st->size = bytes;
		st->reg = d;
		jt_ptlb(st, 0, bytes);
		if (bytes == 1) e_lbu(d, T1, 0);
		else if (bytes == 2) e_lhu(d, T1, 0);
		else e_lw(d, T1, 0);
		st->resume = jp;
		return;
	}
	e_srli(T1, T0, 12);
	e_add(T1, T1, R_MAP);
	e_lb(T1, T1, 0);
	st->at = e_branch(BLT, T1, X0);
	st->size = bytes;
	st->reg = d;
	e_add(T1, T0, R_MEM);
	if (bytes == 1) e_lbu(d, T1, 0);
	else if (bytes == 2) e_lhu(d, T1, 0);
	else e_lw(d, T1, 0);
	st->resume = jp;
}

// store bytes of s at linear address t0
static void jt_store(int s, int bytes)
{
	JStub *st = jt_stub(STUB_WRITE);
	if (jt_mode & JM_PAGED) {
		st->size = bytes;
		st->reg = s;
		jt_ptlb(st, 1, bytes);
		if (bytes == 1) e_sb(s, T1, 0);
		else if (bytes == 2) e_sh(s, T1, 0);
		else e_sw(s, T1, 0);
		st->resume = jp;
		return;
	}
	e_srli(T1, T0, 12);
	e_add(T1, T1, R_MAP);
	e_lb(T1, T1, 0);
	st->at = e_branch(BNE, T1, X0);
	st->size = bytes;
	st->reg = s;
	e_add(T1, T0, R_MEM);
	if (bytes == 1) e_sb(s, T1, 0);
	else if (bytes == 2) e_sh(s, T1, 0);
	else e_sw(s, T1, 0);
	st->resume = jp;
}

// read-modify-write: t1 = the host address of the operand at t0, or the instruction goes to the interpreter
// (MMIO, or translated code in those bytes)
static void jt_rmw_addr(const JIns *in, int bytes)
{
	JStub *st = jt_stub(STUB_RMW);
	if (jt_mode & JM_PAGED) {
		st->eip = in->eip;
		st->size = bytes;
		jt_ptlb(st, 1, bytes);
		st->resume = jp;
		return;
	}
	e_srli(T1, T0, 12);
	e_add(T1, T1, R_MAP);
	e_lb(T1, T1, 0);
	st->at = e_branch(BNE, T1, X0);
	st->eip = in->eip;
	st->size = bytes;
	e_add(T1, T0, R_MEM);
	st->resume = jp;
}
static void jt_host_load(int d, int bytes)
{
	if (bytes == 1) e_lbu(d, T1, 0);
	else if (bytes == 2) e_lhu(d, T1, 0);
	else e_lw(d, T1, 0);
}
static void jt_host_store(int s, int bytes)
{
	if (bytes == 1) e_sb(s, T1, 0);
	else if (bytes == 2) e_sh(s, T1, 0);
	else e_sw(s, T1, 0);
}

// ---- flags
// The stores that write the pending flags to cpu->cc (in tiny386's form), inline.
static void jt_flags_store_code(void)
{
	if (jt_pf <= PF_DEAD || jt_pf_stored) return;
	int op = 0, mask = CF | PF | ZF | SF | OF, bits = jt_pf_bits, b3 = bits == 8 ? 0 : bits == 16 ? 1 : 2;
	switch (jt_pf) {
	case PF_ADD: case PF_SUB:
		op = jt_pf == PF_ADD ? CC_ADD : CC_SUB;
		mask |= AF;
		e_sw(F_SRC1, R_CPU, CPU_OFF(cc.src1));
		e_sw(F_SRC2, R_CPU, CPU_OFF(cc.src2));
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		break;
	case PF_ADC: case PF_SBB:   // op: CC_ADD - carry / CC_SUB - carry (CC_ADC, CC_ADD, CC_SBB, CC_SUB)
		e_sw(F_SRC1, R_CPU, CPU_OFF(cc.src1));
		e_sw(F_SRC2, R_CPU, CPU_OFF(cc.src2));
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		e_li(T6, jt_pf == PF_ADC ? CC_ADD : CC_SUB);
		e_sub(T6, T6, F_CF);
		e_sw(T6, R_CPU, CPU_OFF(cc.op));
		e_li(T6, mask | AF);
		e_sw(T6, R_CPU, CPU_OFF(cc.mask));
		jt_pf_stored = true;
		return;
	case PF_LOGIC:
		op = jt_pf_bits >> 8;   // CC_AND/OR/XOR, kept above the width
		bits &= 0xff;
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		break;
	case PF_INC: case PF_DEC:
		op = (jt_pf == PF_INC ? CC_INC8 : CC_DEC8) + b3;
		mask = PF | AF | ZF | SF | OF;
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_andi(T6, T6, ~CF);
		e_or(T6, T6, F_CF);
		e_sw(T6, R_CPU, CPU_OFF(flags));
		break;
	case PF_NEG:
		op = CC_NEG8 + b3;
		mask |= AF;
		e_sw(F_SRC1, R_CPU, CPU_OFF(cc.src1));
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		break;
	case PF_SHL: case PF_SAR:
		op = jt_pf == PF_SHL ? CC_SHL : CC_SAR;
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		e_sw(F_CF, R_CPU, CPU_OFF(cc.dst2));
		break;
	case PF_SHR:
		op = CC_SHR;
		e_sw(F_SRC1, R_CPU, CPU_OFF(cc.src1));
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		e_sw(F_CF, R_CPU, CPU_OFF(cc.dst2));
		break;
	case PF_IMUL16: case PF_IMUL32:
		op = jt_pf == PF_IMUL16 ? CC_IMUL16 : CC_IMUL32;
		mask |= AF;
		e_sw(F_SRC1, R_CPU, CPU_OFF(cc.src1));
		e_sw(F_SRC2, R_CPU, CPU_OFF(cc.src2));
		e_sw(F_DST, R_CPU, CPU_OFF(cc.dst));
		if (jt_pf == PF_IMUL32) e_sw(F_CF, R_CPU, CPU_OFF(cc.dst2));
		break;
	}
	e_li(T6, op);
	e_sw(T6, R_CPU, CPU_OFF(cc.op));
	e_li(T6, mask);
	e_sw(T6, R_CPU, CPU_OFF(cc.mask));
	jt_pf_stored = true;
}

// ... as a call: they are made once per kind and width (jit_fstore), at the start of the code cache
static u32 *jit_fstore[PF_SBB + 1][3][4];

// the routine that writes out the pending flags pf (pf_bits) as a call (NULL: none made for them, inline)
static u32 *jt_fstore_of(int pf, int pf_bits)
{
	int bits = pf_bits & 0xff, w = bits == 8 ? 0 : bits == 16 ? 1 : 2;
	int lop = pf == PF_LOGIC ? (pf_bits >> 8) - CC_AND + 1 : 0;
	return jit_fstore[pf][w][lop];
}

static void jt_flags_store(void)
{
	if (jt_pf <= PF_DEAD || jt_pf_stored) return;
	u32 *f = jt_fstore_of(jt_pf, jt_pf_bits);
	if (f) e_call_near(f);
	else jt_flags_store_code();
	jt_pf_stored = true;
}

static void jit_make_fstores(void)   // (jp: where)
{
	for (int k = PF_ADD; k <= PF_SBB; k++)
		for (int w = 0; w < 3; w++)
			for (int lop = 0; lop < (k == PF_LOGIC ? 4 : 1); lop++) {
				if (k == PF_LOGIC && !lop) continue;
				jit_fstore[k][w][lop] = jp;
				jt_pf = k;
				jt_pf_bits = (8 << w) | (k == PF_LOGIC ? (CC_AND + lop - 1) << 8 : 0);
				jt_pf_stored = false;
				jt_flags_store_code();
				e_jalr(X0, RA, 0);
			}
}

// Writes that aren't to plain RAM (the STUB_WRITE stubs) come here first: video memory in the VGA's simple write modes
// (t386_vga_fw.h: column-drawn walls, EGA tiles, mode 13h) is written in place, without going through
// t386_jit_wr1/2/4, tiny386's callbacks and vga.c; anything else goes on to them. Made at the start of the code cache,
// once per size: t0 the linear address, t6 the value; every other register but ra and t6 kept, as there.
#include "t386_vga_fw.h"
static u32 *jit_vga_wr[2][3];   // [redo: t386_jit_wrr*, jit_h_write][size]

static void jit_make_vga_writes(void)
{
	static void (*const slows[2][3])(void) = { { t386_jit_wr1, t386_jit_wr2, t386_jit_wr4 },
						    { t386_jit_wrr1, t386_jit_wrr2, t386_jit_wrr4 } };
	const u32 fw = (u32) (uintptr_t) &t386_vga_fw;
	for (int r = 0; r < 2; r++)
	for (int w = 0; w < 3; w++) {
		void (*const *slow)(void) = slows[r];
		const int n = 1 << w;
		jit_vga_wr[r][w] = jp;
		e_addi(SP, SP, -32);
		e_sw(T1, SP, 24);
		e_sw(T2, SP, 20);
		e_sw(T3, SP, 16);
		e_lui(T1, 0xa0000);
		e_sub(T1, T0, T1);                          // t1: the offset in the window at A0000
		e_li(T2, 0x10000 - n);
		u32 *out1 = e_branch(BLTU, T2, T1);         // (not all of it in A0000-AFFFF)
		e_li(T2, fw);
		e_lw(T3, T2, offsetof(struct T386VgaFw, kind));
		e_addi(T3, T3, -T386_FW_PLANE1);
		u32 *plane1 = e_branch(BEQ, T3, X0);
		e_addi(T3, T3, T386_FW_PLANE1 - T386_FW_LINEAR);
		u32 *other = e_branch(BNE, T3, X0);
		e_lw(T3, T2, offsetof(struct T386VgaFw, ram));   // mode 13h, all planes: the bytes as they come
		e_add(T1, T1, T3);
		for (int i = 0; i < n; i++) {
			e_sb(T6, T1, i);
			if (i < n - 1) e_srli(T6, T6, 8);
		}
		u32 *done0 = e_jump();
		j_bind(plane1, jp);
		e_lw(T3, T2, offsetof(struct T386VgaFw, plane1));   // one plane (mode Y): the bytes to it, no more saved
		e_slli(T1, T1, 2);
		e_add(T1, T1, T3);
		for (int i = 0; i < n; i++) {
			e_sb(T6, T1, 4 * i);
			if (i < n - 1) e_srli(T6, T6, 8);
		}
		j_bind(done0, jp);
		u32 *restore = jp;   // (t1-t3 back, t6 0: no code overwritten)
		e_lw(T1, SP, 24);
		e_lw(T2, SP, 20);
		e_lw(T3, SP, 16);
		e_addi(SP, SP, 32);
		e_mv(T6, X0);                               // (no code overwritten)
		e_jalr(X0, RA, 0);
		j_bind(out1, jp);
		// RAM with translated code in its page, but not in the 64-byte lines written (jit_codelines; DOS's data
		// beside its code), and not into the next page: written here (else jit_h_write, t386_code_write)
		u32 *slowpath[4];
		int ns = 0;
		e_srli(T1, T0, 12);
		e_add(T1, T1, R_MAP);
		e_lbu(T1, T1, 0);
		e_andi(T1, T1, ~(PM_CODE | PM_CODE_NEXT) & 0xff);
		slowpath[ns++] = e_branch(BNE, T1, X0);
		if (n > 1) {
			e_addi(T1, T0, n - 1);
			e_xor(T1, T1, T0);
			e_srli(T1, T1, 12);
			slowpath[ns++] = e_branch(BNE, T1, X0);
		}
		e_li(T3, (u32) (uintptr_t) jit_codelines);
		for (int k = 0; k < (n > 1 ? 2 : 1); k++) {   // the first byte's line, and the last's
			e_addi(T1, T0, k ? n - 1 : 0);
			e_srli(T1, T1, 6);
			e_srli(T2, T1, 3);
			e_add(T2, T2, T3);
			e_lbu(T2, T2, 0);
			e_andi(T1, T1, 7);
			e_srl(T2, T2, T1);
			e_andi(T2, T2, 1);
			slowpath[ns++] = e_branch(BNE, T2, X0);
		}
		e_add(T1, T0, R_MEM);
		if (n == 1) e_sb(T6, T1, 0); else if (n == 2) e_sh(T6, T1, 0); else e_sw(T6, T1, 0);
		j_bind(e_jump(), restore);
		for (int k = 0; k < ns; k++) j_bind(slowpath[k], jp);
		e_sw(RA, SP, 28);                           // the others: ra, t4 and t5 saved too
		e_sw(T4, SP, 12);
		e_sw(T5, SP, 8);
		u32 *out1b = e_jump();
		j_bind(other, jp);
		e_lw(T3, T2, offsetof(struct T386VgaFw, kind));
		e_sw(RA, SP, 28);
		e_sw(T4, SP, 12);
		e_sw(T5, SP, 8);
		u32 *out2 = e_branch(BEQ, T3, X0);
		e_lw(T4, T2, offsetof(struct T386VgaFw, ram));
		e_li(T5, T386_FW_CHAIN4);
		u32 *chain4 = e_branch(BEQ, T3, T5);
		e_slli(T1, T1, 2);
		e_add(T4, T4, T1);                          // t4: the four planes' bytes at that offset, a word
		e_lw(T5, T2, offsetof(struct T386VgaFw, wm));
		e_li(T1, T386_FW_LATCH);
		u32 *latch = e_branch(BEQ, T3, T1);
		e_li(T1, T386_FW_WM0);
		u32 *wmodes = e_branch(BGEU, T3, T1);       // (T386_FW_WM0, 2, 3: the last kinds)
		for (int i = 0; i < n; i++) {               // planar: the byte to the planes in wm
			e_andi(T1, T6, 0xff);
			e_slli(T2, T1, 8);
			e_or(T1, T1, T2);
			e_slli(T2, T1, 16);
			e_or(T1, T1, T2);
			e_lw(T3, T4, 4 * i);
			e_xor(T1, T1, T3);
			e_and(T1, T1, T5);
			e_xor(T3, T3, T1);
			e_sw(T3, T4, 4 * i);
			if (i < n - 1) e_srli(T6, T6, 8);
		}
		u32 *done1 = e_jump();
		j_bind(latch, jp);                          // write mode 1: the latches to the planes in wm
		e_lw(T1, T2, offsetof(struct T386VgaFw, latch));
		e_lw(T1, T1, 0);
		for (int i = 0; i < n; i++) {
			e_lw(T3, T4, 4 * i);
			e_xor(T2, T1, T3);
			e_and(T2, T2, T5);
			e_xor(T3, T3, T2);
			e_sw(T3, T4, 4 * i);
		}
		u32 *done2 = e_jump();
		j_bind(wmodes, jp);   // write modes 0 (all of it), 2, 3, as vga.c's vga_mem_write: t1 the value, t3 the bit mask
		for (int i = 0; i < n; i++) {
#define FWO(f) ((int) offsetof(struct T386VgaFw, f))
			e_andi(T1, T6, 0xff);
			e_li(T2, fw);
			e_lw(T3, T2, FWO(kind));
			e_addi(T3, T3, -T386_FW_WM2);
			u32 *m2 = e_branch(BEQ, T3, X0);
			e_lw(T3, T2, FWO(rot));                 // modes 0 and 3: the byte rotated right
			e_srl(T3, T1, T3);
			e_lw(T2, T2, FWO(rot8));
			e_sll(T1, T1, T2);
			e_or(T1, T1, T3);
			e_li(T2, fw);
			e_lw(T3, T2, FWO(kind));
			e_addi(T3, T3, -T386_FW_WM3);
			u32 *m3 = e_branch(BEQ, T3, X0);
			e_andi(T1, T1, 0xff);                   // mode 0: the byte in each plane, set/reset's where it takes
			e_li(T3, 0x01010101);
			e_mul(T1, T1, T3);
			e_lw(T3, T2, FWO(nsetm));
			e_and(T1, T1, T3);
			e_lw(T3, T2, FWO(srv));
			e_or(T1, T1, T3);
			e_lw(T3, T2, FWO(bm32));
			u32 *got0 = e_jump();
			j_bind(m3, jp);                         // mode 3: set/reset's colour; the mask: the byte & the bit mask
			e_lw(T3, T2, FWO(bm8));
			e_and(T1, T1, T3);
			e_li(T3, 0x01010101);
			e_mul(T3, T1, T3);
			e_lw(T1, T2, FWO(srv));
			u32 *got3 = e_jump();
			j_bind(m2, jp);                         // mode 2: the byte's low 4 bits a colour
			e_andi(T1, T1, 15);
			e_slli(T1, T1, 2);
			e_lw(T3, T2, FWO(m16));
			e_add(T1, T1, T3);
			e_lw(T1, T1, 0);
			e_lw(T3, T2, FWO(bm32));
			j_bind(got0, jp);
			j_bind(got3, jp);
			e_sw(T3, SP, 0);                        // (the bit mask, while t3 holds the latches)
			e_lw(T3, T2, FWO(latch));
			e_lw(T3, T3, 0);
			e_lw(T2, T2, FWO(op));                  // the logical operation with the latches
			u32 *none = e_branch(BEQ, T2, X0);
			e_addi(T2, T2, -1);
			u32 *and_ = e_branch(BEQ, T2, X0);
			e_addi(T2, T2, -1);
			u32 *or_ = e_branch(BEQ, T2, X0);
			e_xor(T1, T1, T3);
			u32 *opd1 = e_jump();
			j_bind(and_, jp);
			e_and(T1, T1, T3);
			u32 *opd2 = e_jump();
			j_bind(or_, jp);
			e_or(T1, T1, T3);
			j_bind(none, jp);
			j_bind(opd1, jp);
			j_bind(opd2, jp);
			e_xor(T1, T1, T3);                      // the masked dots from the value, the others the latches'
			e_lw(T2, SP, 0);
			e_and(T1, T1, T2);
			e_xor(T1, T1, T3);
			e_lw(T3, T4, 4 * i);                    // to the planes in wm
			e_xor(T1, T1, T3);
			e_and(T1, T1, T5);
			e_xor(T3, T3, T1);
			e_sw(T3, T4, 4 * i);
			if (i < n - 1) e_srli(T6, T6, 8);
#undef FWO
		}
		u32 *done3 = e_jump();
		j_bind(chain4, jp);                         // chain 4: each byte if its plane (address & 3) is in pm
		e_lw(T5, T2, offsetof(struct T386VgaFw, pm));
		e_add(T4, T4, T1);
		for (int i = 0; i < n; i++) {
			e_addi(T2, T1, i);
			e_andi(T2, T2, 3);
			e_srl(T2, T5, T2);
			e_andi(T2, T2, 1);
			u32 *skip = e_branch(BEQ, T2, X0);
			e_sb(T6, T4, i);
			j_bind(skip, jp);
			if (i < n - 1) e_srli(T6, T6, 8);
		}
		u32 *out3 = e_jump();
		j_bind(out1b, jp);
		j_bind(out2, jp);
		e_call_abs((const void *) slow[w]);         // (t0 and t6 as they came; t6 its answer)
		u32 *slow_done = e_jump();
		j_bind(done1, jp);
		j_bind(done2, jp);
		j_bind(done3, jp);
		j_bind(out3, jp);
		e_mv(T6, X0);                               // (video memory: no code overwritten)
		j_bind(slow_done, jp);
		e_lw(RA, SP, 28);
		e_lw(T1, SP, 24);
		e_lw(T2, SP, 20);
		e_lw(T3, SP, 16);
		e_lw(T4, SP, 12);
		e_lw(T5, SP, 8);
		e_addi(SP, SP, 32);
		e_jalr(X0, RA, 0);
	}
}

static void jt_set_pf(int kind, int bits)
{
	jt_pf = kind;
	jt_pf_bits = bits;
	jt_pf_stored = false;
}

static int jit_h_cond(CPUI386 *cpu, int c) { return fast_cond(cpu, c); }
static int jit_h_cf(CPUI386 *cpu) { return get_CF(cpu); }

// d = CF of the current flags (0/1)
static void jt_cf(int d)
{
	switch (jt_pf) {
	case PF_ADD: e_sltu(d, F_DST, F_SRC2); break;
	case PF_SUB: e_sltu(d, F_SRC1, F_SRC2); break;
	case PF_ADC:   // dst < src2, or dst == src2 with a carry in
		e_xor(A6, F_DST, F_SRC2);
		e_sltiu(A6, A6, 1);
		e_and(A6, A6, F_CF);
		e_sltu(d, F_DST, F_SRC2);
		e_or(d, d, A6);
		break;
	case PF_SBB:   // src1 < src2, or equal with a borrow in
		e_xor(A6, F_SRC1, F_SRC2);
		e_sltiu(A6, A6, 1);
		e_and(A6, A6, F_CF);
		e_sltu(d, F_SRC1, F_SRC2);
		e_or(d, d, A6);
		break;
	case PF_LOGIC: e_mv(d, X0); break;
	case PF_INC: case PF_DEC: case PF_SHL: case PF_SHR: case PF_SAR: if (d != F_CF) e_mv(d, F_CF); break;
	case PF_NEG: e_sltu(d, X0, F_DST); break;
	case PF_IMUL16:
		jt_sext(A6, F_DST, 16);
		e_xor(d, A6, F_DST);
		e_sltu(d, X0, d);
		break;
	case PF_IMUL32:
		e_srai(A6, F_DST, 31);
		e_xor(d, A6, F_CF);
		e_sltu(d, X0, d);
		break;
	default:   // in cpu->cc: from cpu->flags unless CF is lazy (a shared routine)
		e_call_near(jit_r_cf);
		if (d != T6) e_mv(d, T6);
		break;
	}
}

// d = condition c (Jcc's low nibble) of the current flags, 0/1
static void jt_cond(int c, int d)
{
	int k = jt_pf, bits = jt_pf_bits & 0xff;
	if (k <= PF_DEAD) {   // (a shared routine)
		e_li(A1, c);
		e_call_near(jit_r_cond);
		if (d != A0) e_mv(d, A0);
		return;
	}
	switch (c >> 1) {
	case 0:   // OF
		switch (k) {
		case PF_ADD: case PF_ADC:
			e_xor(A6, F_SRC1, F_SRC2);
			e_xori(A6, A6, -1);
			e_xor(d, F_DST, F_SRC2);
			e_and(d, d, A6);
			e_srli(d, d, 31);
			break;
		case PF_SUB: case PF_SBB:
			e_xor(A6, F_SRC1, F_SRC2);
			e_xor(d, F_DST, F_SRC1);
			e_and(d, d, A6);
			e_srli(d, d, 31);
			break;
		case PF_LOGIC: case PF_SAR: e_mv(d, X0); break;
		case PF_INC: case PF_NEG: case PF_DEC:
			e_li(A6, k == PF_DEC ? (bits == 8 ? 0x7f : bits == 16 ? 0x7fff : 0x7fffffff)
					     : (bits == 8 ? 0xffffff80 : bits == 16 ? 0xffff8000 : 0x80000000));
			e_xor(d, F_DST, A6);
			e_sltiu(d, d, 1);
			break;
		case PF_SHL: e_srli(d, F_DST, 31); e_xor(d, d, F_CF); break;
		case PF_SHR: e_srli(d, F_SRC1, 31); break;
		default: jt_cf(d); break;   // IMUL
		}
		break;
	case 1: jt_cf(d); break;
	case 2: e_sltiu(d, F_DST, 1); break;
	case 3:
		jt_cf(d);
		e_sltiu(A6, F_DST, 1);
		e_or(d, d, A6);
		break;
	case 4: e_slt(d, F_DST, X0); break;
	case 5: {   // PF: parity of the low byte
		u32 t = (u32) (uintptr_t) parity_tab;
		s32 lo = (s32) (t << 20) >> 20;
		e_andi(d, F_DST, 255);
		e_lui(A6, t - (u32) lo);
		e_add(d, d, A6);
		e_lbu(d, d, lo);
		break;
	}
	case 6: case 7:
		if (k == PF_SUB) {
			e_slt(d, F_SRC1, F_SRC2);
		} else {
			jt_cond(0, d);              // OF
			e_slt(A5, F_DST, X0);      // SF
			e_xor(d, d, A5);
		}
		if (c >> 1 == 7) {
			e_sltiu(A6, F_DST, 1);
			e_or(d, d, A6);
		}
		break;
	}
	if (c & 1) e_xori(d, d, 1);
}

// a branch taken when condition c holds (to be bound)
static u32 *jt_branch_cond(int c)
{
	int k = jt_pf;
	if (k == PF_SUB) {   // CMP: compare the operands
		switch (c) {
		case 0x2: return e_branch(BLTU, F_SRC1, F_SRC2);
		case 0x3: return e_branch(BGEU, F_SRC1, F_SRC2);
		case 0x4: return e_branch(BEQ, F_SRC1, F_SRC2);
		case 0x5: return e_branch(BNE, F_SRC1, F_SRC2);
		case 0x6: return e_branch(BGEU, F_SRC2, F_SRC1);
		case 0x7: return e_branch(BLTU, F_SRC2, F_SRC1);
		case 0xc: return e_branch(BLT, F_SRC1, F_SRC2);
		case 0xd: return e_branch(BGE, F_SRC1, F_SRC2);
		case 0xe: return e_branch(BGE, F_SRC2, F_SRC1);
		case 0xf: return e_branch(BLT, F_SRC2, F_SRC1);
		}
	}
	if (k > PF_DEAD) {
		switch (c) {
		case 0x4: return e_branch(BEQ, F_DST, X0);
		case 0x5: return e_branch(BNE, F_DST, X0);
		case 0x8: return e_branch(BLT, F_DST, X0);
		case 0x9: return e_branch(BGE, F_DST, X0);
		}
	}
	if (k == PF_LOGIC) {
		switch (c) {
		case 0x6: return e_branch(BEQ, F_DST, X0);
		case 0x7: return e_branch(BNE, F_DST, X0);
		case 0xc: return e_branch(BLT, F_DST, X0);
		case 0xd: return e_branch(BGE, F_DST, X0);
		case 0xe: return e_branch(BGE, X0, F_DST);
		case 0xf: return e_branch(BLT, X0, F_DST);
		}
	}
	jt_cond(c, T0);
	return e_branch(BNE, T0, X0);
}

// ---- helpers called from translated code
static u32 jit_h_read(uword lin, int size)
{
	CPUI386 *cpu = jit_cpu;
	if (size > 1 && (lin & 0xfff) + size > 0x1000) {   // into the next page (which may be MMIO): a byte at a time
		u32 v = 0;
		for (int i = 0; i < size; i++) v |= jit_h_read(lin + i, 1) << (8 * i);
		return v;
	}
	if (in_iomem(lin) && cpu->cb.iomem_read8) return fast_io_read(cpu, lin, size);
	if (lin + size > (uword) cpu->phys_mem_size) return 0;   // as tiny386
	u8 *p = cpu->phys_mem + lin;
	return size == 1 ? *p : size == 2 ? *(u16a *) p : *(u32a *) p;
}

u32 t386_jit_h_read(uword lin, int size) { return jit_h_read(lin, size); }

// a read-modify-write in a page whose writes are checked: 1 if those bytes are plain RAM all the same
int t386_jit_h_rmw(uword lin, int size)
{
	if (pm_io(lin >> 12) || pm_io((lin + size - 1) >> 12)) return 0;
	if (lin + size > (uword) jit_cpu->phys_mem_size) return 0;
	return !jit_bytes_code(lin, size);
}

static u32 jit_h_write(uword lin, int size, u32 v, bool redo);
u32 t386_jit_h_write(uword lin, int size, u32 v) { return jit_h_write(lin, size & 0xff, v, size >> 8); }
int t386_jit_h_rmw(uword lin, int size);

// A write that would overwrite translated code: the block it came from may be gone (a block can run far past a
// 486's prefetch queue). With redo (nothing else of its instruction done yet), it isn't done, and 1 sends the
// instruction to the interpreter (STUB_WRITE), then the new code runs; without (real mode's far CALL and INT, after
// a push), it is done, and the block goes on.
static u32 jit_h_write(uword lin, int size, u32 v, bool redo)
{
	CPUI386 *cpu = jit_cpu;
	if (size > 1 && (lin & 0xfff) + size > 0x1000) {   // into the next page: a byte at a time
		for (int i = 0; i < size; i++)
			if (jit_h_write(lin + i, 1, v >> (8 * i), redo)) return 1;
		return 0;
	}
	if (in_iomem(lin) && cpu->cb.iomem_write8) { fast_io_write(cpu, lin, size, v); return 0; }
	if (lin + size > (uword) cpu->phys_mem_size) return 0;
	long inv = jit_stat_invalidated;
	t386_code_write(lin, size);   // (if there is code in those lines)
	if (redo && jit_stat_invalidated != inv) return 1;
	u8 *p = cpu->phys_mem + lin;
	if (size == 1) *p = v; else if (size == 2) *(u16a *) p = v; else *(u32a *) p = v;
	return 0;
}

// ... with paging (JM_PAGED blocks, on a miss in the TLB): through the fast path's TLB and tiny386's page tables. An
// access that would fault sets jit_pfault, and the stub leaves the block for the interpreter, which redoes the
// instruction and raises the fault.
static int jit_pfault;

// the physical address of lin for a write, if it can be written (RAM); -1 if not
static uword jit_pwrite_addr(CPUI386 *cpu, uword lin)
{
	uword pa;
	if (!translate_lpgno(cpu, 2, lin >> 12, lin, cpu->cpl, &pa)) return (uword) -1;
	pa = (pa & ~0xfff) | (lin & 0xfff);
	return pa;
}

// A REP string instruction's side whose page isn't in the fast path's TLB (jt_repstr_side, with paging): the page
// tables walked and the TLB filled, as a load's or store's miss does, so that the instruction stays in translated
// code: the host address of lin, or 0 for the interpreter (a fault, not plain RAM, the linear frame buffer, or a
// write into a page with translated code: ftlb_miss takes care of one access there, not of a page's part)
static uintptr_t jit_h_reptlb(uword lin, int write)
{
	CPUI386 *cpu = jit_cpu;
	uword pa;
	if (!translate_lpgno(cpu, write ? 2 : 1, lin >> 12, lin, cpu->cpl, &pa)) return 0;
	pa &= ~0xfff;
	if (in_iomem(pa) || pa >= (uword) cpu->phys_mem_size) return 0;
	if (pa - t386_lfb_base < t386_lfb_size) return 0;
	if (write && (t386_pagemap[pa >> 12] & 3)) return 0;
	return (uintptr_t) ftlb_miss(cpu, lin, 1, write);
}
static uintptr_t jit_h_reptlb_r(uword lin) { return jit_h_reptlb(lin, 0); }
static uintptr_t jit_h_reptlb_w(uword lin) { return jit_h_reptlb(lin, 1); }

static u32 jit_h_pread(uword lin, int size)
{
	CPUI386 *cpu = jit_cpu;
	jit_pfault = 0;
	if (size > 1 && (lin & 0xfff) + size > 0x1000) {   // into the next page: a byte at a time
		u32 v = 0;
		for (int i = 0; i < size && !jit_pfault; i++) v |= jit_h_pread(lin + i, 1) << (8 * i);
		return v;
	}
	u8 *host;
	uword pa;
	int w = fast_where(cpu, lin, size, 0, &host, &pa);
	if (w == 1) return size == 1 ? *host : size == 2 ? *(u16a *) host : *(u32a *) host;
	if (w == 2) return fast_io_read(cpu, pa, size);
	jit_pfault = 1;
	return 0;
}

static u32 jit_h_pwrite(uword lin, int size, u32 v, bool redo)
{
	CPUI386 *cpu = jit_cpu;
	jit_pfault = 0;
	if (size > 1 && (lin & 0xfff) + size > 0x1000) {   // both pages writable first (no half-done store), then bytes
		if (jit_pwrite_addr(cpu, lin) == (uword) -1 || jit_pwrite_addr(cpu, lin + size - 1) == (uword) -1) {
			jit_pfault = 1;
			return 0;
		}
		for (int i = 0; i < size && !jit_pfault; i++) jit_h_pwrite(lin + i, 1, v >> (8 * i), redo);
		return 0;
	}
	u8 *host;
	uword pa;
	long inv = jit_stat_invalidated;   // (overwriting translated code: as jit_h_write)
	int w = fast_where(cpu, lin, size, 1, &host, &pa);
	if (w == 1) {   // (into a page with translated code, the fast path's TLB reports the write: ftlb_miss)
		if (redo && jit_stat_invalidated != inv) { jit_pfault = 1; return 0; }
		if (size == 1) *host = v; else if (size == 2) *(u16a *) host = v; else *(u32a *) host = v;
		return 0;
	}
	if (w == 2) {
		fast_io_write(cpu, pa, size, v);
		return 0;
	}
	// translated code in that page (the TLB doesn't map it for writes), or a fault
	pa = jit_pwrite_addr(cpu, lin);
	if (pa == (uword) -1 || in_iomem(pa) || pa + size > (uword) cpu->phys_mem_size) {
		jit_pfault = 1;
		return 0;
	}
	t386_code_write(pa, size);
	if (redo && jit_stat_invalidated != inv) { jit_pfault = 1; return 0; }
	u8 *p = cpu->phys_mem + pa;
	if (size == 1) *p = v; else if (size == 2) *(u16a *) p = v; else *(u32a *) p = v;
	return 0;
}

// read-modify-write: the host address if those bytes are plain RAM that can be written, else 0 (the interpreter)
static u32 jit_h_prmw(uword lin, int size)
{
	u8 *host;
	uword pa;
	if ((lin & 0xfff) + size > 0x1000) return 0;
	return fast_where(jit_cpu, lin, size, 1, &host, &pa) == 1 ? (u32) (uintptr_t) host : 0;
}
uint64_t t386_jit_h_pread(uword lin, int size) { u32 v = jit_h_pread(lin, size); return v | (uint64_t) jit_pfault << 32; }
// a compact stub's access would fault (or overwrite translated code): its x86 address, the instructions to refund
// and the routine that writes out the pending flags (or 0), from its data word w (jt_stub_compact)
u32 t386_jit_cf_eip, t386_jit_cf_refund, t386_jit_cf_f;
static JitBlock *jit_block_of(const u32 *pc);
void t386_jit_h_cfault(const u32 *w)
{
	const JitBlock *b = jit_block_of(w);
	uword eip = b->lin - b->cs_base + (w[0] >> 19 & 0xfff);
	t386_jit_cf_eip = (b->mode & JM_CODE16) ? eip & 0xffff : eip;
	t386_jit_cf_refund = w[0] >> 13 & 63;
	t386_jit_cf_f = (w[0] >> 31) ? w[1] : 0;
}
uint64_t t386_jit_h_pwrite(uword lin, int size, u32 v)
{
	jit_h_pwrite(lin, size & 0xff, v, size >> 8);
	return (uint64_t) jit_pfault << 32;
}
// (a read-modify-write that would overwrite translated code: the interpreter's, once the code there is gone)
u32 t386_jit_h_prmw(uword lin, int size)
{
	long inv = jit_stat_invalidated;
	u32 host = jit_h_prmw(lin, size);
	return jit_stat_invalidated != inv ? 0 : host;
}

// port I/O (real mode) and the flag instructions that go through tiny386's helpers
static u32 jit_h_in(int port, int size)
{
	CPUI386 *cpu = jit_cpu;
	return size == 1 ? cpu->cb.io_read8(cpu->cb.io, port) : size == 2 ? cpu->cb.io_read16(cpu->cb.io, port)
								  : cpu->cb.io_read32(cpu->cb.io, port);
}
static u32 jit_h_out(int port, int size, u32 v)
{
	CPUI386 *cpu = jit_cpu;
	if (size == 1) cpu->cb.io_write8(cpu->cb.io, port, v);
	else if (size == 2) cpu->cb.io_write16(cpu->cb.io, port, v);
	else cpu->cb.io_write32(cpu->cb.io, port, v);
	return 0;
}
static u32 jit_h_lahf(CPUI386 *cpu) { LAHF() return cpu->flags & 0xff; }   // (flags in cpu->cc)
static u32 jit_h_sahf(CPUI386 *cpu, u32 ah)
{
	cpu->cc.mask &= OF;
	cpu->flags = (cpu->flags & (wordmask ^ 0xff)) | (ah & 0xff);
	cpu->flags &= EFLAGS_MASK;
	cpu->flags |= 0x2;
	return 0;
}
// RCL, RCR (kind & 0x100) of x (bits = kind & 63 wide) by cnt, as tiny386's RCL_helper and RCR_helper: by (cnt & 31)
// mod (bits + 1); jit_rcx_by that count (0: nothing at all, nothing stored); CF and OF set (the pending flags stored)
static u32 jit_rcx_by;
static u32 jit_h_rcx(CPUI386 *cpu, u32 x, u32 cnt, u32 kind)
{
	const int bits = kind & 63;
	const u32 y = (cnt & 0x1f) % (bits + 1);
	jit_rcx_by = y;
	if (!y) return x;
	const u32 cf = get_CF(cpu);
	u32 res;
	int cf1, of1;
	if (!(kind & 0x100)) {
		res = (x << y) | (cf << (y - 1)) | (y != 1 ? (x >> (bits + 1 - y)) : 0);
		cf1 = (x >> (bits - y)) & 1;
		of1 = ((res >> (bits - 1)) & 1) ^ cf1;
	} else {
		res = (x >> y) | (cf << (bits - y)) | (y != 1 ? (x << (bits + 1 - y)) : 0);
		cf1 = ((x << (bits - y)) >> (bits - 1)) & 1;
		of1 = ((res ^ (res << 1)) >> (bits - 1)) & 1;
	}
	SET_BIT(cpu->flags, cf1, CF);
	SET_BIT(cpu->flags, of1, OF);
	cpu->cc.mask &= ~(CF | OF);
	return res;
}

// DAA, DAS, AAA, AAS, AAM (by imm, not 0), AAD of ax (op: the opcode), as tiny386's __DAA_helper and the rest: the new
// AX (the pending flags stored first)
static u32 jit_h_bcd(CPUI386 *cpu, u32 ax, u32 op, u32 imm)
{
	u32 al = ax & 0xff, ah = (ax >> 8) & 0xff;
	switch (op) {
	case 0x27: case 0x2f: {   // DAA, DAS
		const int cf = get_CF(cpu), das = op == 0x2f;
		u32 a = al;
		cpu->flags &= ~CF;
		if ((al & 0xf) > 9 || get_AF(cpu)) {
			a = (das ? al - 6 : al + 6) & 0xff;
			if (cf || (das ? al < 6 : al > 0xff - 6)) cpu->flags |= CF;
			cpu->flags |= AF;
		} else {
			cpu->flags &= ~AF;
		}
		if (al > 0x99 || cf) {
			a = (das ? a - 0x60 : a + 0x60) & 0xff;
			cpu->flags |= CF;
		}
		cpu->cc.dst = sext8(a);
		cpu->cc.mask = ZF | SF | PF;
		return (ax & 0xff00) | a;
	}
	case 0x37: case 0x3f:   // AAA, AAS (ZF SF PF of the result before: tiny386 leaves cc.dst)
		if ((al & 0xf) > 9 || get_AF(cpu)) {
			if (op == 0x37) {
				ax = (ax + 0x106) & 0xffff;
			} else {
				ax = (ax - 6) & 0xffff;
				ax = (ax & 0xff) | ((((ax >> 8) - 1) & 0xff) << 8);
			}
			cpu->flags |= AF | CF;
		} else {
			cpu->flags &= ~(AF | CF);
		}
		cpu->cc.mask = ZF | SF | PF;
		return (ax & 0xff00) | (ax & 0xf);
	default: {   // AAM, AAD
		u32 res;
		imm &= 0xff;
		if (op == 0xd4) { ah = al / imm; res = al % imm; }
		else { res = (al + ah * imm) & 0xff; ah = 0; }
		cpu->flags &= ~(OF | AF | CF);
		cpu->cc.dst = sext8(res);
		cpu->cc.mask = ZF | SF | PF;
		return ah << 8 | res;
	}
	}
}

// 16-bit SHLD (op & 0x100 clear) or SHRD of x with y by op & 0xff, as tiny386's SHLD_helper and SHRD_helper (its
// operands swapped beyond 16, and its flags in cc): the result; jit_shxd_by the count (0: nothing at all)
static u32 jit_shxd_by;
static u32 jit_h_shxd16(CPUI386 *cpu, u32 x, u32 y, u32 op)
{
	int count = op & 0x1f;
	jit_shxd_by = count;
	if (!count) return x;
	x &= 0xffff;
	y &= 0xffff;
	if (!(op & 0x100)) {
		cpu->cc.src1 = sext16(x);
		if (16 < count) { u32 z = x; x = y; y = z; count -= 16; }
		cpu->cc.dst = sext16((x << count) | (y >> (16 - count)));
		cpu->cc.dst2 = count == 1 ? sext16(x) : sext16((x << (count - 1)) | (y >> (16 - (count - 1))));
		cpu->cc.op = CC_SHLD;
	} else {
		if (16 < count) { u32 z = x; x = y; y = z; count -= 16; }
		cpu->cc.src1 = sext16(x);
		cpu->cc.dst = sext16((x >> count) | (y << (16 - count)));
		cpu->cc.dst2 = count == 1 ? sext16(x) : sext16((x >> (count - 1)) | (y << (16 - (count - 1))));
		cpu->cc.op = CC_SHRD;
	}
	cpu->cc.mask = CF | PF | ZF | SF | OF;
	return cpu->cc.dst;
}

// LAR (op 2), LSL (op 3) of selector sel, as tiny386's LARdw/LSLdw (larsl_helper): 0 if it raises an exception (the
// interpreter redoes it), else 1 with ZF clear or 2 with ZF set and the access rights or limit in jit_larlsl_v (the
// pending flags stored first; ZF set in cpu->flags as tiny386 does)
static uword jit_larlsl_v;
static int jit_h_larlsl(CPUI386 *cpu, u32 sel, u32 op)
{
	uword res = 0;
	int zf;
	if (!larsl_helper(cpu, sel, op == 2 ? &res : NULL, op == 2 ? NULL : &res, &zf)) return 0;
	if (zf) cpu->flags |= ZF; else cpu->flags &= ~ZF;
	cpu->cc.mask &= ~ZF;
	jit_larlsl_v = res;
	return 1 + !!zf;
}

// POPF of v (n bytes; not in virtual-8086 mode), as the fast path: 1 if the block is to be left after it (an
// interrupt now enabled and waiting, or IOPL changed: JM_IO)
static int jit_h_popf(CPUI386 *cpu, uword v, int n)
{
	uword mask = VM, iopl = cpu->flags & IOPL;
	if (cpu->cr0 & 1) {
		if (cpu->cpl > 0) mask |= IOPL;
		if (get_IOPL(cpu) < cpu->cpl) mask |= IF;
	}
	if (n == 2) cpu->flags = (cpu->flags & (0xffff0000 | mask)) | (v & 0xffff & ~mask);
	else cpu->flags = (cpu->flags & mask) | (v & ~mask);
	cpu->flags &= cpu->flags_mask;
	cpu->flags |= 0x2;
	cpu->cc.mask = 0;
	return (cpu->intr && (cpu->flags & IF)) || (cpu->flags & IOPL) != iopl;
}
// RDTSC: tiny386's time stamp, the high half in jit_tsc_hi
static u32 jit_tsc_hi;
static u32 jit_h_rdtsc(void)
{
	uint64_t t = get_nticks();
	jit_tsc_hi = t >> 32;
	return (u32) t;
}
static u32 jit_h_pushf(CPUI386 *cpu)
{
	refresh_flags(cpu);
	cpu->cc.mask = 0;
	return cpu->flags & ~(RF | VM);
}

// An instruction run in place by the fast path (registers and flags are in *cpu). 0: the block goes on; 2: it jumped
// to CS:next_ip, in the same mode (the block there through the indirect jump cache); else the block is left
// (jit_interp_reason, cpu->next_ip).
static u32 *jit_interp_ops;   // [4 * 540] (PSRAM): by mode (real, virtual-8086, 16-bit and 32-bit protected), then   // instructions run in place, by opcode (0F xx at 256 + xx; FE/FF /r at 512 + 8 * (op &
				  // 1) + r; REP A4-AF at 528 + op - A4): serial "jit ops"

// A protected-mode load of DS ES FS or GS from translated code (jt_seg_load): 0 done; 1 not (a null selector, or it
// would fault): the interpreter does the instruction.
// An x87 instruction (D8-DF) from translated code (K_FPU): tiny386's fpu_exec1 or fpu_exec2 straight away, as its ESC
// does, without leaving the block (it went to the interpreter: x87-heavy code ran at ~1.5 M instructions a second, most
// of it getting there). op: the opcode | ModR/M << 8 | 16-bit operand size << 16; seg, off: the memory operand's. 0: on;
// 1: the block is left (an exception, delivered from the dispatcher; or a store overwrote translated code).
// the frequent memory forms: helpers in the FPU's own file (t386_fpu_jit.h), the operand loaded or stored here
void t386_fpu_arith_f32(FPU *fpu, int g, u32 bits);
void t386_fpu_arith_f64(FPU *fpu, int g, u32 lo, u32 hi);
void t386_fpu_arith_i32(FPU *fpu, int g, u32 v);
void t386_fpu_arith_i16(FPU *fpu, int g, u32 v);
void t386_fpu_ld_f32(FPU *fpu, u32 bits);
void t386_fpu_ld_f64(FPU *fpu, u32 lo, u32 hi);
void t386_fpu_ld_i32(FPU *fpu, u32 v);
void t386_fpu_ld_i16(FPU *fpu, u32 v);
u32 t386_fpu_st_f32(FPU *fpu);
uint64_t t386_fpu_st_f64(FPU *fpu);
u32 t386_fpu_st_i32(FPU *fpu);
u32 t386_fpu_st_i16(FPU *fpu);
void t386_fpu_set_cw(FPU *fpu, u32 cw);
u32 t386_fpu_get_cw(FPU *fpu);
void t386_fpu_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fadd_f32(FPU *fpu, u32 bits);
void t386_fpu_fmul_f32(FPU *fpu, u32 bits);
void t386_fpu_fsub_f32(FPU *fpu, u32 bits);
void t386_fpu_fsubr_f32(FPU *fpu, u32 bits);
void t386_fpu_fdiv_f32(FPU *fpu, u32 bits);
void t386_fpu_fdivr_f32(FPU *fpu, u32 bits);
void t386_fpu_fadd_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fmul_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fsub_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fsubr_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fdiv_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fdivr_reg(FPU *fpu, u32 op_group, u32 i);
// (by group, FCOM FCOMP left out)
static const void *const jt_fpu_f32_helper[8] = {
	(const void *) t386_fpu_fadd_f32, (const void *) t386_fpu_fmul_f32, NULL, NULL, (const void *) t386_fpu_fsub_f32,
	(const void *) t386_fpu_fsubr_f32, (const void *) t386_fpu_fdiv_f32, (const void *) t386_fpu_fdivr_f32,
};
static const void *const jt_fpu_reg_helper[8] = {
	(const void *) t386_fpu_fadd_reg, (const void *) t386_fpu_fmul_reg, NULL, NULL, (const void *) t386_fpu_fsub_reg,
	(const void *) t386_fpu_fsubr_reg, (const void *) t386_fpu_fdiv_reg, (const void *) t386_fpu_fdivr_reg,
};
void t386_fpu_fld_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fxch_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fst_reg(FPU *fpu, u32 op_group, u32 i);
void t386_fpu_fstp_reg(FPU *fpu, u32 op_group, u32 i);
#include "t386_fpu_layout.h"
u32 t386_fpu_get_sw(FPU *fpu);
size_t t386_fpu_state_size(void);
bool t386_fpu_same(const FPU *a, const FPU *b);
// ---- the x87 inline, with the registers the helpers keep as singles (t386_fpu_jit.h, vendor/patches/0013): when
// the registers used are kept as singles, the operands and the result normal singles, and for the arithmetic the
// control word's precision and rounding those of the block's translation (single, and a renderer that chops: its
// rounding mode is in the instruction); else (zeros, another control word...) the helper, called from the end of
// the inline code. a0 the FPU, a3 TOP, a4 the registers kept as singles (sv); f0, f1 for the arithmetic.
static struct T386FpuLayout jt_fl;   // (t386_fpu_layout, at the first use)
static u32 jt_x87_cw;                // the control word when the block was translated: precision and rounding
static u32 *jt_fslow[8];             // branches to the helper's call
static bool jt_fcall_here;            // (jit_translate) the helper's calls in place, not after the block
static bool jt_x87_routine;           // (jit_x87_routine_of) making one: jt_fpu_slow returns (and clears it)
static int jt_nfslow;

static inline void e_fmv_w_x(int fd, int rs) { E(RV_R(0x78, 0, rs, 0, fd, 0x53)); }
static inline void e_fmv_x_w(int rd, int fs) { E(RV_R(0x70, 0, fs, 0, rd, 0x53)); }
static inline void e_fop_s(int f7, int rm, int fd, int a, int b) { E(RV_R(f7, b, a, rm, fd, 0x53)); }
static inline void e_fcvt_w_s(int rd, int fs, int rm) { E(RV_R(0x60, 0, fs, rm, rd, 0x53)); }
static inline void e_fcvt_s_w(int fd, int rs) { E(RV_R(0x68, 0, rs, 0, fd, 0x53)); }
static const u8 jt_x87_rm[4] = { 0, 2, 3, 1 };   // the x87's RC: nearest, down, up, chop as RISC-V's rne rdn rup rtz

static void jt_fslow_if(int f3, int a, int b)
{
	if (jt_nfslow < 8) jt_fslow[jt_nfslow++] = e_branch(f3, a, b);
	else joverflow = true;
}
static void jt_fpu_begin(void)   // a0 the FPU, a3 TOP, a4 sv
{
	if (!jt_fl.sb) t386_fpu_layout(&jt_fl);
	jt_nfslow = 0;
	e_lw(A0, R_CPU, CPU_OFF(fpu));
	e_lw(A3, A0, jt_fl.top);
	e_lbu(A4, A0, jt_fl.sv);
}
static void jt_fpu_sti(int d, int i)   // d = ST(i)'s register, (TOP + i) & 7
{
	e_addi(d, A3, i);
	e_andi(d, d, 7);
}
static void jt_fpu_kept(int idx)   // the helper unless register idx (0-7, in a register) is kept as a single
{
	e_srl(T6, A4, idx);
	e_andi(T6, T6, 1);
	jt_fslow_if(BEQ, T6, X0);
}
static void jt_fpu_sb(int d, int addr, int idx)   // addr = the FPU + idx * 4 (sb[idx] at jt_fl.sb), d = sb[idx]
{
	e_slli(addr, idx, 2);
	e_add(addr, addr, A0);
	if (d != X0) e_lw(d, addr, jt_fl.sb);
}
static bool jt_fpu_single(void)   // the block translated in single precision: inline arithmetic
{
	return (jt_x87_cw & 0x300) == 0;
}
static void jt_fpu_cw_as_translated(void)   // the helper unless the control word's PC and RC are the translation's
{
	e_lhu(T6, A0, jt_fl.cw);
	e_srli(T6, T6, 8);   // (PC and RC: bits 8-11, out of the immediates' 12 signed bits)
	e_andi(T6, T6, 0xf);
	if (jt_x87_cw & 0xc00) e_xori(T6, T6, (jt_x87_cw >> 8) & 0xc);
	jt_fslow_if(BNE, T6, X0);
}
static void jt_fpu_exp_in(int r, int lo, int n)   // the helper unless r's exponent is lo .. lo + n - 1
{
	e_srli(T6, r, 23);
	e_andi(T6, T6, 0xff);
	e_addi(T6, T6, -lo);
	e_sltiu(T6, T6, n);
	jt_fslow_if(BEQ, T6, X0);
}
// register idx changed: rawtagw's bit (its 80 bits, rawst, no longer its value: FNSAVE makes them again). Also for
// a register kept as a single already: a read keeps one (t386_st_f32) with the 80 bits still current
static void jt_fpu_dirty(int idx)
{
	e_li(T6, 1);
	e_sll(T6, T6, idx);
	e_lbu(T1, A0, jt_fl.rawtagw);
	e_or(T1, T1, T6);
	e_sb(T1, A0, jt_fl.rawtagw);
}
static void jt_fpu_mark(int idx)   // register idx kept as a single, written: sv's and rawtagw's bit (a4 = sv)
{
	jt_fpu_dirty(idx);
	e_or(A4, A4, T6);
	e_sb(A4, A0, jt_fl.sv);
}
static void jt_fpu_set_top(int r)   // TOP = r & 7 (r changed)
{
	e_andi(r, r, 7);
	e_sw(r, A0, jt_fl.top);
}
static void jt_fpu_nz(int r)   // the helper unless r is a normal single or a zero
{
	e_slli(T6, r, 1);
	u32 *zero = e_branch(BEQ, T6, X0);
	jt_fpu_exp_in(r, 1, 254);
	j_bind(zero, jp);
}
// dst = a op b as fparith's group g does it (5, 7: b op a); the helper unless the result is the x87's (as
// t386_hw_f32: well inside a single's range, or an exact zero: a sum's or a difference's, or with a zero operand)
static void jt_fpu_arith(int g, int dst, int a, int b)
{
	static const u8 f7[8] = { 0x00, 0x08, 0, 0, 0x04, 0x04, 0x0c, 0x0c };
	const bool rev = g == 5 || g == 7;
	e_fmv_w_x(0, a);
	e_fmv_w_x(1, b);
	e_fop_s(f7[g], jt_x87_rm[(jt_x87_cw >> 10) & 3], 0, rev ? 1 : 0, rev ? 0 : 1);
	e_fmv_x_w(dst, 0);
	e_srli(T6, dst, 23);
	e_andi(T6, T6, 0xff);
	e_addi(T6, T6, -2);
	e_sltiu(T6, T6, 252);
	u32 *ok = e_branch(BNE, T6, X0), *ok2 = NULL;
	e_slli(T6, dst, 1);
	jt_fslow_if(BNE, T6, X0);
	if (g == 1 || g >= 6) {   // FMUL FDIV(R): a zero operand
		e_slli(T6, a, 1);
		ok2 = e_branch(BEQ, T6, X0);
		e_slli(T6, b, 1);
		jt_fslow_if(BNE, T6, X0);
	}
	j_bind(ok, jp);
	j_bind(ok2, jp);
}
static JStub *jt_stub(int kind);
// CR0's EM or TS: #NM, raised by the interpreter (CWSDPMI's task switches set TS). Once a block, until an instruction
// that may change CR0 (jt_x87_nm_ok: MOV CR0, CLTS, a task switch: the interpreter's or a far transfer's)
static bool jt_x87_nm_ok;
static void jt_fpu_nm(void)
{
	if (jt_x87_nm_ok) return;
	e_lw(T6, R_CPU, CPU_OFF(cr0));
	e_andi(T6, T6, 0xc);
	JStub *nm = jt_stub(STUB_EXIT);
	nm->at = e_branch(BNE, T6, X0);
	jt_x87_nm_ok = true;
}
// The inline code's end: its branches to the helper go to a call of fn (a0 the FPU, a1 and a2 set unless -1) after
// the block (STUB_FCALL), which comes back here. (Out of the way: the code run is shorter, by a jump too.)
static void jt_fpu_slow(const void *fn, int a1, int a2)
{
	if (jt_x87_routine) {   // (jit_x87_routine_of) a return; the helper's call, the return address kept
		jt_x87_routine = false;
		e_jalr(X0, RA, 0);
		for (int k = 0; k < jt_nfslow; k++) j_bind(jt_fslow[k], jp);
		e_addi(SP, SP, -16);
		e_sw(RA, SP, 12);
		e_lw(A0, R_CPU, CPU_OFF(fpu));
		if (a1 >= 0) e_li(A1, a1);
		if (a2 >= 0) e_li(A2, a2);
		e_call_abs(fn);
		e_lw(RA, SP, 12);
		e_addi(SP, SP, 16);
		e_jalr(X0, RA, 0);
		return;
	}
	if (jt_nfslow > 6 || jt_fcall_here) {   // (more than a stub's branches, or a long block: the call here)
		u32 *done = e_jump();
		for (int k = 0; k < jt_nfslow; k++) j_bind(jt_fslow[k], jp);
		e_lw(A0, R_CPU, CPU_OFF(fpu));
		if (a1 >= 0) e_li(A1, a1);
		if (a2 >= 0) e_li(A2, a2);
		e_call_abs(fn);
		j_bind(done, jp);
		return;
	}
	JStub *st = jt_stub(STUB_FCALL);
	for (int k = 0; k < jt_nfslow; k++) {
		if (k) st->more[k - 1] = jt_fslow[k];
		else st->at = jt_fslow[k];
	}
	st->fn = fn;
	st->a1 = a1;
	st->a2 = a2;
	st->resume = jp;
}

// register forms (ESC, group, ST(i)) t386_fpu_reg does: those that can't fault and don't need the CPU
static bool jt_fpu_reg_form(int esc, int g, int i)
{
	switch (esc) {
	case 0: case 4: case 6: return true;                          // arithmetic, compares (DE D9: FCOMPP)
	case 1: return g <= 1 || g >= 6 || (g == 4 && (i <= 1 || i == 4 || i == 5)) || (g == 5 && i <= 6);
	case 5: return g >= 1 && g <= 5;                              // FXCH FST FSTP FUCOM FUCOMP
	}
	return false;
}
enum { FF_NONE, FF_ARITH, FF_ARITHG, FF_LD, FF_ST, FF_STP, FF_LDCW, FF_STCW };   // (FF_ARITHG: the group's own helper)
struct JitFpuForm { u8 how, bytes; const void *fn; };
// ESC (D8-DF) / group with a memory operand: how, the operand's size, the helper; FF_NONE: jit_h_fpu
static struct JitFpuForm jt_fpu_form(int esc, int g)
{
	switch (esc) {
	case 0:
		if (jt_fpu_f32_helper[g]) return (struct JitFpuForm) { FF_ARITHG, 4, jt_fpu_f32_helper[g] };
		return (struct JitFpuForm) { FF_ARITH, 4, (const void *) t386_fpu_arith_f32 };
	case 2: return (struct JitFpuForm) { FF_ARITH, 4, (const void *) t386_fpu_arith_i32 };
	case 4: return (struct JitFpuForm) { FF_ARITH, 8, (const void *) t386_fpu_arith_f64 };
	case 6: return (struct JitFpuForm) { FF_ARITH, 2, (const void *) t386_fpu_arith_i16 };
	case 1:
		if (g == 0) return (struct JitFpuForm) { FF_LD, 4, (const void *) t386_fpu_ld_f32 };
		if (g == 2 || g == 3) return (struct JitFpuForm) { g == 3 ? FF_STP : FF_ST, 4, (const void *) t386_fpu_st_f32 };
		if (g == 5) return (struct JitFpuForm) { FF_LDCW, 2, (const void *) t386_fpu_set_cw };
		if (g == 7) return (struct JitFpuForm) { FF_STCW, 2, (const void *) t386_fpu_get_cw };
		break;
	case 3:
		if (g == 0) return (struct JitFpuForm) { FF_LD, 4, (const void *) t386_fpu_ld_i32 };
		if (g == 2 || g == 3) return (struct JitFpuForm) { g == 3 ? FF_STP : FF_ST, 4, (const void *) t386_fpu_st_i32 };
		break;
	case 5:
		if (g == 0) return (struct JitFpuForm) { FF_LD, 8, (const void *) t386_fpu_ld_f64 };
		if (g == 2 || g == 3) return (struct JitFpuForm) { g == 3 ? FF_STP : FF_ST, 8, (const void *) t386_fpu_st_f64 };
		break;
	case 7:
		if (g == 0) return (struct JitFpuForm) { FF_LD, 2, (const void *) t386_fpu_ld_i16 };
		if (g == 2 || g == 3) return (struct JitFpuForm) { g == 3 ? FF_STP : FF_ST, 2, (const void *) t386_fpu_st_i16 };
		break;
	}
	return (struct JitFpuForm) { FF_NONE, 0, NULL };
}

static long jit_stat_fpu;     // (serial "jit")
#ifdef T386_FPU_STATS
static u32 jit_fpu_form[128];  // x87 instructions run by translated code, by ESC * 16 + (register form) * 8 + group
extern uint32_t t386_fpu_stat[];
#endif
static int jit_h_fpu(CPUI386 *cpu, u32 op, uword seg, uword off, uword eip, uword next)
{
	const int esc = (op & 0xff) - 0xd8, modrm = (op >> 8) & 0xff;
	const long inv = jit_stat_invalidated;
	bool ok = true;
	jit_stat_fpu++;
	cpu->ip = eip;
	cpu->next_ip = next;
	if (cpu->cr0 & 0xc) {   // EM or TS: #NM (CWSDPMI's task switches set TS)
		cpu->excno = EX_NM;
		ok = false;
	} else if (cpu->fpu) {
		if ((modrm >> 6) != 3) ok = fpu_exec2(cpu->fpu, cpu, (op >> 16) & 1, esc, (modrm >> 3) & 7, seg, off);
		else ok = fpu_exec1(cpu->fpu, cpu, esc, (modrm >> 3) & 7, modrm & 7);
	}
	if (!ok) {
		jit_exception = true;
		jit_interp_reason = EXIT_STOP;
		return 1;
	}
	if (jit_stat_invalidated != inv) {   // (an FST/FIST/FSAVE over translated code: on from the dispatcher)
		jit_interp_reason = EXIT_JUMP;
		return 1;
	}
	return 0;
}

static long jit_stat_pm[8];   // protected mode from translated code: segment loads, far CALL, JMP, RETF, IRET, INT
static int jit_h_ldseg(CPUI386 *cpu, int s, uword sel)   // (s | JM_FLAT << 8: from a JM_FLAT block)
{
	const int flat = s >> 8;
	s &= 0xff;
	jit_stat_pm[0]++;
	__typeof__(cpu->seg[0]) was = cpu->seg[s];
	if (!(sel & ~3) || !set_seg(cpu, s, sel)) return 1;
	if (flat && (s == SEG_DS || s == SEG_ES) && cpu->seg[s].base) {   // the interpreter loads it, after the block
		cpu->seg[s] = was;
		return 1;
	}
	JIT_NULLSEGS(cpu) = fast_nullsegs(cpu);
	return 0;
}

static long jit_stat_nulled;   // the dispatcher found a block using a null segment: the interpreter
static long jit_stat_ileave[3];   // jit_h_interp left the block: for another mode; SS or a null segment; a jump
static u8 jit_run_mode;          // the mode of the blocks running (they reach only blocks of their JM_FLAT)

// the state that decides whether a block can go on after an instruction done outside it (jit_after)
struct JitBefore {
	uword cr0, cr3, cs_base, cs_sel, ss_sel, ss_base, sp_mask, iopl, vm;
	bool code16;
	int cpl;
	long inv;
	unsigned tlb_gen;   // t386_tlb_gen (tiny386's TLB emptied: INVLPG)
};
static void jit_before(CPUI386 *cpu, struct JitBefore *b)
{
	b->cr0 = cpu->cr0;
	b->cr3 = cpu->cr3;
	b->cs_base = cpu->seg[SEG_CS].base;
	b->cs_sel = cpu->seg[SEG_CS].sel;
	b->ss_sel = cpu->seg[SEG_SS].sel;
	b->ss_base = cpu->seg[SEG_SS].base;
	b->sp_mask = cpu->sp_mask;
	b->iopl = cpu->flags & IOPL;
	b->vm = cpu->flags & VM;
	b->code16 = cpu->code16;
	b->cpl = cpu->cpl;
	b->inv = jit_stat_invalidated;
	b->tlb_gen = t386_tlb_gen;
}
static int jit_after(CPUI386 *cpu, const struct JitBefore *b, bool ok, uword next);

static bool jit_count_ops;   // counting the instructions run in place, by opcode (since the first "jit ops")
static int jit_h_interp(CPUI386 *cpu, uword eip, uword next)
{
	if (jit_count_ops) {   // (counted by opcode: serial "jit ops"; with paging, if the TLB has the page)
		uword l = cpu->seg[SEG_CS].base + eip;
		if (cpu->cr0 & CR0_PG) {
			struct ftlb_entry *e = &ftlb[(l >> 12) & (FTLB_N - 1)];
			l = e->rtag == l >> 12 ? (uword) (l + e->delta - (uintptr_t) cpu->phys_mem) : (uword) cpu->phys_mem_size;
		}
		const u8 *p = cpu->phys_mem + l;
		int k = 0, rep = 0;
		while (k < 14 && l + k < (uword) cpu->phys_mem_size &&
		       (p[k] == 0x66 || p[k] == 0x67 || p[k] == 0xf2 || p[k] == 0xf3 || p[k] == 0xf0 || p[k] == 0x26 ||
			p[k] == 0x2e || p[k] == 0x36 || p[k] == 0x3e || p[k] == 0x64 || p[k] == 0x65))
			rep |= p[k] == 0xf2 || p[k] == 0xf3, k++;
		if (!jit_interp_ops) jit_interp_ops = (u32 *) t386_jit_alloc(4 * 540 * sizeof(u32));
		if (jit_interp_ops && l + k + 2 < (uword) cpu->phys_mem_size)
			jit_interp_ops[540 * (!(cpu->cr0 & 1) ? 0 : (cpu->flags & VM) ? 1 : cpu->code16 ? 2 : 3) + (p[k] == 0x0f ? 256 + p[k + 1] : p[k] >= 0xfe ? 512 + 8 * (p[k] & 1) + ((p[k + 1] >> 3) & 7) :
				       rep && p[k] >= 0xa4 && p[k] <= 0xaf ? 528 + p[k] - 0xa4 : p[k])]++;
	}
	struct JitBefore st;
	jit_before(cpu, &st);
	cpu->next_ip = eip;
	cpu->cycle--;   // (fast_exec counts the instruction, and so does the block)
	return jit_after(cpu, &st, fast_exec(cpu, 1), next);
}

// What an instruction done outside translated code changed: 0 the block goes on (at next); 2 it jumped to
// CS:next_ip in the same mode; 1 the block is left (jit_interp_reason).
static int jit_after(CPUI386 *cpu, const struct JitBefore *b, bool ok, uword next)
{
	if (!ok) {   // an exception: delivered once out of the block
		jit_exception = true;
		jit_interp_reason = EXIT_STOP;
		return 1;
	}
	if (cpu->halt || (cpu->intr && (cpu->flags & IF))) {
		jit_interp_reason = EXIT_STOP;
		return 1;
	}
	jit_interp_reason = EXIT_JUMP;
	if (jit_stat_invalidated != b->inv)   // it overwrote translated code (this block's, maybe): on from the dispatcher
		return 1;
	if (cpu->cr0 != b->cr0 || cpu->cr3 != b->cr3 || (cpu->flags & VM) != b->vm || cpu->code16 != b->code16 ||
	    cpu->sp_mask != b->sp_mask || (cpu->flags & IOPL) != b->iopl || cpu->cpl != b->cpl)
		return jit_stat_ileave[0]++, 1;
	// tiny386 emptied its TLB (INVLPG): the block leaves, the mapping of its own code may be another now (the fast
	// path, after the instruction, may already have emptied its TLB and the jump cache: compared with before, not
	// with ftlb_gen)
	if (t386_tlb_gen != b->tlb_gen)
		return jit_stat_ileave[0]++, 1;
	if ((b->cr0 & 1) && !b->vm) {   // protected mode: blocks check on entry that the segments they use aren't null
		if (cpu->seg[SEG_SS].sel != b->ss_sel || cpu->seg[SEG_SS].base != b->ss_base) return jit_stat_ileave[1]++, 1;
		if ((jit_run_mode & JM_FLAT) && (cpu->seg[SEG_DS].base | cpu->seg[SEG_ES].base))
			return jit_stat_ileave[0]++, 1;   // (as for another mode)
		unsigned ns = fast_nullsegs(cpu);
		if (ns & ~JIT_NULLSEGS(cpu)) return jit_stat_ileave[1]++, 1;   // one now null: its accesses fault
		JIT_NULLSEGS(cpu) = ns;                 // (16-bit protected-mode code loads ES and DS all the time)
	}
	if (cpu->next_ip != next || cpu->seg[SEG_CS].base != b->cs_base || cpu->seg[SEG_CS].sel != b->cs_sel)
		return jit_stat_ileave[2]++, 2;   // a jump (far CALL, RETF, ...) in the same mode: on through the indirect jump cache
	return 0;
}

// A far CALL or JMP (direct, or through memory: the pointer read by the block), RETF, IRET or INT in protected
// mode, from translated code (K_PMFAR): tiny386's pmcall, pmret or call_isr, as the fast path does them, without
// going through it. op: the opcode (9A for a CALL, EA for a JMP), | 0x100 for a 16-bit operand size; a: the offset,
// RETF's bytes to drop or INT's number; sel: the selector. As jit_h_interp's.
static int jit_h_pmfar(CPUI386 *cpu, u32 op, uword a, uword sel, uword eip, uword next)
{
	struct JitBefore st;
	jit_before(cpu, &st);
	const bool opsz16 = (op >> 8) & 1;
	bool ok;
	cpu->ip = eip;
	cpu->next_ip = next;
	jit_stat_pm[(op & 0xff) == 0x9a ? 1 : (op & 0xff) == 0xea ? 2 : (op & 0xff) == 0xcf ? 4 : (op & 0xff) == 0xcd ? 5 : 3]++;
	switch (op & 0xff) {
	case 0x9a: ok = pmcall(cpu, opsz16, a, sel, false); break;
	case 0xea: ok = pmcall(cpu, opsz16, a, sel, true); break;
	case 0xcf: ok = pmret(cpu, opsz16, 0, true); break;
	case 0xcd:   // (as tiny386's INT: ip the return address while the gate is taken)
		t386_int_vec[a & 0xff]++;
		cpu->ip = next;
		ok = call_isr(cpu, a & 0xff, false, 0);
		if (!ok) cpu->ip = eip;
		break;
	default: ok = pmret(cpu, opsz16, a, false); break;   // RETF
	}
	ftlb_check(cpu);   // (the privilege level may be another; a task switch empties the TLB)
	return jit_after(cpu, &st, ok, next);
}

// The shared routines blocks jump to (made at the start of the cache, after the flags' and the VGA writes'):
// jit_r_ijc: on to the block at linear address t0 through the indirect jump cache (t6 the CS key: its base, xor
//   R_MAP with paging; a3 the mode), or leave for the dispatcher at a1 (EXIT_JUMP).
// jit_r_stop: after jit_h_interp stopped a block (a0, a3 the mode): a jump in the same mode (2) on through
//   jit_r_ijc, else leave with jit_interp_reason at cpu->next_ip.
// jit_r_interp: called with a1, a2 the instruction's address and the next: jit_h_interp, with the guest registers
//   in *cpu (stored before, loaded after); a0 its verdict, every other register but t6 and ra kept.
static u32 *jit_tramp(const void *fn)   // a jump to fn, ra kept
{
	u32 *at = jp, t = (u32) (uintptr_t) fn;
	s32 lo = (s32) (t << 20) >> 20;
	e_lui(A6, t - (u32) lo);
	e_jalr(X0, A6, lo);
	return at;
}

// t1 = the host address of the access at t0 (write: for a write) through the fast path's TLB, or a branch to bind
// to its miss returned (or, bytes > 1, an access running on into the next page); uses t6 and a6
static u32 *jit_emit_tlb(int write, int bytes)
{
	if (bytes > 1) {   // the entry of the first byte's page, checked against the last byte's
		e_addi(T1, T0, bytes - 1);
		e_srli(T1, T1, 12);
		e_srli(A6, T0, 12);
		e_andi(T6, A6, FTLB_N - 1);
	} else {
		e_srli(T1, T0, 12);
		e_andi(T6, T1, FTLB_N - 1);
	}
	e_slli(T6, T6, 4);
	e_add(T6, T6, R_MAP);
	e_lw(A6, T6, write ? (int) offsetof(struct ftlb_entry, wtag) : (int) offsetof(struct ftlb_entry, rtag));
	u32 *miss = e_branch(BNE, A6, T1);
	e_lw(T6, T6, (int) offsetof(struct ftlb_entry, delta));
	e_add(T1, T0, T6);
	return miss;
}

static void jit_make_shared(void)
{
	u32 t = (u32) (uintptr_t) t386_jit_exit;
	s32 lo = (s32) (t << 20) >> 20;
	jit_r_ijc = jp;
	e_srli(T2, T0, 15);   // (jit_ijc_hash, times 32: the set)
	e_xor(T2, T2, T0);
	e_li(T1, JIT_IJC_MUL);
	e_mul(T1, T1, T2);
	e_srli(T1, T1, 32 - JIT_IJC_BITS);
	e_slli(T1, T1, 5);
	e_li(T2, (u32) (uintptr_t) jit_ijc);
	e_add(T1, T1, T2);
	u32 *miss[6];
	for (int k = 0; k < 2; k++) {   // its two entries
		e_lw(T2, T1, 16 * k);
		miss[3 * k] = e_branch(BNE, T2, T0);
		e_lw(T2, T1, 16 * k + 4);
		miss[3 * k + 1] = e_branch(BNE, T2, T6);
		e_lw(T2, T1, 16 * k + 8);
		miss[3 * k + 2] = e_branch(BNE, T2, A3);
		e_lw(T2, T1, 16 * k + 12);
		e_jalr(X0, T2, 0);
		for (int m = 3 * k; m < 3 * k + 3; m++) j_bind(miss[m], jp);
	}
	e_li(A0, EXIT_JUMP);
	e_lui(T6, t - (u32) lo);
	e_jalr(X0, T6, lo);

	jit_r_stop = jp;
	e_li(T6, 2);
	u32 *other = e_branch(BNE, A0, T6);
	e_lw(A1, R_CPU, CPU_OFF(next_ip));
	e_lw(T6, R_CPU, SEG_BASE_OFF(SEG_CS));
	e_add(T0, A1, T6);
	e_andi(T2, A3, JM_PAGED);
	u32 *flat = e_branch(BEQ, T2, X0);
	e_xor(T6, T6, R_MAP);
	j_bind(flat, jp);
	j_bind(e_jump(), jit_r_ijc);
	j_bind(other, jp);
	e_lw_abs(A0, &jit_interp_reason);
	e_lw(A1, R_CPU, CPU_OFF(next_ip));
	e_lui(T6, t - (u32) lo);
	e_jalr(X0, T6, lo);

	// jit_r_tlb[write][size]: t1 = the host address of the access at t0 through the fast path's TLB (R_MAP: its
	// address, in blocks with paging), or 0: a miss, or (bytes > 1) an access running on into the next page. Uses
	// t6 and a6 (t0 is kept: jt_push).
	// jit_r_tlbs[seg][write][size]: the same, segment seg's base added to t0 first (jt_seg_base)
	for (int seg = -1; seg < 6; seg++)
	for (int w = 0; w < 2; w++)
		for (int k = 0; k < 3; k++) {
			int bytes = 1 << k;
			if (seg >= 0) {
				jit_r_tlbs[seg][w][k] = jp;
				e_lw(T6, R_CPU, SEG_BASE_OFF(seg));
				e_add(T0, T0, T6);
			} else {
				jit_r_tlb[w][k] = jp;
			}
			u32 *miss = jit_emit_tlb(w, bytes);
			e_jalr(X0, RA, 0);
			j_bind(miss, jp);
			e_mv(T1, X0);
			e_jalr(X0, RA, 0);
		}

	// trampolines to the slow paths of accesses with paging and to the exits for the interpreter, which blocks reach
	// with a jal (they use a6, free there)
	static void (*const prd[3])(void) = { t386_jit_prd1, t386_jit_prd2, t386_jit_prd4 };
	static void (*const pwr[2][3])(void) = { { t386_jit_pwn1, t386_jit_pwn2, t386_jit_pwn4 },
						 { t386_jit_pwr1, t386_jit_pwr2, t386_jit_pwr4 } };
	static void (*const prmw[3])(void) = { t386_jit_prmw1, t386_jit_prmw2, t386_jit_prmw4 };
	for (int k = 0; k < 3; k++) {
		jit_t_prd[k] = jit_tramp((const void *) prd[k]);
		jit_t_pwr[0][k] = jit_tramp((const void *) pwr[0][k]);
		jit_t_pwr[1][k] = jit_tramp((const void *) pwr[1][k]);
		jit_t_prmw[k] = jit_tramp((const void *) prmw[k]);
	}
	jit_t_leave = jit_tramp((const void *) t386_jit_leave);
	for (int r = 0; r < 5; r++) {
		u32 *at = jp;
		e_lw(A1, RA, 0);
		e_li(A0, r);
		jit_tramp((const void *) t386_jit_exit);
		jit_t_exit[r] = at;
	}
	jit_t_leave_f = jit_tramp((const void *) t386_jit_leave_f);
	jit_t_bexit = jp;   // (a2: the stub; a1 isn't the address, the block's start: the dispatcher sets it)
	e_addi(A2, RA, -4);
	e_li(A0, EXIT_BUDGET);
	jit_tramp((const void *) t386_jit_exit);
	jit_t_lexit = jp;   // (the exit: the jal before the x86 address)
	e_lw(A1, RA, 0);
	e_addi(A2, RA, -4);
	e_li(A0, EXIT_LINK);
	jit_tramp((const void *) t386_jit_exit);
	// the compact stubs' routines (jt_stub_compact), by size and register (not x0-x4, a6): a read's moves the value
	// from t6 and goes on at the stub's resume, a write's moves the value to t6
	static void (*const cprd[3])(void) = { t386_jit_cprd1, t386_jit_cprd2, t386_jit_cprd4 };
	static void (*const cpwr[2][3])(void) = { { t386_jit_cpwn1, t386_jit_cpwn2, t386_jit_cpwn4 },
						  { t386_jit_cpwr1, t386_jit_cpwr2, t386_jit_cpwr4 } };
	u32 *cresume = jp;   // (as the end of t386_jit_cpwr*)
	e_lw(A6, RA, 0);
	e_slli(A6, A6, 19);
	e_srli(A6, A6, 17);
	e_sub(A6, RA, A6);
	e_jalr(X0, A6, 0);
	// jit_r_push[stack16][4 bytes][value]: with paging, a push of a0 or s1-s8 (2 or 4 bytes), as jt_push's code;
	// back after the block's data word (jt_push_call), or as a compact stub at a fault or onto translated code
	for (int k = 0; k < 2; k++) {
		const int n = 2 << k;
		u32 *slow = jit_tramp((const void *) (k ? t386_jit_cpsh4 : t386_jit_cpsh2));
		for (int s16 = 0; s16 < 2; s16++)
			for (int v = 0; v < 9; v++) {
				const int r = v ? jg[v - 1] : A0;
				jit_r_push[s16][k][v] = jp;
				e_addi(T0, jg[4], -n);
				if (s16) jt_zext(T0, T0, 16);
				e_lw(T6, R_CPU, SEG_BASE_OFF(SEG_SS));
				e_add(T0, T0, T6);
				u32 *miss = jit_emit_tlb(1, n);
				if (n == 2) e_sh(r, T1, 0);
				else e_sw(r, T1, 0);
				u32 *done = jp;
				if (s16) {   // (SP from t0, as jt_push)
					e_lw(T6, R_CPU, SEG_BASE_OFF(SEG_SS));
					e_sub(A6, T0, T6);
					jt_set_r(4, A6, 16, true);
				} else {
					e_addi(jg[4], jg[4], -n);
				}
				e_jalr(X0, RA, 4);
				j_bind(miss, jp);
				e_mv(T6, r);
				E(rv_jal((int) ((u8 *) slow - (u8 *) jp), T1));
				j_bind(e_jump(), done);
			}
	}
	// jit_r_pop[stack16][4 bytes][to]: with paging, a pop (2 or 4 bytes) into a0, a1, s1-s8 (zero-extended), or
	// (to 10-17, 2 bytes) the low half of s1-s8, as jt_pop's code; back as the pushes' (jt_pop_call)
	memset(jit_r_pop, 0, sizeof jit_r_pop);
	for (int k = 0; k < 2; k++) {
		const int n = 2 << k;
		u32 *slow = jit_tramp((const void *) (k ? t386_jit_cpop4 : t386_jit_cpop2));
		for (int s16 = 0; s16 < 2; s16++)
			for (int v = 0; v < (k ? 10 : 18); v++) {
				const int r = v == 0 ? A0 : v == 1 ? A1 : jg[(v - 2) & 7], half = v >= 10, to = half ? A6 : r;
				if (r == jg[4]) continue;   // (not into ESP)
				jit_r_pop[s16][k][v] = jp;
				if (s16) jt_zext(T0, jg[4], 16);
				else e_mv(T0, jg[4]);
				e_lw(T6, R_CPU, SEG_BASE_OFF(SEG_SS));
				e_add(T0, T0, T6);
				u32 *miss = jit_emit_tlb(0, n);
				if (n == 2) e_lhu(to, T1, 0);
				else e_lw(to, T1, 0);
				u32 *done = jp;
				if (half) {   // (as jt_set_r)
					e_lui(T6, 0xffff0000);
					e_and(r, r, T6);
					e_or(r, r, A6);
				}
				if (s16) { e_addi(A6, jg[4], n); jt_set_r(4, A6, 16, false); }
				else e_addi(jg[4], jg[4], n);
				e_jalr(X0, RA, 4);
				j_bind(miss, jp);
				E(rv_jal((int) ((u8 *) slow - (u8 *) jp), T1));
				e_mv(to, T6);
				j_bind(e_jump(), done);
			}
	}
	memset(jit_t_cprd, 0, sizeof jit_t_cprd);
	memset(jit_t_cpwr, 0, sizeof jit_t_cpwr);
	for (int k = 0; k < 3; k++) {
		u32 *rd = jit_tramp((const void *) cprd[k]), *wr[2];
		for (int w = 0; w < 2; w++) wr[w] = jit_tramp((const void *) cpwr[w][k]);
		for (int r = 5; r < 32; r++) {
			if (r == A6) continue;
			jit_t_cprd[k][r] = jp;
			E(rv_jal((int) ((u8 *) rd - (u8 *) jp), T6));
			if (r != T6) e_mv(r, T6);
			j_bind(e_jump(), cresume);
			for (int w = 0; w < 2; w++) {
				jit_t_cpwr[w][k][r] = jp;
				if (r != T6) e_mv(T6, r);
				j_bind(e_jump(), wr[w]);
			}
		}
	}

	// jit_r_cf: t6 = CF (0/1) of the flags in cpu->cc and cpu->flags (jt_cf, when the block doesn't hold them); keeps
	// every other register but ra
	jit_r_cf = jp;
	e_lw(T6, R_CPU, CPU_OFF(cc.mask));
	e_andi(T6, T6, CF);
	{
		u32 *lazy = e_branch(BNE, T6, X0);
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_andi(T6, T6, 1);
		e_jalr(X0, RA, 0);
		j_bind(lazy, jp);
	}
	e_addi(SP, SP, -16);
	e_sw(RA, SP, 12);
	e_sw(A0, SP, 8);
	e_mv(A0, R_CPU);
	e_call((const void *) jit_h_cf);
	e_mv(T6, A0);
	e_lw(A0, SP, 8);
	e_lw(RA, SP, 12);
	e_addi(SP, SP, 16);
	e_jalr(X0, RA, 0);
	// jit_r_cond: a0 = condition a1 (Jcc's low nibble, 0/1) of those flags (jt_cond); keeps every other register but
	// ra and t6
	jit_r_cond = jp;
	e_addi(SP, SP, -16);
	e_sw(RA, SP, 12);
	e_mv(A0, R_CPU);
	e_call((const void *) jit_h_cond);
	e_lw(RA, SP, 12);
	e_addi(SP, SP, 16);
	e_jalr(X0, RA, 0);

	// jit_r_callc: as jit_r_interp, for the C function in t6 (through t386_jit_thunk), with a1-a5 its arguments
	jit_r_callc = jp;
	e_addi(SP, SP, -16);
	e_sw(RA, SP, 12);
	for (int i = 0; i < 8; i++) e_sw(jg[i], R_CPU, 4 * i);
	e_mv(A0, R_CPU);
	e_call_abs((const void *) t386_jit_thunk);
	for (int i = 0; i < 8; i++) e_lw(jg[i], R_CPU, 4 * i);
	e_lw(RA, SP, 12);
	e_addi(SP, SP, 16);
	e_jalr(X0, RA, 0);

	jit_r_interp = jp;
	e_addi(SP, SP, -16);
	e_sw(RA, SP, 12);
	for (int i = 0; i < 8; i++) e_sw(jg[i], R_CPU, 4 * i);
	e_mv(A0, R_CPU);
	e_call((const void *) jit_h_interp);
	for (int i = 0; i < 8; i++) e_lw(jg[i], R_CPU, 4 * i);
	e_lw(RA, SP, 12);
	e_addi(SP, SP, 16);
	e_jalr(X0, RA, 0);
}

// The routines blocks call (the flags' stores, the VGA writes, the shared routines and trampolines) are made at the
// start of the cache, and again further on when blocks are made more than JIT_RT_REACH after the last copy, so that
// a jal (+-1 MB) reaches them from every block (e_call_near). Emptying the cache goes back to the first copy.
#define JIT_RT_REACH ((1 << 20) - (64 << 10))
#define JIT_RT_MAX (32 << 10)   // (bytes: more than a copy takes)
static u32 *jit_rt_at;   // the newest copy
static u32 *jit_x87_free, *jit_x87_end;   // its pool for the x87 routines (jit_x87_routine_of)
#define JIT_X87_POOL (24 << 10)   // bytes
#define JIT_X87_MEMO 512
static struct { u32 key; u32 *code; } jit_x87_memo[JIT_X87_MEMO];   // its routines (key 0: none)
static u32 *jit_rt_copy[8], jit_rt_bytes;   // (t386_jit_block_at) the copies, the first first; a copy's size
static int jit_rt_copies;
#define JIT_RT(X) X(jit_fstore) X(jit_vga_wr) X(jit_r_ijc) X(jit_r_stop) X(jit_r_interp) X(jit_r_callc) X(jit_r_cf) X(jit_r_cond) X(jit_r_tlb) X(jit_r_tlbs) X(jit_t_prd) \
		  X(jit_t_pwr) X(jit_t_prmw) X(jit_t_leave) X(jit_t_leave_f) X(jit_t_exit) X(jit_t_lexit) X(jit_t_bexit) X(jit_t_cprd) X(jit_t_cpwr) X(jit_r_push) X(jit_r_pop) X(jit_rt_at) X(jit_x87_free) X(jit_x87_end)
#define JIT_RT_MEMBER(v) __typeof__(v) v;
static struct { JIT_RT(JIT_RT_MEMBER) } jit_rt_first;
static void jit_rt_save(void)
{
#define JIT_RT_SAVE(v) memcpy(&jit_rt_first.v, &v, sizeof v);
	JIT_RT(JIT_RT_SAVE)
}
static void jit_rt_restore(void)
{
#define JIT_RT_RESTORE(v) memcpy(&v, &jit_rt_first.v, sizeof v);
	if (jit_rt_first.jit_rt_at) { JIT_RT(JIT_RT_RESTORE) jit_rt_copies = 1; }
	memset(jit_x87_memo, 0, sizeof jit_x87_memo);   // (the first copy's pool, emptied)
}
// A read-modify-write in a page whose writes are checked (STUB_RMW) comes here first: RAM with translated code in its
// page but not in the 64-byte lines it touches (jit_codelines; data beside code: a DOS program's variables, a
// benchmark's stack in the buffer it copied its loop to) and not into the next page is plain RAM after all, t6 1;
// anything else goes on to t386_jit_rmw1/2/4 (jumped to: they return to the stub). t0 the linear address; every
// register but t6 kept.
static u32 *jit_rmw_chk[3];
static void jit_make_rmw_checks(void)
{
	static void (*const slows[3])(void) = { t386_jit_rmw1, t386_jit_rmw2, t386_jit_rmw4 };
	for (int w = 0; w < 3; w++) {
		const int n = 1 << w;
		jit_rmw_chk[w] = jp;
		e_addi(SP, SP, -16);
		e_sw(T1, SP, 8);
		e_sw(T2, SP, 4);
		e_sw(T3, SP, 0);
		u32 *slow[4];
		int ns = 0;
		e_srli(T1, T0, 12);
		e_add(T1, T1, R_MAP);
		e_lbu(T1, T1, 0);
		e_andi(T1, T1, ~(PM_CODE | PM_CODE_NEXT) & 0xff);
		slow[ns++] = e_branch(BNE, T1, X0);
		if (n > 1) {
			e_addi(T1, T0, n - 1);
			e_xor(T1, T1, T0);
			e_srli(T1, T1, 12);
			slow[ns++] = e_branch(BNE, T1, X0);
		}
		e_li(T3, (u32) (uintptr_t) jit_codelines);
		for (int k = 0; k < (n > 1 ? 2 : 1); k++) {   // the first byte's line, and the last's
			e_addi(T1, T0, k ? n - 1 : 0);
			e_srli(T1, T1, 6);
			e_srli(T2, T1, 3);
			e_add(T2, T2, T3);
			e_lbu(T2, T2, 0);
			e_andi(T1, T1, 7);
			e_srl(T2, T2, T1);
			e_andi(T2, T2, 1);
			slow[ns++] = e_branch(BNE, T2, X0);
		}
		e_lw(T1, SP, 8);
		e_lw(T2, SP, 4);
		e_lw(T3, SP, 0);
		e_addi(SP, SP, 16);
		e_li(T6, 1);
		e_jalr(X0, RA, 0);
		for (int k = 0; k < ns; k++) j_bind(slow[k], jp);
		e_lw(T1, SP, 8);
		e_lw(T2, SP, 4);
		e_lw(T3, SP, 0);
		e_addi(SP, SP, 16);
		const u32 t = (u32) (uintptr_t) slows[w];   // (through t6: ra is the stub's)
		const s32 lo = (s32) (t << 20) >> 20;
		e_lui(T6, t - (u32) lo);
		e_jalr(X0, T6, lo);
	}
}

static void jit_make_routines(void)   // (at jp)
{
	u32 *at = jp;
	jit_make_fstores();
	jit_make_vga_writes();
	jit_make_rmw_checks();
	jit_make_shared();
	jit_rt_at = at;
	if ((u8 *) jp - (u8 *) at > JIT_RT_MAX) abort();
	jit_x87_free = jp;   // the pool: empty
	jp += JIT_X87_POOL / 4;
	jit_x87_end = jp;
	memset(jit_x87_memo, 0, sizeof jit_x87_memo);
	jit_rt_bytes = (u8 *) jp - (u8 *) at;
	if (jit_rt_copies < 8) jit_rt_copy[jit_rt_copies++] = at;
}

// ---- classifying
static void jt_classify(JIns *in, u8 mode, bool *ends)
{
	u8 b = in->op;
	bool rm_mode = !(mode & JM_PROTECTED);
	in->kind = K_INTERP;
	in->fl = FL_READ;
	*ends = false;
	if (!in->two && b >= 0xd8 && b <= 0xdf) { in->kind = K_FPU; in->fl = FL_NONE; return; }   // x87 (jit_h_fpu)
	if (in->two) {
		if (b >= 0x80 && b <= 0x8f) { in->kind = K_JCC; *ends = true; }
		else if (b >= 0x90 && b <= 0x9f) in->kind = K_SETCC;
		else if (b == 0xaf) { in->kind = K_IMUL; in->fl = FL_WRITE; }
		else if ((b == 0xba && in->reg >= 4 && (in->mod == 3 || !(in->imm & 0xe0))) ||
			 ((b == 0xa3 || b == 0xab || b == 0xb3 || b == 0xbb) && (in->mod == 3 || !in->opsz16))) {
			in->kind = K_BT;   // BT BTS BTR BTC. They change CF only. (In memory, a bit number in a register can
			in->fl = FL_READ;  // be beyond the operand: 32-bit ones here; an immediate one, below 32 here.)
		}
		else if ((b == 0xa4 || b == 0xac) && !in->opsz16) {   // SHLD, SHRD r/m32, r32, imm8
			if (in->imm & 31) { in->kind = K_SHXD; in->fl = FL_WRITE; }
			else if (in->mod == 3) { in->kind = K_NOP; in->fl = FL_NONE; }   // (by 0: nothing, the flags as they were)
		}
		else if ((b == 0xa5 || b == 0xad) && !in->opsz16) in->kind = K_SHXD;   // ... by CL (FL_READ: by 0, nothing)
		else if (b == 0xa4 || b == 0xa5 || b == 0xac || b == 0xad) in->kind = K_SHXD16;   // 16-bit (FL_READ: by 0)
		else if ((b == 0xbc || b == 0xbd) && !in->opsz16) in->kind = K_BSCAN;   // BSF, BSR (FL_READ: ZF alone changes)
		else if (b >= 0xc8 && b <= 0xcf && !in->opsz16) { in->kind = K_BSWAP; in->fl = FL_NONE; }
		else if (b == 0x31) { in->kind = K_RDTSC; in->fl = FL_NONE; }
		else if (b == 0xb0 || b == 0xb1) { in->kind = K_CMPXCHG; in->fl = FL_WRITE; }
		else if (b == 0xc0 || b == 0xc1) { in->kind = K_XADD; in->fl = FL_WRITE; }
		else if (b == 0xb6 || b == 0xb7 || b == 0xbe || b == 0xbf) { in->kind = K_MOVX; in->fl = FL_NONE; }
		else if (b == 0xa0 || b == 0xa8) { in->kind = K_PUSH; in->fl = FL_NONE; }
		else if (b == 0xa1 || b == 0xa9) { in->kind = K_POPSEG; in->fl = FL_NONE; }
		else if ((b == 0xb4 || b == 0xb5) && in->mod != 3) { in->kind = K_LDSEG; in->fl = FL_NONE; }   // LFS, LGS
		else if ((b == 0x02 || b == 0x03) && (mode & JM_PROTECTED) && !(mode & JM_VM))
			in->kind = K_LARLSL;   // LAR, LSL (FL_READ: ZF alone changes)
		return;
	}
	if (b < 0x40 && (b & 7) < 6) {
		int op = b >> 3;
		if (op != 2 && op != 3) { in->kind = K_ALU; in->fl = FL_WRITE; }
		else in->kind = K_ADCSBB;   // reads CF
		return;
	}
	switch (b) {
	case 0x06: case 0x0e: case 0x16: case 0x1e: in->kind = K_PUSH; in->fl = FL_NONE; return;
	case 0x07: case 0x1f: in->kind = K_POPSEG; in->fl = FL_NONE; return;   // (protected mode: jit_h_ldseg)
	case 0x17:   // POP SS in real mode, as MOV SS (jit_translate keeps the instruction after it in the block)
		if (rm_mode && (mode & JM_STACK16)) { in->kind = K_POPSEG; in->fl = FL_NONE; }
		return;
	case 0x68: case 0x6a: in->kind = K_PUSH; in->fl = FL_NONE; return;
	case 0x69: case 0x6b: in->kind = K_IMUL; in->fl = FL_WRITE; return;
	case 0x80: case 0x81: case 0x82: case 0x83:
		if (in->reg != 2 && in->reg != 3) { in->kind = K_ALU; in->fl = FL_WRITE; }
		else in->kind = K_ADCSBB;
		return;
	case 0x84: case 0x85: case 0xa8: case 0xa9: in->kind = K_TEST; in->fl = FL_WRITE; return;
	case 0x86: case 0x87: in->kind = K_XCHG; in->fl = FL_NONE; return;
	case 0x88: case 0x89: case 0x8a: case 0x8b: case 0xa0: case 0xa1: case 0xa2: case 0xa3:
		in->kind = K_MOV; in->fl = FL_NONE; return;
	case 0x8c: if (in->reg <= 5) { in->kind = K_MOVSEG; in->fl = FL_NONE; } return;
	case 0x8d: if (in->mod != 3) { in->kind = K_LEA; in->fl = FL_NONE; } return;
	case 0x8e:   // (SS too, with a 16-bit stack as it leaves it: jit_translate keeps the instruction after it in the block)
		if (in->reg == SEG_ES || (in->reg == SEG_SS && rm_mode && (mode & JM_STACK16)) || in->reg == SEG_DS ||
		    in->reg == SEG_FS || in->reg == SEG_GS) {
			in->kind = K_MOVSEG;
			in->fl = FL_NONE;
		}
		return;
	case 0x8f:   // POP (to memory: not if its address uses ESP, which it does after ESP moves: the interpreter)
		if (in->reg == 0 && (in->mod == 3 || (in->base != 4 && in->index != 4))) { in->kind = K_POP; in->fl = FL_NONE; }
		return;
	case 0x90: in->kind = K_NOP; in->fl = FL_NONE; return;
	case 0x9b: in->kind = K_FWAIT; in->fl = FL_NONE; return;
	case 0x98: in->kind = K_CBW; in->fl = FL_NONE; return;
	case 0x99: in->kind = K_CWD; in->fl = FL_NONE; return;
	case 0xc0: case 0xc1: case 0xd0: case 0xd1: {
		int bits = (b & 1) ? (in->opsz16 ? 16 : 32) : 8;
		int n = (b < 0xd0 ? in->imm : 1) & 31;
		if (in->reg >= 4 && n > 0 && n < bits) { in->kind = K_SHIFT; in->fl = FL_WRITE; }
		else if (in->reg < 2 && (b < 0xd0 ? in->imm : 1) >= 1 && (b < 0xd0 ? in->imm : 1) < (u32) bits) in->kind = K_ROT;
		else if (in->reg < 4 && in->reg >= 2 && n == 1 && (b >= 0xd0 || in->imm == 1)) in->kind = K_ROT;
		else if (in->reg < 4 && in->reg >= 2 && b < 0xd0) in->kind = K_RCX;   // (FL_READ: CF in, by 0 nothing)
		return;
	}
	case 0xd2: case 0xd3:
		if (in->reg >= 4) in->kind = K_SHIFTCL; else if (in->reg < 2) in->kind = K_ROTCL; else in->kind = K_RCX;
		return;
	case 0x27: case 0x2f: case 0x37: case 0x3f: in->kind = K_BCD; return;   // (FL_READ: CF and AF in)
	case 0xd4: case 0xd5: if (b == 0xd5 || (in->imm & 0xff)) in->kind = K_BCD; return;   // (AAM by 0: #DE, run in place)
	case 0xe4: case 0xe5: case 0xec: case 0xed: if (mode & JM_IO) { in->kind = K_IN; in->fl = FL_NONE; } return;
	case 0xe6: case 0xe7: case 0xee: case 0xef: if (mode & JM_IO) { in->kind = K_OUT; in->fl = FL_NONE; } return;
	case 0xa4: case 0xa5: case 0xaa: case 0xab: case 0xac: case 0xad:
		if (!in->rep) { in->kind = K_STR; in->fl = FL_NONE; }
		else in->kind = K_REPSTR;   // (FL_READ, as run in place)
		return;
	case 0xa6: case 0xa7: case 0xae: case 0xaf:
		if (!in->rep) { in->kind = K_STR; in->fl = FL_WRITE; }
		else in->kind = K_REPCMP;   // (FL_READ: a count of 0 leaves the flags; run in place, it reads them)
		return;
	case 0xf5: case 0xf8: case 0xf9: in->kind = K_FLAGOP; return;
	case 0xfa: case 0xfb: if (mode & JM_IO) { in->kind = K_FLAGOP; in->fl = FL_NONE; } return;   // CLI, STI
	case 0xc8: if (in->imm2 == 0) { in->kind = K_ENTER; in->fl = FL_NONE; } return;   // (nesting level 0)
	case 0x60: in->kind = K_PUSHA; in->fl = FL_NONE; return;
	case 0x61: in->kind = K_POPA; in->fl = FL_NONE; return;
	case 0xd7: in->kind = K_XLAT; in->fl = FL_NONE; return;
	case 0x9f: in->kind = K_LAHF; return;
	case 0x9e: in->kind = K_SAHF; return;
	case 0x9c: if (!(mode & JM_VM)) in->kind = K_PUSHF; return;
	case 0x9d: if (!(mode & JM_VM)) in->kind = K_POPF; return;   // (FL_READ: the flags as they were if it faults)
	case 0xc2: case 0xc3: in->kind = K_RET; *ends = true; return;
	case 0xc4: case 0xc5: if (in->mod != 3) { in->kind = K_LDSEG; in->fl = FL_NONE; } return;
	case 0xc6: case 0xc7: if (in->reg == 0) { in->kind = K_MOV; in->fl = FL_NONE; } return;
	case 0xc9: in->kind = K_LEAVE; in->fl = FL_NONE; return;
	case 0xe0: case 0xe1: in->kind = K_LOOP; *ends = true; return;
	case 0xe2: case 0xe3: in->kind = K_LOOP; in->fl = FL_NONE; *ends = true; return;
	case 0xe8: in->kind = K_CALL; in->fl = FL_NONE; *ends = true; return;
	case 0xe9: case 0xeb: in->kind = K_JMP; in->fl = FL_NONE; *ends = true; return;
	case 0xf6: case 0xf7:
		if (in->reg < 2) { in->kind = K_TEST; in->fl = FL_WRITE; }
		else if (in->reg == 2) { in->kind = K_NOTNEG; in->fl = FL_NONE; }
		else if (in->reg == 3) { in->kind = K_NOTNEG; in->fl = FL_WRITE; }
		else if (in->reg < 6) { in->kind = K_MULDIV; in->fl = FL_WRITE; }
		else { in->kind = K_MULDIV; in->fl = FL_NONE; }   // DIV, IDIV leave the flags
		return;
	case 0xfc: in->kind = K_CLD; in->fl = FL_NONE; return;
	case 0xfd: in->kind = K_STD; in->fl = FL_NONE; return;
	case 0xfe: if (in->reg < 2) in->kind = K_INCDEC; return;
	case 0xff:
		if (in->reg < 2) in->kind = K_INCDEC;
		else if (in->reg == 2) { in->kind = K_CALLI; in->fl = FL_NONE; *ends = true; }
		else if (in->reg == 4) { in->kind = K_JMPI; in->fl = FL_NONE; *ends = true; }
		else if (in->reg == 6) { in->kind = K_PUSH; in->fl = FL_NONE; }
		else if ((in->reg == 3 || in->reg == 5) && rm_mode && in->mod != 3) { in->kind = K_FARIND; in->fl = FL_NONE; *ends = true; }
		else if ((in->reg == 3 || in->reg == 5) && (mode & JM_PROTECTED) && in->mod != 3) { in->kind = K_PMFAR; *ends = true; }
		else *ends = true;   // far CALL, JMP
		return;
	case 0x9a: case 0xea: case 0xca: case 0xcb: case 0xcd: case 0xcf:
		*ends = true;   // far transfers, INT, IRET: in real mode here, else the interpreter; the block ends
		if (rm_mode) {
			in->kind = b == 0x9a ? K_CALLF : b == 0xea ? K_JMPF : b == 0xcd ? K_INT : b == 0xcf ? K_IRET : K_RETF;
			in->fl = b == 0xcd ? FL_READ : FL_NONE;   // (INT pushes the flags)
			if ((b == 0xcf && !in->opsz16) || ((mode & JM_VM) && (b == 0xcd || b == 0xcf))) { in->kind = K_INTERP; in->fl = FL_READ; }
		} else if (mode & JM_PROTECTED) {
			in->kind = K_PMFAR;   // (tiny386's pmcall, pmret, call_isr: jit_h_pmfar)
		}
		return;
	case 0xcc: case 0xce: case 0xf4:
		*ends = true;   // INT3, INTO, HLT: the interpreter
		return;
	}
	if (b >= 0x40 && b <= 0x4f) { in->kind = K_INCDEC; return; }
	if (b >= 0x50 && b <= 0x57) { in->kind = K_PUSH; in->fl = FL_NONE; return; }
	if (b >= 0x58 && b <= 0x5f) { in->kind = K_POP; in->fl = FL_NONE; return; }
	if (b >= 0x70 && b <= 0x7f) { in->kind = K_JCC; *ends = true; return; }
	if (b >= 0x91 && b <= 0x97) { in->kind = K_XCHG; in->fl = FL_NONE; return; }
	if (b >= 0xb0 && b <= 0xbf) { in->kind = K_MOV; in->fl = FL_NONE; return; }
}

// the segments a block's own memory accesses use (protected mode: checked against null selectors on entry)
static unsigned jt_segs_used(const JIns *in)
{
	switch (in->kind) {
	case K_STR:
		return 1 << SEG_ES | 1 << (in->seg >= 0 ? in->seg : SEG_DS);
	case K_XLAT:
		return 1 << (in->seg >= 0 ? in->seg : SEG_DS);
	case K_PUSHA: case K_POPA: case K_PUSHF: case K_POPF:
		return 1 << SEG_SS;
	case K_CALLF: case K_RETF: case K_INT: case K_IRET:
		return 1 << SEG_SS;
	case K_PMFAR:
		return 1 << SEG_SS | (in->op == 0xff ? 1 << (in->seg >= 0 ? in->seg : in->dseg) : 0);
	case K_FARIND:
		return 1 << SEG_SS | 1 << (in->seg >= 0 ? in->seg : in->dseg);
	case K_PUSH: case K_POP: case K_CALL: case K_RET: case K_CALLI: case K_LEAVE: case K_POPSEG: case K_ENTER:
		return 1 << SEG_SS | (in->has_modrm && in->mod != 3 ? 1 << (in->seg >= 0 ? in->seg : in->dseg) : 0);
	case K_MOV:
		if (!in->two && in->op >= 0xa0 && in->op <= 0xa3) return 1 << (in->seg >= 0 ? in->seg : SEG_DS);
		break;
	case K_INTERP:
		return 0;
	case K_REPSTR: case K_REPCMP: {   // MOVS, CMPS: both; STOS, SCAS: ES:(E)DI; LODS: seg:(E)SI
		const unsigned src = 1 << (in->seg >= 0 ? in->seg : SEG_DS);
		const u8 b = in->op;
		return b >= 0xac && b <= 0xad ? src : b == 0xaa || b == 0xab || b >= 0xae ? 1 << SEG_ES : 1 << SEG_ES | src;
	}
	}
	if (in->has_modrm && in->mod != 3 && in->kind != K_LEA) return 1 << (in->seg >= 0 ? in->seg : in->dseg);
	return 0;
}

// The segment an instruction loads (jt_seg_load): after it, in the block, it isn't null (a null selector leaves the
// block before the instruction, jit_h_ldseg), so the entry check leaves out the segments a block loads before using
// them (16-bit protected-mode code: LES BX,[BP+6] / MOV AX,ES:[BX] with ES null on entry)
static unsigned jt_seg_loaded(const JIns *in)
{
	const int b = in->op;
	switch (in->kind) {
	case K_POPSEG:
		return 1u << (in->two ? (b == 0xa1 ? SEG_FS : SEG_GS) : b >> 3);
	case K_MOVSEG:
		return b == 0x8e ? 1u << in->reg : 0;
	case K_LDSEG:
		return 1u << (in->two ? (b == 0xb4 ? SEG_FS : SEG_GS) : b == 0xc4 ? SEG_ES : SEG_DS);
	}
	return 0;
}

// ---- emitting one instruction

// the r/m operand's value (bits wide, zero-extended) into d; rmw: keep its host address in t1 for jt_put_e
static bool jt_rmw_ins(const JIns *in)   // does it read-modify-write a memory operand (jt_get_e(..., true))?
{
	if (!in->has_modrm || in->mod == 3) return false;
	switch (in->kind) {
	case K_ALU: return (in->op < 0x40 ? (in->op & 7) < 2 : true) && (in->op < 0x40 ? in->op >> 3 : in->reg) != 7;
	case K_INCDEC: return in->op >= 0xfe;
	case K_NOTNEG: case K_SHIFT: case K_ROT: case K_SHIFTCL: case K_ROTCL: case K_RCX: case K_SHXD16: return true;
	case K_ADCSBB: return in->op < 0x40 ? (in->op & 7) < 2 : true;
	case K_XCHG: return in->op == 0x86 || in->op == 0x87;
	case K_BT: return in->op == 0xba ? in->reg != 4 : in->op != 0xa3;
	case K_SHXD: case K_CMPXCHG: case K_XADD: return true;
	}
	return false;
}

// does it only read memory (no write, which may find translated code there and leave the block: jit_h_write)?
static bool jt_reads_only(const JIns *in)
{
	u8 b = in->op;
	switch (in->kind) {
	case K_MOV: return !in->two && (b == 0x8a || b == 0x8b || b == 0xa0 || b == 0xa1);
	case K_MOVX: case K_TEST: case K_IMUL: case K_XLAT: case K_RET: case K_JMPI: return true;
	case K_BT: return in->op == 0xba ? in->reg == 4 : in->op == 0xa3;
	case K_BSCAN: return true;
	case K_POP: return in->op != 0x8f;   // (to a register)
	case K_ALU: return in->op < 0x40 ? (in->op & 7) == 2 || (in->op & 7) == 3 || in->op >> 3 == 7 : in->reg == 7;
	}
	return false;
}

static void jt_get_e(const JIns *in, int d, int bits, bool rmw)
{
	if (in->mod == 3) { jt_get_r(d, in->rm, bits); return; }
	jt_ea(in, 0);
	if (rmw) { jt_rmw_addr(in, bits / 8); jt_host_load(d, bits / 8); }
	else jt_load(d, bits / 8);
}
// the r/m operand = s (after jt_get_e(..., rmw) for memory, or a plain store)
static void jt_put_e(const JIns *in, int s, int bits, bool rmw, bool zext)
{
	if (in->mod == 3) { jt_set_r(in->rm, s, bits, zext); return; }
	if (rmw) { jt_host_store(s, bits / 8); return; }
	jt_ea(in, 0);
	jt_store(s, bits / 8);
}

// With paging, a push of s (a0 or a guest register) as a call to its routine (jit_r_push) and a word after it as a
// compact stub's (jt_stub_compact, with no resume nor flags: the pending flags written out first), if it can be
static bool jt_push_call(int s, int bytes, int store)
{
	if (!(jt_mode & JM_PAGED) || bytes != store || bytes == 1 || jt_no_redo) return false;
	int v = s == A0 ? 0 : -1;
	for (int i = 0; i < 8; i++)
		if (s == jg[i]) v = i + 1;
	uword off = jt_cs_base + jt_cur_eip - jt_lin;
	int refund = jt_refund;
#ifdef T386_JIT_VERIFY
	refund = 0;
#endif
	if (v < 0 || off > 0xfff || refund < 0 || refund > 63) return false;
	if (jt_pf > PF_DEAD && !jt_pf_stored) {
		if (!jt_fstore_of(jt_pf, jt_pf_bits)) return false;
		jt_flags_store();
	}
	e_call_near(jit_r_push[!!(jt_mode & JM_STACK16)][bytes == 4][v]);
	E((u32) refund << 13 | (u32) off << 19);
	jt_no_redo = true;
	return true;
}
// push s (bytes: the slot; store: what is written)
static void jt_push(int s, int bytes, int store)
{
	if (jt_push_call(s, bytes, store)) return;
	if (jt_mode & JM_STACK16) {   // (the new SP, zero-extended: t0, kept by the store, less SS's base)
		jt_stack_ea(-bytes);
		jt_store(s, store);
		e_lw(T6, R_CPU, SEG_BASE_OFF(SEG_SS));
		e_sub(A6, T0, T6);
		jt_set_r(4, A6, 16, true);
		jt_no_redo = true;
		return;
	}
	jt_stack_ea(-bytes);
	jt_store(s, store);
	e_addi(jg[4], jg[4], -bytes);
	jt_no_redo = true;   // (real mode's far CALL and INT push again)
}
// push a, then b (n bytes each): (E)SP moved once both are stored (with paging, a fault on the second leaves the
// instruction undone, for the interpreter to do again). The second, into translated code, is done in place: the
// first may have overwritten what the instruction read (a far CALL's pointer, just above the stack).
static void jt_push2(int a, int b, int n)
{
	jt_stack_ea(-n);
	jt_store(a, n);
	jt_no_redo = true;
	jt_stack_ea(-2 * n);
	jt_store(b, n);
	if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], -2 * n); jt_set_r(4, A6, 16, false); }
	else e_addi(jg[4], jg[4], -2 * n);
	jt_no_redo = true;
}
// With paging, a pop as a call to its routine (jit_r_pop[..][..][to]) and a word after it, as jt_push_call's
static bool jt_pop_call(int to, int bytes)
{
	if (!(jt_mode & JM_PAGED) || (bytes != 2 && bytes != 4) || to < 0 || !jit_r_pop[0][bytes == 4][to]) return false;
	uword off = jt_cs_base + jt_cur_eip - jt_lin;
	int refund = jt_refund;
#ifdef T386_JIT_VERIFY
	refund = 0;
#endif
	if (off > 0xfff || refund < 0 || refund > 63) return false;
	if (jt_pf > PF_DEAD && !jt_pf_stored) {
		if (!jt_fstore_of(jt_pf, jt_pf_bits)) return false;
		jt_flags_store();
	}
	e_call_near(jit_r_pop[!!(jt_mode & JM_STACK16)][bytes == 4][to]);
	E((u32) refund << 13 | (u32) off << 19);
	return true;
}
// pop into d, zero-extended (not ESP)
static void jt_pop(int d, int bytes)
{
	int to = d == A0 ? 0 : d == A1 ? 1 : -1;
	for (int i = 0; i < 8; i++)
		if (d == jg[i]) to = i + 2;
	if (jt_pop_call(to, bytes)) return;
	jt_stack_ea(0);
	jt_load(d, bytes);
	if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], bytes); jt_set_r(4, A6, 16, false); }
	else e_addi(jg[4], jg[4], bytes);
}
static int jit_h_ldseg(CPUI386 *cpu, int s, uword sel);
static void jt_seg_load_rm(int s, int v);

// Load segment register s (DS ES FS GS in protected mode; any in real mode) with the selector in v. Protected mode:
// tiny386's set_seg (jit_h_ldseg), and if the selector is null or it would fault, the block is left before the
// instruction (nothing of it done yet), for the interpreter.
static void jt_seg_load(int s, int v)
{
	if (jt_mode & JM_PROTECTED) {
		if (v != A2) e_mv(A2, v);
		e_mv(A0, R_CPU);
		e_li(A1, s | (jt_mode & JM_FLAT) << 8);
		e_call((const void *) jit_h_ldseg);
		JStub *st = jt_stub(STUB_EXIT);
		st->at = e_branch(BNE, A0, X0);
		return;
	}
	jt_seg_load_rm(s, v);
}

static void jt_seg_load_rm(int s, int v)   // real mode
{
	e_sw(v, R_CPU, SEG_SEL_OFF(s));
	e_slli(T6, v, 4);
	e_sw(T6, R_CPU, SEG_BASE_OFF(s));
	e_li(T6, 0xffff);
	e_sw(T6, R_CPU, SEG_LIMIT_OFF(s));
	e_sw(X0, R_CPU, SEG_FLAGS_OFF(s));
	if (s == SEG_CS && !(jt_mode & JM_CODE16)) {   // 32-bit code just after protected mode was left: 16-bit now
		e_li(T6, 1);
		e_sb(T6, R_CPU, CPU_OFF(code16));
		e_sw(X0, R_CPU, CPU_OFF(cpl));
	}
}


static void jt_interp(const JIns *in)
{
	jt_flags_store();
	e_li(A1, in->eip);
	e_addi(A2, A1, in->len);
	e_call_near(jit_r_interp);
	JStub *st = jt_stub(STUB_STOP);
	st->at = e_branch(BNE, A0, X0);
	jt_pf = PF_NONE;
}

// Self-checking blocks: before each instruction, compare its bytes with those it was translated from; a change
// sends it to the interpreter (the first one's: noted, see jit_exec).

static void jt_check_bytes(const JIns *in, const u8 *code, bool first)
{
	e_li(T0, jt_cs_base + in->eip + jt_phys_off);
	e_add(T0, T0, R_MEM);
	JStub *st = jt_stub(first ? STUB_CHECK0 : STUB_EXIT);
	st->eip = in->eip;
	int k = in->op_dyn, nb = 0, len = in->imm_dyn ? in->imm_at : in->len;   // (a rewritten immediate may change;
	                                                                        // a rewritten Jcc checks its opcode)
	while (k < len) {
		int c = len - k >= 4 ? 4 : len - k >= 2 ? 2 : 1;
		u32 v = code[k] | (c > 1 ? (u32) code[k + 1] << 8 : 0) | (c > 2 ? (u32) code[k + 2] << 16 | (u32) code[k + 3] << 24 : 0);
		if (c == 4) e_lw(T1, T0, k); else if (c == 2) e_lhu(T1, T0, k); else e_lbu(T1, T0, k);
		e_li(T2, v);
		u32 *br = e_branch(BNE, T1, T2);
		if (nb == 0) st->at = br; else st->more[nb - 1] = br;
		nb++;
		k += c;
	}
}

// Leave for the x86 address in a1: through the indirect jump cache, else the dispatcher. The CS base is the block's
// (csreg -1), or in csreg after a far transfer. With paging, entries have the CS base xor the TLB in use (r_map: the
// privilege level's, as the page may be a supervisor one), and go when the TLB is flushed (jit_ijc_unmap).
static void e_jump_abs(const u32 *target)   // (through ra)
{
	u32 t = (u32) (uintptr_t) target;
	s32 lo = (s32) (t << 20) >> 20;
	e_lui(RA, t - (u32) lo);
	e_jalr(X0, RA, lo);
}

// on to the block at CS:a1 (CS based at csreg, or at the block's CS base if -1) through the indirect jump cache,
// or leave for the dispatcher: jit_r_ijc with t0 the linear address, t6 the entry's CS key, a3 the mode
static void jt_ijc_exit(int csreg)
{
	bool paged = jt_mode & JM_PAGED;
	if (csreg >= 0) {
		e_add(T0, A1, csreg);
		if (paged) e_xor(T6, csreg, R_MAP); else e_mv(T6, csreg);
	} else if (jt_cs_base) {
		e_li(T6, jt_cs_base);
		e_add(T0, A1, T6);
		if (paged) e_xor(T6, T6, R_MAP);
	} else {
		e_mv(T0, A1);
		e_mv(T6, paged ? R_MAP : X0);
	}
	e_li(A3, jt_mode);
	e_jump_abs(jit_r_ijc);
}

// d = the instruction's immediate (v, as translated), or, if the program rewrites it (in->imm_dyn), read where it is
static void jt_imm(const JIns *in, int d, u32 v)
{
	if (!in->imm_dyn) { e_li(d, v); return; }
	e_li(d, jt_cs_base + in->eip + jt_phys_off + in->imm_at);
	e_add(d, d, R_MEM);
	if (in->imm_sz == 4) e_lw(d, d, 0);
	else if (in->imm_sz == 2) e_lhu(d, d, 0);
	else if (!in->two && in->op == 0x83) e_lb(d, d, 0);   // (sign-extended, as v)
	else e_lbu(d, d, 0);
}

static uword jt_target(const JIns *in, u32 disp)
{
	uword t = in->eip + in->len + disp;
	return (jt_mode & JM_CODE16) ? (t & 0xffff) : t;
}

// The x87 forms with quick inline code (the frequent ones, on registers kept as singles): their helper for the rest
static const void *jt_fpu_reg_fn(int esc, int g)
{
	if ((esc == 0 || esc == 4 || esc == 6) && jt_fpu_reg_helper[g]) return jt_fpu_reg_helper[g];
	return esc == 1 && g == 0 ? (const void *) t386_fpu_fld_reg : esc == 1 && g == 1 ? (const void *) t386_fpu_fxch_reg :
	       esc == 5 && g == 2 ? (const void *) t386_fpu_fst_reg : esc == 5 && g == 3 ? (const void *) t386_fpu_fstp_reg :
	       (const void *) t386_fpu_reg;
}
// in's quick code (a memory operand in a1 already; a value to store left in a0), its helper's call after it for the
// rest (jt_fpu_slow); false, nothing made: in has none
static bool jt_fpu_core(const JIns *in, bool make)
{
	const u8 b = in->op;
	if (in->mod != 3) {
		const struct JitFpuForm ff = jt_fpu_form(b - 0xd8, in->reg);
		if (ff.how == FF_ARITHG && jt_fpu_single()) {   // FADD.. m32: ST(0) op= the operand
			if (!make) return true;
			jt_fpu_begin();
			jt_fpu_cw_as_translated();
			jt_fpu_kept(A3);
			jt_fpu_nz(A1);
			jt_fpu_sb(A2, A5, A3);
			jt_fpu_arith(in->reg, T2, A2, A1);   // (t2: a call's to change, nothing kept in it here)
			e_sw(T2, A5, jt_fl.sb);
			jt_fpu_dirty(A3);
			jt_fpu_slow(ff.fn, -1, -1);
			return true;
		}
		if (ff.how == FF_LD && ff.bytes == 4 && b == 0xd9) {   // FLD m32: pushed
			if (!make) return true;
			jt_fpu_begin();
			jt_fpu_nz(A1);
			e_addi(A3, A3, -1);
			e_andi(A3, A3, 7);
			jt_fpu_sb(X0, A5, A3);
			e_sw(A1, A5, jt_fl.sb);
			jt_fpu_mark(A3);
			jt_fpu_set_top(A3);
			jt_fpu_slow(ff.fn, -1, -1);
			return true;
		}
		if (ff.how == FF_LD && ff.bytes == 4 && b == 0xdb) {   // FILD m32: pushed as a single, if exactly one
			if (!make) return true;
			jt_fpu_begin();
			e_srai(T6, A1, 24);   // (-2^24 .. 2^24 - 1: fcvt.s.w exact, as t386_fpu_ld_int's)
			e_addi(T6, T6, 1);
			e_sltiu(T6, T6, 2);
			jt_fslow_if(BEQ, T6, X0);
			e_fcvt_s_w(0, A1);
			e_fmv_x_w(A1, 0);
			e_addi(A3, A3, -1);
			e_andi(A3, A3, 7);
			jt_fpu_sb(X0, A5, A3);
			e_sw(A1, A5, jt_fl.sb);
			jt_fpu_mark(A3);
			jt_fpu_set_top(A3);
			jt_fpu_slow(ff.fn, -1, -1);
			return true;
		}
		if ((ff.how == FF_ST || ff.how == FF_STP) && ff.bytes == 4 && b == 0xd9) {   // FST(P) m32: ST(0) kept as a single
			if (!make) return true;
			jt_fpu_begin();
			jt_fpu_kept(A3);
			jt_fpu_sb(X0, A5, A3);
			e_lw(A0, A5, jt_fl.sb);
			jt_fpu_slow(ff.fn, -1, -1);   // (back here with the helper's a0)
			return true;
		}
		if ((ff.how == FF_ST || ff.how == FF_STP) && ff.bytes == 4 && b == 0xdb && jt_fpu_single()) {
			// FIST(P) m32: ST(0) kept as a single, below 2^30 (t386_hw_f32_to_i32's test), by fcvt.w.s in the
			// rounding mode of the translation
			if (!make) return true;
			jt_fpu_begin();
			jt_fpu_cw_as_translated();
			jt_fpu_kept(A3);
			jt_fpu_sb(A2, A5, A3);
			e_srli(T6, A2, 23);
			e_andi(T6, T6, 0xff);
			e_sltiu(T6, T6, 127 + 30);
			jt_fslow_if(BEQ, T6, X0);
			e_fmv_w_x(0, A2);
			e_fcvt_w_s(A0, 0, jt_x87_rm[(jt_x87_cw >> 10) & 3]);
			jt_fpu_slow(ff.fn, -1, -1);
			return true;
		}
		return false;
	}
	const int esc = b - 0xd8, g = in->reg, i = in->rm;
	if (b == 0xd9 && g == 5 && (i == 0 || i == 6)) {   // FLD1, FLDZ: a single pushed
		if (!make) return true;
		jt_fpu_begin();
		e_addi(A3, A3, -1);
		e_andi(A3, A3, 7);
		jt_fpu_sb(X0, A5, A3);
		e_li(T0, i == 0 ? 0x3f800000 : 0);
		e_sw(T0, A5, jt_fl.sb);
		jt_fpu_mark(A3);
		jt_fpu_set_top(A3);
		return true;
	}
	const void *fn = jt_fpu_reg_fn(esc, g);
	const bool arith = (esc == 0 || esc == 4 || esc == 6) && jt_fpu_reg_helper[g];
	if (arith && jt_fpu_single()) {   // ST(0) or ST(i) = ST(0) op ST(i), DE's popped
		if (!make) return true;
		jt_fpu_begin();
		jt_fpu_cw_as_translated();
		jt_fpu_kept(A3);
		jt_fpu_sti(A6, i);
		jt_fpu_kept(A6);
		jt_fpu_sb(A2, A5, A3);
		jt_fpu_sb(T0, A1, A6);
		jt_fpu_arith(g, T2, A2, T0);
		e_sw(T2, esc ? A1 : A5, jt_fl.sb);
		jt_fpu_dirty(esc ? A6 : A3);
		if (esc == 6) {
			e_addi(A3, A3, 1);
			jt_fpu_set_top(A3);
		}
	} else if (fn == (const void *) t386_fpu_fxch_reg) {   // both kept as singles, swapped
		if (!make) return true;
		jt_fpu_begin();
		jt_fpu_kept(A3);
		jt_fpu_sti(A6, i);
		jt_fpu_kept(A6);
		jt_fpu_sb(A2, A5, A3);
		jt_fpu_sb(T0, A1, A6);
		e_sw(T0, A5, jt_fl.sb);
		e_sw(A2, A1, jt_fl.sb);
		jt_fpu_dirty(A3);
		jt_fpu_dirty(A6);
	} else if (fn == (const void *) t386_fpu_fld_reg) {   // ST(i) kept as a single, pushed
		if (!make) return true;
		jt_fpu_begin();
		jt_fpu_sti(A6, i);
		jt_fpu_kept(A6);
		jt_fpu_sb(T0, A1, A6);
		e_addi(A3, A3, -1);
		e_andi(A3, A3, 7);
		jt_fpu_sb(X0, A5, A3);
		e_sw(T0, A5, jt_fl.sb);
		jt_fpu_mark(A3);
		jt_fpu_set_top(A3);
	} else if (fn == (const void *) t386_fpu_fst_reg || fn == (const void *) t386_fpu_fstp_reg) {
		// FST(P) ST(i): ST(0) kept as a single, copied
		if (!make) return true;
		jt_fpu_begin();
		jt_fpu_kept(A3);
		jt_fpu_sti(A6, i);
		jt_fpu_sb(A2, A5, A3);
		jt_fpu_sb(X0, A1, A6);
		e_sw(A2, A1, jt_fl.sb);
		jt_fpu_mark(A6);
		if (g == 3) {
			e_addi(A3, A3, 1);
			jt_fpu_set_top(A3);
		}
	} else {
		return false;
	}
	jt_fpu_slow(fn, esc << 3 | g, i);
	return true;
}

// The quick code of each x87 form used is made once (per rounding mode), as a routine blocks call, in a pool after
// each copy of the routines (jit_make_routines: within a call's reach of the blocks that use it), the first time a
// block needs it there: a call instead of ~30 words in each block (the code cache's lines go further; in x87-heavy
// programs most translated code is x87 instructions). The helper's call in it for the rest, the return address kept.
static u32 *jit_x87_routine_of(const JIns *in)
{
	const u32 key = 1u << 31 | (u32) in->op << 16 | (in->mod == 3) << 15 | in->reg << 12 | (in->mod == 3 ? in->rm : 0) << 8 |
			((jt_x87_cw >> 8) & 0xf);   // (PC and RC: the translation's)
	unsigned h = (key ^ key >> 9 ^ key >> 17) & (JIT_X87_MEMO - 1);
	for (unsigned k = 0; k < JIT_X87_MEMO; k++, h = (h + 1) & (JIT_X87_MEMO - 1)) {
		if (jit_x87_memo[h].key == key) return jit_x87_memo[h].code;
		if (!jit_x87_memo[h].key) break;
	}
	if (jit_x87_memo[h].key || !jit_x87_free) return NULL;   // (full)
	u32 *const save_jp = jp, *const save_limit = jlimit;
	const bool save_over = joverflow;
	u32 *code = jp = jit_x87_free;
	jlimit = jit_x87_end;
	joverflow = false;
	jt_x87_routine = true;
	jt_fpu_core(in, true);
	if (jt_x87_routine) e_jalr(X0, RA, 0);   // (no helper: the return not made yet)
	jt_x87_routine = false;
	const bool ok = !joverflow;
	u32 *const end = jp;
	jp = save_jp, jlimit = save_limit, joverflow = save_over;
	if (!ok) {   // the pool is full
		jit_x87_free = NULL;
		return NULL;
	}
	t386_jit_sync(code, (u8 *) end - (u8 *) code);
	jit_x87_free = end;
	jit_x87_memo[h].key = key;
	jit_x87_memo[h].code = code;
	return code;
}
// in's quick code: its routine r called, or in place (no room in the pool); false: in has none
static bool jt_fpu_call_core(const JIns *in, u32 *r)
{
	if (!jt_fpu_core(in, false)) return false;
	if (r) e_call_near(r);
	else jt_fpu_core(in, true);
	return true;
}

// 32-bit DIV / IDIV of a 64-bit dividend hi:lo (EDX:EAX) by d, from translated code (K_MULDIV): the quotient, and the
// remainder in jit_div_rem; jit_div_fault 1 if the CPU raises #DE instead (d 0, or a quotient that doesn't fit)
static u32 jit_div_rem, jit_div_fault;
static u32 jit_h_div64(u32 lo, u32 hi, u32 d, u32 sgn)
{
	const uint64_t n = (uint64_t) hi << 32 | lo;
	jit_div_fault = 1;
	if (!d) return 0;
	if (sgn) {
		const int64_t sn = (int64_t) n, sd = (s32) d;
		if (sn == INT64_MIN && sd == -1) return 0;
		const int64_t q = sn / sd;
		if (q != (s32) q) return 0;
		jit_div_rem = (u32) (sn % sd);
		jit_div_fault = 0;
		return (u32) q;
	}
	const uint64_t q = n / d;
	if (q >> 32) return 0;
	jit_div_rem = (u32) (n % d);
	jit_div_fault = 0;
	return (u32) q;
}

// REP MOVS / STOS from translated code (K_REPSTR), a page's part at a time (both sides plain RAM): bytes of elements
// of n (kind & 7) at host address d, from s (MOVS: element by element where they overlap with d after s, as the CPU
// does, else as memcpy) or of the value s (kind & 0x100: STOS); with DF set (kind & 0x400) d and s are the highest
// element's and the elements go down from there, one by one. Returns bytes.
static u32 jit_h_repstr(u8 *d, uintptr_t s, u32 bytes, u32 kind)
{
	const u32 n = kind & 7;
	if (kind & 0x400) {   // DF set: d (and s) the highest element's, the elements down from there
		if (kind & 0x100) d -= bytes - n;   // (STOS: the same bytes whichever way they go)
		else {
			const u8 *src = (const u8 *) s;
			for (u32 i = 0; i < bytes; i += n) { u32 v; memcpy(&v, src - i, n); memcpy(d - i, &v, n); }
			return bytes;
		}
	}
	if (kind & 0x100) {
		if (n == 1) { memset(d, (int) s, bytes); return bytes; }
		for (u32 i = 0; i < bytes; i += n) memcpy(d + i, &s, n);   // (little-endian: the low n bytes)
		return bytes;
	}
	const u8 *src = (const u8 *) s;
	if (d + bytes <= src || d >= src + bytes || d < src) { memmove(d, src, bytes); return bytes; }   // (d before s:
	for (u32 i = 0; i < bytes; i += n) { u32 v; memcpy(&v, src + i, n); memcpy(d + i, &v, n); }   // as forwards)
	return bytes;
}

// REP MOVS / STOS between plain RAM and the VGA window (A0000-BFFFF), what jt_repstr's fast way doesn't take (not
// RAM): tried before the interpreter, without paging and with DF clear. jit_repio_regs: ECX, ESI, EDI in and out;
// kind: the element's bytes | 0x100 MOVS | 0x200 16-bit addresses | the source segment << 12; v: STOS's value.
// Each element as the interpreter writes or reads it (fast_io_write / fast_io_read: t386_vga_ext.h's fast paths,
// else tiny386's callbacks), the text modes' bulk write for a MOVS into the window. 1 if done, 0 to the interpreter
// (RAM with translated code to write, the A20 gate's wrapping, a 64 KB wrap, ...)
bool t386_vga_text_write_string(uint32_t off, const uint8_t *buf, int len);   // (t386_vga_ext.h)
static bool repio_pages(uword a, u32 bytes, bool write)   // plain RAM (to write: without translated code)?
{
	for (uword p = a >> 12; p <= (a + bytes - 1) >> 12; p++)
		if (write ? t386_pagemap[p] != PM_RAM : (s8) t386_pagemap[p] < 0) return false;
	return true;
}
static u32 jit_repio_regs[3];
static int jit_h_repio(CPUI386 *cpu, u32 kind, u32 v)
{
	const u32 n = kind & 7, a16 = kind & 0x200;
	const bool movs = kind & 0x100;
	const u32 m = a16 ? 0xffff : 0xffffffff;
	const u32 cnt = jit_repio_regs[0] & m, si = jit_repio_regs[1] & m, di = jit_repio_regs[2] & m;
	if (!cnt || cnt > 0x40000 || (cpu->cr0 & 0x80000000) || (cpu->flags & DF)) return 0;   // (forwards only)
	const u32 bytes = cnt * n;
	if (a16 && (di + bytes > 0x10000 || (movs && si + bytes > 0x10000))) return 0;
	const uword d = cpu->seg[SEG_ES].base + di, sl = movs ? cpu->seg[(kind >> 12) & 7].base + si : 0;
	const bool pm = cpu->cr0 & 1;
	u8 *mem = (u8 *) cpu->phys_mem;
#define REPIO_RAM(a, write) ((((a) + bytes <= 0xa0000) || (pm && (a) >= 0x100000 && (a) + bytes <= (uword) cpu->phys_mem_size)) && \
	                     repio_pages(a, bytes, write))
	if (d >= 0xa0000 && d + bytes <= 0xc0000 && (!movs || REPIO_RAM(sl, false))) {   // into the window
		if (!movs || !t386_vga_text_write_string(d - 0xa0000, mem + sl, bytes))
			for (u32 i = 0; i < bytes; i += n) {
				u32 x = v;
				if (movs) { x = 0; memcpy(&x, mem + sl + i, n); }
				fast_io_write(cpu, d + i, n, x);
			}
	} else if (movs && sl >= 0xa0000 && sl + bytes <= 0xc0000 && REPIO_RAM(d, true)) {   // out of it, into RAM
		for (u32 i = 0; i < bytes; i += n) {
			const u32 x = fast_io_read(cpu, sl + i, n);
			memcpy(mem + d + i, &x, n);
		}
	} else {
		return 0;
	}
#undef REPIO_RAM
	jit_repio_regs[0] &= ~m;
	if (movs) jit_repio_regs[1] = (jit_repio_regs[1] & ~m) | ((si + bytes) & m);
	jit_repio_regs[2] = (jit_repio_regs[2] & ~m) | ((di + bytes) & m);
	return 1;
}

// REPE / REPNE CMPS, SCAS from translated code (K_REPCMP), the same way: compares the elements of n (kind & 7) at
// host address s (CMPS; SCAS, kind & 0x100: the value s) with those at d, at most bytes of them, until one pair is
// unequal (REPE, kind & 0x200) or equal (REPNE); with DF set (kind & 0x400) from the highest element down. Returns
// the bytes compared; the last pair in jit_cmp_l (s's side),
// jit_cmp_r, and jit_cmp_more 1 if the repeat goes on (no pair stopped it: the count decides)
static u32 jit_cmp_l, jit_cmp_r, jit_cmp_more;
static u32 jit_h_repcmp(const u8 *d, uintptr_t s, u32 bytes, u32 kind)
{
	const u32 n = kind & 7, mask = n == 4 ? 0xffffffff : n == 2 ? 0xffff : 0xff;
	const bool scas = kind & 0x100, repe = kind & 0x200;
	const int step = kind & 0x400 ? -(int) n : (int) n;   // (DF set: down from the highest element)
	const u8 *src = (const u8 *) s;
	u32 i = 0, l = (u32) s & mask, r;
	int at = 0;
	bool eq;
	do {
		r = 0;
		memcpy(&r, d + at, n);
		if (!scas) { l = 0; memcpy(&l, src + at, n); }
		i += n;
		at += step;
		eq = l == r;
	} while (eq == repe && i < bytes);
	jit_cmp_l = l;
	jit_cmp_r = r;
	jit_cmp_more = eq == repe;
	return i;
}

// a1 = min(a1, t0)
static void jt_min_a1(void)
{
	u32 *k = e_branch(BGEU, T0, A1);
	e_mv(A1, T0);
	j_bind(k, jp);
}

// the linear address of seg:(E)SI or (E)DI (reg 6 or 7) into d, the bytes in a1 cut to what is left of its page
// (and of its 64 KB with a 16-bit address: the next part starts at offset 0, as the CPU wraps; with DF set, a5, what
// is left below it down to its page's start, or offset 0: the next part starts at the top), and to slow (the
// K_REPSTR's) unless that page is plain RAM (write: without translated code), and in real mode below 1 MB
// (misaligned is fine: jit_h_repstr copies with memcpy; a1 may come out no whole element, see jt_rep_parts). With
// paging, d is then its host address through the fast path's TLB, a miss walking the page tables and filling it
// (jit_h_reptlb); a fault, not RAM, or a write to a page with translated code (ftlb_miss gives such pages no write
// entry) to slow
static void jt_repstr_side(const JIns *in, int d, int reg, int seg, int n, bool write, u32 **slow, int *ns)
{
	const bool paged = jt_mode & JM_PAGED;
	if (in->adsz16) {   // (a5: DF, jt_rep_count. Set: down to offset 0, the bytes to the element's end)
		jt_zext(d, jg[reg], 16);
		e_li(T0, 0x10000);
		e_sub(T0, T0, d);
		u32 *fw = e_branch(BEQ, A5, X0);
		if (n > 1) {   // (down: the element at d not across 64 KB's end)
			e_li(T1, 0x10000 - n);
			slow[(*ns)++] = e_branch(BLTU, T1, d);
		}
		e_addi(T0, d, n);
		j_bind(fw, jp);
		jt_min_a1();
	} else {
		e_mv(d, jg[reg]);
	}
	if (jt_based(seg)) { e_lw(T6, R_CPU, SEG_BASE_OFF(seg)); e_add(d, d, T6); }
	e_slli(T6, d, 20);
	e_srli(T6, T6, 20);
	e_li(T0, 0x1000);
	e_sub(T0, T0, T6);
	u32 *fw = e_branch(BEQ, A5, X0);
	if (n > 1) {   // (DF set: the element at d not across its page's end)
		e_li(T1, 0x1000 - n);
		slow[(*ns)++] = e_branch(BLTU, T1, T6);
	}
	e_addi(T0, T6, n);   // (DF set: down to its page's start)
	j_bind(fw, jp);
	jt_min_a1();   // (to its page's end)
	if (!(jt_mode & JM_PROTECTED)) {   // (the A20 gate's wrapping: the interpreter's; paging needs protected mode)
		e_add(T6, d, A1);
		u32 *fw2 = e_branch(BEQ, A5, X0);
		e_addi(T6, d, n);   // (DF set: the part ends with the element at d)
		j_bind(fw2, jp);
		e_li(T0, 0x100000);
		slow[(*ns)++] = e_branch(BLTU, T0, T6);
	}
	if (paged) {   // (jit_r_tlb: t0 in, t1 out, t6 and a6 used)
		e_mv(T0, d);
		e_call_near(jit_r_tlb[write][0]);
		u32 *hit = e_branch(BNE, T1, X0);
		e_mv(A0, d);   // a miss: the page tables walked and the TLB filled (jit_h_reptlb), else to slow
		e_call(write ? (const void *) jit_h_reptlb_w : (const void *) jit_h_reptlb_r);
		slow[(*ns)++] = e_branch(BEQ, A0, X0);
		e_mv(T1, A0);
		j_bind(hit, jp);
		e_mv(d, T1);
		return;
	}
	e_srli(T6, d, 12);
	e_add(T6, T6, R_MAP);
	e_lb(T6, T6, 0);
	slow[(*ns)++] = write ? e_branch(BNE, T6, X0) : e_branch(BLT, T6, X0);
	e_add(d, d, R_MEM);   // (its host address)
}

// REP string instructions (K_REPSTR, K_REPCMP), a part at a time: the count into a0 (none: to *none, nothing
// done), DF into a5 (0 up, 1 down: kept to the end), a1 = its bytes, at most a page's worth (the sides cut it
// further: jt_repstr_side)
static void jt_rep_count(const JIns *in, int sh, u32 **none, u32 **slow, int *ns)
{
	if (in->adsz16) jt_zext(A0, jg[1], 16); else e_mv(A0, jg[1]);
	*none = e_branch(BEQ, A0, X0);
	e_lw(T6, R_CPU, CPU_OFF(flags));
	e_andi(T6, T6, DF);
	e_srli(A5, T6, 10);   // a5: DF (1: down)
	e_li(T0, 0x1000 >> sh);
	u32 *k = e_branch(BGEU, T0, A0);
	e_mv(A0, T0);
	j_bind(k, jp);
	e_slli(A1, A0, sh);
}

// ... after the sides: a1 whole elements, slow if not one (an element across a page's or 64 KB's end)
static void jt_rep_parts(int n, u32 **slow, int *ns)
{
	if (n > 1) e_andi(A1, A1, -n);
	slow[(*ns)++] = e_branch(BEQ, A1, X0);
}

// ... a0 bytes done: (E)DI and/or (E)SI on by them (back with DF set), the count down by their elements; t1 = the
// count left
static void jt_rep_done(const JIns *in, int sh, bool di, bool si)
{
	e_sub(A4, X0, A5);   // a4: the bytes, negated with DF set (a5 1; not t6: jt_set_r's)
	e_xor(A4, A0, A4);
	e_add(A4, A4, A5);
	for (int r = 7; r >= 6; r--) {
		if (r == 7 ? !di : !si) continue;
		e_add(T2, jg[r], A4);
		if (in->adsz16) jt_set_r(r, T2, 16, false); else e_mv(jg[r], T2);
	}
	e_srli(T1, A0, sh);
	e_sub(T2, jg[1], T1);
	if (in->adsz16) jt_set_r(1, T2, 16, false); else e_mv(jg[1], T2);
	if (in->adsz16) jt_zext(T1, jg[1], 16); else e_mv(T1, jg[1]);
}

// REP MOVS, STOS (jit_h_repstr), LODS (the last element into AL/AX/EAX), a page's part at a time while the count
// lasts, up or (DF set) down; the interpreter takes over (the rest) where a part isn't plain RAM, ...
static void jt_repstr(const JIns *in, int bits)
{
	const u8 b = in->op;
	const int n = (b & 1) ? bits / 8 : 1, sh = n == 1 ? 0 : n == 2 ? 1 : 2;
	const bool movs = b <= 0xa5, lods = b >= 0xac;
	jt_flags_store();   // (both ways: the interpreter's needs them, and after it the flags are in cpu->cc)
	u32 *slow[12], *none;
	int ns = 0;
	const u32 *top = jp;
	jt_rep_count(in, sh, &none, slow, &ns);
	if (!lods) jt_repstr_side(in, A2, 7, SEG_ES, n, true, slow, &ns);   // (host addresses: a2 the destination,
	if (movs || lods) jt_repstr_side(in, A3, 6, in->seg >= 0 ? in->seg : SEG_DS, n, false, slow, &ns);   // a3 the
	else jt_get_r(A3, 0, n * 8);                                                                         // source)
	jt_rep_parts(n, slow, &ns);
	if (lods) {   // (the last element: the part's highest, or with DF set its lowest)
		e_add(T0, A3, A1);
		e_addi(T0, T0, -n);
		u32 *fw = e_branch(BEQ, A5, X0);
		e_sub(T0, A3, A1);
		e_addi(T0, T0, n);
		j_bind(fw, jp);
		if (n == 4) e_lw(T2, T0, 0); else if (n == 2) e_lhu(T2, T0, 0); else e_lbu(T2, T0, 0);
		jt_set_r(0, T2, n * 8, true);
		e_mv(A0, A1);
	} else {
		e_mv(T1, A1);
		e_mv(A0, A2);
		e_mv(A2, T1);
		e_mv(A1, A3);
		e_li(A3, n | (movs ? 0 : 0x100));
		e_slli(T6, A5, 10);   // (DF set: 0x400)
		e_or(A3, A3, T6);
		e_call((const void *) jit_h_repstr);   // (a0: the bytes)
	}
	jt_rep_done(in, sh, !lods, movs || lods);
	e_branch_to(BNE, T1, X0, top);   // more: the next part
	u32 *done = e_jump();
	for (int k = 0; k < ns; k++) j_bind(slow[k], jp);
	u32 *done3 = NULL;
	if (!lods) {   // the VGA window and RAM (jit_h_repio), else the interpreter
		const uword regs = (uword) (uintptr_t) jit_repio_regs;
		e_li(T0, regs);
		e_sw(jg[1], T0, 0);
		e_sw(jg[6], T0, 4);
		e_sw(jg[7], T0, 8);
		if (!movs) jt_get_r(A2, 0, n * 8);
		e_li(A1, n | (movs ? 0x100 : 0) | (in->adsz16 ? 0x200 : 0) | (in->seg >= 0 ? in->seg : SEG_DS) << 12);
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_repio);
		u32 *no = e_branch(BEQ, A0, X0);
		e_li(T0, regs);
		e_lw(jg[1], T0, 0);
		e_lw(jg[6], T0, 4);
		e_lw(jg[7], T0, 8);
		done3 = e_jump();
		j_bind(no, jp);
	}
	jt_interp(in);
	u32 *done2 = e_jump();
	j_bind(none, jp);
	j_bind(done, jp);
	j_bind(done2, jp);
	if (done3) j_bind(done3, jp);
}

// REPE / REPNE CMPS, SCAS (jit_h_repcmp), the same way: the flags those of the last pair compared (in cpu->cc
// after it, both ways)
static void jt_repcmp(const JIns *in, int bits)
{
	const u8 b = in->op;
	const int n = (b & 1) ? bits / 8 : 1, sh = n == 1 ? 0 : n == 2 ? 1 : 2;
	const bool cmps = b <= 0xa7;
	jt_flags_store();
	u32 *slow[12], *none;
	int ns = 0;
	const u32 *top = jp;
	jt_rep_count(in, sh, &none, slow, &ns);
	jt_repstr_side(in, A2, 7, SEG_ES, n, false, slow, &ns);
	if (cmps) jt_repstr_side(in, A3, 6, in->seg >= 0 ? in->seg : SEG_DS, n, false, slow, &ns);
	else jt_get_r(A3, 0, n * 8);
	jt_rep_parts(n, slow, &ns);
	e_mv(T1, A1);
	e_mv(A0, A2);
	e_mv(A2, T1);
	e_mv(A1, A3);
	e_li(A3, n | (cmps ? 0 : 0x100) | (in->rep == 1 ? 0x200 : 0));
	e_slli(T6, A5, 10);   // (DF set: 0x400)
	e_or(A3, A3, T6);
	e_call((const void *) jit_h_repcmp);   // (a0: the bytes)
	jt_rep_done(in, sh, true, cmps);
	e_lw_abs(T2, &jit_cmp_l);
	jt_sext(F_SRC1, T2, n * 8);
	e_lw_abs(T2, &jit_cmp_r);
	jt_sext(F_SRC2, T2, n * 8);
	e_sub(F_DST, F_SRC1, F_SRC2);
	jt_sext(F_DST, F_DST, n * 8);
	jt_set_pf(PF_SUB, n * 8);
	jt_flags_store();
	e_lw_abs(T2, &jit_cmp_more);
	u32 *stop = e_branch(BEQ, T2, X0);   // a pair ended it
	e_branch_to(BNE, T1, X0, top);       // else the count: the next part
	j_bind(stop, jp);
	u32 *done = e_jump();
	for (int k = 0; k < ns; k++) j_bind(slow[k], jp);
	jt_interp(in);
	u32 *done2 = e_jump();
	j_bind(none, jp);
	j_bind(done, jp);
	j_bind(done2, jp);
	jt_pf = PF_NONE;
}

static JIT_SMALL void jt_emit(const JIns *in)
{
	int bits = in->opsz16 ? 16 : 32;
	u8 b = in->op;
	bool live = in->fl_live;
	switch (in->kind) {
	case K_NOP:
		break;
	case K_ALU: {
		int op, form;   // form: 0 E,G  1 G,E  2 A,imm  3 E,imm
		if (b < 0x40) {
			op = b >> 3;
			form = (b & 7) < 2 ? 0 : (b & 7) < 4 ? 1 : 2;
			if (!(b & 1)) bits = 8;
		} else {
			op = in->reg;
			form = 3;
			if (b == 0x80 || b == 0x82) bits = 8;
		}
		bool cmp = op == 7;
		bool mem_dest = (form == 0 || form == 3) && in->mod != 3;
		if (bits == 32 && !mem_dest) {   // a register: worked on in place; the operands kept only if the flags are read
			const int d = jg[form == 1 ? in->reg : form == 2 ? 0 : in->rm];
			int s = form == 1 ? (in->mod == 3 ? jg[in->rm] : A0) : form == 0 ? jg[in->reg] : -1;
			const u32 v = form == 3 && b == 0x83 ? (u32) (s32) (s8) in->imm : in->imm;
			if (s == A0) jt_get_e(in, A0, 32, false);   // (first: it may leave for the slow path, the flags as they were)
			if (live && (op == 0 || op == 5 || op == 7)) {
				if (s < 0) jt_imm(in, F_SRC2, v);
				else e_mv(F_SRC2, s);
				e_mv(F_SRC1, d);
				if (op == 0) e_add(F_DST, F_SRC1, F_SRC2);
				else e_sub(F_DST, F_SRC1, F_SRC2);
				if (!cmp) e_mv(d, F_DST);
				jt_set_pf(op == 0 ? PF_ADD : PF_SUB, 32);
				break;
			}
			if (cmp) { jt_pf = PF_DEAD; break; }
			if (s < 0) {
				const u32 u = op == 5 ? 0u - v : v;   // (unsigned: SUB's 0x80000000 negated without overflow)
				if (!in->imm_dyn && u + 2048 < 4096) {   // -2048 .. 2047: an I-type immediate
					const int k = (int) (u + 2048) - 2048;
					if (op == 0 || op == 5) e_addi(d, d, k);
					else if (op == 1) e_ori(d, d, k);
					else if (op == 4) e_andi(d, d, k);
					else e_xori(d, d, k);
					s = X0;
				} else {
					jt_imm(in, A0, v);
					s = A0;
				}
			}
			if (s != X0) {
				switch (op) {
				case 0: e_add(d, d, s); break;
				case 1: e_or(d, d, s); break;
				case 4: e_and(d, d, s); break;
				case 6: e_xor(d, d, s); break;
				default: e_sub(d, d, s); break;
				}
			}
			if (!live) { jt_pf = PF_DEAD; break; }
			e_mv(F_DST, d);
			jt_set_pf(PF_LOGIC, 32 | (op == 1 ? CC_OR : op == 4 ? CC_AND : CC_XOR) << 8);
			break;
		}
		// T2: the destination's value, A0: the source's
		if (form == 2) {
			jt_get_r(T2, 0, bits);
			jt_imm(in, A0, in->imm);
		} else if (form == 1) {
			jt_get_r(T2, in->reg, bits);
			jt_get_e(in, A0, bits, false);
		} else {
			jt_get_e(in, T2, bits, !cmp);
			if (form == 0) jt_get_r(A0, in->reg, bits);
			else jt_imm(in, A0, b == 0x83 ? (u32) (s32) (s8) in->imm : in->imm);
		}
		switch (op) {
		case 0: e_add(A1, T2, A0); break;
		case 1: e_or(A1, T2, A0); break;
		case 4: e_and(A1, T2, A0); break;
		case 6: e_xor(A1, T2, A0); break;
		default: e_sub(A1, T2, A0); break;   // SUB, CMP
		}
		if (!cmp) {
			if (form == 1) jt_set_r(in->reg, A1, bits, false);
			else if (form == 2) jt_set_r(0, A1, bits, false);
			else if (mem_dest) jt_host_store(A1, bits / 8);
			else jt_set_r(in->rm, A1, bits, false);
		}
		if (!live) { jt_pf = PF_DEAD; break; }
		if (op == 0 || op == 5 || op == 7) {
			jt_sext(F_SRC1, T2, bits);
			jt_sext(F_SRC2, A0, bits);
			jt_sext(F_DST, A1, bits);
			jt_set_pf(op == 0 ? PF_ADD : PF_SUB, bits);
		} else {
			jt_sext(F_DST, A1, bits);
			jt_set_pf(PF_LOGIC, bits | (op == 1 ? CC_OR : op == 4 ? CC_AND : CC_XOR) << 8);
		}
		break;
	}
	case K_SHXD: {   // SHLD / SHRD r/m32, r32, imm8 (1-31) or CL: tiny386's lazy state written out as it does (src1,
		         // dst, dst2); by CL & 31 = 0 nothing at all (the flags as they were: stored first)
		bool left = b == 0xa4 || b == 0xa5, cl = b == 0xa5 || b == 0xad;
		u32 *none = NULL;
		if (cl) {
			jt_flags_store();
			e_andi(A4, jg[1], 31);
			none = e_branch(BEQ, A4, X0);
		}
		jt_get_e(in, T2, 32, true);   // x
		jt_get_r(A0, in->reg, 32);    // y
		if (!cl) {
			int c = in->imm & 31;
			if (left) { e_slli(A1, T2, c); e_srli(A2, A0, 32 - c); }
			else { e_srli(A1, T2, c); e_slli(A2, A0, 32 - c); }
			e_or(A1, A1, A2);            // the result
			if (c == 1) e_mv(A3, T2);    // dst2: the result had the count been one less (CF, OF)
			else if (left) { e_slli(A3, T2, c - 1); e_srli(A2, A0, 33 - c); e_or(A3, A3, A2); }
			else { e_srli(A3, T2, c - 1); e_slli(A2, A0, 33 - c); e_or(A3, A3, A2); }
		} else {
			e_li(A5, 32);
			e_sub(A5, A5, A4);   // 32 - c (1-31)
			if (left) { e_sll(A1, T2, A4); e_srl(A2, A0, A5); }
			else { e_srl(A1, T2, A4); e_sll(A2, A0, A5); }
			e_or(A1, A1, A2);
			e_addi(A5, A4, -1);   // dst2: by c - 1, y's part shifted once more (none for c = 1)
			if (left) { e_sll(A3, T2, A5); e_srli(A2, A2, 1); }
			else { e_srl(A3, T2, A5); e_slli(A2, A2, 1); }
			e_or(A3, A3, A2);
		}
		jt_put_e(in, A1, 32, true, false);
		if (!live) {   // (a span drawer's: four in a row, then AND)
			if (none) j_bind(none, jp);
			jt_pf = cl ? PF_NONE : PF_DEAD;
			break;
		}
		jt_flags_store();   // (the state before: tiny386 changes only these fields)
		e_sw(T2, R_CPU, CPU_OFF(cc.src1));
		e_sw(A1, R_CPU, CPU_OFF(cc.dst));
		e_sw(A3, R_CPU, CPU_OFF(cc.dst2));
		e_li(T6, left ? CC_SHLD : CC_SHRD);
		e_sw(T6, R_CPU, CPU_OFF(cc.op));
		e_li(T6, CF | PF | ZF | SF | OF);
		e_sw(T6, R_CPU, CPU_OFF(cc.mask));
		if (none) j_bind(none, jp);
		jt_pf = PF_NONE;
		break;
	}
	case K_BSCAN: {   // BSF, BSR r32, r/m32: tiny386 takes the flags from cpu->flags as they are (cc.mask 0), with ZF
		          // set if the source is 0 (the destination as it was), else cleared
		jt_flags_store();
		jt_get_e(in, T2, 32, false);
		u32 *zero = e_branch(BEQ, T2, X0);
		if (b == 0xbc) { e_sub(A0, X0, T2); e_and(A0, A0, T2); }   // BSF: the lowest bit set alone
		else e_mv(A0, T2);
		e_li(A1, 0);
		for (int k = 16; k; k >>= 1) {   // the highest bit set's number, by halves
			e_srli(A2, A0, k);
			u32 *low = e_branch(BEQ, A2, X0);
			e_mv(A0, A2);
			e_addi(A1, A1, k);
			j_bind(low, jp);
		}
		jt_set_r(in->reg, A1, 32, false);
		if (live) { e_lw(T6, R_CPU, CPU_OFF(flags)); e_andi(T6, T6, ~ZF); e_sw(T6, R_CPU, CPU_OFF(flags)); }
		u32 *done = e_jump();
		j_bind(zero, jp);
		if (live) { e_lw(T6, R_CPU, CPU_OFF(flags)); e_ori(T6, T6, ZF); e_sw(T6, R_CPU, CPU_OFF(flags)); }
		j_bind(done, jp);
		if (live) e_sw(X0, R_CPU, CPU_OFF(cc.mask));
		jt_pf = PF_NONE;
		break;
	}
	case K_BSWAP: {   // BSWAP r32
		const int r = b & 7;
		jt_get_r(T2, r, 32);
		e_slli(A0, T2, 24);
		e_srli(A1, T2, 24);
		e_or(A0, A0, A1);
		e_li(A2, 0xff00);
		e_and(A1, T2, A2);
		e_slli(A1, A1, 8);
		e_or(A0, A0, A1);
		e_srli(A1, T2, 8);
		e_and(A1, A1, A2);
		e_or(A0, A0, A1);
		jt_set_r(r, A0, 32, false);
		break;
	}
	case K_RDTSC:   // EDX:EAX = tiny386's time stamp (jit_h_rdtsc: a0 its low half, jit_tsc_hi the high)
		e_call((const void *) jit_h_rdtsc);
		jt_set_r(0, A0, 32, false);
		e_lw_abs(A1, &jit_tsc_hi);
		jt_set_r(2, A1, 32, false);
		break;
	case K_FWAIT: {   // (F)WAIT: #NM if CR0's MP and TS are both set (the interpreter raises it), else nothing
		e_lw(T6, R_CPU, CPU_OFF(cr0));
		e_andi(T6, T6, 0xa);
		e_xori(T6, T6, 0xa);
		JStub *nm = jt_stub(STUB_EXIT);
		nm->at = e_branch(BEQ, T6, X0);
		break;
	}
	case K_CMPXCHG: {   // CMPXCHG r/m, r: the flags of (E)AX - r/m (vendor/patches/0014); equal: r/m = r, else
		            // (E)AX = r/m (r/m not written)
		if (b == 0xb0) bits = 8;
		jt_get_e(in, T2, bits, true);
		jt_get_r(A0, 0, bits);
		e_sub(A1, A0, T2);
		jt_zext(A2, A1, bits);
		u32 *ne = e_branch(BNE, A2, X0);
		jt_get_r(A3, in->reg, bits);
		jt_put_e(in, A3, bits, true, false);
		u32 *done = e_jump();
		j_bind(ne, jp);
		jt_set_r(0, T2, bits, false);
		j_bind(done, jp);
		if (!live) { jt_pf = PF_DEAD; break; }
		jt_sext(F_SRC1, A0, bits);
		jt_sext(F_SRC2, T2, bits);
		jt_sext(F_DST, A1, bits);
		jt_set_pf(PF_SUB, bits);
		break;
	}
	case K_XADD: {   // XADD r/m, r: r = r/m, then r/m = the sum (in that order, as tiny386: XADD r, r leaves the sum)
		if (b == 0xc0) bits = 8;
		jt_get_e(in, T2, bits, true);
		jt_get_r(A0, in->reg, bits);
		e_add(A1, T2, A0);
		jt_set_r(in->reg, T2, bits, false);
		jt_put_e(in, A1, bits, true, false);
		if (!live) { jt_pf = PF_DEAD; break; }
		jt_sext(F_SRC1, T2, bits);
		jt_sext(F_SRC2, A0, bits);
		jt_sext(F_DST, A1, bits);
		jt_set_pf(PF_ADD, bits);
		break;
	}
	case K_BT: {   // BT BTS BTR BTC: tiny386 sets CF and takes it out of the lazy state's mask, the rest as it was
		int op = b == 0xba ? in->reg : b == 0xa3 ? 4 : b == 0xab ? 5 : b == 0xb3 ? 6 : 7;
		jt_flags_store();
		if (b != 0xba && in->mod != 3) {   // in memory, by a register (32 bits): its dword, as tiny386 finds it
			jt_ea(in, 0);              // (the bit number unsigned: / 32 * 4)
			jt_get_r(A0, in->reg, 32);
			e_srli(A1, A0, 5);
			e_slli(A1, A1, 2);
			e_add(T0, T0, A1);
			if (op != 4) { jt_rmw_addr(in, 4); jt_host_load(T2, 4); }
			else jt_load(T2, 4);
		} else {
			jt_get_e(in, T2, bits, op != 4);
		}
		if (b == 0xba) {
			int n = in->imm & (bits - 1);
			e_srli(A1, T2, n);
			if (op != 4) e_li(A2, 1u << n);
		} else {
			jt_get_r(A0, in->reg, bits);
			e_andi(A0, A0, bits - 1);
			e_srl(A1, T2, A0);
			if (op != 4) { e_li(A2, 1); e_sll(A2, A2, A0); }
		}
		e_andi(A1, A1, 1);   // the bit: CF
		if (op == 5) e_or(T2, T2, A2);
		else if (op == 6) { e_xori(A2, A2, -1); e_and(T2, T2, A2); }
		else if (op == 7) e_xor(T2, T2, A2);
		if (op != 4) jt_put_e(in, T2, bits, true, false);
		if (live) {
			e_lw(T6, R_CPU, CPU_OFF(cc.mask));
			e_andi(T6, T6, ~CF);
			e_sw(T6, R_CPU, CPU_OFF(cc.mask));
			e_lw(T6, R_CPU, CPU_OFF(flags));
			e_andi(T6, T6, ~CF);
			e_or(T6, T6, A1);
			e_sw(T6, R_CPU, CPU_OFF(flags));
		}
		jt_pf = PF_NONE;
		break;
	}
	case K_TEST: {
		if (!(b & 1)) bits = 8;
		if (b == 0xa8 || b == 0xa9) {
			jt_get_r(T2, 0, bits);
			e_li(A0, in->imm);
		} else {
			jt_get_e(in, T2, bits, false);
			if (b == 0xf6 || b == 0xf7) e_li(A0, in->imm);
			else jt_get_r(A0, in->reg, bits);
		}
		if (!live) { jt_pf = PF_DEAD; break; }
		e_and(A1, T2, A0);
		jt_sext(F_DST, A1, bits);
		jt_set_pf(PF_LOGIC, bits | CC_AND << 8);
		break;
	}
	case K_MOV:
		if (b >= 0xb0 && b <= 0xb7) { jt_imm(in, T2, in->imm); jt_set_r(b & 7, T2, 8, true); break; }
		if (b >= 0xb8 && b <= 0xbf) {   // (32 bits: into the register itself, as the loads and stores below)
			if (bits == 32) jt_imm(in, jg[b & 7], in->imm);
			else { jt_imm(in, T2, in->imm); jt_set_r(b & 7, T2, bits, true); }
			break;
		}
		if (b >= 0xa0 && b <= 0xa3) {
			int sz = (b & 1) ? bits : 8;
			int seg = in->seg >= 0 ? in->seg : SEG_DS;
			e_li(T0, in->imm);
			if (jt_based(seg)) { e_lw(T1, R_CPU, SEG_BASE_OFF(seg)); e_add(T0, T0, T1); }
			if (sz == 32) { if (b < 0xa2) jt_load(jg[0], 4); else jt_store(jg[0], 4); }
			else if (b < 0xa2) { jt_load(T2, sz / 8); jt_set_r(0, T2, sz, true); }
			else { jt_get_r(T2, 0, sz); jt_store(T2, sz / 8); }
			break;
		}
		if (!(b & 1)) bits = 8;
		if (b == 0xc6 || b == 0xc7) { jt_imm(in, T2, in->imm); jt_put_e(in, T2, bits, false, true); }
		else if ((b == 0x89 || b == 0x8b) && in->mod == 3 && bits == 32)   // register to register: one move
			e_mv(jg[b == 0x89 ? in->rm : in->reg], jg[b == 0x89 ? in->reg : in->rm]);
		else if (bits == 32) {   // with memory: loaded into the register, or stored from it, directly
			if (b == 0x89) jt_put_e(in, jg[in->reg], 32, false, true);
			else jt_get_e(in, jg[in->reg], 32, false);
		}
		else if (b == 0x88 || b == 0x89) { jt_get_r(T2, in->reg, bits); jt_put_e(in, T2, bits, false, true); }
		else { jt_get_e(in, T2, bits, false); jt_set_r(in->reg, T2, bits, true); }
		break;
	case K_LEA: {
		// the offset only: the address computation without the segment
		JIns t = *in;
		t.seg = SEG_DS;
		jt_ea(&t, 0);
		jt_ea_linear();
		e_lw(T1, R_CPU, SEG_BASE_OFF(SEG_DS));
		e_sub(T0, T0, T1);
		jt_set_r(in->reg, T0, bits, false);
		break;
	}
	case K_INCDEC: {
		bool dec;
		int r = -1;
		if (b >= 0x40 && b <= 0x4f) { dec = b >= 0x48; r = b & 7; }
		else { dec = in->reg == 1; if (b == 0xfe) bits = 8; }
		if (r >= 0 && bits == 32) {   // a 32-bit register: in place, CF straight into F_CF (nothing can leave here)
			if (live) jt_cf(F_CF);
			e_addi(jg[r], jg[r], dec ? -1 : 1);
			if (!live) { jt_pf = PF_DEAD; break; }
			e_mv(F_DST, jg[r]);
			jt_set_pf(dec ? PF_DEC : PF_INC, bits);
			break;
		}
		if (live) jt_cf(A3);   // INC and DEC keep CF (into F_CF below: the operand may still send the instruction
		                       // to the interpreter, which needs the flags as they were)
		if (r >= 0) jt_get_r(T2, r, bits);
		else jt_get_e(in, T2, bits, true);
		e_addi(A1, T2, dec ? -1 : 1);
		if (r >= 0) jt_set_r(r, A1, bits, false);
		else jt_put_e(in, A1, bits, true, false);
		if (!live) { jt_pf = PF_DEAD; break; }
		e_mv(F_CF, A3);
		jt_sext(F_DST, A1, bits);
		jt_set_pf(dec ? PF_DEC : PF_INC, bits);
		break;
	}
	case K_NOTNEG:
		if (b == 0xf6) bits = 8;
		jt_get_e(in, T2, bits, true);
		if (in->reg == 2) e_xori(A1, T2, -1);
		else e_sub(A1, X0, T2);
		jt_put_e(in, A1, bits, true, false);
		if (in->reg == 2) break;
		if (!live) { jt_pf = PF_DEAD; break; }
		jt_sext(F_SRC1, T2, bits);
		jt_sext(F_DST, A1, bits);
		jt_set_pf(PF_NEG, bits);
		break;
	case K_SHIFT: {
		if (!(b & 1)) bits = 8;
		int n = (b < 0xd0 ? in->imm : 1) & 31;
		if (!live && bits == 32 && in->mod == 3) {   // a register, its flags not read: in place
			const int d = jg[in->rm];
			if (in->reg == 5) e_srli(d, d, n);
			else if (in->reg == 7) e_srai(d, d, n);
			else e_slli(d, d, n);
			jt_pf = PF_DEAD;
			break;
		}
		jt_get_e(in, T2, bits, true);
		if (in->reg == 5) e_srli(A1, T2, n);
		else if (in->reg == 7) { jt_sext(A2, T2, bits); e_srai(A1, A2, n); }
		else e_slli(A1, T2, n);
		jt_put_e(in, A1, bits, true, false);
		if (!live) { jt_pf = PF_DEAD; break; }
		if (in->reg == 5) {
			jt_sext(F_SRC1, T2, bits);
			jt_sext(F_DST, A1, bits);
			e_srli(F_CF, T2, n - 1);
			e_andi(F_CF, F_CF, 1);
			jt_set_pf(PF_SHR, bits);
		} else if (in->reg == 7) {
			e_mv(F_DST, A1);
			e_srai(F_CF, A2, n - 1);
			e_andi(F_CF, F_CF, 1);
			jt_set_pf(PF_SAR, bits);
		} else {
			jt_sext(F_DST, A1, bits);
			e_srli(F_CF, T2, bits - n);
			e_andi(F_CF, F_CF, 1);
			jt_set_pf(PF_SHL, bits);
		}
		break;
	}
	case K_IMUL:
		// T2 = sext(the first operand), A0 = sext(the second)
		if (in->two) {
			jt_get_r(T2, in->reg, bits);
			jt_get_e(in, A0, bits, false);
			jt_sext(T2, T2, bits);
			jt_sext(A0, A0, bits);
		} else {
			jt_get_e(in, T2, bits, false);
			jt_sext(T2, T2, bits);
			e_li(A0, b == 0x6b ? (u32) (s32) (s8) in->imm : bits == 16 ? (u32) (s32) (s16) in->imm : in->imm);
		}
		e_mul(A1, T2, A0);
		if (bits == 32 && live) e_mulh(F_CF, T2, A0);
		jt_set_r(in->reg, A1, bits, false);
		if (!live) { jt_pf = PF_DEAD; break; }
		e_mv(F_SRC1, T2);
		e_mv(F_SRC2, A0);
		e_mv(F_DST, A1);
		jt_set_pf(bits == 16 ? PF_IMUL16 : PF_IMUL32, bits);
		break;
	case K_PUSH:
		if (in->two || b == 0x06 || b == 0x0e || b == 0x16 || b == 0x1e) {
			int s = in->two ? (b == 0xa0 ? SEG_FS : SEG_GS) : b >> 3;
			e_lw(A0, R_CPU, SEG_SEL_OFF(s));
			jt_push(A0, bits / 8, 2);
		} else if (b == 0x68 || b == 0x6a) {
			e_li(A0, b == 0x6a ? (u32) (s32) (s8) in->imm : in->imm);
			jt_push(A0, bits / 8, bits / 8);
		} else if (b == 0xff) {
			jt_get_e(in, A0, bits, false);
			jt_push(A0, bits / 8, bits / 8);
		} else {   // (the register itself: the store writes its low bits)
			jt_push(jg[b & 7], bits / 8, bits / 8);
		}
		break;
	case K_POP:
		if (b == 0x8f && in->mod != 3) {   // to memory: stored before ESP moves (a fault or jit_h_write: undone)
			jt_stack_ea(0);
			jt_load(A0, bits / 8);
			jt_ea(in, 0);
			jt_store(A0, bits / 8);
			if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], bits / 8); jt_set_r(4, A6, 16, false); }
			else e_addi(jg[4], jg[4], bits / 8);
			break;
		}
		{
			const int r = b == 0x8f ? in->rm : b & 7;
			if (r != 4 && bits == 16 && jt_pop_call(10 + r, 2)) break;   // (into its low half)
			if (r != 4 && bits == 32) { jt_pop(jg[r], 4); break; }
			jt_pop(A0, bits / 8);
			jt_set_r(r, A0, bits, true);
		}
		break;
	case K_ENTER:   // (level 0) push (E)BP, (E)BP = (E)SP, (E)SP -= imm16
		jt_get_r(A0, 5, bits);
		jt_push(A0, bits / 8, bits / 8);
		if (jt_mode & JM_STACK16) jt_zext(A1, jg[4], 16);
		else e_mv(A1, jg[4]);
		jt_set_r(5, A1, bits, true);
		e_li(T6, in->imm & 0xffff);
		if (jt_mode & JM_STACK16) { e_sub(A6, jg[4], T6); jt_set_r(4, A6, 16, false); }
		else e_sub(jg[4], jg[4], T6);
		break;
	case K_POPSEG: {
		int s = in->two ? (b == 0xa1 ? SEG_FS : SEG_GS) : b >> 3;
		jt_stack_ea(0);
		jt_load(A0, 2);
		jt_seg_load(s, A0);   // (with protected mode, ESP moves on only once it is loaded)
		if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], bits / 8); jt_set_r(4, A6, 16, false); }
		else e_addi(jg[4], jg[4], bits / 8);
		break;
	}
	case K_XCHG:
		if (b >= 0x91) {
			int r = b & 7;
			jt_get_r(T2, r, bits);
			jt_get_r(A0, 0, bits);
			jt_set_r(r, A0, bits, true);
			jt_set_r(0, T2, bits, true);
		} else {
			if (b == 0x86) bits = 8;
			jt_get_e(in, T2, bits, true);
			jt_get_r(A0, in->reg, bits);
			jt_put_e(in, A0, bits, true, true);
			jt_set_r(in->reg, T2, bits, true);
		}
		break;
	case K_CBW:
		if (in->opsz16) { e_slli(T2, jg[0], 24); e_srai(T2, T2, 24); jt_set_r(0, T2, 16, false); }
		else { e_slli(jg[0], jg[0], 16); e_srai(jg[0], jg[0], 16); }
		break;
	case K_CWD:
		if (in->opsz16) { e_slli(T2, jg[0], 16); e_srai(T2, T2, 31); jt_set_r(2, T2, 16, false); }
		else e_srai(jg[2], jg[0], 31);
		break;
	case K_MOVX: {
		int sb = (b & 1) ? 16 : 8;
		if (bits == 32) {   // into the register itself (no move after)
			jt_get_e(in, jg[in->reg], sb, false);
			if (b >= 0xbe) jt_sext(jg[in->reg], jg[in->reg], sb);
			break;
		}
		jt_get_e(in, T2, sb, false);
		if (b >= 0xbe) { jt_sext(T2, T2, sb); jt_set_r(in->reg, T2, bits, false); }
		else jt_set_r(in->reg, T2, bits, true);
		break;
	}
	case K_SETCC:
		jt_cond(b & 15, T2);
		jt_put_e(in, T2, 8, false, true);
		break;
	case K_MOVSEG:
		if (b == 0x8c) {
			e_lw(T2, R_CPU, SEG_SEL_OFF(in->reg));
			if (in->mod == 3) jt_set_r(in->rm, T2, bits, true);
			else jt_put_e(in, T2, 16, false, true);
		} else {
			jt_get_e(in, T2, 16, false);
			jt_seg_load(in->reg, T2);
		}
		break;
	case K_LDSEG: {   // LES LDS, LFS LGS
		int s = in->two ? (b == 0xb4 ? SEG_FS : SEG_GS) : b == 0xc4 ? SEG_ES : SEG_DS;
		jt_ea(in, 0);
		jt_load(T2, bits / 8);
		jt_ea(in, bits / 8);
		jt_load(A0, 2);
		jt_seg_load(s, A0);
		jt_set_r(in->reg, T2, bits, true);
		break;
	}
	case K_LEAVE:
		if (jt_mode & JM_STACK16) jt_zext(T0, jg[5], 16);
		else e_mv(T0, jg[5]);
		if (jt_based(SEG_SS)) { e_lw(T1, R_CPU, SEG_BASE_OFF(SEG_SS)); e_add(T0, T0, T1); }
		jt_load(A0, bits / 8);
		e_addi(A1, jg[5], bits / 8);
		if (jt_mode & JM_STACK16) jt_set_r(4, A1, 16, false);
		else e_mv(jg[4], A1);
		jt_set_r(5, A0, bits, true);
		break;
	case K_CLD: case K_STD:
		e_lw(T6, R_CPU, CPU_OFF(flags));
		if (in->kind == K_CLD) e_andi(T6, T6, ~DF);
		else e_ori(T6, T6, DF);
		e_sw(T6, R_CPU, CPU_OFF(flags));
		break;
	case K_ADCSBB: {
		int op, form;
		if (b < 0x40) {
			op = b >> 3;
			form = (b & 7) < 2 ? 0 : (b & 7) < 4 ? 1 : 2;
			if (!(b & 1)) bits = 8;
		} else {
			op = in->reg;
			form = 3;
			if (b == 0x80 || b == 0x82) bits = 8;
		}
		bool mem_dest = (form == 0 || form == 3) && in->mod != 3;
		jt_cf(A3);   // the carry in (before the operands: one may send the instruction to the interpreter)
		if (bits == 32 && !mem_dest) {   // a register, in place (as K_ALU)
			const int d = jg[form == 1 ? in->reg : form == 2 ? 0 : in->rm];
			int s = form == 1 ? (in->mod == 3 ? jg[in->rm] : A0) : form == 0 ? jg[in->reg] : A0;
			if (form == 1 && in->mod != 3) jt_get_e(in, A0, 32, false);
			else if (form >= 2) jt_imm(in, A0, form == 3 && b == 0x83 ? (u32) (s32) (s8) in->imm : in->imm);
			if (!live) {
				if (op == 2) { e_add(d, d, s); e_add(d, d, A3); }
				else { e_sub(d, d, s); e_sub(d, d, A3); }
				jt_pf = PF_DEAD;
				break;
			}
			e_mv(F_SRC2, s);
			e_mv(F_SRC1, d);
			if (op == 2) { e_add(F_DST, F_SRC1, F_SRC2); e_add(F_DST, F_DST, A3); }
			else { e_sub(F_DST, F_SRC1, F_SRC2); e_sub(F_DST, F_DST, A3); }
			e_mv(d, F_DST);
			e_mv(F_CF, A3);
			jt_set_pf(op == 2 ? PF_ADC : PF_SBB, 32);
			break;
		}
		if (form == 2) {
			jt_get_r(T2, 0, bits);
			jt_imm(in, A0, in->imm);
		} else if (form == 1) {
			jt_get_r(T2, in->reg, bits);
			jt_get_e(in, A0, bits, false);
		} else {
			jt_get_e(in, T2, bits, true);
			if (form == 0) jt_get_r(A0, in->reg, bits);
			else jt_imm(in, A0, b == 0x83 ? (u32) (s32) (s8) in->imm : in->imm);
		}
		if (op == 2) { e_add(A1, T2, A0); e_add(A1, A1, A3); }
		else { e_sub(A1, T2, A0); e_sub(A1, A1, A3); }
		if (form == 1) jt_set_r(in->reg, A1, bits, false);
		else if (form == 2) jt_set_r(0, A1, bits, false);
		else if (mem_dest) jt_host_store(A1, bits / 8);
		else jt_set_r(in->rm, A1, bits, false);
		if (!live) { jt_pf = PF_DEAD; break; }
		jt_sext(F_SRC1, T2, bits);
		jt_sext(F_SRC2, A0, bits);
		jt_sext(F_DST, A1, bits);
		e_mv(F_CF, A3);
		jt_set_pf(op == 2 ? PF_ADC : PF_SBB, bits);
		break;
	}
	case K_ROT: {   // ROL, ROR by 1..bits-1; RCL, RCR by 1: CF and OF go to cpu->flags (tiny386's way)
		if (!(b & 1)) bits = 8;
		int n = b < 0xd0 ? in->imm : 1, op = in->reg;
		if (op >= 2) jt_cf(A3);
		jt_flags_store();
		jt_get_e(in, T2, bits, true);
		if (op == 0) {        // ROL
			e_slli(A1, T2, n);
			e_srli(A0, T2, bits - n);
			e_or(A1, A1, A0);
		} else if (op == 1) { // ROR
			e_srli(A1, T2, n);
			e_slli(A0, T2, bits - n);
			e_or(A1, A1, A0);
		} else if (op == 2) { // RCL 1
			e_slli(A1, T2, 1);
			e_or(A1, A1, A3);
		} else {              // RCR 1
			e_srli(A1, T2, 1);
			e_slli(A0, A3, bits - 1);
			e_or(A1, A1, A0);
		}
		jt_put_e(in, A1, bits, true, false);
		jt_sext(A2, A1, bits);   // res, sign-extended
		// A4 = CF, A5 = OF
		if (op == 0) { e_andi(A4, A2, 1); e_srli(A5, A2, 31); e_xor(A5, A5, A4); }
		else if (op == 2) { e_srli(A4, T2, bits - 1); e_andi(A4, A4, 1); e_srli(A5, A2, 31); e_xor(A5, A5, A4); }
		else {
			if (op == 1) { e_srli(A4, A2, bits - 1); e_andi(A4, A4, 1); }
			else e_andi(A4, T2, 1);
			e_slli(A5, A2, 1);
			e_xor(A5, A5, A2);
			e_srli(A5, A5, bits - 1);
			e_andi(A5, A5, 1);
		}
		e_li(A6, ~(CF | OF));
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_and(T6, T6, A6);
		e_or(T6, T6, A4);
		e_slli(A5, A5, 11);
		e_or(T6, T6, A5);
		e_sw(T6, R_CPU, CPU_OFF(flags));
		e_lw(T6, R_CPU, CPU_OFF(cc.mask));
		e_and(T6, T6, A6);
		e_sw(T6, R_CPU, CPU_OFF(cc.mask));
		jt_pf = PF_NONE;
		break;
	}
	case K_ROTCL: {   // ROL, ROR by CL, as tiny386: CL not masked (0: nothing at all, else CF and OF set); by CL &
		if (!(b & 1)) bits = 8;   // (bits - 1), and if that is 0 the result is the value as it was, not sign-extended
		const int op = in->reg;   // (the operand is got first, even for 0: a fault on it is taken all the same)
		jt_flags_store();
		jt_get_e(in, T2, bits, true);
		e_andi(A4, jg[1], 255);
		u32 *none = e_branch(BEQ, A4, X0);
		e_andi(A5, A4, bits - 1);
		u32 *same = e_branch(BEQ, A5, X0);
		e_li(A0, bits);
		e_sub(A0, A0, A5);
		if (op == 0) { e_sll(A1, T2, A5); e_srl(A0, T2, A0); }
		else { e_srl(A1, T2, A5); e_sll(A0, T2, A0); }
		e_or(A1, A1, A0);
		jt_put_e(in, A1, bits, true, false);
		jt_sext(A2, A1, bits);
		u32 *done = e_jump();
		j_bind(same, jp);
		e_mv(A2, T2);
		j_bind(done, jp);
		// A4 = CF, A5 = OF
		if (op == 0) { e_andi(A4, A2, 1); e_srli(A5, A2, 31); e_xor(A5, A5, A4); }
		else {
			e_srli(A4, A2, bits - 1);
			e_andi(A4, A4, 1);
			e_slli(A5, A2, 1);
			e_xor(A5, A5, A2);
			e_srli(A5, A5, bits - 1);
			e_andi(A5, A5, 1);
		}
		e_li(A6, ~(CF | OF));
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_and(T6, T6, A6);
		e_or(T6, T6, A4);
		e_slli(A5, A5, 11);
		e_or(T6, T6, A5);
		e_sw(T6, R_CPU, CPU_OFF(flags));
		e_lw(T6, R_CPU, CPU_OFF(cc.mask));
		e_and(T6, T6, A6);
		e_sw(T6, R_CPU, CPU_OFF(cc.mask));
		j_bind(none, jp);
		jt_pf = PF_NONE;
		break;
	}
	case K_SHIFTCL: {   // SHL, SHR, SAR by CL: nothing at all if CL & 31 is 0
		if (!(b & 1)) bits = 8;
		jt_flags_store();
		e_andi(A4, jg[1], 31);
		u32 *none = e_branch(BEQ, A4, X0);
		jt_get_e(in, T2, bits, true);
		if (in->reg == 5) e_srl(A1, T2, A4);
		else if (in->reg == 7) { jt_sext(A2, T2, bits); e_sra(A1, A2, A4); }
		else e_sll(A1, T2, A4);
		jt_put_e(in, A1, bits, true, false);
		e_addi(A5, A4, -1);
		if (in->reg == 5) {
			jt_sext(F_SRC1, T2, bits);
			jt_sext(F_DST, A1, bits);
			e_srl(F_CF, T2, A5);
			e_andi(F_CF, F_CF, 1);
			jt_set_pf(PF_SHR, bits);
		} else if (in->reg == 7) {
			e_mv(F_DST, A1);
			e_sra(F_CF, A2, A5);
			e_andi(F_CF, F_CF, 1);
			jt_set_pf(PF_SAR, bits);
		} else {   // CF: x >> (bits - n), as tiny386 computes it (the hardware takes the low 5 bits of the count)
			jt_sext(F_DST, A1, bits);
			e_li(A5, bits);
			e_sub(A5, A5, A4);
			e_srl(F_CF, T2, A5);
			e_andi(F_CF, F_CF, 1);
			jt_set_pf(PF_SHL, bits);
		}
		jt_flags_store();
		j_bind(none, jp);
		jt_pf = PF_NONE;
		break;
	}
	case K_MULDIV: {
		if (b == 0xf6) bits = 8;
		int op = in->reg;
		jt_get_e(in, A0, bits, false);
		if (op < 6) {   // MUL, IMUL: the flags as tiny386's MULb/w/d, IMULb/w/d leave them
			int cc = (op == 4 ? CC_MUL8 : CC_IMUL8) + (bits == 8 ? 0 : bits == 16 ? 1 : 2);
			if (op == 5) jt_sext(A0, A0, bits);
			if (bits == 8) {
				jt_get_r(T2, 0, 8);
				if (op == 5) jt_sext(T2, T2, 8);
				e_mul(A1, T2, A0);
				jt_set_r(0, A1, 16, false);
				if (op == 4) jt_sext(A1, A1, 16);
			} else if (bits == 16) {
				jt_get_r(T2, 0, 16);
				if (op == 5) jt_sext(T2, T2, 16);
				e_mul(A1, T2, A0);
				jt_set_r(0, A1, 16, false);
				e_srli(A2, A1, 16);
				jt_set_r(2, A2, 16, false);
			} else {
				e_mv(T2, jg[0]);
				e_mul(A1, T2, A0);
				if (op == 4) e_mulhu(A2, T2, A0); else e_mulh(A2, T2, A0);
				e_mv(jg[0], A1);
				e_mv(jg[2], A2);
			}
			if (!live) { jt_pf = PF_DEAD; break; }
			e_sw(T2, R_CPU, CPU_OFF(cc.src1));
			e_sw(A0, R_CPU, CPU_OFF(cc.src2));
			e_sw(A1, R_CPU, CPU_OFF(cc.dst));
			if (bits == 32) e_sw(A2, R_CPU, CPU_OFF(cc.dst2));
			e_li(T6, cc);
			e_sw(T6, R_CPU, CPU_OFF(cc.op));
			e_li(T6, CF | PF | AF | ZF | SF | OF);
			e_sw(T6, R_CPU, CPU_OFF(cc.mask));
			jt_pf = PF_NONE;
			break;
		}
		// DIV, IDIV: whatever would fault (and tiny386's CPU-detection quirks) goes to the interpreter, before
		// anything changes
		JStub *st = jt_stub(STUB_EXIT);
		u32 *big = NULL;
		st->eip = in->eip;
		bool sgn = op == 7;
		if (bits == 8) { jt_get_r(T2, 0, 16); if (sgn) { jt_sext(T2, T2, 16); jt_sext(A0, A0, 8); } }
		else if (bits == 16) {
			jt_get_r(T2, 0, 16);
			e_slli(A1, jg[2], 16);
			e_or(T2, T2, A1);
			if (sgn) jt_sext(A0, A0, 16);
		} else {
			e_mv(T2, jg[0]);
			u32 *small = sgn ? (e_srai(A1, jg[0], 31), e_branch(BEQ, A1, jg[2])) : e_branch(BEQ, jg[2], X0);
			// a 64-bit dividend (a 16.16 fixed-point divide: EDX:EAX = a << 16): jit_h_div64; a zero divisor or a quotient
			// too big: the interpreter (#DE), before anything changes
			e_mv(A2, A0);
			e_mv(A0, jg[0]);
			e_mv(A1, jg[2]);
			e_li(A3, sgn);
			e_call((const void *) jit_h_div64);
			e_lw_abs(T6, &jit_div_fault);
			st->at = e_branch(BNE, T6, X0);
			e_mv(jg[0], A0);
			e_lw_abs(jg[2], &jit_div_rem);
			big = e_jump();
			j_bind(small, jp);
		}
		JStub *z = jt_stub(STUB_EXIT);
		z->eip = in->eip;
		z->at = e_branch(BEQ, A0, X0);
		if (sgn && bits == 32) {   // INT_MIN / -1 overflows (RISC-V's div doesn't trap)
			JStub *v = jt_stub(STUB_EXIT);
			v->eip = in->eip;
			e_li(A1, -1);
			u32 *no = e_branch(BNE, A0, A1);
			e_li(A1, 0x80000000);
			v->at = e_branch(BEQ, T2, A1);
			j_bind(no, jp);
		}
		if (!sgn && bits < 32) {   // the quirks: 5 / 2 (Cyrix test), 0x5555 / 2 (NexGen test)
			JStub *q = jt_stub(STUB_EXIT);
			q->eip = in->eip;
			e_li(A1, bits == 8 ? 5 : 0x5555);
			u32 *no = e_branch(BNE, T2, A1);
			e_li(A1, 2);
			q->at = e_branch(BEQ, A0, A1);
			j_bind(no, jp);
		}
		if (sgn) { E(RV_R(1, A0, T2, 4, A1, 0x33)); E(RV_R(1, A0, T2, 6, A2, 0x33)); }   // div, rem
		else { E(RV_R(1, A0, T2, 5, A1, 0x33)); E(RV_R(1, A0, T2, 7, A2, 0x33)); }      // divu, remu
		if (bits < 32 || sgn) {   // does the quotient fit?
			JStub *o = jt_stub(STUB_EXIT);
			o->eip = in->eip;
			if (sgn) { jt_sext(A3, A1, bits); o->at = e_branch(BNE, A3, A1); }
			else { e_srli(A3, A1, bits); o->at = e_branch(BNE, A3, X0); }
		}
		if (bits == 8) { jt_set_r(0, A1, 8, false); jt_set_r(4, A2, 8, false); }
		else if (bits == 16) { jt_set_r(0, A1, 16, false); jt_set_r(2, A2, 16, false); }
		else { e_mv(jg[0], A1); e_mv(jg[2], A2); }
		if (big) j_bind(big, jp);
		break;
	}
	case K_IN: case K_OUT: {
		int sz = (b & 1) ? bits / 8 : 1;
		if (b < 0xe8) e_li(A0, in->imm & 0xff);
		else jt_zext(A0, jg[2], 16);
		e_li(A1, sz);
		if (in->kind == K_IN) {
			e_call((const void *) jit_h_in);
			jt_set_r(0, A0, sz * 8, sz == 4);
		} else {
			jt_get_r(A2, 0, sz * 8);
			e_call((const void *) jit_h_out);
		}
		break;
	}
	case K_STR: {   // MOVS, CMPS, STOS, LODS, SCAS without REP; (E)SI and (E)DI move by A4 (DF)
		int n = (b & 1) ? bits / 8 : 1;
		int seg = in->seg >= 0 ? in->seg : SEG_DS;
		e_lw(A4, R_CPU, CPU_OFF(flags));
		e_srli(A4, A4, 10);
		e_andi(A4, A4, 1);
		e_slli(A4, A4, n == 1 ? 1 : n == 2 ? 2 : 3);
		e_li(A5, n);
		e_sub(A4, A5, A4);
		bool si = b == 0xa4 || b == 0xa5 || b == 0xa6 || b == 0xa7 || b == 0xac || b == 0xad;
		bool di = b != 0xac && b != 0xad;
		if (si) {   // T2 = [seg:SI]
			if (in->adsz16) jt_zext(T0, jg[6], 16); else e_mv(T0, jg[6]);
			if (jt_based(seg)) { e_lw(T1, R_CPU, SEG_BASE_OFF(seg)); e_add(T0, T0, T1); }
			jt_load(T2, n);
		}
		if (di) {
			if (in->adsz16) jt_zext(T0, jg[7], 16); else e_mv(T0, jg[7]);
			if (jt_based(SEG_ES)) { e_lw(T1, R_CPU, SEG_BASE_OFF(SEG_ES)); e_add(T0, T0, T1); }
			if (b == 0xa4 || b == 0xa5) jt_store(T2, n);
			else if (b == 0xaa || b == 0xab) { jt_get_r(A0, 0, n * 8); jt_store(A0, n); }
			else jt_load(A0, n);   // CMPS, SCAS: A0 = [ES:DI]
		}
		if (b == 0xac || b == 0xad) jt_set_r(0, T2, n * 8, true);
		if (b == 0xae || b == 0xaf) jt_get_r(T2, 0, n * 8);   // SCAS: AL/AX/EAX - [ES:DI]
		if (si) {
			e_add(A6, jg[6], A4);
			if (in->adsz16) jt_set_r(6, A6, 16, false); else e_mv(jg[6], A6);
		}
		if (di) {
			e_add(A6, jg[7], A4);
			if (in->adsz16) jt_set_r(7, A6, 16, false); else e_mv(jg[7], A6);
		}
		if (b == 0xa6 || b == 0xa7 || b == 0xae || b == 0xaf) {
			if (!live) { jt_pf = PF_DEAD; break; }
			jt_sext(F_SRC1, T2, n * 8);
			jt_sext(F_SRC2, A0, n * 8);
			e_sub(F_DST, F_SRC1, F_SRC2);
			jt_sext(F_DST, F_DST, n * 8);
			jt_set_pf(PF_SUB, n * 8);
		}
		break;
	}
	case K_FLAGOP:
		if (b == 0xfa) {   // CLI
			e_lw(T6, R_CPU, CPU_OFF(flags));
			e_andi(T6, T6, ~IF);
			e_sw(T6, R_CPU, CPU_OFF(flags));
			break;
		}
		if (b == 0xfb) {   // STI (an interrupt waiting is taken after the next instruction: jit_translate)
			e_lw(T6, R_CPU, CPU_OFF(flags));
			e_ori(T6, T6, IF);
			e_sw(T6, R_CPU, CPU_OFF(flags));
			break;
		}
		if (b == 0xf5) jt_cf(A3);   // CMC
		jt_flags_store();
		e_lw(T6, R_CPU, CPU_OFF(cc.mask));
		e_andi(T6, T6, ~CF);
		e_sw(T6, R_CPU, CPU_OFF(cc.mask));
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_andi(T6, T6, ~CF);
		if (b == 0xf9) e_ori(T6, T6, CF);
		else if (b == 0xf5) { e_xori(A3, A3, 1); e_or(T6, T6, A3); }
		e_sw(T6, R_CPU, CPU_OFF(flags));
		jt_pf = PF_NONE;
		break;
	case K_PUSHA: {   // AX CX DX BX SP BP SI DI, each slot at its own (wrapping) stack address
		int n = bits / 8;
		for (int i = 0; i < 8; i++) {
			jt_stack_ea(-n * (i + 1));
			jt_store(jg[i], n);
		}
		if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], -8 * n); jt_set_r(4, A6, 16, false); }
		else e_addi(jg[4], jg[4], -8 * n);
		break;
	}
	case K_POPA: {   // DI SI BP (SP skipped) BX DX CX AX
		int n = bits / 8;
		static const u8 tmp[8] = { T2, A0, A1, A2, A3, A4, A5, 0 };
		static const u8 reg[7] = { 7, 6, 5, 3, 2, 1, 0 };
		for (int i = 0, k = 0; i < 8; i++) {
			if (i == 3) continue;
			jt_stack_ea(n * i);
			jt_load(tmp[k], n);
			k++;
		}
		if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], 8 * n); jt_set_r(4, A6, 16, false); }
		else e_addi(jg[4], jg[4], 8 * n);
		for (int k = 0; k < 7; k++) jt_set_r(reg[k], tmp[k], bits, true);
		break;
	}
	case K_XLAT: {
		int seg = in->seg >= 0 ? in->seg : SEG_DS;
		jt_get_r(T0, 0, 8);
		e_add(T0, T0, jg[3]);
		if (in->adsz16) jt_zext(T0, T0, 16);
		if (jt_based(seg)) { e_lw(T1, R_CPU, SEG_BASE_OFF(seg)); e_add(T0, T0, T1); }
		jt_load(T2, 1);
		jt_set_r(0, T2, 8, true);
		break;
	}
	case K_RCX: {   // RCL, RCR by CL or an immediate (jit_h_rcx: the operand stored unless the count comes to 0)
		if (!(b & 1)) bits = 8;
		const bool mem = in->mod != 3;
		jt_flags_store();
		jt_get_e(in, T2, bits, true);
		if (mem) { e_addi(SP, SP, -16); e_sw(T1, SP, 0); }   // (the operand's host address, for jt_put_e)
		e_mv(A1, T2);
		if (b >= 0xd0) e_andi(A2, jg[1], 255); else e_li(A2, in->imm & 0xff);
		e_li(A3, bits | (in->reg == 3 ? 0x100 : 0));
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_rcx);
		if (mem) { e_lw(T1, SP, 0); e_addi(SP, SP, 16); }
		e_lw_abs(T2, &jit_rcx_by);
		u32 *none = e_branch(BEQ, T2, X0);
		jt_put_e(in, A0, bits, true, false);
		j_bind(none, jp);
		jt_pf = PF_NONE;
		break;
	}
	case K_SHXD16: {   // 16-bit SHLD, SHRD (jit_h_shxd16: the operand stored unless the count is 0)
		const bool mem = in->mod != 3;
		jt_flags_store();
		jt_get_e(in, T2, 16, true);
		if (mem) { e_addi(SP, SP, -16); e_sw(T1, SP, 0); }   // (the operand's host address, for jt_put_e)
		e_mv(A1, T2);
		jt_get_r(A2, in->reg, 16);
		if (b & 1) e_andi(A3, jg[1], 255); else e_li(A3, in->imm & 0xff);
		if (b >= 0xac) e_ori(A3, A3, 0x100);
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_shxd16);
		if (mem) { e_lw(T1, SP, 0); e_addi(SP, SP, 16); }
		e_lw_abs(T2, &jit_shxd_by);
		u32 *none = e_branch(BEQ, T2, X0);
		jt_put_e(in, A0, 16, true, false);
		j_bind(none, jp);
		jt_pf = PF_NONE;
		break;
	}
	case K_LARLSL: {   // LAR, LSL (jit_h_larlsl): the register written only with ZF set; an exception: the interpreter
		jt_flags_store();
		jt_get_e(in, A1, 16, false);
		e_mv(A0, R_CPU);
		e_li(A2, b);
		e_call((const void *) jit_h_larlsl);
		u32 *exc = e_branch(BEQ, A0, X0);
		e_addi(A0, A0, -1);
		u32 *nz = e_branch(BEQ, A0, X0);   // (ZF clear: the register as it was)
		e_lw_abs(T2, &jit_larlsl_v);
		jt_set_r(in->reg, T2, bits, false);
		j_bind(nz, jp);
		u32 *done = e_jump();
		j_bind(exc, jp);
		jt_interp(in);
		j_bind(done, jp);
		jt_pf = PF_NONE;
		break;
	}
	case K_BCD:
		jt_flags_store();
		jt_get_r(A1, 0, 16);
		e_li(A2, b);
		e_li(A3, in->imm & 0xff);
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_bcd);
		jt_set_r(0, A0, 16, true);
		jt_pf = PF_NONE;
		break;
	case K_LAHF:
		jt_flags_store();
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_lahf);
		jt_set_r(4, A0, 8, true);
		jt_pf = PF_NONE;
		break;
	case K_SAHF:
		jt_flags_store();
		jt_get_r(A1, 4, 8);
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_sahf);
		jt_pf = PF_NONE;
		break;
	case K_PUSHF:
		jt_flags_store();
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_pushf);
		jt_push(A0, bits / 8, bits / 8);
		jt_pf = PF_NONE;
		break;
	case K_POPF: {   // the block is left after it if an interrupt is now due, or IOPL changed (jit_h_popf)
		jt_flags_store();
		jt_stack_ea(0);
		jt_load(A1, bits / 8);
		e_mv(A0, R_CPU);
		e_li(A2, bits / 8);
		e_call((const void *) jit_h_popf);
		if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], bits / 8); jt_set_r(4, A6, 16, false); }
		else e_addi(jg[4], jg[4], bits / 8);
		jt_pf = PF_NONE;
		JStub *st = jt_stub(STUB_IRQ);
		st->eip = in->eip + in->len;
		st->at = e_branch(BNE, A0, X0);
		break;
	}
	// ---- the instructions that end a block
	case K_JCC: {
		u32 disp = in->two ? (in->opsz16 ? (u32) (s32) (s16) in->imm : in->imm) : (u32) (s32) (s8) in->imm;
		jt_flags_store();
		u32 *other = NULL;
		if (in->op_dyn) {   // the opcode as it is now: this Jcc (below), another one (after), else the interpreter
			e_li(T0, jt_cs_base + in->eip + jt_phys_off);
			e_add(T0, T0, R_MEM);
			e_lbu(T1, T0, 0);
			e_addi(T1, T1, -0x70);
			e_li(T2, 16);
			JStub *st = jt_stub(STUB_EXIT);
			st->eip = in->eip;
			st->at = e_branch(BGEU, T1, T2);
			e_li(T2, b - 0x70);
			other = e_branch(BNE, T1, T2);
		}
		u32 *taken = jt_branch_cond(b & 15);
		jt_link_exit(in->eip + in->len);
		j_bind(taken, jp);
		jt_link_exit(jt_target(in, disp));
		if (other) {
			j_bind(other, jp);
			e_mv(A1, T1);
			e_call_near(jit_r_cond);
			u32 *taken2 = e_branch(BNE, A0, X0);
			jt_link_exit(in->eip + in->len);
			j_bind(taken2, jp);
			jt_link_exit(jt_target(in, disp));
		}
		break;
	}
	case K_JMP: {
		u32 disp = b == 0xeb ? (u32) (s32) (s8) in->imm : in->opsz16 ? (u32) (s32) (s16) in->imm : in->imm;
		jt_flags_store();
		jt_link_exit(jt_target(in, disp));
		break;
	}
	case K_CALL: {
		u32 disp = in->opsz16 ? (u32) (s32) (s16) in->imm : in->imm;
		e_li(A0, in->eip + in->len);
		jt_push(A0, bits / 8, bits / 8);
		jt_flags_store();
		jt_link_exit(jt_target(in, disp));
		break;
	}
	case K_RET:
		jt_pop(A1, bits / 8);
		if (b == 0xc2) {
			e_li(T6, in->imm);   // (0-65535: more than an addi takes)
			if (jt_mode & JM_STACK16) { e_add(A6, jg[4], T6); jt_set_r(4, A6, 16, false); }
			else e_add(jg[4], jg[4], T6);
		}
		jt_flags_store();
		jt_ijc_exit(-1);
		break;
	case K_FPU: {   // x87: the frequent memory forms by their helpers, the rest by jit_h_fpu (the block goes on)
		jt_flags_store();   // (the calls below may change any register a call may)
		u32 *const routine = jt_fpu_core(in, false) ? jit_x87_routine_of(in) : NULL;
#ifdef T386_FPU_STATS
		e_li(A0, (u32) (uintptr_t) &jit_fpu_form[(b - 0xd8) * 16 + (in->mod == 3) * 8 + in->reg]);
		e_lw(T6, A0, 0);
		e_addi(T6, T6, 1);
		e_sw(T6, A0, 0);
#endif
		const struct JitFpuForm ff = in->mod != 3 ? jt_fpu_form(b - 0xd8, in->reg) : (struct JitFpuForm) { FF_NONE, 0, NULL };
		if (ff.how != FF_NONE) {
			jt_fpu_nm();
			const int words = ff.bytes == 8 ? 2 : 1, wbytes = ff.bytes == 8 ? 4 : ff.bytes;
			// the operand: into a1 (a2), FF_ARITH's into a2 (a3)
			if (ff.how == FF_ARITH || ff.how == FF_ARITHG || ff.how == FF_LD || ff.how == FF_LDCW) {
				const int r0 = ff.how == FF_ARITH ? A2 : A1;
				for (int w = 0; w < words; w++) {
					jt_ea(in, 4 * w);
					jt_load(r0 + w, wbytes);
				}
				if (!jt_fpu_call_core(in, routine)) {
					e_lw(A0, R_CPU, CPU_OFF(fpu));
					if (ff.how == FF_ARITH) e_li(A1, in->reg);
					e_call_abs(ff.fn);
				}
			} else {   // the value (a0, a1) stored, then FSTP's pop
				if (!jt_fpu_call_core(in, routine)) {
					e_lw(A0, R_CPU, CPU_OFF(fpu));
					e_call_abs(ff.fn);
				}
				e_mv(A2, A0);
				if (words == 2) e_mv(A3, A1);
				for (int w = 0; w < words; w++) {
					jt_ea(in, 4 * w);
					jt_store(A2 + w, wbytes);
				}
				if (ff.how == FF_STP) {   // (t386_fpu_pop's: TOP + 1)
					if (!jt_fl.sb) t386_fpu_layout(&jt_fl);
					const int top = jt_fl.top;
					e_lw(A0, R_CPU, CPU_OFF(fpu));
					e_lw(T6, A0, top);
					e_addi(T6, T6, 1);
					e_andi(T6, T6, 7);
					e_sw(T6, A0, top);
				}
			}
			jt_pf = PF_NONE;
			break;
		}
		if (in->mod == 3 && jt_fpu_reg_form(b - 0xd8, in->reg, in->rm)) {   // register forms: t386_fpu_reg
			jt_fpu_nm();
			if (!jt_fpu_call_core(in, routine)) {   // (the arithmetic and the stack moves by their own helpers)
				const int esc = b - 0xd8, g = in->reg;
				e_lw(A0, R_CPU, CPU_OFF(fpu));
				e_li(A1, esc << 3 | g);
				e_li(A2, in->rm);
				e_call_abs(jt_fpu_reg_fn(esc, g));
			}
			jt_pf = PF_NONE;
			break;
		}
		if (b == 0xdf && in->mod == 3 && in->reg == 4 && in->rm == 0) {   // FNSTSW AX
			jt_fpu_nm();
			e_lw(A0, R_CPU, CPU_OFF(fpu));
			e_call_abs((const void *) t386_fpu_get_sw);
			jt_set_r(0, A0, 16, false);
			jt_pf = PF_NONE;
			break;
		}
		if (in->mod != 3) {
			JIns t = *in;
			t.seg = SEG_DS;
			jt_ea(&t, 0);
			jt_ea_linear();
			e_lw(T1, R_CPU, SEG_BASE_OFF(SEG_DS));
			e_sub(A3, T0, T1);
		} else {
			e_li(A3, 0);
		}
		e_li(A1, b | (in->mod << 14 | in->reg << 11 | in->rm << 8) | (in->opsz16 ? 0x10000 : 0));
		e_li(A2, in->seg >= 0 ? in->seg : in->dseg >= 0 ? in->dseg : SEG_DS);
		e_li(A4, in->eip);
		e_li(A5, in->eip + in->len);
		// a plain call (no thunk): the flags are stored, nothing else lives in the registers a call may change, and
		// the guest's are in the ones it keeps (s1-s8); FNSTSW AX (DF E0) is the one that changes a guest register
		const bool fnstsw_ax = b == 0xdf && in->mod == 3 && in->reg == 4;
		if (fnstsw_ax) e_sw(jg[0], R_CPU, 0);
		e_mv(A0, R_CPU);
		e_call_abs((const void *) jit_h_fpu);
		if (fnstsw_ax) e_lw(jg[0], R_CPU, 0);
		jt_pf = PF_NONE;
		JStub *st = jt_stub(STUB_STOP);
		st->at = e_branch(BNE, A0, X0);
		break;
	}
	case K_PMFAR: {   // protected mode: jit_h_pmfar, then on through jit_r_stop (the block ends here)
		u32 op = b;
		jt_flags_store();
		if (b == 0xff) {   // CALL or JMP FAR [m]: the pointer read here (a fault leaves before anything is done)
			jt_ea(in, 0);
			jt_load(A2, bits / 8);
			jt_ea(in, bits / 8);
			jt_load(A3, 2);
			op = in->reg == 3 ? 0x9a : 0xea;
		} else {
			e_li(A2, b == 0x9a || b == 0xea ? in->imm : b == 0xca || b == 0xcd ? in->imm & 0xffff : 0);
			e_li(A3, b == 0x9a || b == 0xea ? in->imm2 & 0xffff : 0);
		}
		e_li(A1, op | (in->opsz16 ? 0x100 : 0));
		e_li(A4, in->eip);
		e_li(A5, in->eip + in->len);
		{
			u32 f = (u32) (uintptr_t) jit_h_pmfar;
			s32 lo = (s32) (f << 20) >> 20;
			e_lui(T6, f - (u32) lo);
			e_addi(T6, T6, lo);
		}
		e_call_near(jit_r_callc);
		jt_pf = PF_NONE;
		JStub *st = jt_stub(STUB_STOP);
		st->at = e_branch(BNE, A0, X0);
		jt_link_exit(in->eip + in->len);   // (0: on at the next instruction after all)
		break;
	}
	// ---- real mode far transfers: CS is a selector times 16 (tiny386's set_seg)
	case K_JMPF: case K_CALLF:
		if (in->kind == K_CALLF) {
			e_lw(A0, R_CPU, SEG_SEL_OFF(SEG_CS));
			e_li(A4, in->eip + in->len);
			jt_push2(A0, A4, bits / 8);
		}
		jt_flags_store();
		e_li(A0, in->imm2 & 0xffff);
		jt_seg_load(SEG_CS, A0);
		jt_link_exit_cs(in->imm, (in->imm2 & 0xffff) << 4);   // (real and virtual-8086 mode: CS is a selector times 16)
		break;
	case K_RETF: {
		int n = bits / 8;
		jt_stack_ea(0);
		jt_load(A1, n);
		jt_stack_ea(n);
		jt_load(A3, 2);
		u32 add = 2 * n + (b == 0xca ? in->imm : 0);
		if (jt_mode & JM_STACK16) { e_li(T6, add); e_add(A6, jg[4], T6); jt_set_r(4, A6, 16, false); }
		else { e_li(T6, add); e_add(jg[4], jg[4], T6); }
		jt_flags_store();
		jt_seg_load(SEG_CS, A3);
		e_slli(A2, A3, 4);
		jt_ijc_exit(A2);
		break;
	}
	case K_FARIND: {
		int n = bits / 8;
		jt_ea(in, 0);
		jt_load(A1, n);
		jt_ea(in, n);
		jt_load(A3, 2);
		if (in->reg == 3) {   // CALL: the return address
			e_lw(A0, R_CPU, SEG_SEL_OFF(SEG_CS));
			e_li(A4, in->eip + in->len);
			jt_push2(A0, A4, n);
		}
		jt_flags_store();
		jt_seg_load(SEG_CS, A3);
		e_slli(A2, A3, 4);
		jt_ijc_exit(A2);
		break;
	}
	case K_INT: {   // as tiny386's call_isr in real mode: FLAGS, CS, IP pushed; IF and TF cleared
		jt_flags_store();
		e_lw_abs(T6, &t386_int_vec[in->imm & 0xff]);   // (counted: serial "cpu")
		e_addi(T6, T6, 1);
		e_sw_abs(T6, T0, &t386_int_vec[in->imm & 0xff]);
		e_mv(A0, R_CPU);
		e_call((const void *) jit_h_pushf);   // (the flags made whole: cc.mask = 0)
		jt_push(A0, 2, 2);
		e_lw(A0, R_CPU, SEG_SEL_OFF(SEG_CS));
		jt_push(A0, 2, 2);
		e_li(A0, in->eip + in->len);
		jt_push(A0, 2, 2);
		e_lw(T0, R_CPU, CPU_OFF(idt.base));
		e_addi(T0, T0, (in->imm & 0xff) * 4);
		jt_load(A1, 2);
		e_addi(T0, T0, 2);
		jt_load(A3, 2);
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_andi(T6, T6, ~(IF | TF));
		e_sw(T6, R_CPU, CPU_OFF(flags));
		jt_pf = PF_NONE;
		jt_seg_load(SEG_CS, A3);
		e_slli(A2, A3, 4);
		jt_ijc_exit(A2);
		break;
	}
	case K_IRET: {   // real mode, 16 bits: IP, CS, FLAGS popped; an interrupt now enabled and pending: the dispatcher
		jt_stack_ea(0);
		jt_load(A1, 2);
		jt_stack_ea(2);
		jt_load(A3, 2);
		jt_stack_ea(4);
		jt_load(A4, 2);
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_srli(T6, T6, 16);
		e_slli(T6, T6, 16);
		e_or(T6, T6, A4);
		e_lw(A5, R_CPU, CPU_OFF(flags_mask));
		e_and(T6, T6, A5);
		e_ori(T6, T6, 2);
		e_sw(T6, R_CPU, CPU_OFF(flags));
		e_sw(X0, R_CPU, CPU_OFF(cc.mask));
		jt_pf = PF_NONE;
		if (jt_mode & JM_STACK16) { e_addi(A6, jg[4], 6); jt_set_r(4, A6, 16, false); }
		else e_addi(jg[4], jg[4], 6);
		jt_seg_load(SEG_CS, A3);
		e_lw(T6, R_CPU, CPU_OFF(flags));
		e_andi(T6, T6, IF);
		u32 *no = e_branch(BEQ, T6, X0);
		e_lbu(T6, R_CPU, CPU_OFF(intr));
		u32 *none = e_branch(BEQ, T6, X0);
		e_li(A0, EXIT_STOP);   // (a1: the next eip)
		{
			u32 t = (u32) (uintptr_t) t386_jit_exit;
			s32 lo = (s32) (t << 20) >> 20;
			e_lui(T6, t - (u32) lo);
			e_jalr(X0, T6, lo);
		}
		j_bind(no, jp);
		j_bind(none, jp);
		e_slli(A2, A3, 4);
		jt_ijc_exit(A2);
		break;
	}
	case K_JMPI: case K_CALLI:
		jt_get_e(in, A1, bits, false);
		if (in->kind == K_CALLI) {
			e_li(A0, in->eip + in->len);
			jt_push(A0, bits / 8, bits / 8);
		}
		jt_flags_store();
		jt_ijc_exit(-1);
		break;
	case K_LOOP: {
		u32 disp = (u32) (s32) (s8) in->imm;
		jt_flags_store();
		if (b == 0xe3) {   // JCXZ
			if (in->adsz16) jt_zext(T2, jg[1], 16);
			else e_mv(T2, jg[1]);
			e_sltiu(T2, T2, 1);
		} else {
			e_addi(T2, jg[1], -1);
			if (in->adsz16) { jt_set_r(1, T2, 16, false); jt_zext(T2, T2, 16); }
			else e_mv(jg[1], T2);
			e_sltu(T2, X0, T2);   // CX != 0
			if (b != 0xe2) {      // LOOPE, LOOPNE: and ZF
				jt_cond(b == 0xe1 ? 4 : 5, A0);
				e_and(T2, T2, A0);
			}
		}
		u32 *taken = e_branch(BNE, T2, X0);
		jt_link_exit(in->eip + in->len);
		j_bind(taken, jp);
		jt_link_exit(jt_target(in, disp));
		break;
	}
	case K_REPSTR:
		jt_repstr(in, bits);
		break;
	case K_REPCMP:
		jt_repcmp(in, bits);
		break;
	default:
		jt_interp(in);
		break;
	}
}

// ---- a block

// leave for the interpreter at st->eip, with the flags as they were then: through t386_jit_leave(_f), the x86
// address (and the flags' routine) after the call
static void jt_stub_leave(JStub *st)
{
	if (st->refund) e_addi(R_BUDGET, R_BUDGET, st->refund);   // it and the rest weren't done here
	jt_pf = st->pf;
	jt_pf_bits = st->pf_bits;
	jt_pf_stored = st->pf_stored;
	u32 *f = NULL;
	if (jt_pf > PF_DEAD && !jt_pf_stored) {
		f = jt_fstore_of(jt_pf, jt_pf_bits);
		if (!f) {   // (none made for these: inline)
			jt_flags_store();
			jt_exit(EXIT_INTERP, st->eip);
			return;
		}
	}
	e_call_near(f ? jit_t_leave_f : jit_t_leave);
	E(st->eip);
	if (f) E((u32) (uintptr_t) f);
}

#ifdef T386_JIT_SIZES   // (a measurement: where the code's words go, printed at exit)
static long jsz_kind[64][2], jsz_stub[32][2], jsz_pro[2];
// (serial "jit sizes": since the last time) the words of each kind's
// translations, of each kind's stubs (+8: with paging) and of the blocks in all
void t386_jit_sizes_report(void (*out)(const char *))
{
	char line[80];
	for (int k = 0; k < 64; k++)
		if (jsz_kind[k][1]) {
			snprintf(line, sizeof line, "kind %2d: %8ld ins %6.1f words, %8ld in all", k, jsz_kind[k][1],
				 (double) jsz_kind[k][0] / jsz_kind[k][1], jsz_kind[k][0]);
			out(line);
		}
	for (int k = 0; k < 32; k++)
		if (jsz_stub[k][1]) {
			snprintf(line, sizeof line, "stub %2d: %8ld      %6.1f words, %8ld in all", k, jsz_stub[k][1],
				 (double) jsz_stub[k][0] / jsz_stub[k][1], jsz_stub[k][0]);
			out(line);
		}
	snprintf(line, sizeof line, "inline, all: %ld blocks %.1f words each", jsz_pro[1], (double) jsz_pro[0] / (jsz_pro[1] + !jsz_pro[1]));
	out(line);
	memset(jsz_kind, 0, sizeof jsz_kind);
	memset(jsz_stub, 0, sizeof jsz_stub);
	memset(jsz_pro, 0, sizeof jsz_pro);
}
#ifndef ESP_PLATFORM
static void jsz_line(const char *l) { fprintf(stderr, "%s\n", l); }
__attribute__((destructor)) static void jsz_report(void) { t386_jit_sizes_report(jsz_line); }
#endif
#define JSZ(a, k, from) (a[k][0] += jp - (from), a[k][1]++)
#else
#define JSZ(a, k, from)
#endif
// With paging, a read's or a write's slow path in two words (three with pending flags), if it can be: a call to the
// routine for its size and register (jit_t_cprd, jit_t_cpwr), then a word for its slow path (t386_jit_cprd*,
// t386_jit_cpwr*) to go on at st->resume, back from that word by its bits 0-12 in words, or to leave as
// jt_stub_leave would, with bits 13-18 the instructions to refund, bits 19-30 st->eip from the block's and bit 31
// the routine that writes out the pending flags in the word after it (t386_jit_h_cfault)
static bool jt_stub_compact(const JStub *st, int k)
{
	u32 *const *to = st->kind == STUB_READ ? jit_t_cprd[k] : jit_t_cpwr[st->redo][k];
	if (st->reg >= 32 || !to[st->reg]) return false;
	u32 *f = NULL;
	if (st->pf > PF_DEAD && !st->pf_stored && !(f = jt_fstore_of(st->pf, st->pf_bits))) return false;
	uword off = jt_cs_base + st->eip - jt_lin;
	int refund = st->refund;
#ifdef T386_JIT_VERIFY
	refund = 0;
#endif
	if (off > 0xfff || refund < 0 || refund > 63) return false;
	e_call_near(to[st->reg]);
	long back = jp - st->resume;
	if (back <= 0 || back >= 1 << 13) abort();   // (blocks are far smaller)
	E((u32) back | (u32) refund << 13 | (u32) off << 19 | (f ? 1u << 31 : 0));
	if (f) E((u32) (uintptr_t) f);
	return true;
}

static void jt_emit_stubs(void)
{
#ifdef T386_JIT_SIZES
	u32 *jsz_from = jp;
#endif
	for (int i = 0; i < jt_nstubs; i++) {
		JStub *st = &jt_stubs[i];
#ifdef T386_JIT_SIZES
		if (i) JSZ(jsz_stub, jt_stubs[i - 1].kind + ((jt_mode & JM_PAGED) ? 8 : 0), jsz_from);
		jsz_from = jp;
#endif
		j_bind(st->at, jp);
		for (int k = 0; k < 5; k++) j_bind(st->more[k], jp);
		if ((jt_mode & JM_PAGED) && (st->kind == STUB_READ || st->kind == STUB_WRITE)) {   // with paging
			int k = st->size == 1 ? 0 : st->size == 2 ? 1 : 2;
			if (jt_stub_compact(st, k)) continue;
			if (st->kind == STUB_WRITE) e_mv(T6, st->reg);
			e_call_near(st->kind == STUB_READ ? jit_t_prd[k] : jit_t_pwr[st->redo][k]);
			if (st->kind == STUB_READ) e_addi(st->reg, T6, 0);   // (one instruction even for t6: the helper skips it)
			j_bind(e_jump(), st->resume);
			jt_stub_leave(st);   // (a fault: the helper returns here)
			continue;
		}
		if ((jt_mode & JM_PAGED) && st->kind == STUB_RMW) {   // plain RAM after all: its host address in t1
			int k = st->size == 1 ? 0 : st->size == 2 ? 1 : 2;
			e_call_near(jit_t_prmw[k]);
			u32 *no = e_branch(BEQ, T6, X0);
			e_mv(T1, T6);
			j_bind(e_jump(), st->resume);
			j_bind(no, jp);
			jt_stub_leave(st);
			continue;
		}
		switch (st->kind) {
		case STUB_FCALL:   // an x87 instruction's helper, when its inline code can't (jt_fpu_slow)
			e_lw(A0, R_CPU, CPU_OFF(fpu));
			if (st->a1 >= 0) e_li(A1, st->a1);
			if (st->a2 >= 0) e_li(A2, st->a2);
			e_call_abs(st->fn);
			j_bind(e_jump(), st->resume);
			break;
		case STUB_READ:
			e_call_abs(st->size == 1 ? (void *) t386_jit_rd1 : st->size == 2 ? (void *) t386_jit_rd2 : (void *) t386_jit_rd4);
			e_mv(st->reg, T6);
			j_bind(e_jump(), st->resume);
			break;
		case STUB_WRITE: {   // t6 nonzero after: the write would overwrite translated code (jit_h_write)
			e_mv(T6, st->reg);
			e_call_near(jit_vga_wr[st->redo][st->size == 1 ? 0 : st->size == 2 ? 1 : 2]);
			u32 *code = st->redo ? e_branch(BNE, T6, X0) : NULL;
			j_bind(e_jump(), st->resume);
			if (code) {   // not written: the interpreter does the instruction, then the new code runs
				j_bind(code, jp);
				jt_stub_leave(st);
			}
			break;
		}
		case STUB_RMW: {   // plain RAM after all? back with its host address in t1; else as STUB_EXIT
			e_call_near(jit_rmw_chk[st->size == 1 ? 0 : st->size == 2 ? 1 : 2]);
			u32 *no = e_branch(BEQ, T6, X0);
			e_add(T1, T0, R_MEM);
			j_bind(e_jump(), st->resume);
			j_bind(no, jp);
			jt_stub_leave(st);
			break;
		}
		case STUB_CHECK0:   // a self-checking block's first instruction changed: noted, then as STUB_EXIT
			e_li(T6, st->eip + jt_cs_base);
			e_sw_abs(T6, T0, &jit_check_fail_lin);
			// fall through
		case STUB_EXIT:   // the instruction at eip goes to the interpreter, with the flags as they were
			jt_stub_leave(st);
			break;
		case STUB_IRQ:   // STI with an interrupt waiting: leave after the next instruction, to have it taken
			if (st->refund > 1) e_addi(R_BUDGET, R_BUDGET, st->refund - 1);   // the rest wasn't done
			jt_pf = st->pf;
			jt_pf_bits = st->pf_bits;
			jt_pf_stored = st->pf_stored;
			jt_flags_store();
			jt_exit(EXIT_STOP, st->eip);
			break;
		case STUB_BUDGET:   // (the block's entry: one word, jit_t_bexit)
			e_call_near(jit_t_bexit);
			break;
		case STUB_STOP: {   // jit_h_interp stopped the block: its reason, at cpu->next_ip
			if (st->refund > 1) e_addi(R_BUDGET, R_BUDGET, st->refund - 1);   // the rest wasn't done
			e_li(A3, jt_mode);
			e_jump_abs(jit_r_stop);
			break;
		}
		}
	}
#ifdef T386_JIT_SIZES
	if (jt_nstubs) JSZ(jsz_stub, jt_stubs[jt_nstubs - 1].kind + ((jt_mode & JM_PAGED) ? 8 : 0), jsz_from);
#endif
}

static inline bool jt_is_sti(const JIns *in) { return in->kind == K_FLAGOP && in->op == 0xfb && !in->two; }
static inline bool jt_is_movss(const JIns *in)   // MOV SS or POP SS
{
	return (in->kind == K_MOVSEG && in->op == 0x8e && in->reg == SEG_SS) || (in->kind == K_POPSEG && in->op == 0x17);
}

// Decode and translate the block at CS:eip (linear lin); NULL if not even its first instruction can be.
// (phys: the physical address of lin; with paging, JM_PAGED, the block stays within that page)
static JitBlock *jit_translate_n(CPUI386 *cpu, uword eip, uword lin, u8 mode, int max_ins, uword phys);
// A block too long for its branches to reach the x87 helpers' calls after it (STUB_FCALL) is made again with the
// calls in place (jt_fpu_slow); one too long even so: the callers make a shorter one.
// A dead block translated from the bytes at phys as they are now, for the same address, CS base and mode, back in
// place (see jit_grave), or NULL. (Its exits' links went when the blocks they led to died, or still lead to live ones;
// the links into it were kept, its entry guarded: jit_invalidate.)
static JitBlock *jit_revive(CPUI386 *cpu, uword lin, u8 mode, uword phys)
{
	if (!jit_grave || (mode & JM_PAGED)) return NULL;
	const uword cs_base = cpu->seg[SEG_CS].base;
	const bool checked = jit_volatile(phys);
	for (JitBlock **g = &jit_grave[jit_grave_hash(phys)]; *g; g = &(*g)->gnext) {
		JitBlock *b = *g;
		if (b->phys != phys || b->lin != lin || b->cs_base != cs_base || b->mode != mode || b->checked != checked ||
		    phys + b->nbytes > (uword) cpu->phys_mem_size || memcmp(b->src, cpu->phys_mem + phys, b->nbytes))
			continue;
		*g = b->gnext;
		b->gnext = NULL;
		b->dead = 0;
		b->fails = 0;
		b->born = cpu->cycle;
		if (b->guarded) {   // (the links into it lead here again)
			b->code[0] = b->entry[0];
			b->code[1] = b->entry[1];
			t386_jit_sync(b->code, 8);
			JIT_SUM_OF(b->code);
			b->guarded = 0;
		}
		if (checked) {   // (as jit_translate_n: in the table only)
			unsigned h = jit_hash_of(b->lin);
			b->hnext = jit_hash[h];
			jit_hash[h] = b;
		} else {
			jit_add_block(b);
		}
		JIT_SUM_SET(b);
		jit_stat_revived++;
		return b;
	}
	return NULL;
}

static JitBlock *jit_translate(CPUI386 *cpu, uword eip, uword lin, u8 mode, int max_ins, uword phys)
{
	JitBlock *r = jit_revive(cpu, lin, mode, phys);
	if (r) return r;
	JitBlock *b = jit_translate_n(cpu, eip, lin, mode, max_ins, phys);
	if (!b && joverflow && !jt_fcall_here) {
		jt_fcall_here = true;
		b = jit_translate_n(cpu, eip, lin, mode, max_ins, phys);
		jt_fcall_here = false;
	}
	return b;
}
static uword jit_code_phys(CPUI386 *cpu, uword lin);

// A block whose last instruction runs on into the next page (jit_translate_n): it holds while that page still lies
// after the block's own in RAM. (Translated so like code without paging, its bytes in one stretch of RAM, written
// there as such; the dispatcher checks this before it enters the block, and other blocks jump into it through its
// xentry, which leaves for the dispatcher after a TLB flush. Linux's memmove has an instruction across a page, run
// for every 16 bytes it moves: before, each time by the interpreter, from the dispatcher.)
static bool jit_crossing_ok(CPUI386 *cpu, JitBlock *b)
{
	const uword next = (b->lin | 0xfff) + 1;
	return jit_code_phys(cpu, next) == (b->phys | 0xfff) + 1;
}

static JIT_SMALL JitBlock *jit_translate_n(CPUI386 *cpu, uword eip, uword lin, u8 mode, int max_ins, uword phys)
{
	const bool paged = mode & JM_PAGED;
	bool crossing = false;
	// Code that keeps being rewritten soon after it was translated (jit_volatile) is translated to check itself
	// instead (a program that writes a RETF into its compiled scalers and restores it, for every column drawn).
	bool checked = jit_volatile(phys);
	// pass 1: decode
	int n = 0, ndyn = 0;
	u32 dyn[4] = { 0, 0, 0, 0 };
	uword cur = eip, bytes = 0;
	while (n < max_ins && bytes < 1024) {
		uword l = lin + bytes, pl = phys + bytes;
		int avail = 15;
		if (paged) {   // the page's end ends the block (an instruction across it too, but the last: jit_crossing_ok)
			if ((l >> 12) != (lin >> 12)) break;
			if (0x1000 - (l & 0xfff) < 15) {
				const uword next = (pl | 0xfff) + 1;
				if ((t386_jit_xlink & 2) && next < (uword) cpu->phys_mem_size && !pm_io(next >> 12) &&
				    jit_code_phys(cpu, (l | 0xfff) + 1) == next)
					avail = 15;
				else
					avail = 0x1000 - (l & 0xfff);
			}
		} else {   // up to the next page that isn't RAM
			if (l >= (uword) cpu->phys_mem_size || pm_io(l >> 12)) break;
			uword next_page = (l | 0xfff) + 1;
			if (next_page - l < 15 && (next_page >= (uword) cpu->phys_mem_size || pm_io(next_page >> 12)))
				avail = next_page - l;
		}
		JIns *in = &jt_ins[n];
		if (!jit_decode(cpu->phys_mem + pl, avail, mode & JM_CODE16, in)) break;
		if (n && jit_volatile(pl)) break;   // a self-modifying block starts here: the interpreter
		if ((mode & JM_CODE16) && cur + in->len > 0x10000) break;   // wrapping at 64 KB: the interpreter
		in->eip = cur;
		bool ends;
		jt_classify(in, mode, &ends);
		// a rewritten immediate: the interpreter reads it (not one of a jump that ends the block: its target is
		// translated in, and a write to it invalidates the block)
		if (in->imm_sz && ndyn < 4 && (in->kind == K_INTERP || !ends) && jit_imm_dynamic(pl + in->imm_at, in->imm_sz)) {
			in->imm_dyn = 1;
			dyn[ndyn++] = (pl + in->imm_at) << 3 | in->imm_sz;
			bool reads = in->kind == K_ALU || (in->kind == K_MOV && !in->two &&
				     ((in->op >= 0xb0 && in->op <= 0xbf) || in->op == 0xc6 || in->op == 0xc7));   // (jt_imm)
			if (!reads) { in->kind = K_INTERP; in->fl = FL_READ; }
		}
		// a short Jcc whose condition is rewritten (a game's ray caster turns JGE into JLE and back for
		// every ray): its opcode is read where it runs (jt_emit), and writes to it don't invalidate the block
		if (in->kind == K_JCC && !in->two && in->len == 2 && ndyn < 4 && jit_imm_dynamic(pl, 1)) {
			in->op_dyn = 1;
			dyn[ndyn++] = pl << 3 | 1;
		}
		// a rewritten displacement: read where it is (jt_ea; the interpreter reads it anyway)
		if (in->disp_at && in->mod != 3 && !checked && ndyn < 4 && jit_imm_dynamic(pl + in->disp_at, 4)) {
			in->disp_dyn = 1;
			dyn[ndyn++] = (pl + in->disp_at) << 3 | 4;
		}
		// MOV SS and STI: the instruction after them comes before any interrupt (after MOV SS, MOV SP as a rule;
		// after STI, the HLT of STI; HLT): in the same block. A block ending with one would leave before it, and
		// an interrupt could come in between, at the end of a step.
		if ((jt_is_movss(in) || jt_is_sti(in)) && (n + 1 >= max_ins || ends)) break;
		in->ends = ends;
		// (with paging, any access may fault; without, a write may find translated code there: jit_h_write; and
		// without memory, DIV may fault, and MOV to a segment register in protected mode)
		in->may_exit = jt_rmw_ins(in) || checked || paged || in->op_dyn || (jt_segs_used(in) && !jt_reads_only(in)) ||
			       (in->kind == K_MULDIV && in->reg >= 6) || in->kind == K_FPU || in->kind == K_FWAIT ||
			       (in->kind == K_MOVSEG && in->op == 0x8e && (mode & JM_PROTECTED));
		// two after an STI: the block may leave before it, for an interrupt (the flags must be there)
		if (n >= 2 && jt_is_sti(&jt_ins[n - 2])) in->may_exit = true;
		n++;
		cur += in->len;
		bytes += in->len;
		if (paged && ((l & 0xfff) + in->len > 0x1000)) {   // (a jump too: before the break for one that ends)
			crossing = true;
			break;
		}
		if (ends) break;
	}
	if (n && (jt_is_movss(&jt_ins[n - 1]) || jt_is_sti(&jt_ins[n - 1]))) {
		n--;   // MOV SS and STI can't end a block (see above): they go with the next instruction, in another (or the
		       // interpreter, which runs both in one step)
		cur -= jt_ins[n].len;
		bytes -= jt_ins[n].len;
	}
	if (!n) return NULL;
	// pass 2: whose flags are read. At the end of the block they are, always: the instructions after it may change
	// (looking ahead into them made the block depend on code it isn't invalidated with), and an interrupt between
	// blocks pushes them.
	jt_cs_base = cpu->seg[SEG_CS].base;
	bool live = true;
	unsigned segs = 0;
	for (int i = n - 1; i >= 0; i--) {
		jt_ins[i].fl_live = live;
		if (jt_ins[i].fl == FL_WRITE) live = false;
		if (jt_ins[i].fl == FL_READ || jt_ins[i].may_exit) live = true;
		segs = (segs & ~jt_seg_loaded(&jt_ins[i])) | jt_segs_used(&jt_ins[i]);
	}
	// pass 3: emit
	if (!jt_stubs && !(jt_stubs = (JStub *) t386_jit_alloc(sizeof(JStub) * JIT_MAX_STUBS))) return NULL;
	if (jit_nblocks == jit_max_blocks || jit_cache_end - jit_cache_free < 4096 + (JIT_RT_MAX + JIT_X87_POOL) / 4) jit_flush();
	jp = jit_cache_free;
	jlimit = jit_cache_end;
	joverflow = false;
	if ((u8 *) jp - (u8 *) jit_rt_at > JIT_RT_REACH) {   // the routines out of a jal's reach: a copy here
		u32 *at = jp;
		jit_make_routines();
		t386_jit_sync(at, (u8 *) jp - (u8 *) at);
		jit_cache_free = jp;
	}
	jt_nstubs = 0;
	jt_stubs_full = false;
	jt_refund = 0;
	jt_pf = PF_NONE;
	jt_pf_stored = false;
	jt_x87_nm_ok = false;
	jt_seg_pending = -1;
	jt_mode = mode;
	jt_x87_cw = cpu->fpu ? t386_fpu_get_cw(cpu->fpu) : 0x37f;
	jt_cs_base = cpu->seg[SEG_CS].base;
	jt_lin = lin;
	jt_phys_off = phys - lin;
	jt_cur_eip = eip;
	u32 *code = jp;
	JStub *st = jt_stub(STUB_BUDGET);   // instructions left for this step? (a block may run on past its end:
	st->size = 0;                       // interrupts come a few instructions later, and no instructions are left
	st->eip = eip;                      // for the interpreter)
	st->at = e_branch(BGE, X0, R_BUDGET);
	// null selectors fault: the interpreter. The same exit as for the budget, and the dispatcher sends a block
	// whose segments are null to the interpreter (SS is never null in protected mode: loading a null one faults)
	const unsigned nsegs = (mode & JM_PROTECTED) ? segs & ~(1u << SEG_SS) : 0;
	if (nsegs) {
		e_lbu(T0, R_CPU, JIT_NS_OFF);
		e_andi(T0, T0, nsegs);
		st->more[0] = e_branch(BNE, T0, X0);
	}
	e_addi(R_BUDGET, R_BUDGET, -n);
	jt_refund = n;   // (none of it done, if it leaves at once)
	for (int i = 0; i < n; i++) {
		jt_refund = n - i;
		jt_cur_eip = jt_ins[i].eip;
		jt_no_redo = false;
		if (checked) jt_check_bytes(&jt_ins[i], cpu->phys_mem + phys + (jt_ins[i].eip - eip), i == 0);
#ifdef T386_JIT_SIZES
		u32 *jsz_from = jp;
		jt_emit(&jt_ins[i]);
		JSZ(jsz_kind, jt_ins[i].kind, jsz_from);
#else
		jt_emit(&jt_ins[i]);
#endif
		if (jt_seg_pending >= 0) {   // (an address not used: a mistake here; the block is not made)
			jt_seg_pending = -1;
			joverflow = true;
		}
		switch (jt_ins[i].kind) {   // (those that may change CR0: jt_fpu_nm)
		case K_INTERP: case K_PMFAR: case K_JMPF: case K_CALLF: case K_RETF: case K_FARIND: case K_INT: case K_IRET:
			jt_x87_nm_ok = false;
		}
		if (i && jt_is_sti(&jt_ins[i - 1]) && !jt_ins[i].ends) {   // an interrupt waiting since STI: now
			e_lbu(T6, R_CPU, CPU_OFF(intr));
			JStub *st = jt_stub(STUB_IRQ);
			st->eip = jt_ins[i].eip + jt_ins[i].len;
			st->at = e_branch(BNE, T6, X0);
		}
	}
	// on to the next instructions, unless the last one left the block itself (an interpreted one may not)
	if (!jt_ins[n - 1].ends || jt_ins[n - 1].kind == K_INTERP) {
		jt_flags_store();
		jt_link_exit(cur);
	}
	jt_refund = 0;
#ifdef T386_JIT_SIZES
	jsz_pro[0] += 0;   // (prologue and the fall-through exit: the rest of the block's words, below)
	u32 *jsz_st = jp;
#endif
	jt_emit_stubs();
#ifdef T386_JIT_SIZES
	if (!jt_stubs_full && !joverflow) { jsz_pro[0] += jsz_st - code; jsz_pro[1]++; }
#endif
	if (jt_stubs_full) return n > 1 ? jit_translate(cpu, eip, lin, mode, n / 2, phys) : NULL;   // (jp: back at the start)
	if (joverflow) return NULL;
	JitBlock *b = &jit_blocks[jit_nblocks++];
	memset(b, 0, sizeof(*b));
	b->lin = lin;
	b->phys = phys;
	b->cs_base = cpu->seg[SEG_CS].base;
	b->mode = mode;
	b->nsegs = nsegs;
	b->nbytes = bytes;
	b->born = cpu->cycle;
	memcpy(b->dyn, dyn, sizeof dyn);
	if (jit_src && !paged && jit_src_free + bytes <= jit_src + JIT_SRC_BYTES) {   // (jit_revive)
		memcpy(jit_src_free, cpu->phys_mem + phys, bytes);
		b->src = jit_src_free;
		jit_src_free += bytes;
	}
	b->checked = checked;
	b->crossing = crossing && (bytes + (lin & 0xfff) > 0x1000);
	b->code = code;
#if defined(T386_JIT_SIZES) && !defined(ESP_PLATFORM)   // (some blocks' code, to look at: riscv32 objdump -b binary -m riscv:rv32 -D)
	{
		static int dumped;
		if (jit_stat_ins > 150000 && dumped < 40) {
			char name[64];
			snprintf(name, sizeof name, "blk_%02d_%08x_m%02x.bin", dumped++, lin, mode);
			FILE *f = fopen(name, "wb");
			if (f) { fwrite(code, 1, (u8 *) jp - (u8 *) code, f); fclose(f); }
		}
	}
#endif
	jit_cache_free = jp;
	jit_stat_ins += n;
	jit_stat_bytes += (u8 *) jp - (u8 *) code;
	t386_jit_sync(code, (u8 *) jp - (u8 *) code);
	JIT_SUM_SET(b);
	if (b > jit_blocks) JIT_SUM_SET(b - 1);   // (its end: this block's start; routines copied between them, maybe)
	if (checked) {   // in the table only: writes there don't concern it
		unsigned h = jit_hash_of(b->lin);
		b->hnext = jit_hash[h];
		jit_hash[h] = b;
	} else {
		jit_add_block(b);
	}
	return b;
}

// ---- running

static inline u8 jit_mode_of(CPUI386 *cpu)
{
	if (cpu->flags & VM)
		return JM_VM | (cpu->code16 ? JM_CODE16 : 0) | (cpu->sp_mask == 0xffff ? JM_STACK16 : 0) |
		       ((cpu->cr0 & CR0_PG) ? JM_PAGED : 0);
	return (cpu->code16 ? JM_CODE16 : 0) | ((cpu->cr0 & 1) ? JM_PROTECTED : 0) | (cpu->sp_mask == 0xffff ? JM_STACK16 : 0) |
	       ((cpu->cr0 & CR0_PG) ? JM_PAGED : 0) |
	       (!(cpu->cr0 & 1) || cpu->cpl <= get_IOPL(cpu) ? JM_IO : 0) |
	       ((cpu->cr0 & 1) && !(cpu->seg[SEG_DS].base | cpu->seg[SEG_ES].base | cpu->seg[SEG_SS].base) ? JM_FLAT : 0);
}

static long jit_stat_blocks, jit_stat_translated, jit_stat_interp, jit_stat_links;

void t386_jit_ops_report(char *buf, int len)   // the opcodes run in place most
{
	static const char *const modes[4] = { "real", "v86", "pm16", "pm32" };
	int n = 0;
	buf[0] = 0;
	if (!jit_count_ops) {   // (counting costs every instruction run in place a little: from now on only)
		jit_count_ops = true;
		snprintf(buf, len, "counting the instructions run in place from now on: ask again\n");
		return;
	}
	if (!jit_interp_ops) return;
	for (int k = 0; k < 20 && n < len - 40; k++) {
		int best = -1;
		for (int i = 0; i < 4 * 540; i++)
			if (jit_interp_ops[i] && (best < 0 || jit_interp_ops[i] > jit_interp_ops[best])) best = i;
		if (best < 0) break;
		int op = best % 540;
		n += snprintf(buf + n, len - n, "%-4s ", modes[best / 540]);
		if (op >= 528)
			n += snprintf(buf + n, len - n, "rep %02x: %u\n", op - 528 + 0xa4, jit_interp_ops[best]);
		else if (op >= 512)
			n += snprintf(buf + n, len - n, "%02x /%d: %u\n", 0xfe + ((op - 512) >> 3), op & 7, jit_interp_ops[best]);
		else
			n += snprintf(buf + n, len - n, "%s%02x: %u\n", op >= 256 ? "0f " : "", op & 255, jit_interp_ops[best]);
		jit_interp_ops[best] = 0;
	}
}

// The block whose translated code holds host address pc (where time in translated code goes):
// 1 and its address, size and kind; 2 for the routines blocks call (any copy), *lin which (t386_jit_rt_names) and
// *nbytes the instruction in them; 0 if pc isn't in the cache.
const char *const t386_jit_rt_names[] = { "flags stores", "VGA writes", "indirect jumps (jit_r_ijc)", "jit_r_stop",
	"TLB lookups (jit_r_tlb)", "to paging's slow paths", "to the exits", "flags not in the block (jit_r_cf, jit_r_cond)",
	"jit_r_callc", "jit_r_interp",
	"x87 (jit_x87_routine_of)" };
int t386_jit_block_at(uint32_t pc, uword *lin, int *nbytes, int *checked, int *dead)
{
	if (!jit_ready || pc < (uint32_t) (uintptr_t) jit_cache || pc >= (uint32_t) (uintptr_t) jit_cache_free) return 0;
	for (int i = 0; i < jit_rt_copies; i++) {
		const u32 off = pc - (u32) (uintptr_t) jit_rt_copy[i];
		if (off >= jit_rt_bytes) continue;
		const u32 *const first = jit_rt_first.jit_rt_at, *const at = first + off / 4;   // (the copies are alike)
		const u32 *const starts[] = { first, jit_rt_first.jit_vga_wr[0][0], jit_rt_first.jit_r_ijc,
			jit_rt_first.jit_r_stop, jit_rt_first.jit_r_tlb[0][0], jit_rt_first.jit_t_prd[0],
			jit_rt_first.jit_t_leave, jit_rt_first.jit_r_cf, jit_rt_first.jit_r_callc, jit_rt_first.jit_r_interp,
			jit_rt_first.jit_x87_free };   // (that one: where the pool begins, the copies' saved before any)
		*lin = 0;
		for (unsigned k = 1; k < sizeof starts / sizeof *starts; k++)
			if (at >= starts[k]) *lin = k;
		*nbytes = at - starts[*lin];   // (its instruction)
		return 2;
	}
	int lo = 0, hi = jit_nblocks - 1;   // blocks are made in the order of their code: the last one at or before pc
	if (hi < 0) return 0;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if ((uint32_t) (uintptr_t) jit_blocks[mid].code <= pc) lo = mid; else hi = mid - 1;
	}
	JitBlock *b = &jit_blocks[lo];
	*lin = b->lin;
	*nbytes = b->nbytes;
	*checked = b->checked;
	*dead = b->dead;
	return 1;
}

// serial "jit code <hex lin>": the newest block at linear address lin, its x86 bytes (from its first page) and its
// translated code, as hex lines for objdump (-b binary -m i386, and riscv32's)
void t386_jit_code_report(uword lin, void (*out)(const char *))
{
	char line[120];
	JitBlock *b = NULL;
	for (int i = jit_ready ? jit_nblocks - 1 : -1; i >= 0 && !b; i--)
		if (jit_blocks[i].lin == lin && !jit_blocks[i].dead) b = &jit_blocks[i];
	if (!b) { out("no block there"); return; }
	snprintf(line, sizeof line, "block %08x +%u, mode %02x, code at %p", (unsigned) b->lin, b->nbytes, b->mode, (void *) b->code);
	out(line);
	int n = snprintf(line, sizeof line, "x86");
	for (unsigned k = 0; k < b->nbytes && ((b->phys + k) & 4095) >= (b->phys & 4095); k++) {
		n += snprintf(line + n, sizeof line - n, " %02x", jit_cpu->phys_mem[b->phys + k]);
		if (n > 100) { out(line); n = snprintf(line, sizeof line, "x86"); }
	}
	if (n > 3) out(line);
	const u32 *end = b + 1 < jit_blocks + jit_nblocks ? b[1].code : jit_cache_free;
	for (const u32 *p = b->code; p < end && p < b->code + 600; p += 8) {
		n = snprintf(line, sizeof line, "rv");
		for (int k = 0; k < 8 && p + k < end; k++) n += snprintf(line + n, sizeof line - n, " %08x", (unsigned) p[k]);
		out(line);
	}
}

// the host address of the code of the block that holds host address pc (t386_jit_block_at's)
uint32_t t386_jit_block_code(uint32_t pc)
{
	int lo = 0, hi = jit_nblocks - 1;
	if (!jit_ready || hi < 0) return 0;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if ((uint32_t) (uintptr_t) jit_blocks[mid].code <= pc) lo = mid; else hi = mid - 1;
	}
	return (uint32_t) (uintptr_t) jit_blocks[lo].code;
}

void t386_jit_inv_report(char *buf, int len)
{
	int n = 0;
	for (unsigned k = 0; k < 32 && k < jit_inv_n && n < len - 80; k++) {
		unsigned i = (jit_inv_n - 1 - k) & 31;
		n += snprintf(buf + n, len - n, "block %08x+%u: write %08x/%u by %04x:%08x\n", jit_inv_log[i].lin,
			      jit_inv_log[i].nbytes, jit_inv_log[i].waddr, jit_inv_log[i].wlen, jit_inv_log[i].cs, jit_inv_log[i].eip);
	}
}

#ifdef T386_FPU_STATS
// serial "fpu" (T386_FPU_STATS builds): the x87 instructions translated code ran since the last time, the most
// first, and the helpers' quick ways taken and why not (t386_fpu_jit.h); then the counts start again
void t386_fpu_report(char *buf, int len)
{
	static const char *const why[] = { "m32 arith: quick", "operand", "ST(0)", "PC", "result",
		"reg arith: quick", "ST(0)", "ST(i)", "PC", "result", "FLD m32: quick", "other", "FST m32: quick",
		"other", "FIST: quick", "other", "FLD/FXCH/FST ST(i): quick", "other", "fparith: calls", "by its hook",
		"arith after the inline code: not kept", "a zero operand", "a zero result", "the control word" };
	u32 total = 0;
	for (int i = 0; i < 128; i++) total += jit_fpu_form[i];
	int n = snprintf(buf, len, "x87 in translated code: %u\n", (unsigned) total);
	for (int k = 0; k < 24 && n < len; k++) {
		int best = -1;
		for (int i = 0; i < 128; i++)
			if (jit_fpu_form[i] && (best < 0 || jit_fpu_form[i] > jit_fpu_form[best])) best = i;
		if (best < 0) break;
		n += snprintf(buf + n, len - n, "  %02X /%d %s: %u (%.1f%%)\n", 0xd8 + best / 16, best % 8,
			      best & 8 ? "reg" : "mem", (unsigned) jit_fpu_form[best], 100.0 * jit_fpu_form[best] / total);
		jit_fpu_form[best] = 0;
	}
	for (int i = 0; i < (int) (sizeof(why) / sizeof(why[0])) && n < len; i++)
		n += snprintf(buf + n, len - n, "%s%s %u", strchr(why[i], ':') ? "\n" : ", ", why[i],
			      (unsigned) t386_fpu_stat[i]);
	if (n < len) snprintf(buf + n, len - n, "\n");
	memset(jit_fpu_form, 0, sizeof(jit_fpu_form));
	memset(t386_fpu_stat, 0, sizeof(why) / sizeof(why[0]) * sizeof(u32));
}
#endif

// serial "jit": what the translator has been doing
void t386_jit_report(char *buf, int len)
{
	const long made = jit_stat_translated - jit_stat_revived;
	snprintf(buf, len, "jit %s: %ld blocks entered, %ld translated (%ld of them revived; %d now, %u KB of code; room for "
		 "%d, %u KB), %ld "
		 "invalidated, %u cache "
		 "emptyings, %ld links, %ld instructions to the interpreter between blocks, %.1f instructions per block, "
		 "%.0f bytes of code per instruction; exits: %ld jump, %ld link, %ld interp, %ld budget, %ld stop; "
		 "%ld runs of self-modifying code (the last at %08x); %u TLB flushes; interpreted instructions left blocks "
		 "%ld times for another mode, %ld for SS or a null segment, %ld by jumps; %ld blocks used a null segment (interpreted); protected mode: %ld segment loads, far %ld "
		 "CALL %ld JMP %ld RETF %ld IRET %ld INT; %ld x87 instructions; %ld block lookups (%ld found), %.1f blocks looked at each",
		 t386_jit_enabled ? "on" : "off",
		 jit_stat_blocks, jit_stat_translated, jit_stat_revived, jit_nblocks,
		 jit_cache ? (unsigned) ((jit_cache_free - jit_cache) * 4 / 1024) : 0, jit_max_blocks,
		 (unsigned) (jit_cache_size / 1024), jit_stat_invalidated,
		 jit_generation, jit_stat_links, jit_stat_interp, made ? (double) jit_stat_ins / made : 0.0,
		 jit_stat_ins ? (double) jit_stat_bytes / jit_stat_ins : 0.0, jit_stat_exit[0], jit_stat_exit[1],
		 jit_stat_exit[2], jit_stat_exit[3], jit_stat_exit[4], jit_stat_volatile, jit_last_volatile, ftlb_flushes,
		 jit_stat_ileave[0], jit_stat_ileave[1], jit_stat_ileave[2], jit_stat_nulled, jit_stat_pm[0], jit_stat_pm[1], jit_stat_pm[2],
		 jit_stat_pm[3], jit_stat_pm[4], jit_stat_pm[5], jit_stat_fpu, jit_stat_finds, jit_stat_found,
		 jit_stat_finds ? (double) jit_stat_probes / jit_stat_finds : 0.0);
}


// With paging: the physical address of code at lin (through the fast path's TLB), or -1 (not mapped for this
// privilege level, or not RAM: the interpreter, which faults or fetches it its way; the TLB maps the linear frame
// buffer too)
static uword jit_code_phys(CPUI386 *cpu, uword lin)
{
	struct ftlb_entry *e = &ftlb[(lin >> 12) & (FTLB_N - 1)];
	uintptr_t host;
	if (e->rtag == lin >> 12) {
		host = lin + e->delta;
	} else {
		u8 *h = ftlb_miss(cpu, lin, 1, 0);
		if (!h) return (uword) -1;
		host = (uintptr_t) h;
	}
	const uword pa = (uword) (host - (uintptr_t) cpu->phys_mem);
	return pa < (uword) cpu->phys_mem_size ? pa : (uword) -1;
}

// the block whose code holds host address pc (blocks are made in the order of their code), or NULL
static JitBlock *jit_block_of(const u32 *pc)
{
	int lo = 0, hi = jit_nblocks - 1;
	if (hi < 0 || pc < jit_blocks[0].code) return NULL;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (jit_blocks[mid].code <= pc) lo = mid; else hi = mid - 1;
	}
	return &jit_blocks[lo];
}

#ifdef T386_TRACE
static const u32 *jit_block_end(const JitBlock *b)
{
	return b + 1 < jit_blocks + jit_nblocks ? b[1].code : jit_cache_free;
}
static u32 jit_code_sum(const JitBlock *b)
{
	u32 h = 0x811c9dc5u;
	for (const u32 *p = b->code; p < jit_block_end(b); p++) h = (h ^ *p) * 16777619u;
	return h;
}
static void jit_sum_set(JitBlock *b)
{
	if (!jit_sums) jit_sums = (u32 *) t386_jit_alloc(sizeof(u32) * jit_max_blocks);
	if (jit_sums && b) jit_sums[b - jit_blocks] = jit_code_sum(b);
}
static void jit_sum_of(const u32 *p) { jit_sum_set(jit_block_of(p)); }   // (a jump patched at p)
static void jit_sums_check(void)
{
	if (!jit_sums || jit_bad_block) return;
	for (int i = 0; i < jit_nblocks; i++) {
		JitBlock *b = &jit_blocks[i];
		const u32 h = jit_code_sum(b);
		if (h == jit_sums[i]) continue;
		jit_bad_block = b;
		jit_bad_sum = jit_sums[i];
		jit_bad_now = h;
		for (int k = 0; k < 48; k++) jit_bad_words[k] = b->code + k < jit_block_end(b) ? b->code[k] : 0;
		JIT_TR(TR_CORRUPT, b->lin, (u32) (uintptr_t) b->code, i);
		jit_trace_frozen = true;
#ifndef ESP_PLATFORM
		fprintf(stderr, "code sums: block %08x (%d of %d) changed after it was made\n", (unsigned) b->lin, i, jit_nblocks);
#endif
		return;
	}
}
#endif

// Run up to stepcount instructions, as fast_exec.
#if T386_JIT_HOLD
static bool jit_exec_in(CPUI386 *cpu, int stepcount);

// (builds with T386_JIT_HOLD: tab5_386.ino) Translated code runs from PSRAM; these builds keep core 0's cache
// maintenance on PSRAM (esp_cache_msync: the screen's buffers, the PPA's) apart from it. Core 0 holds translated code
// first (t386_jit_hold_on: until the step running it is done, a few microseconds), and core 1 meanwhile runs the fast
// path (from flash). inside: core 1 is in translated code; hold: core 0 asks it not to be.
volatile u8 t386_jit_hold, t386_jit_inside;
void t386_jit_hold_on(void)
{
	t386_jit_hold = 1;
	__sync_synchronize();
	while (t386_jit_inside) {}
}
void t386_jit_hold_off(void)
{
	__sync_synchronize();
	t386_jit_hold = 0;
}

static bool jit_exec(CPUI386 *cpu, int stepcount)
{
	t386_jit_inside = 1;
	__sync_synchronize();
	if (t386_jit_hold) {
		t386_jit_inside = 0;
		return fast_exec(cpu, stepcount);
	}
	const bool r = jit_exec_in(cpu, stepcount);
	__sync_synchronize();
	t386_jit_inside = 0;
	return r;
}
#else
#define jit_exec_in jit_exec
#endif

static bool jit_exec_in(CPUI386 *cpu, int stepcount)
{
#ifdef T386_FAST_PD
	// While the translator is off, its code cache holds the fast path's pre-decoded blocks (t386_fast_pd.h); it's
	// emptied when the translator is on again. (Both here: core 1, between steps.)
	static bool lent;
	if (jit_ready && lent == t386_jit_enabled) {
		if (lent) {
			pd_lend(NULL, 0);
			jit_flush();
		} else {   // (emptied first: its invalidations would write into its blocks there, now the fast path's)
			jit_flush();
			pd_lend((u8 *) jit_cache_base, (u8 *) jit_cache_end - (u8 *) jit_cache_base);
		}
		lent = !lent;
	}
#endif
	if (!t386_jit_enabled || (!jit_ready && !jit_init(cpu))) return fast_exec(cpu, stepcount);
	int budget = stepcount;
	ftlb_check(cpu);
	ftlb_lfb_poll();
	while (budget > 0) {
		uword eip = cpu->next_ip, cs_base = cpu->seg[SEG_CS].base, lin = cs_base + eip;
		u8 mode = jit_mode_of(cpu);
		uword phys = lin;
		if (mode & JM_PAGED) {   // (the TLB as the interpreter left it: CR3 or the privilege level may be new)
			ftlb_check(cpu);
			phys = jit_code_phys(cpu, lin);
		}
		int interp = 0;   // instructions for the interpreter
		JitBlock *b = NULL;
		bool cold = false;
#ifdef T386_TRACE
		{
			static u32 every;
			if (!(++every & 0x3ffff)) jit_sums_check();   // (~every 50 M instructions)
		}
		if (!(mode & JM_PROTECTED) && lin == 0xffff0)
			jit_trace_frozen = true;   // (the reset vector: a reboot)
		if (!(mode & JM_PROTECTED) && lin >= 0x110000)
			jit_trace_frozen = true;   // (real mode can't reach there: CS's base is wrong)
#endif
		if (phys != (uword) -1) {
			b = jit_find(lin, cs_base, mode, phys);
			if (b && b->crossing && !jit_crossing_ok(cpu, b)) {   // (the next page is elsewhere now)
				jit_invalidate(b);
				b = NULL;
			}
			if (!b && !jit_warm(phys, true)) cold = true;
			else if (!b) {
				b = jit_translate(cpu, eip, lin, mode, JIT_MAX_INS, phys);
				if (!b && joverflow) b = jit_translate(cpu, eip, lin, mode, 8, phys);   // too long: smaller
				if (b) jit_stat_translated++;
			}
		}
		// (with paging, the entry holds while the TLB does, for the privilege level's TLB: jt_ijc_exit)
		if (b) {   // (the newer entry of its set; the one there goes second, unless it's this one's)
			struct JitIjc *ij = &jit_ijc[2 * jit_ijc_hash(lin)];
			if (ij[0].lin != lin) ij[1] = ij[0];
			ij[0].lin = lin;
			ij[0].cs_base = mode & JM_PAGED ? cs_base ^ (uword) (uintptr_t) ftlb : cs_base;
			ij[0].mode = mode;
			ij[0].code = b->code;
		}
		JIT_TR(b ? TR_BLOCK : cold ? TR_COLD : TR_INTERP, lin, cpu->seg[SEG_CS].sel << 16 | (eip & 0xffff),
		       b ? (u32) (uintptr_t) b->code : cpu->gprx[4].r32);
		const bool nulled = b && (b->nsegs & fast_nullsegs(cpu));   // a segment it uses is null: the interpreter,
		if (nulled) b = NULL, jit_stat_nulled++;                   // which faults
		if (b) {
			if (b->xentry && (mode & JM_PAGED)) {   // (found for its linear address with this TLB: jit_xentry)
				b->xepoch = ftlb_flushes;
				b->xmap = (u32) (uintptr_t) ftlb;
			}
			JIT_NULLSEGS(cpu) = fast_nullsegs(cpu);
			jit_run_mode = mode;
			jit_exception = false;
			jit_stat_blocks++;
			// (R_MAP: with paging, the TLB for this privilege level, which holds while blocks run on: those of
			// a mode only reach each other, and an interpreted instruction that changes it leaves, jit_h_interp)
			int reason = t386_jit_enter(cpu, b->code, budget, cpu->phys_mem,
						    mode & JM_PAGED ? (u8 *) ftlb : t386_pagemap);
			if (reason == EXIT_BUDGET) {   // at the start of the block whose entry left (jit_t_bexit: maybe one
				const JitBlock *e = jit_block_of(jit_link_slot);   // linked to from b)
				cpu->next_ip = e->lin - e->cs_base;
			}
			cpu->cycle += budget - jit_budget_left;
			budget = jit_budget_left;
			jit_stat_exit[reason]++;
			switch (reason) {
			case EXIT_JUMP:
				break;
			case EXIT_DEAD: {   // into a dead block (jit_invalidate): on at its address; it lives again, or the
				JitBlock *d = jit_block_of(jit_link_slot);   // links into it go (another block is there now)
				cpu->next_ip = d->lin - d->cs_base;
				if (d->dead && d->guarded) {
					const u8 m = jit_mode_of(cpu);
					if (m == d->mode && d->cs_base == cpu->seg[SEG_CS].base && !jit_find(d->lin, d->cs_base, m, d->phys) &&
					    jit_revive(cpu, d->lin, m, d->phys))
						jit_stat_translated++;
					if (d->dead) jit_unlink_into(d);
				}
				break;
			}
			case EXIT_LINK: {   // (a far JMP or CALL leaves with another CS)
				unsigned gen = jit_generation;
				cs_base = cpu->seg[SEG_CS].base;
				mode = jit_mode_of(cpu);
				uword tlin = cs_base + cpu->next_ip, tphys = tlin;
				// (not to a block of the other JM_FLAT: a translated segment load may have changed it)
				JitBlock *from = jit_block_of(jit_link_slot);
				bool linkable = from && !((from->mode ^ mode) & JM_FLAT), other_page = false;
				if (mode & JM_PAGED) {   // within a page the mapping is the same for both blocks; else jit_xentry
					tphys = jit_code_phys(cpu, tlin);
					linkable = linkable && from->mode == mode;
					other_page = (from->lin >> 12) != (tlin >> 12);
					if (tphys == (uword) -1) break;
				}
				JitBlock *t = jit_find(tlin, cs_base, mode, tphys);
				if (!t && jit_warm(tphys, false)) {
					t = jit_translate(cpu, cpu->next_ip, tlin, mode, JIT_MAX_INS, tphys);
					if (t) jit_stat_translated++;
				}
				// (if translating emptied the cache, the exit is gone)
				if (t && t->crossing && !jit_crossing_ok(cpu, t)) t = NULL;
				u32 *dest = t && (other_page || t->crossing) && linkable && gen == jit_generation
					    ? jit_xentry(t) : t ? t->code : NULL;
				if (dest && linkable && gen == jit_generation && (jit_link_free || jit_nlinks < jit_max_blocks * 2)) {
					JitLink *l = jit_link_free ? jit_link_free : &jit_links[jit_nlinks++];
					if (l == jit_link_free) jit_link_free = l->next;
					l->slot = jit_link_slot;
					l->rt = jit_link_slot + rv_jal_off(jit_link_slot[0]) / 4;
					l->next = t->links;
					t->links = l;
					jit_patch_jump(jit_link_slot, dest);
					jit_stat_links++;
				}
				break;
			}
			case EXIT_INTERP:
				interp = 1;
				if (jit_check_fail_lin != (uword) -1) {   // a self-checking block that keeps failing at once: a new one
					u8 m = jit_mode_of(cpu);
					uword cl = jit_check_fail_lin, cp = m & JM_PAGED ? jit_code_phys(cpu, cl) : cl;
					JitBlock *c = jit_find(cl, cpu->seg[SEG_CS].base, m, cp);
					if (c && c->checked && ++c->fails > 8) jit_invalidate(c);
					jit_check_fail_lin = -1;
				}
				break;
			case EXIT_BUDGET:   // none left for this step (or some left: its segments are null, see above)
				if (budget > 0) break;
				return true;
			case EXIT_STOP:
				return !jit_exception;
			}
		} else if (nulled) {
			interp = 1;
		} else if (cold) {   // not reached often enough yet: the interpreter, for a few instructions
			interp = JIT_COLD_RUN;
		} else {   // nothing to translate here: the interpreter; a stretch of self-modifying code at once
			interp = phys < (uword) cpu->phys_mem_size && jit_volatile(phys) ? 64 : 1;
			if (interp > 1) { jit_stat_volatile++; jit_last_volatile = lin; }
		}
		if (interp && budget > 0) {
			if (interp > budget) interp = budget;
			jit_stat_interp += interp;
			bool ok = fast_exec(cpu, interp);
			budget -= interp;
			if (!ok) return false;
			if (cpu->halt || (cpu->intr && (cpu->flags & IF))) return true;
		}
	}
	return true;
}
