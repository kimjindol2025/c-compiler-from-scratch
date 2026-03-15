/*
 * x86_encode.c — x86-64 Instruction Encoder
 *
 * Produces raw machine-code bytes for x86-64 instructions.
 * Uses REX prefixes for 64-bit operations.
 * Handles ModRM, SIB, and displacement bytes.
 */

#include "../include/x86_encode.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* =========================================================
 * Byte buffer
 * ========================================================= */

void buf_init(Buf *b) {
    b->cap  = 64;
    b->size = 0;
    b->data = malloc(b->cap);
    if (!b->data) { perror("malloc"); exit(1); }
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

static void buf_grow(Buf *b, size_t need) {
    if (b->size + need <= b->cap) return;
    while (b->cap < b->size + need) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
    if (!b->data) { perror("realloc"); exit(1); }
}

void buf_write8(Buf *b, uint8_t v) {
    buf_grow(b, 1);
    b->data[b->size++] = v;
}

void buf_write16(Buf *b, uint16_t v) {
    buf_grow(b, 2);
    b->data[b->size++] = (uint8_t)(v);
    b->data[b->size++] = (uint8_t)(v >> 8);
}

void buf_write32(Buf *b, uint32_t v) {
    buf_grow(b, 4);
    b->data[b->size++] = (uint8_t)(v);
    b->data[b->size++] = (uint8_t)(v >> 8);
    b->data[b->size++] = (uint8_t)(v >> 16);
    b->data[b->size++] = (uint8_t)(v >> 24);
}

void buf_write64(Buf *b, uint64_t v) {
    buf_write32(b, (uint32_t)v);
    buf_write32(b, (uint32_t)(v >> 32));
}

void buf_write32_at(Buf *b, size_t off, uint32_t v) {
    b->data[off]     = (uint8_t)(v);
    b->data[off + 1] = (uint8_t)(v >> 8);
    b->data[off + 2] = (uint8_t)(v >> 16);
    b->data[off + 3] = (uint8_t)(v >> 24);
}

void buf_writei32_at(Buf *b, size_t off, int32_t v) {
    buf_write32_at(b, off, (uint32_t)v);
}

void buf_append(Buf *dst, const Buf *src) {
    buf_grow(dst, src->size);
    memcpy(dst->data + dst->size, src->data, src->size);
    dst->size += src->size;
}

size_t buf_pos(const Buf *b) { return b->size; }

/* =========================================================
 * Encoder lifecycle
 * ========================================================= */

Encoder *enc_new(void) {
    Encoder *e = calloc(1, sizeof(Encoder));
    if (!e) { perror("calloc"); exit(1); }
    buf_init(&e->buf);
    return e;
}

void enc_free(Encoder *e) {
    if (!e) return;
    buf_free(&e->buf);
    /* free fixups */
    Fixup *f = e->fixups;
    while (f) { Fixup *nxt = f->next; free(f->label); free(f); f = nxt; }
    /* free labels */
    LabelDef *l = e->labels;
    while (l) { LabelDef *nxt = l->next; free(l->name); free(l); l = nxt; }
    free(e);
}

void enc_label(Encoder *e, const char *name) {
    LabelDef *l = malloc(sizeof(LabelDef));
    l->name  = strdup(name);
    l->off   = e->buf.size;
    l->next  = e->labels;
    e->labels = l;
}

char *enc_make_label(Encoder *e, const char *prefix) {
    char *buf = malloc(64);
    snprintf(buf, 64, ".%s%d", prefix ? prefix : "L", e->label_counter++);
    return buf;  /* caller must free */
}

static size_t label_offset(Encoder *e, const char *name) {
    for (LabelDef *l = e->labels; l; l = l->next) {
        if (strcmp(l->name, name) == 0) return l->off;
    }
    return (size_t)-1;
}

void enc_resolve_fixups(Encoder *e) {
    for (Fixup *f = e->fixups; f; f = f->next) {
        size_t target = label_offset(e, f->label);
        if (target == (size_t)-1) {
            fprintf(stderr, "enc: unresolved label '%s'\n", f->label);
            continue;
        }
        int32_t disp = (int32_t)((int64_t)target - (int64_t)f->inst_end);
        buf_writei32_at(&e->buf, f->patch_off, disp);
    }
}

const uint8_t *enc_bytes(const Encoder *e) { return e->buf.data; }
size_t         enc_size(const Encoder *e)  { return e->buf.size; }

/* =========================================================
 * x86-64 encoding helpers
 *
 * REX byte: 0100 WRXB
 *   W=1: 64-bit operand
 *   R=1: reg field extension (ModRM.reg uses R8–R15)
 *   X=1: SIB index extension
 *   B=1: rm/base extension
 * ========================================================= */

#define REX_W  0x48
#define REX_WR 0x4C
#define REX_WB 0x49
#define REX_WRB 0x4D
#define REX_WX  0x4A
#define REX_WXB 0x4B
#define REX_WRXB 0x4F

/* Emit REX prefix if needed */
static void emit_rex(Buf *b, bool W, Reg reg, Reg index, Reg rm_base) {
    bool R = (reg   >= REG_R8 && reg   != REG_NONE);
    bool X = (index >= REG_R8 && index != REG_NONE);
    bool B = (rm_base >= REG_R8 && rm_base != REG_NONE);

    if (W || R || X || B) {
        uint8_t rex = 0x40
                    | (W ? 0x08 : 0)
                    | (R ? 0x04 : 0)
                    | (X ? 0x02 : 0)
                    | (B ? 0x01 : 0);
        buf_write8(b, rex);
    }
}

/* Emit REX.W (always 64-bit) */
static void emit_rexw(Buf *b, Reg reg, Reg index, Reg rm) {
    emit_rex(b, true, reg, index, rm);
}

/* For 32-bit ops that still need R/X/B extension */
static void emit_rex_no_w(Buf *b, Reg reg, Reg index, Reg rm) {
    emit_rex(b, false, reg, index, rm);
}

/* For 8-bit ops (byte registers) — if using rsi/rdi/rsp/rbp in low byte,
   an empty REX must be emitted to access sil/dil/spl/bpl */
static void emit_rex_byte(Buf *b, Reg reg, Reg rm) {
    bool R = (reg >= REG_R8 && reg != REG_NONE);
    bool B = (rm  >= REG_R8 && rm  != REG_NONE);
    /* Also need REX if using RSP/RBP/RSI/RDI as byte registers (SPL etc.) */
    bool need_rex_for_new_regs =
        (reg >= 4 && reg <= 7) || (rm >= 4 && rm <= 7);
    if (R || B || need_rex_for_new_regs) {
        uint8_t rex = 0x40
                    | (R ? 0x04 : 0)
                    | (B ? 0x01 : 0);
        buf_write8(b, rex);
    }
}

/* ModRM byte: mod(2) reg(3) rm(3) */
static uint8_t modrm(uint8_t mod, Reg reg, Reg rm) {
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* SIB byte: scale(2) index(3) base(3) */
static uint8_t sib_byte(int scale, Reg index, Reg base) {
    uint8_t ss;
    if      (scale == 1) ss = 0;
    else if (scale == 2) ss = 1;
    else if (scale == 4) ss = 2;
    else                 ss = 3;  /* scale == 8 */
    return (uint8_t)((ss << 6) | ((index & 7) << 3) | (base & 7));
}

/* Emit ModRM + optional SIB + displacement for memory operand:
   [base + index*scale + disp]
   reg_field: the /r field (may be an opcode extension) */
static void emit_modrm_mem(Buf *b, uint8_t reg_field, Reg base, Reg index, int scale, int32_t disp) {
    bool has_index = (index != REG_NONE);
    bool has_disp8 = (!has_index && base != REG_RSP && base != REG_R12 &&
                      disp >= -128 && disp <= 127 && disp != 0);
    bool has_disp0 = (!has_index && base != REG_RSP && base != REG_R12 &&
                      base != REG_RBP && base != REG_R13 && disp == 0);
    bool has_disp32 = !has_disp8 && !has_disp0;

    /* Also need disp32 if base==RBP/R13 and disp==0 (disambiguate from RIP-relative) */
    if ((base == REG_RBP || base == REG_R13) && disp == 0 && !has_index) {
        has_disp0  = false;
        has_disp8  = false;
        has_disp32 = true;
    }

    uint8_t mod;
    if      (has_disp0)  mod = 0;
    else if (has_disp8)  mod = 1;
    else                 mod = 2;

    if (has_index) {
        /* SIB addressing */
        buf_write8(b, modrm(mod, (Reg)reg_field, REG_RSP));  /* rm=100 → SIB follows */
        buf_write8(b, sib_byte(scale, index, base));
    } else if (base == REG_RSP || base == REG_R12) {
        /* RSP/R12 base requires SIB with no-index */
        /* mod depends on disp */
        if (disp == 0) mod = 0;
        else if (disp >= -128 && disp <= 127) mod = 1;
        else mod = 2;
        buf_write8(b, modrm(mod, (Reg)reg_field, REG_RSP));
        buf_write8(b, sib_byte(1, REG_RSP, base));  /* index=RSP means no index */
    } else {
        buf_write8(b, modrm(mod, (Reg)reg_field, base));
    }

    /* Emit displacement */
    if (has_index) {
        if (disp == 0 && base != REG_RBP && base != REG_R13) {
            /* mod=0, no disp */
        } else if (disp >= -128 && disp <= 127) {
            buf_write8(b, (uint8_t)(int8_t)disp);
        } else {
            buf_write32(b, (uint32_t)disp);
        }
    } else if (base == REG_RSP || base == REG_R12) {
        /* RSP/R12 base: displacement is only needed if disp != 0 */
        if (disp == 0) {
            /* mod=0 was used above — no displacement bytes needed */
        } else if (disp >= -128 && disp <= 127) {
            buf_write8(b, (uint8_t)(int8_t)disp);
        } else {
            buf_write32(b, (uint32_t)disp);
        }
    } else if (has_disp8) {
        buf_write8(b, (uint8_t)(int8_t)disp);
    } else if (!has_disp0) {
        buf_write32(b, (uint32_t)disp);
    }
}

/* =========================================================
 * MOV instructions
 * ========================================================= */

void enc_mov_rr(Encoder *e, OpSize sz, Reg dst, Reg src) {
    Buf *b = &e->buf;
    switch (sz) {
    case SZ_64:
        emit_rexw(b, src, REG_NONE, dst);
        buf_write8(b, 0x89);  /* MOV r/m64, r64 */
        buf_write8(b, modrm(3, src, dst));
        break;
    case SZ_32:
        emit_rex_no_w(b, src, REG_NONE, dst);
        buf_write8(b, 0x89);
        buf_write8(b, modrm(3, src, dst));
        break;
    case SZ_16:
        buf_write8(b, 0x66);
        emit_rex_no_w(b, src, REG_NONE, dst);
        buf_write8(b, 0x89);
        buf_write8(b, modrm(3, src, dst));
        break;
    case SZ_8:
        emit_rex_byte(b, src, dst);
        buf_write8(b, 0x88);  /* MOV r/m8, r8 */
        buf_write8(b, modrm(3, src, dst));
        break;
    }
}

void enc_mov_ri(Encoder *e, OpSize sz, Reg dst, int64_t imm) {
    Buf *b = &e->buf;
    switch (sz) {
    case SZ_64:
        if (imm >= 0 && imm <= 0xFFFFFFFF) {
            /* MOV r32, imm32 (zero-extends to 64 bits) */
            emit_rex_no_w(b, REG_NONE, REG_NONE, dst);
            buf_write8(b, 0xB8 | (dst & 7));
            buf_write32(b, (uint32_t)imm);
        } else {
            /* MOV r64, imm64 */
            emit_rexw(b, REG_NONE, REG_NONE, dst);
            buf_write8(b, 0xB8 | (dst & 7));
            buf_write64(b, (uint64_t)imm);
        }
        break;
    case SZ_32:
        emit_rex_no_w(b, REG_NONE, REG_NONE, dst);
        buf_write8(b, 0xB8 | (dst & 7));
        buf_write32(b, (uint32_t)imm);
        break;
    case SZ_16:
        buf_write8(b, 0x66);
        buf_write8(b, 0xB8 | (dst & 7));
        buf_write16(b, (uint16_t)imm);
        break;
    case SZ_8:
        emit_rex_byte(b, REG_NONE, dst);
        buf_write8(b, 0xB0 | (dst & 7));
        buf_write8(b, (uint8_t)imm);
        break;
    }
}

void enc_mov_rm(Encoder *e, OpSize sz, Reg dst, Reg base, Reg idx, int scale, int32_t disp) {
    Buf *b = &e->buf;
    switch (sz) {
    case SZ_64:
        emit_rexw(b, dst, idx, base);
        buf_write8(b, 0x8B);
        emit_modrm_mem(b, dst, base, idx, scale, disp);
        break;
    case SZ_32:
        emit_rex_no_w(b, dst, idx, base);
        buf_write8(b, 0x8B);
        emit_modrm_mem(b, dst, base, idx, scale, disp);
        break;
    case SZ_16:
        buf_write8(b, 0x66);
        emit_rex_no_w(b, dst, idx, base);
        buf_write8(b, 0x8B);
        emit_modrm_mem(b, dst, base, idx, scale, disp);
        break;
    case SZ_8:
        emit_rex_no_w(b, dst, idx, base);
        buf_write8(b, 0x8A);
        emit_modrm_mem(b, dst, base, idx, scale, disp);
        break;
    }
}

void enc_mov_mr(Encoder *e, OpSize sz, Reg base, Reg idx, int scale, int32_t disp, Reg src) {
    Buf *b = &e->buf;
    switch (sz) {
    case SZ_64:
        emit_rexw(b, src, idx, base);
        buf_write8(b, 0x89);
        emit_modrm_mem(b, src, base, idx, scale, disp);
        break;
    case SZ_32:
        emit_rex_no_w(b, src, idx, base);
        buf_write8(b, 0x89);
        emit_modrm_mem(b, src, base, idx, scale, disp);
        break;
    case SZ_16:
        buf_write8(b, 0x66);
        emit_rex_no_w(b, src, idx, base);
        buf_write8(b, 0x89);
        emit_modrm_mem(b, src, base, idx, scale, disp);
        break;
    case SZ_8:
        emit_rex_byte(b, src, base);
        buf_write8(b, 0x88);
        emit_modrm_mem(b, src, base, idx, scale, disp);
        break;
    }
}

void enc_mov_mi(Encoder *e, OpSize sz, Reg base, Reg idx, int scale, int32_t disp, int32_t imm) {
    Buf *b = &e->buf;
    switch (sz) {
    case SZ_64:
        emit_rexw(b, REG_NONE, idx, base);
        buf_write8(b, 0xC7);
        emit_modrm_mem(b, 0, base, idx, scale, disp);
        buf_write32(b, (uint32_t)imm);
        break;
    case SZ_32:
        emit_rex_no_w(b, REG_NONE, idx, base);
        buf_write8(b, 0xC7);
        emit_modrm_mem(b, 0, base, idx, scale, disp);
        buf_write32(b, (uint32_t)imm);
        break;
    case SZ_16:
        buf_write8(b, 0x66);
        buf_write8(b, 0xC7);
        emit_modrm_mem(b, 0, base, idx, scale, disp);
        buf_write16(b, (uint16_t)imm);
        break;
    case SZ_8:
        buf_write8(b, 0xC6);
        emit_modrm_mem(b, 0, base, idx, scale, disp);
        buf_write8(b, (uint8_t)imm);
        break;
    }
}

/* MOVZX */
void enc_movzx_rr(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg src) {
    Buf *b = &e->buf;
    if (dst_sz == SZ_64) emit_rexw(b, dst, REG_NONE, src);
    else emit_rex_no_w(b, dst, REG_NONE, src);
    buf_write8(b, 0x0F);
    buf_write8(b, src_sz == SZ_8 ? 0xB6 : 0xB7);
    buf_write8(b, modrm(3, dst, src));
}

void enc_movzx_rm(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg base, int32_t disp) {
    Buf *b = &e->buf;
    if (dst_sz == SZ_64) emit_rexw(b, dst, REG_NONE, base);
    else emit_rex_no_w(b, dst, REG_NONE, base);
    buf_write8(b, 0x0F);
    buf_write8(b, src_sz == SZ_8 ? 0xB6 : 0xB7);
    emit_modrm_mem(b, dst, base, REG_NONE, 1, disp);
}

/* MOVSX */
void enc_movsx_rr(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg src) {
    Buf *b = &e->buf;
    if (dst_sz == SZ_64) emit_rexw(b, dst, REG_NONE, src);
    else emit_rex_no_w(b, dst, REG_NONE, src);
    buf_write8(b, 0x0F);
    buf_write8(b, src_sz == SZ_8 ? 0xBE : 0xBF);
    buf_write8(b, modrm(3, dst, src));
}

void enc_movsx_rm(Encoder *e, OpSize dst_sz, OpSize src_sz, Reg dst, Reg base, int32_t disp) {
    Buf *b = &e->buf;
    if (dst_sz == SZ_64) emit_rexw(b, dst, REG_NONE, base);
    else emit_rex_no_w(b, dst, REG_NONE, base);
    buf_write8(b, 0x0F);
    buf_write8(b, src_sz == SZ_8 ? 0xBE : 0xBF);
    emit_modrm_mem(b, dst, base, REG_NONE, 1, disp);
}

/* MOVSXD: 32→64 sign-extension */
void enc_movsxd_rr(Encoder *e, Reg dst, Reg src) {
    Buf *b = &e->buf;
    emit_rexw(b, dst, REG_NONE, src);
    buf_write8(b, 0x63);
    buf_write8(b, modrm(3, dst, src));
}

void enc_movsxd_rm(Encoder *e, Reg dst, Reg base, int32_t disp) {
    Buf *b = &e->buf;
    emit_rexw(b, dst, REG_NONE, base);
    buf_write8(b, 0x63);
    emit_modrm_mem(b, dst, base, REG_NONE, 1, disp);
}

/* =========================================================
 * LEA
 * ========================================================= */

void enc_lea(Encoder *e, Reg dst, Reg base, Reg idx, int scale, int32_t disp) {
    Buf *b = &e->buf;
    emit_rexw(b, dst, idx, base);
    buf_write8(b, 0x8D);
    emit_modrm_mem(b, dst, base, idx, scale, disp);
}

void enc_lea_label(Encoder *e, Reg dst, const char *label) {
    Buf *b = &e->buf;
    /* LEA dst, [RIP + rel32] */
    emit_rexw(b, dst, REG_NONE, REG_NONE);
    buf_write8(b, 0x8D);
    /* ModRM: mod=0, rm=101 (RIP-relative) */
    buf_write8(b, modrm(0, dst, 5));
    /* Add fixup */
    Fixup *f = malloc(sizeof(Fixup));
    f->patch_off = b->size;
    f->label     = strdup(label);
    f->inst_end  = b->size + 4;
    f->next      = e->fixups;
    e->fixups    = f;
    buf_write32(b, 0); /* placeholder */
}

/* =========================================================
 * Arithmetic
 * ========================================================= */

/* Generic ALU instruction: opcode /r form */
static void alu_rr(Buf *b, uint8_t op, OpSize sz, Reg dst, Reg src) {
    if (sz == SZ_64) emit_rexw(b, src, REG_NONE, dst);
    else if (sz == SZ_32) emit_rex_no_w(b, src, REG_NONE, dst);
    else if (sz == SZ_16) buf_write8(b, 0x66);
    else emit_rex_byte(b, src, dst);
    buf_write8(b, sz == SZ_8 ? op : (op | 1));
    buf_write8(b, modrm(3, src, dst));
}

/* Generic ALU reg, imm */
static void alu_ri(Buf *b, uint8_t grp_code, OpSize sz, Reg dst, int32_t imm) {
    bool imm8 = (imm >= -128 && imm <= 127);
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, dst);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, dst);
    else if (sz == SZ_16) buf_write8(b, 0x66);

    if (sz == SZ_8) {
        buf_write8(b, 0x80);
        buf_write8(b, modrm(3, (Reg)grp_code, dst));
        buf_write8(b, (uint8_t)imm);
    } else if (imm8 && sz != SZ_16) {
        buf_write8(b, 0x83);
        buf_write8(b, modrm(3, (Reg)grp_code, dst));
        buf_write8(b, (uint8_t)(int8_t)imm);
    } else {
        buf_write8(b, 0x81);
        buf_write8(b, modrm(3, (Reg)grp_code, dst));
        if (sz == SZ_16) buf_write16(b, (uint16_t)imm);
        else             buf_write32(b, (uint32_t)imm);
    }
}

static void alu_rm(Buf *b, uint8_t op, OpSize sz, Reg dst, Reg base, int32_t disp) {
    if (sz == SZ_64) emit_rexw(b, dst, REG_NONE, base);
    else if (sz == SZ_32) emit_rex_no_w(b, dst, REG_NONE, base);
    buf_write8(b, sz == SZ_8 ? op : (op | 1));
    emit_modrm_mem(b, dst, base, REG_NONE, 1, disp);
}

static void alu_mr(Buf *b, uint8_t op, OpSize sz, Reg base, int32_t disp, Reg src) {
    if (sz == SZ_64) emit_rexw(b, src, REG_NONE, base);
    else if (sz == SZ_32) emit_rex_no_w(b, src, REG_NONE, base);
    buf_write8(b, sz == SZ_8 ? op : (op | 1));
    emit_modrm_mem(b, src, base, REG_NONE, 1, disp);
}

/* ADD */
void enc_add_rr(Encoder *e, OpSize sz, Reg dst, Reg src) { alu_rr(&e->buf, 0x00, sz, dst, src); }
void enc_add_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm) { alu_ri(&e->buf, 0, sz, dst, imm); }
void enc_add_rm(Encoder *e, OpSize sz, Reg dst, Reg base, int32_t disp) { alu_rm(&e->buf, 0x02, sz, dst, base, disp); }
void enc_add_mr(Encoder *e, OpSize sz, Reg base, int32_t disp, Reg src) { alu_mr(&e->buf, 0x00, sz, base, disp, src); }

/* SUB */
void enc_sub_rr(Encoder *e, OpSize sz, Reg dst, Reg src) { alu_rr(&e->buf, 0x28, sz, dst, src); }
void enc_sub_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm) { alu_ri(&e->buf, 5, sz, dst, imm); }
void enc_sub_rm(Encoder *e, OpSize sz, Reg dst, Reg base, int32_t disp) { alu_rm(&e->buf, 0x2A, sz, dst, base, disp); }
void enc_sub_mr(Encoder *e, OpSize sz, Reg base, int32_t disp, Reg src) { alu_mr(&e->buf, 0x28, sz, base, disp, src); }

/* IMUL */
void enc_imul_rr(Encoder *e, OpSize sz, Reg dst, Reg src) {
    Buf *b = &e->buf;
    if (sz == SZ_64) emit_rexw(b, dst, REG_NONE, src);
    else emit_rex_no_w(b, dst, REG_NONE, src);
    buf_write8(b, 0x0F); buf_write8(b, 0xAF);
    buf_write8(b, modrm(3, dst, src));
}

void enc_imul_rri(Encoder *e, OpSize sz, Reg dst, Reg src, int32_t imm) {
    Buf *b = &e->buf;
    bool imm8 = (imm >= -128 && imm <= 127);
    if (sz == SZ_64) emit_rexw(b, dst, REG_NONE, src);
    else emit_rex_no_w(b, dst, REG_NONE, src);
    buf_write8(b, imm8 ? 0x6B : 0x69);
    buf_write8(b, modrm(3, dst, src));
    if (imm8) buf_write8(b, (uint8_t)(int8_t)imm);
    else      buf_write32(b, (uint32_t)imm);
}

void enc_imul_rm(Encoder *e, OpSize sz, Reg dst, Reg base, int32_t disp) {
    Buf *b = &e->buf;
    if (sz == SZ_64) emit_rexw(b, dst, REG_NONE, base);
    else emit_rex_no_w(b, dst, REG_NONE, base);
    buf_write8(b, 0x0F); buf_write8(b, 0xAF);
    emit_modrm_mem(b, dst, base, REG_NONE, 1, disp);
}

/* IDIV/DIV: uses RAX (and RDX for 64-bit) */
void enc_idiv_r(Encoder *e, OpSize sz, Reg src) {
    Buf *b = &e->buf;
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, src);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, src);
    buf_write8(b, sz == SZ_8 ? 0xF6 : 0xF7);
    buf_write8(b, modrm(3, 7, src));  /* /7 */
}

void enc_div_r(Encoder *e, OpSize sz, Reg src) {
    Buf *b = &e->buf;
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, src);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, src);
    buf_write8(b, sz == SZ_8 ? 0xF6 : 0xF7);
    buf_write8(b, modrm(3, 6, src));  /* /6 */
}

/* NEG, NOT */
void enc_neg_r(Encoder *e, OpSize sz, Reg r) {
    Buf *b = &e->buf;
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, r);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, r);
    buf_write8(b, sz == SZ_8 ? 0xF6 : 0xF7);
    buf_write8(b, modrm(3, 3, r));
}

void enc_not_r(Encoder *e, OpSize sz, Reg r) {
    Buf *b = &e->buf;
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, r);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, r);
    buf_write8(b, sz == SZ_8 ? 0xF6 : 0xF7);
    buf_write8(b, modrm(3, 2, r));
}

/* =========================================================
 * Bitwise
 * ========================================================= */

void enc_and_rr(Encoder *e, OpSize sz, Reg dst, Reg src) { alu_rr(&e->buf, 0x20, sz, dst, src); }
void enc_and_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm) { alu_ri(&e->buf, 4, sz, dst, imm); }
void enc_or_rr(Encoder *e, OpSize sz, Reg dst, Reg src)  { alu_rr(&e->buf, 0x08, sz, dst, src); }
void enc_or_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm) { alu_ri(&e->buf, 1, sz, dst, imm); }
void enc_xor_rr(Encoder *e, OpSize sz, Reg dst, Reg src) { alu_rr(&e->buf, 0x30, sz, dst, src); }
void enc_xor_ri(Encoder *e, OpSize sz, Reg dst, int32_t imm) { alu_ri(&e->buf, 6, sz, dst, imm); }

/* =========================================================
 * Shifts
 * ========================================================= */

static void shift_ri(Buf *b, uint8_t grp, OpSize sz, Reg dst, uint8_t cnt) {
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, dst);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, dst);
    if (cnt == 1) {
        buf_write8(b, sz == SZ_8 ? 0xD0 : 0xD1);
        buf_write8(b, modrm(3, (Reg)grp, dst));
    } else {
        buf_write8(b, sz == SZ_8 ? 0xC0 : 0xC1);
        buf_write8(b, modrm(3, (Reg)grp, dst));
        buf_write8(b, cnt);
    }
}

static void shift_cl(Buf *b, uint8_t grp, OpSize sz, Reg dst) {
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, dst);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, dst);
    buf_write8(b, sz == SZ_8 ? 0xD2 : 0xD3);
    buf_write8(b, modrm(3, (Reg)grp, dst));
}

void enc_shl_ri(Encoder *e, OpSize sz, Reg dst, uint8_t cnt) { shift_ri(&e->buf, 4, sz, dst, cnt); }
void enc_shr_ri(Encoder *e, OpSize sz, Reg dst, uint8_t cnt) { shift_ri(&e->buf, 5, sz, dst, cnt); }
void enc_sar_ri(Encoder *e, OpSize sz, Reg dst, uint8_t cnt) { shift_ri(&e->buf, 7, sz, dst, cnt); }
void enc_shl_r(Encoder *e, OpSize sz, Reg dst)               { shift_cl(&e->buf, 4, sz, dst); }
void enc_shr_r(Encoder *e, OpSize sz, Reg dst)               { shift_cl(&e->buf, 5, sz, dst); }
void enc_sar_r(Encoder *e, OpSize sz, Reg dst)               { shift_cl(&e->buf, 7, sz, dst); }

/* =========================================================
 * Compare / Test
 * ========================================================= */

void enc_cmp_rr(Encoder *e, OpSize sz, Reg a, Reg b_reg) {
    alu_rr(&e->buf, 0x38, sz, a, b_reg);
}

void enc_cmp_ri(Encoder *e, OpSize sz, Reg a, int32_t imm) {
    alu_ri(&e->buf, 7, sz, a, imm);
}

void enc_cmp_rm(Encoder *e, OpSize sz, Reg a, Reg base, int32_t disp) {
    alu_rm(&e->buf, 0x3A, sz, a, base, disp);
}

void enc_test_rr(Encoder *e, OpSize sz, Reg a, Reg b_reg) {
    alu_rr(&e->buf, 0x84, sz, a, b_reg);
    /* Note: TEST uses opcode 0x84 (byte) or 0x85 (others), src in reg field */
    /* The alu_rr helper puts src in ModRM.reg which is correct for TEST */
}

void enc_test_ri(Encoder *e, OpSize sz, Reg a, int32_t imm) {
    Buf *b = &e->buf;
    if (sz == SZ_64) emit_rexw(b, REG_NONE, REG_NONE, a);
    else if (sz == SZ_32) emit_rex_no_w(b, REG_NONE, REG_NONE, a);
    buf_write8(b, sz == SZ_8 ? 0xF6 : 0xF7);
    buf_write8(b, modrm(3, 0, a));
    if (sz == SZ_8)  buf_write8(b, (uint8_t)imm);
    else if (sz == SZ_16) buf_write16(b, (uint16_t)imm);
    else buf_write32(b, (uint32_t)imm);
}

void enc_setcc(Encoder *e, CondCode cc, Reg dst) {
    Buf *b = &e->buf;
    emit_rex_byte(b, REG_NONE, dst);
    buf_write8(b, 0x0F);
    buf_write8(b, (uint8_t)(0x90 | (uint8_t)cc));
    buf_write8(b, modrm(3, 0, dst));
}

/* =========================================================
 * Jumps
 * ========================================================= */

static void add_fixup(Encoder *e, const char *label) {
    Fixup *f    = malloc(sizeof(Fixup));
    f->patch_off = e->buf.size;
    f->label     = strdup(label);
    f->inst_end  = e->buf.size + 4;
    f->next      = e->fixups;
    e->fixups    = f;
    buf_write32(&e->buf, 0);  /* placeholder */
}

void enc_jmp(Encoder *e, const char *label) {
    buf_write8(&e->buf, 0xE9);  /* JMP rel32 */
    add_fixup(e, label);
}

void enc_jcc(Encoder *e, CondCode cc, const char *label) {
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, (uint8_t)(0x80 | (uint8_t)cc));
    add_fixup(e, label);
}

void enc_jmp_r(Encoder *e, Reg r) {
    Buf *b = &e->buf;
    if (r >= REG_R8) buf_write8(b, 0x41);
    buf_write8(b, 0xFF);
    buf_write8(b, modrm(3, 4, r));
}

/* =========================================================
 * Call / Ret
 * ========================================================= */

void enc_call(Encoder *e, const char *label) {
    buf_write8(&e->buf, 0xE8);  /* CALL rel32 */
    add_fixup(e, label);
}

void enc_call_r(Encoder *e, Reg r) {
    Buf *b = &e->buf;
    if (r >= REG_R8) buf_write8(b, 0x41);
    buf_write8(b, 0xFF);
    buf_write8(b, modrm(3, 2, r));
}

void enc_ret(Encoder *e) {
    buf_write8(&e->buf, 0xC3);
}

/* =========================================================
 * Stack
 * ========================================================= */

void enc_push_r(Encoder *e, Reg r) {
    Buf *b = &e->buf;
    if (r >= REG_R8) buf_write8(b, 0x41);
    buf_write8(b, (uint8_t)(0x50 | (r & 7)));
}

void enc_push_i(Encoder *e, int32_t imm) {
    Buf *b = &e->buf;
    if (imm >= -128 && imm <= 127) {
        buf_write8(b, 0x6A);
        buf_write8(b, (uint8_t)(int8_t)imm);
    } else {
        buf_write8(b, 0x68);
        buf_write32(b, (uint32_t)imm);
    }
}

void enc_pop_r(Encoder *e, Reg r) {
    Buf *b = &e->buf;
    if (r >= REG_R8) buf_write8(b, 0x41);
    buf_write8(b, (uint8_t)(0x58 | (r & 7)));
}

/* =========================================================
 * Sign extension
 * ========================================================= */

void enc_cdq(Encoder *e) { buf_write8(&e->buf, 0x99); }           /* EAX→EDX:EAX */
void enc_cqo(Encoder *e) { buf_write8(&e->buf, 0x48); buf_write8(&e->buf, 0x99); }  /* RAX→RDX:RAX */
void enc_cbw(Encoder *e) { buf_write8(&e->buf, 0x66); buf_write8(&e->buf, 0x98); }
void enc_cwde(Encoder *e){ buf_write8(&e->buf, 0x98); }
void enc_cdqe(Encoder *e){ buf_write8(&e->buf, 0x48); buf_write8(&e->buf, 0x98); }

/* =========================================================
 * Misc
 * ========================================================= */

void enc_nop(Encoder *e)     { buf_write8(&e->buf, 0x90); }
void enc_int3(Encoder *e)    { buf_write8(&e->buf, 0xCC); }
void enc_syscall(Encoder *e) { buf_write8(&e->buf, 0x0F); buf_write8(&e->buf, 0x05); }

/* =========================================================
 * SSE2 double-precision scalar instructions
 *
 * Convention: xmm_dst / xmm_src are register numbers 0-7.
 * RBP-relative memory operands: mod=10, rm=5 (4-byte disp).
 * ========================================================= */

/* Helper: emit F2 0F <op> xmm_reg, [rbp+off32] */
static void sse2_xmm_rbp(Encoder *e, uint8_t op, int xmm_reg, int off) {
    buf_write8(&e->buf, 0xF2);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, op);
    /* ModRM: mod=10 (disp32), reg=xmm_reg, rm=5 (rbp) */
    buf_write8(&e->buf, (uint8_t)(0x85 | ((xmm_reg & 7) << 3)));
    buf_write32(&e->buf, (uint32_t)(int32_t)off);
}

/* movsd xmm_dst, [rbp+off] */
void enc_movsd_load(Encoder *e, int xmm_dst, int off) {
    sse2_xmm_rbp(e, 0x10, xmm_dst, off);
}

/* movsd [rbp+off], xmm_src */
void enc_movsd_store(Encoder *e, int off, int xmm_src) {
    buf_write8(&e->buf, 0xF2);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x11);  /* MOVSD m64, xmm */
    buf_write8(&e->buf, (uint8_t)(0x85 | ((xmm_src & 7) << 3)));
    buf_write32(&e->buf, (uint32_t)(int32_t)off);
}

/* movsd xmm_dst, xmm_src */
void enc_movsd_rr(Encoder *e, int xmm_dst, int xmm_src) {
    buf_write8(&e->buf, 0xF2);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x10);
    /* ModRM: mod=11, reg=xmm_dst, rm=xmm_src */
    buf_write8(&e->buf, (uint8_t)(0xC0 | ((xmm_dst & 7) << 3) | (xmm_src & 7)));
}

/* Helper: emit F2 0F <op> xmm0, xmm1 (binop xmm0 op= xmm1) */
static void sse2_binop(Encoder *e, uint8_t op) {
    buf_write8(&e->buf, 0xF2);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, op);
    buf_write8(&e->buf, 0xC1);  /* ModRM: mod=11, reg=xmm0, rm=xmm1 */
}

void enc_addsd(Encoder *e) { sse2_binop(e, 0x58); }  /* xmm0 += xmm1 */
void enc_subsd(Encoder *e) { sse2_binop(e, 0x5C); }  /* xmm0 -= xmm1 */
void enc_mulsd(Encoder *e) { sse2_binop(e, 0x59); }  /* xmm0 *= xmm1 */
void enc_divsd(Encoder *e) { sse2_binop(e, 0x5E); }  /* xmm0 /= xmm1 */

/* ucomisd xmm0, xmm1 — sets ZF/PF/CF for comparisons */
void enc_ucomisd(Encoder *e) {
    buf_write8(&e->buf, 0x66);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x2E);
    buf_write8(&e->buf, 0xC1);  /* ModRM: mod=11, reg=xmm0, rm=xmm1 */
}

/* cvttsd2si rax, xmm0 — truncating double → int64 */
void enc_cvttsd2si(Encoder *e) {
    buf_write8(&e->buf, 0xF2);
    buf_write8(&e->buf, 0x48);  /* REX.W */
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x2C);
    buf_write8(&e->buf, 0xC0);  /* ModRM: mod=11, reg=rax(0), rm=xmm0(0) */
}

/* cvtsi2sd xmm0, rax — int64 → double */
void enc_cvtsi2sd(Encoder *e) {
    buf_write8(&e->buf, 0xF2);
    buf_write8(&e->buf, 0x48);  /* REX.W */
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x2A);
    buf_write8(&e->buf, 0xC0);  /* ModRM: mod=11, reg=xmm0(0), rm=rax(0) */
}

/* movsd xmm0, [rip + disp32]  — for loading doubles from .rodata
 * Returns the offset of the 4-byte displacement field (for relocation). */
size_t enc_movsd_rip(Encoder *e) {
    buf_write8(&e->buf, 0xF2);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x10);
    buf_write8(&e->buf, 0x05);  /* ModRM: mod=00, reg=xmm0, rm=5 (RIP-relative) */
    size_t disp_off = enc_size(e);
    buf_write32(&e->buf, 0);    /* placeholder */
    return disp_off;
}

/* sub rsp, 8; movsd [rsp], xmm0  — push xmm0 onto stack */
void enc_push_xmm0(Encoder *e) {
    buf_write8(&e->buf, 0x48); buf_write8(&e->buf, 0x83);
    buf_write8(&e->buf, 0xEC); buf_write8(&e->buf, 0x08);  /* sub rsp, 8 */
    buf_write8(&e->buf, 0xF2); buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x11); buf_write8(&e->buf, 0x04);
    buf_write8(&e->buf, 0x24);  /* movsd [rsp], xmm0 */
}

/* movsd xmm1, [rsp]; add rsp, 8  — pop stack into xmm1 */
void enc_pop_xmm1(Encoder *e) {
    buf_write8(&e->buf, 0xF2); buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x10); buf_write8(&e->buf, 0x0C);
    buf_write8(&e->buf, 0x24);  /* movsd xmm1, [rsp] */
    buf_write8(&e->buf, 0x48); buf_write8(&e->buf, 0x83);
    buf_write8(&e->buf, 0xC4); buf_write8(&e->buf, 0x08);  /* add rsp, 8 */
}

/* ── MOVQ: transfer bit-pattern between XMM and integer registers ── */

/* movq xmm0, rax  (GPR→XMM, preserves bit pattern; same as movq via PEXTRQ/PINSRQ but simpler) */
/* Encoding: 66 REX.W 0F 6E /r   ModRM=C0 (mod=11, reg=xmm0(0), rm=rax(0)) */
void enc_movq_xmm0_rax(Encoder *e) {
    buf_write8(&e->buf, 0x66);
    buf_write8(&e->buf, 0x48);  /* REX.W */
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x6E);
    buf_write8(&e->buf, 0xC0);
}

/* movq xmm1, rax  (GPR→XMM for right operand) */
/* ModRM=C8: mod=11, reg=xmm1(1), rm=rax(0) */
void enc_movq_xmm1_rax(Encoder *e) {
    buf_write8(&e->buf, 0x66);
    buf_write8(&e->buf, 0x48);  /* REX.W */
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x6E);
    buf_write8(&e->buf, 0xC8);
}

/* movq rax, xmm0  (XMM→GPR, preserves bit pattern) */
/* Encoding: 66 REX.W 0F 7E /r   ModRM=C0 (mod=11, reg=xmm0(0), rm=rax(0)) */
void enc_movq_rax_xmm0(Encoder *e) {
    buf_write8(&e->buf, 0x66);
    buf_write8(&e->buf, 0x48);  /* REX.W */
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x7E);
    buf_write8(&e->buf, 0xC0);
}

/* movq xmm1, rcx  (GPR→XMM for left operand stored in rcx after pop) */
/* ModRM=C9: mod=11, reg=xmm1(1), rm=rcx(1) */
void enc_movq_xmm1_rcx(Encoder *e) {
    buf_write8(&e->buf, 0x66);
    buf_write8(&e->buf, 0x48);  /* REX.W */
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x6E);
    buf_write8(&e->buf, 0xC9);
}

/* movq xmmN, src_gpr  (GPR→XMM for argument N)
 * Encoding: 66 REX.W [REX.R if xmm>=8] 0F 6E /r
 * xmm 0-7 only for now (REX.R not needed) */
void enc_movq_xmm_gpr(Encoder *e, int xmm_dst, Reg src) {
    buf_write8(&e->buf, 0x66);
    uint8_t rex = 0x48;  /* REX.W */
    if ((int)src >= 8) rex |= 0x01;    /* REX.B for extended src */
    if (xmm_dst >= 8)  rex |= 0x04;   /* REX.R for extended xmm */
    buf_write8(&e->buf, rex);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x6E);
    uint8_t modrm = (uint8_t)(0xC0 | ((xmm_dst & 7) << 3) | ((int)src & 7));
    buf_write8(&e->buf, modrm);
}

/* movq dst_gpr, xmmN  (XMM→GPR)
 * Encoding: 66 REX.W [REX.R] 0F 7E /r */
void enc_movq_gpr_xmm(Encoder *e, Reg dst, int xmm_src) {
    buf_write8(&e->buf, 0x66);
    uint8_t rex = 0x48;  /* REX.W */
    if ((int)dst >= 8) rex |= 0x01;
    if (xmm_src >= 8)  rex |= 0x04;
    buf_write8(&e->buf, rex);
    buf_write8(&e->buf, 0x0F);
    buf_write8(&e->buf, 0x7E);
    uint8_t modrm = (uint8_t)(0xC0 | ((xmm_src & 7) << 3) | ((int)dst & 7));
    buf_write8(&e->buf, modrm);
}
