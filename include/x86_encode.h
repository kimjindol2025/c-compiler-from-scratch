#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================
 * x86-64 Instruction Encoder
 * Produces raw machine-code bytes (AT&T / Intel semantics)
 * ========================================================= */

/* ---- Register numbers (matching ModRM encoding) ---- */
typedef enum {
    REG_RAX = 0,  REG_RCX = 1,  REG_RDX = 2,  REG_RBX = 3,
    REG_RSP = 4,  REG_RBP = 5,  REG_RSI = 6,  REG_RDI = 7,
    REG_R8  = 8,  REG_R9  = 9,  REG_R10 = 10, REG_R11 = 11,
    REG_R12 = 12, REG_R13 = 13, REG_R14 = 14, REG_R15 = 15,
    REG_NONE = -1,
} Reg;

/* Condition codes */
typedef enum {
    CC_O  = 0,  CC_NO = 1,  CC_B  = 2,  CC_NB = 3,
    CC_E  = 4,  CC_NE = 5,  CC_BE = 6,  CC_NBE= 7,
    CC_S  = 8,  CC_NS = 9,  CC_P  = 10, CC_NP = 11,
    CC_L  = 12, CC_NL = 13, CC_LE = 14, CC_NLE= 15,
    /* aliases */
    CC_Z  = CC_E,  CC_NZ = CC_NE,
    CC_AE = CC_NB, CC_NA = CC_BE,
    CC_GE = CC_NL, CC_G  = CC_NLE,
    CC_NGE= CC_L,  CC_NG = CC_LE,
} CondCode;

/* Operand size in bytes */
typedef enum {
    SZ_8  = 1,
    SZ_16 = 2,
    SZ_32 = 4,
    SZ_64 = 8,
} OpSize;

/* ---- Byte buffer ---- */
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_write8(Buf *b, uint8_t byte);
void buf_write16(Buf *b, uint16_t v);
void buf_write32(Buf *b, uint32_t v);
void buf_write64(Buf *b, uint64_t v);
void buf_write32_at(Buf *b, size_t off, uint32_t v);   /* patch */
void buf_writei32_at(Buf *b, size_t off, int32_t v);
void buf_append(Buf *dst, const Buf *src);
size_t buf_pos(const Buf *b);

/* ---- Encoder context ---- */

/* A fixup: a relative 32-bit displacement that needs patching
   once the target label is known */
typedef struct Fixup {
    size_t  patch_off;   /* offset in buf where the 4-byte disp goes */
    char   *label;       /* target label name */
    size_t  inst_end;    /* end of instruction (for PC-relative calc) */
    struct Fixup *next;
} Fixup;

typedef struct LabelDef {
    char   *name;
    size_t  off;          /* offset in buf */
    struct LabelDef *next;
} LabelDef;

typedef struct Encoder {
    Buf      buf;
    Fixup   *fixups;
    LabelDef *labels;
    int      label_counter;  /* for auto-generated labels */
} Encoder;

Encoder *enc_new(void);
void     enc_free(Encoder *e);

/* Emit a label at current position */
void enc_label(Encoder *e, const char *name);
/* Get unique auto-label */
char *enc_make_label(Encoder *e, const char *prefix);
/* Resolve all fixups — call once after all code emitted */
void enc_resolve_fixups(Encoder *e);

/* ===== Instruction emitters =====
 * Naming: enc_<mnemonic>_<operand-form>
 *   rr  = reg, reg
 *   ri  = reg, imm
 *   rm  = reg, [mem]
 *   mr  = [mem], reg
 *   mi  = [mem], imm
 *   r   = reg (single)
 *   m   = [mem]
 *   i   = imm
 *
 * mem is encoded as: base + index*scale + disp
 *   index==REG_NONE → no SIB (or RSP base forces SIB)
 *   scale: 1, 2, 4, or 8
 */

/* --- MOV --- */
void enc_mov_rr(Encoder *e, OpSize sz, Reg dst, Reg src);
void enc_mov_ri(Encoder *e, OpSize sz, Reg dst, int64_t imm);
void enc_mov_rm(Encoder *e, OpSize sz, Reg dst, Reg base, Reg idx, int scale, int32_t disp);
void enc_mov_mr(Encoder *e, OpSize sz, Reg base, Reg idx, int scale, int32_t disp, Reg src);
void enc_mov_mi(Encoder *e, OpSize sz, Reg base, Reg idx, int scale, int32_t disp, int32_t imm);

/* Load with zero-extension */
void enc_movzx_rr(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg src);
void enc_movzx_rm(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg base, int32_t disp);
/* Load with sign-extension */
void enc_movsx_rr(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg src);
void enc_movsx_rm(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg base, int32_t disp);
/* MOVSXD: sign-extend 32→64 */
void enc_movsxd_rr(Encoder *e, Reg dst, Reg src);
void enc_movsxd_rm(Encoder *e, Reg dst, Reg base, int32_t disp);

/* --- LEA --- */
void enc_lea(Encoder *e, Reg dst, Reg base, Reg idx, int scale, int32_t disp);
void enc_lea_label(Encoder *e, Reg dst, const char *label);  /* RIP-relative */

/* --- Arithmetic --- */
void enc_add_rr(Encoder *e, OpSize sz, Reg dst, Reg src);
void enc_add_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm);
void enc_add_rm(Encoder *e, OpSize sz, Reg dst, Reg base, int32_t disp);
void enc_add_mr(Encoder *e, OpSize sz, Reg base, int32_t disp, Reg src);

void enc_sub_rr(Encoder *e, OpSize sz, Reg dst, Reg src);
void enc_sub_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm);
void enc_sub_rm(Encoder *e, OpSize sz, Reg dst, Reg base, int32_t disp);
void enc_sub_mr(Encoder *e, OpSize sz, Reg base, int32_t disp, Reg src);

void enc_imul_rr(Encoder *e, OpSize sz, Reg dst, Reg src);
void enc_imul_rri(Encoder *e, OpSize sz, Reg dst, Reg src, int32_t imm);
void enc_imul_rm(Encoder *e, OpSize sz, Reg dst, Reg base, int32_t disp);

/* IDIV: divides RDX:RAX by src. Result: RAX=quotient, RDX=remainder */
void enc_idiv_r(Encoder *e, OpSize sz, Reg src);
void enc_div_r(Encoder *e, OpSize sz, Reg src);

void enc_neg_r(Encoder *e, OpSize sz, Reg r);
void enc_not_r(Encoder *e, OpSize sz, Reg r);

/* --- Bitwise --- */
void enc_and_rr(Encoder *e, OpSize sz, Reg dst, Reg src);
void enc_and_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm);
void enc_or_rr(Encoder *e,  OpSize sz, Reg dst, Reg src);
void enc_or_ri(Encoder *e,  OpSize sz, Reg dst, int32_t imm);
void enc_xor_rr(Encoder *e, OpSize sz, Reg dst, Reg src);
void enc_xor_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm);

/* --- Shifts --- */
void enc_shl_ri(Encoder *e, OpSize sz, Reg dst, uint8_t count);
void enc_shr_ri(Encoder *e, OpSize sz, Reg dst, uint8_t count);
void enc_sar_ri(Encoder *e, OpSize sz, Reg dst, uint8_t count);
void enc_shl_r(Encoder *e, OpSize sz, Reg dst);   /* shift by CL */
void enc_shr_r(Encoder *e, OpSize sz, Reg dst);
void enc_sar_r(Encoder *e, OpSize sz, Reg dst);

/* --- Compare / test --- */
void enc_cmp_rr(Encoder *e, OpSize sz, Reg a, Reg b);
void enc_cmp_ri(Encoder *e, OpSize sz, Reg a, int32_t imm);
void enc_cmp_rm(Encoder *e, OpSize sz, Reg a, Reg base, int32_t disp);
void enc_test_rr(Encoder *e, OpSize sz, Reg a, Reg b);
void enc_test_ri(Encoder *e, OpSize sz, Reg a, int32_t imm);

/* SETcc: set byte register to 0 or 1 based on condition */
void enc_setcc(Encoder *e, CondCode cc, Reg dst);

/* --- Jumps --- */
void enc_jmp(Encoder *e, const char *label);
void enc_jcc(Encoder *e, CondCode cc, const char *label);
void enc_jmp_r(Encoder *e, Reg r);            /* jmp *reg */

/* --- Call / ret --- */
void enc_call(Encoder *e, const char *label);
void enc_call_r(Encoder *e, Reg r);
void enc_ret(Encoder *e);

/* --- Stack --- */
void enc_push_r(Encoder *e, Reg r);
void enc_push_i(Encoder *e, int32_t imm);
void enc_pop_r(Encoder *e, Reg r);

/* --- Sign-extension helpers --- */
void enc_cdq(Encoder *e);   /* EAX→EDX:EAX */
void enc_cqo(Encoder *e);   /* RAX→RDX:RAX */
void enc_cbw(Encoder *e);   /* AL→AX */
void enc_cwde(Encoder *e);  /* AX→EAX */
void enc_cdqe(Encoder *e);  /* EAX→RAX */

/* --- Misc --- */
void enc_nop(Encoder *e);
void enc_int3(Encoder *e);
void enc_syscall(Encoder *e);

/* Get raw bytes (encoder owns the buffer) */
const uint8_t *enc_bytes(const Encoder *e);
size_t         enc_size(const Encoder *e);

/* ===== SSE2 / scalar-double instructions =====
 * Convention: xmm registers are passed as plain int (0-15).
 * Binary ops use xmm0 op= xmm1 pattern.
 */

/* movsd xmm_dst, [rbp + off] */
void enc_movsd_load(Encoder *e, int xmm_dst, int off);
/* movsd [rbp + off], xmm_src */
void enc_movsd_store(Encoder *e, int off, int xmm_src);
/* movsd xmm_dst, xmm_src */
void enc_movsd_rr(Encoder *e, int xmm_dst, int xmm_src);

/* scalar-double binops: xmm0 op= xmm1 */
void enc_addsd(Encoder *e);
void enc_subsd(Encoder *e);
void enc_mulsd(Encoder *e);
void enc_divsd(Encoder *e);

/* ucomisd xmm0, xmm1  (sets ZF/PF/CF for FP comparisons) */
void enc_ucomisd(Encoder *e);

/* cvttsd2si rax, xmm0  (truncating double→int64) */
void enc_cvttsd2si(Encoder *e);
/* cvtsi2sd xmm0, rax   (int64→double) */
void enc_cvtsi2sd(Encoder *e);

/* movsd xmm0, [rip+disp32]  — returns offset of disp field for relocation */
size_t enc_movsd_rip(Encoder *e);

/* push/pop xmm0 via stack (for binary op evaluation) */
void enc_push_xmm0(Encoder *e);   /* sub rsp,8; movsd [rsp],xmm0 */
void enc_pop_xmm1(Encoder *e);    /* movsd xmm1,[rsp]; add rsp,8  */

/* MOVQ: transfer 64-bit bit-pattern between XMM and integer registers */
void enc_movq_xmm0_rax(Encoder *e);  /* movq xmm0, rax */
void enc_movq_xmm1_rax(Encoder *e);  /* movq xmm1, rax */
void enc_movq_xmm1_rcx(Encoder *e);  /* movq xmm1, rcx */
void enc_movq_rax_xmm0(Encoder *e);  /* movq rax, xmm0 */

/* General form: movq xmmN, gpr and movq gpr, xmmN */
void enc_movq_xmm_gpr(Encoder *e, int xmm_dst, Reg src);
void enc_movq_gpr_xmm(Encoder *e, Reg dst, int xmm_src);
