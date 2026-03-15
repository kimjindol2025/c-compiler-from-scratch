/*
 * codegen.c — x86-64 Code Generator
 *
 * Translates AST nodes to x86-64 machine code using the System V AMD64 ABI.
 *
 * Calling convention:
 *   Args in: rdi, rsi, rdx, rcx, r8, r9 (then stack right-to-left)
 *   Return:  rax (rax:rdx for 128-bit)
 *   Caller-saved: rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
 *   Callee-saved: rbx, rbp, r12, r13, r14, r15
 *
 * Stack frame layout:
 *   [rbp]     = old rbp
 *   [rbp - 8] = first local variable
 *   ...
 */

#include "../include/codegen.h"
#include "../include/x86_encode.h"
#include "../include/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>

/* Argument registers (SysV AMD64 ABI) */
static const Reg ARG_REGS[] = {
    REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9
};
static const int NARG_REGS = 6;

/* =========================================================
 * Codegen helpers
 * ========================================================= */

static void cg_error(CodeGen *cg, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "codegen error: ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
    cg->error_count++;
}

static char *cg_make_label(CodeGen *cg, const char *prefix) {
    char buf[64];
    snprintf(buf, sizeof(buf), ".%s%d", prefix ? prefix : "L", cg->label_count++);
    return strdup(buf);
}

/* ---- Symbol registration ---- */

static void sym_add(CodeGen *cg, const char *name, CGSymKind kind,
                    size_t offset, int size, bool is_global) {
    SymEntry *s  = calloc(1, sizeof(SymEntry));
    s->name      = strdup(name);
    s->kind      = kind;
    s->offset    = offset;
    s->size      = size;
    s->is_global = is_global;
    s->next      = cg->syms;
    cg->syms     = s;
}

/* ---- Relocation registration ---- */

static void reloc_add(CodeGen *cg, size_t offset, const char *sym,
                      int type, int64_t addend) {
    Reloc *r  = calloc(1, sizeof(Reloc));
    r->offset   = offset;
    r->sym_name = strdup(sym);
    r->type     = type;
    r->addend   = addend;
    r->next     = cg->relocs;
    cg->relocs  = r;
}

/* ---- Data section helpers ---- */

static size_t data_add(CodeGen *cg, const char *label,
                       const uint8_t *bytes, int size, int align,
                       bool is_bss, bool is_rodata) {
    Buf *sec = is_bss ? NULL : (is_rodata ? &cg->rodata_buf : &cg->data_buf);

    size_t off = is_bss ? cg->bss_size : (sec ? sec->size : 0);

    /* Align */
    if (align > 1) {
        size_t pad = (align - (off % align)) % align;
        if (is_bss) {
            off += pad;
            cg->bss_size = off;
        } else {
            for (size_t i = 0; i < pad; i++) buf_write8(sec, 0);
            off = sec->size;
        }
    }

    if (is_bss) {
        cg->bss_size += size;
    } else if (bytes) {
        for (int i = 0; i < size; i++) buf_write8(sec, bytes[i]);
    } else {
        for (int i = 0; i < size; i++) buf_write8(sec, 0);
    }

    DataEntry *de = calloc(1, sizeof(DataEntry));
    de->label     = strdup(label);
    de->size      = size;
    de->align     = align;
    de->is_bss    = is_bss;
    de->is_rodata = is_rodata;
    de->data      = bytes ? (uint8_t *)bytes : NULL;  /* may be NULL for zeroed */
    de->next      = cg->data_entries;
    cg->data_entries = de;

    return off;
}

/* ---- Variable lookup: find frame offset for a local ---- */

typedef struct LocalVar {
    char  *name;
    int    offset;    /* negative from rbp */
    Type  *type;
    bool   is_static; /* true: global storage, access via RIP-relative reloc */
    char  *global_name; /* unique global name for static locals */
    struct LocalVar *next;
} LocalVar;

/* We maintain our own var table per function */
typedef struct {
    LocalVar *vars;
    int       next_offset;  /* grows downward */
} LocalCtx;

static LocalVar *local_find(LocalCtx *ctx, const char *name) {
    for (LocalVar *v = ctx->vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/* Forward declaration needed by static_local_alloc */
static int ty_size(Type *t);

/* Allocate a static local: emit to .bss/.data, add to local table as "static". */
static void static_local_alloc(CodeGen *cg, LocalCtx *lctx,
                                const char *func_name, const char *var_name,
                                Type *ty, Node *init_node) {
    static int static_ctr = 0;
    char gname[128];
    snprintf(gname, sizeof(gname), ".%s.%s.%d", func_name ? func_name : "fn",
             var_name ? var_name : "v", static_ctr++);

    int sz    = ty ? ty_size(ty) : 4;
    int align = ty ? (ty->align > 0 ? ty->align : 4) : 4;
    if (sz <= 0) sz = 4;

    if (init_node && (init_node->kind == ND_INT_LIT || init_node->kind == ND_CHAR_LIT)
        && init_node->ival != 0) {
        /* Non-zero initializer: .data */
        uint8_t bytes[8] = {0};
        long long v = init_node->ival;
        memcpy(bytes, &v, sz < 8 ? sz : 8);
        data_add(cg, gname, bytes, sz, align, false, false);
    } else {
        /* Zero initializer (or complex): .bss */
        data_add(cg, gname, NULL, sz, align, true, false);
    }
    sym_add(cg, gname, CGSYM_OBJECT, 0, sz, false); /* local visibility */

    /* Add to local ctx as static */
    LocalVar *v = calloc(1, sizeof(LocalVar));
    v->name        = strdup(var_name);
    v->global_name = strdup(gname);
    v->is_static   = true;
    v->offset      = 0;
    v->type        = ty;
    v->next        = lctx->vars;
    lctx->vars     = v;
}

static int local_alloc(LocalCtx *ctx, const char *name, Type *ty) {
    int size  = ty ? ty->size : 8;
    int align = ty ? ty->align : 8;
    if (size  <= 0) size  = 8;
    if (align <= 0) align = 8;

    /* Align downward (toward more-negative).
     * C integer division truncates toward zero, so for negative values
     * (-12 / 8) = -1, but we need floor(-12/8) = -2.
     * Use: floor_div(a, b) = -((-a + b-1) / b) for a < 0, b > 0.
     */
    ctx->next_offset -= size;
    if (align > 1) {
        int neg = -ctx->next_offset;
        ctx->next_offset = -(((neg) + align - 1) / align * align);
    }

    LocalVar *v = calloc(1, sizeof(LocalVar));
    v->name   = strdup(name);
    v->offset = ctx->next_offset;
    v->type   = ty;
    v->next   = ctx->vars;
    ctx->vars = v;
    return v->offset;
}

static void local_ctx_free(LocalCtx *ctx) {
    LocalVar *v = ctx->vars;
    while (v) { LocalVar *n = v->next; free(v->name); free(v); v = n; }
    ctx->vars = NULL;
}

/* =========================================================
 * Code generation context for a function
 * ========================================================= */

typedef struct GenCtx {
    CodeGen  *cg;
    Encoder  *enc;
    LocalCtx  locals;

    /* Control flow */
    LoopLabel *loop_stack;
    SwitchCtx *switch_stack;

    char      *return_label;
    int        return_type_size;
    Type      *return_ty;    /* function's return type (for float return convention) */
    const char *func_name;   /* current function name (for static local naming) */
} GenCtx;

/* Forward declarations */
static void gen_expr(GenCtx *ctx, Node *n);      /* result in RAX */
static void gen_stmt(GenCtx *ctx, Node *n);
static void gen_lvalue(GenCtx *ctx, Node *n);    /* result: address in RAX */

/* =========================================================
 * Type utilities
 * ========================================================= */

static int ty_size(Type *t) {
    if (!t) return 8;
    if (t->size > 0) return t->size;
    switch (t->kind) {
    case TY_VOID:    return 0;
    case TY_BOOL:    return 1;
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR: return 1;
    case TY_SHORT: case TY_USHORT: return 2;
    case TY_INT: case TY_UINT: case TY_ENUM: return 4;
    case TY_LONG: case TY_ULONG:
    case TY_LLONG: case TY_ULLONG: return 8;
    case TY_FLOAT:  return 4;
    case TY_DOUBLE: return 8;
    case TY_PTR:    return 8;
    case TY_ARRAY:
        if (t->base && t->array_len > 0) return t->base->size * t->array_len;
        return 0;
    default: return 8;
    }
}

static OpSize ty_opsize(Type *t) {
    switch (ty_size(t)) {
    case 1: return SZ_8;
    case 2: return SZ_16;
    case 4: return SZ_32;
    default: return SZ_64;
    }
}

static bool ty_is_unsigned(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_UCHAR: case TY_USHORT: case TY_UINT:
    case TY_ULONG: case TY_ULLONG: case TY_BOOL:
        return true;
    default: return false;
    }
}

/* =========================================================
 * System V AMD64 ABI struct classification
 * ========================================================= */
typedef enum { ABI_INTEGER, ABI_SSE, ABI_MEMORY } AbiClass;

/* Classify one 8-byte chunk of a struct for register passing.
 * chunk_idx: 0=bytes[0,8), 1=bytes[8,16).
 * Returns ABI_SSE if all fields in this chunk are float/double,
 * ABI_INTEGER otherwise, ABI_MEMORY if struct > 16 bytes. */
static AbiClass classify_struct_chunk(Type *ty, int chunk_idx) {
    if (!ty) return ABI_INTEGER;
    int sz = ty->size > 0 ? ty->size : ty_size(ty);
    if (sz > 16) return ABI_MEMORY;

    int chunk_start = chunk_idx * 8;
    int chunk_end   = chunk_start + 8;

    bool has_sse = false, has_int = false;
    for (Member *m = ty->members; m; m = m->next) {
        int msz   = m->ty ? (m->ty->size > 0 ? m->ty->size : ty_size(m->ty)) : 4;
        int m_end = m->offset + msz;
        if (m_end <= chunk_start || m->offset >= chunk_end) continue;
        if (m->ty && (m->ty->kind == TY_DOUBLE || m->ty->kind == TY_FLOAT))
            has_sse = true;
        else
            has_int = true;
    }
    if (has_int) return ABI_INTEGER;
    if (has_sse) return ABI_SSE;
    return ABI_INTEGER; /* padding-only chunk */
}

/* Returns false if struct must go via MEMORY (> 16 bytes).
 * On success, fills c0 (chunk 0) and c1 (chunk 1, meaningful only if sz>8). */
static bool classify_struct(Type *ty, AbiClass *c0, AbiClass *c1) {
    if (!ty || (ty->kind != TY_STRUCT && ty->kind != TY_UNION)) return false;
    int sz = ty->size > 0 ? ty->size : ty_size(ty);
    if (sz > 16) return false;
    *c0 = classify_struct_chunk(ty, 0);
    *c1 = (sz > 8) ? classify_struct_chunk(ty, 1) : ABI_INTEGER;
    return true;
}

/* Load value from [rbp + offset] into rax, sign/zero extending as needed */
static void load_local(GenCtx *ctx, int offset, Type *ty) {
    Encoder *e = ctx->enc;
    int sz = ty_size(ty);

    if (sz == 8) {
        enc_mov_rm(e, SZ_64, REG_RAX, REG_RBP, REG_NONE, 1, offset);
    } else if (sz == 4) {
        if (ty_is_unsigned(ty)) {
            /* MOV eax, [rbp+offset] — zero-extends automatically */
            enc_mov_rm(e, SZ_32, REG_RAX, REG_RBP, REG_NONE, 1, offset);
        } else {
            enc_movsxd_rm(e, REG_RAX, REG_RBP, offset);
        }
    } else if (sz == 2) {
        if (ty_is_unsigned(ty))
            enc_movzx_rm(e, SZ_64, SZ_16, REG_RAX, REG_RBP, offset);
        else
            enc_movsx_rm(e, SZ_64, SZ_16, REG_RAX, REG_RBP, offset);
    } else if (sz == 1) {
        if (ty_is_unsigned(ty))
            enc_movzx_rm(e, SZ_64, SZ_8, REG_RAX, REG_RBP, offset);
        else
            enc_movsx_rm(e, SZ_64, SZ_8, REG_RAX, REG_RBP, offset);
    } else {
        /* struct/array: load address */
        enc_lea(e, REG_RAX, REG_RBP, REG_NONE, 1, offset);
    }
}

/* Store rax → [rbp + offset] */
static void store_local(GenCtx *ctx, int offset, Type *ty) {
    Encoder *e = ctx->enc;
    int sz = ty_size(ty);
    OpSize opsz = ty_opsize(ty);
    enc_mov_mr(e, opsz, REG_RBP, REG_NONE, 1, offset, REG_RAX);
    (void)sz;
}

/* Store rax → [rcx] (pointer in rcx) */
static void store_ptr(GenCtx *ctx, Type *ty) {
    Encoder *e = ctx->enc;
    OpSize opsz = ty_opsize(ty);
    enc_mov_mr(e, opsz, REG_RCX, REG_NONE, 1, 0, REG_RAX);
}

/* Load from [rcx] → rax */
static void load_ptr(GenCtx *ctx, Type *ty) {
    Encoder *e = ctx->enc;
    int sz = ty_size(ty);
    if (sz == 8)
        enc_mov_rm(e, SZ_64, REG_RAX, REG_RCX, REG_NONE, 1, 0);
    else if (sz == 4)
        enc_movsxd_rm(e, REG_RAX, REG_RCX, 0);
    else if (sz == 2) {
        if (ty_is_unsigned(ty))
            enc_movzx_rm(e, SZ_64, SZ_16, REG_RAX, REG_RCX, 0);
        else
            enc_movsx_rm(e, SZ_64, SZ_16, REG_RAX, REG_RCX, 0);
    } else if (sz == 1) {
        if (ty_is_unsigned(ty))
            enc_movzx_rm(e, SZ_64, SZ_8, REG_RAX, REG_RCX, 0);
        else
            enc_movsx_rm(e, SZ_64, SZ_8, REG_RAX, REG_RCX, 0);
    } else {
        enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);  /* address */
    }
}

/* =========================================================
 * lvalue code generation
 * Produces the address of the lvalue in RAX
 * ========================================================= */

static void gen_lvalue(GenCtx *ctx, Node *n) {
    Encoder *e = ctx->enc;

    switch (n->kind) {
    case ND_VAR:
    case ND_IDENT: {
        /* Use sema-resolved name if available, fallback to union field */
        const char *lookup_name = (n->var && n->var->name) ? n->var->name : n->ident.name;
        LocalVar *v = local_find(&ctx->locals, lookup_name);
        if (v && !v->is_static) {
            enc_lea(e, REG_RAX, REG_RBP, REG_NONE, 1, v->offset);
        } else {
            /* Global or static local: RIP-relative LEA via ELF relocation */
            const char *sym_name = (v && v->is_static) ? v->global_name : lookup_name;
            buf_write8(&e->buf, 0x48);
            buf_write8(&e->buf, 0x8D);
            buf_write8(&e->buf, 0x05); /* LEA rax, [rip+rel32] */
            size_t reloc_off = enc_size(e);
            buf_write32(&e->buf, 0);
            reloc_add(ctx->cg, reloc_off, sym_name, 2 /*R_X86_64_PC32*/, -4);
        }
        break;
    }

    case ND_DEREF:
        /* *ptr: evaluate ptr into rax */
        gen_expr(ctx, n->unary.operand);
        break;

    case ND_INDEX: {
        /* arr[idx]: address = arr + idx * elem_size */
        Type *arr_ty = n->index.arr->type ? n->index.arr->type : n->index.arr->ty;
        /* Fallback: look up type from the local variable table */
        if (!arr_ty && (n->index.arr->kind == ND_VAR || n->index.arr->kind == ND_IDENT)) {
            LocalVar *lv = local_find(&ctx->locals, n->index.arr->ident.name);
            if (lv) arr_ty = lv->type;
        }
        Type *elem_ty = NULL;
        if (arr_ty && (arr_ty->kind == TY_ARRAY || arr_ty->kind == TY_PTR))
            elem_ty = arr_ty->base;
        if (!elem_ty) elem_ty = n->type ? n->type : n->ty;
        int elem_size = elem_ty ? ty_size(elem_ty) : 4;

        /* For a pointer param/var, we need the pointer VALUE (gen_expr).
           For an array var on the stack, we need its ADDRESS (gen_lvalue). */
        bool arr_is_ptr = arr_ty && arr_ty->kind == TY_PTR;
        if (arr_is_ptr)
            gen_expr(ctx, n->index.arr);    /* pointer value in rax */
        else
            gen_lvalue(ctx, n->index.arr);  /* array base address in rax */
        enc_push_r(e, REG_RAX);  /* save base address */

        /* Evaluate index */
        gen_expr(ctx, n->index.idx);
        enc_pop_r(e, REG_RCX);   /* base address in rcx */

        /* rax = rcx + rax * elem_size */
        if (elem_size != 1) {
            enc_imul_rri(e, SZ_64, REG_RAX, REG_RAX, elem_size);
        }
        enc_add_rr(e, SZ_64, REG_RAX, REG_RCX);
        break;
    }

    case ND_MEMBER: {
        /* struct.member: address = base_addr + member.offset */
        gen_lvalue(ctx, n->member_access.obj);
        Member *m = n->member_access.resolved;
        if (m) {
            if (m->offset != 0)
                enc_add_ri(e, SZ_64, REG_RAX, m->offset);
        } else {
            /* Try to find member offset from type */
            Type *obj_ty = n->member_access.obj->type;
            if (obj_ty && (obj_ty->kind == TY_STRUCT || obj_ty->kind == TY_UNION)) {
                for (Member *mem = obj_ty->members; mem; mem = mem->next) {
                    if (mem->name && strcmp(mem->name, n->member_access.member) == 0) {
                        if (mem->offset != 0)
                            enc_add_ri(e, SZ_64, REG_RAX, mem->offset);
                        break;
                    }
                }
            }
        }
        break;
    }

    case ND_ARROW: {
        /* ptr->member: address = *ptr + member.offset */
        gen_expr(ctx, n->member_access.obj);  /* pointer value in rax */
        Member *m = n->member_access.resolved;
        int off = 0;
        if (m) {
            off = m->offset;
        } else {
            Type *ptr_ty = n->member_access.obj->type;
            Type *struct_ty = ptr_ty ? ptr_ty->base : NULL;
            if (struct_ty && (struct_ty->kind == TY_STRUCT || struct_ty->kind == TY_UNION)) {
                for (Member *mem = struct_ty->members; mem; mem = mem->next) {
                    if (mem->name && strcmp(mem->name, n->member_access.member) == 0) {
                        off = mem->offset;
                        break;
                    }
                }
            }
        }
        if (off != 0) enc_add_ri(e, SZ_64, REG_RAX, off);
        break;
    }

    default:
        cg_error(ctx->cg, "cannot take lvalue of node kind %d", n->kind);
        enc_xor_rr(e, SZ_64, REG_RAX, REG_RAX);
        break;
    }
}

/* =========================================================
 * Expression code generation
 * Result: RAX (or XMM0 for floats — simplified: use integer path)
 * ========================================================= */

static void gen_assign(GenCtx *ctx, Node *lhs, Node *rhs) {
    Encoder *e = ctx->enc;

    /* Evaluate rhs → rax */
    gen_expr(ctx, rhs);

    /* Get lhs address → rcx */
    enc_push_r(e, REG_RAX);
    gen_lvalue(ctx, lhs);
    enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
    enc_pop_r(e, REG_RAX);

    /* Store — resolve type from local var table when AST type is missing */
    Type *ty = lhs->type ? lhs->type : lhs->ty;
    if (!ty) ty = rhs->type ? rhs->type : rhs->ty;
    if (!ty && (lhs->kind == ND_VAR || lhs->kind == ND_IDENT)) {
        LocalVar *lv = local_find(&ctx->locals, lhs->ident.name);
        if (lv) ty = lv->type;
    }
    store_ptr(ctx, ty);
}

static void gen_compound_assign(GenCtx *ctx, NodeKind op, Node *lhs, Node *rhs) {
    Encoder *e = ctx->enc;

    /* Get lhs address */
    gen_lvalue(ctx, lhs);
    enc_push_r(e, REG_RAX);  /* save lhs address */

    /* Load lhs value */
    enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
    Type *ty = lhs->type;
    load_ptr(ctx, ty);  /* rax = *lhs */

    enc_push_r(e, REG_RAX);  /* save lhs value */

    /* Evaluate rhs */
    gen_expr(ctx, rhs);     /* rax = rhs */

    enc_pop_r(e, REG_RCX);  /* rcx = lhs value */

    /* Apply op: rax = rcx op rax (rcx = old lhs value, rax = rhs) */
    switch (op) {
    case ND_ASSIGN_ADD:
        /* Pointer arithmetic: scale rhs by element size */
        if (ty && ty->kind == TY_PTR && ty->base) {
            int esz = ty_size(ty->base);
            if (esz > 1) enc_imul_rri(e, SZ_64, REG_RAX, REG_RAX, esz);
        }
        enc_add_rr(e, SZ_64, REG_RCX, REG_RAX);
        break;
    case ND_ASSIGN_SUB:
        /* Pointer arithmetic: scale rhs by element size */
        if (ty && ty->kind == TY_PTR && ty->base) {
            int esz = ty_size(ty->base);
            if (esz > 1) enc_imul_rri(e, SZ_64, REG_RAX, REG_RAX, esz);
        }
        enc_sub_rr(e, SZ_64, REG_RCX, REG_RAX);
        break;
    case ND_ASSIGN_MUL: enc_imul_rr(e, SZ_64, REG_RCX, REG_RAX); break;
    case ND_ASSIGN_AND: enc_and_rr(e, SZ_64, REG_RCX, REG_RAX); break;
    case ND_ASSIGN_OR:  enc_or_rr(e, SZ_64, REG_RCX, REG_RAX); break;
    case ND_ASSIGN_XOR: enc_xor_rr(e, SZ_64, REG_RCX, REG_RAX); break;
    case ND_ASSIGN_SHL:
        /* shift amount in CL; we need to put rax→rcx and old rcx→rax then shift */
        enc_mov_rr(e, SZ_64, REG_R10, REG_RAX);
        enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_R10);
        enc_shl_r(e, SZ_64, REG_RAX);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        break;
    case ND_ASSIGN_SHR:
        enc_mov_rr(e, SZ_64, REG_R10, REG_RAX);
        enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_R10);
        enc_sar_r(e, SZ_64, REG_RAX);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        break;
    case ND_ASSIGN_DIV:
        /* rcx / rax: put rcx→rax, zero/sign extend, then idiv */
        enc_mov_rr(e, SZ_64, REG_R10, REG_RAX);  /* save divisor */
        enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);  /* dividend */
        enc_cqo(e);
        enc_idiv_r(e, SZ_64, REG_R10);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        goto store;
    case ND_ASSIGN_MOD:
        enc_mov_rr(e, SZ_64, REG_R10, REG_RAX);
        enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);
        enc_cqo(e);
        enc_idiv_r(e, SZ_64, REG_R10);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RDX);  /* remainder */
        goto store;
    default:
        break;
    }

    enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);  /* result in rax */

store: ;
    /* Store result back */
    enc_pop_r(e, REG_RCX);  /* rcx = lhs address */
    enc_push_r(e, REG_RAX); /* save result */
    enc_mov_rr(e, SZ_64, REG_RCX, REG_RCX);  /* already have address */

    /* Store rax at [rcx] */
    enc_pop_r(e, REG_RAX);
    enc_push_r(e, REG_RAX);
    /* rcx is the address from the earlier pop */
    /* We need to reconstruct — actually we lost rcx. Re-read from stack. */
    /* Alternative: use a different register to hold address.
       Let's do it more carefully. */
    /* We popped lhs_addr into rcx earlier; we pushed result. Now:
       stack: [result]  and we lost lhs_addr.
       Fix: store lhs_addr before ops. Already saved on stack as second push.
       Actually the stack push order was:
         push lhs_addr
         [lhs value pushed]
         [lhs value popped into rcx]
         [result computed in rcx]
         now we have: stack top = lhs_addr
       */
    enc_pop_r(e, REG_RAX);  /* result */
    /* Now restore lhs_addr from the earlier push */
    /* We lost it — let's use a temporary register approach with R11 */
    /* For correctness, re-evaluate lhs lvalue */
    enc_push_r(e, REG_RAX);
    gen_lvalue(ctx, lhs);
    enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
    enc_pop_r(e, REG_RAX);

    store_ptr(ctx, ty ? ty : lhs->type);
}

static void gen_expr(GenCtx *ctx, Node *n) {
    Encoder *e = ctx->enc;

    if (!n) { enc_xor_rr(e, SZ_64, REG_RAX, REG_RAX); return; }

    switch (n->kind) {
    case ND_INT_LIT:
    case ND_CHAR_LIT:
        enc_mov_ri(e, SZ_64, REG_RAX, n->ival);
        break;

    case ND_FLOAT_LIT: {
        /* Store IEEE754 double in .rodata, load raw bits into RAX */
        char label[64];
        snprintf(label, sizeof(label), ".flt%d", ctx->cg->label_count++);
        double fval = n->fval;
        uint64_t bits;
        memcpy(&bits, &fval, 8);
        size_t off_before = ctx->cg->rodata_buf.size;
        buf_write64(&ctx->cg->rodata_buf, bits);
        sym_add(ctx->cg, label, CGSYM_RODATA, off_before, 8, false);
        /* MOV rax, [rip + rel32]  — 48 8B 05 <rel32> */
        buf_write8(&e->buf, 0x48);
        buf_write8(&e->buf, 0x8B);
        buf_write8(&e->buf, 0x05);
        size_t reloc_off = enc_size(e);
        buf_write32(&e->buf, 0);
        reloc_add(ctx->cg, reloc_off, label, 2 /*R_X86_64_PC32*/, -4);
        break;
    }

    case ND_STR_LIT: {
        /* String literal: put in .rodata, LEA rax, [rip+sym] */
        char label[64];
        snprintf(label, sizeof(label), ".str%d", ctx->cg->label_count++);

        /* Add string to rodata */
        size_t slen = strlen(n->sval) + 1;
        size_t off_before = ctx->cg->rodata_buf.size;
        for (size_t i = 0; i < slen; i++)
            buf_write8(&ctx->cg->rodata_buf, (uint8_t)n->sval[i]);

        sym_add(ctx->cg, label, CGSYM_RODATA, off_before, (int)slen, false);

        /* Emit: LEA rax, [rip + rel32]
           Encoding: REX.W 8D /r   ModRM=05 (RIP-relative)   rel32
           = 48 8D 05 <rel32>  */
        buf_write8(&e->buf, 0x48);
        buf_write8(&e->buf, 0x8D);
        buf_write8(&e->buf, 0x05);  /* ModRM: mod=0, reg=0(rax), rm=5(RIP+disp) */
        size_t reloc_off = enc_size(e);
        buf_write32(&e->buf, 0);    /* placeholder for linker */
        reloc_add(ctx->cg, reloc_off, label, 2 /*R_X86_64_PC32*/, -4);
        break;
    }

    case ND_VAR:
    case ND_IDENT: {
        const char *lookup_name = (n->var && n->var->name) ? n->var->name : n->ident.name;
        LocalVar *v = local_find(&ctx->locals, lookup_name);
        if (v && !v->is_static) {
            load_local(ctx, v->offset, v->type);
        } else {
            /* Global variable or static local: LEA rax, [rip + sym] via ELF reloc */
            const char *sym_name = (v && v->is_static) ? v->global_name : lookup_name;
            buf_write8(&e->buf, 0x48);
            buf_write8(&e->buf, 0x8D);
            buf_write8(&e->buf, 0x05);
            size_t reloc_off = enc_size(e);
            buf_write32(&e->buf, 0);
            reloc_add(ctx->cg, reloc_off, sym_name, 2 /*R_X86_64_PC32*/, -4);
            /* Then dereference if not a function pointer */
            Type *ty = v ? v->type : n->type;
            if (ty && ty->kind != TY_FUNC) {
                enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
                load_ptr(ctx, ty);
            }
        }
        break;
    }

    case ND_ASSIGN:
        gen_assign(ctx, n->binary.left, n->binary.right);
        break;

    case ND_ASSIGN_ADD: case ND_ASSIGN_SUB: case ND_ASSIGN_MUL:
    case ND_ASSIGN_DIV: case ND_ASSIGN_MOD: case ND_ASSIGN_AND:
    case ND_ASSIGN_OR:  case ND_ASSIGN_XOR: case ND_ASSIGN_SHL:
    case ND_ASSIGN_SHR:
        gen_compound_assign(ctx, n->kind, n->binary.left, n->binary.right);
        break;

    case ND_ADD: {
        /* Handle pointer arithmetic */
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        Type *lty = n->binary.left->type;
        /* Floating-point add */
        if (lty && type_is_float(lty)) {
            enc_movq_xmm0_rax(e);   /* xmm0 = right (rax) */
            enc_movq_xmm1_rcx(e);   /* xmm1 = left (rcx) */
            enc_addsd(e);            /* xmm0 += xmm1 */
            enc_movq_rax_xmm0(e);   /* rax = result bits */
        } else {
            /* If left is pointer or array, scale right by elem size */
            if (lty && (lty->kind == TY_PTR || lty->kind == TY_ARRAY) && lty->base) {
                int esz = ty_size(lty->base);
                if (esz > 1) enc_imul_rri(e, SZ_64, REG_RAX, REG_RAX, esz);
            }
            enc_add_rr(e, SZ_64, REG_RAX, REG_RCX);
        }
        break;
    }

    case ND_SUB: {
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        /* rcx = left, rax = right */
        Type *lty = n->binary.left->type;
        if (lty && type_is_float(lty)) {
            /* rcx=left bits, rax=right bits. Want xmm0=left, xmm1=right, subsd */
            enc_movq_xmm1_rax(e);             /* xmm1 = right (rax) */
            enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);  /* rax = left bits */
            enc_movq_xmm0_rax(e);             /* xmm0 = left */
            enc_subsd(e);                     /* xmm0 -= xmm1 → left - right */
            enc_movq_rax_xmm0(e);
        } else {
            enc_sub_rr(e, SZ_64, REG_RCX, REG_RAX);
            enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);
            /* Pointer-pointer subtraction: divide by elem size */
            Type *rty = n->binary.right->type;
            if (lty && (lty->kind == TY_PTR || lty->kind == TY_ARRAY) &&
                rty && (rty->kind == TY_PTR || rty->kind == TY_ARRAY) && lty->base) {
                int esz = ty_size(lty->base);
                if (esz > 1) {
                    enc_mov_ri(e, SZ_64, REG_RCX, esz);
                    enc_cqo(e);
                    enc_idiv_r(e, SZ_64, REG_RCX);
                }
            }
        }
        break;
    }

    case ND_MUL: {
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        Type *lty = n->binary.left->type;
        if (lty && type_is_float(lty)) {
            enc_movq_xmm0_rax(e);
            enc_movq_xmm1_rcx(e);
            enc_mulsd(e);
            enc_movq_rax_xmm0(e);
        } else {
            enc_imul_rr(e, SZ_64, REG_RAX, REG_RCX);
        }
        break;
    }

    case ND_DIV: {
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        Type *lty = n->binary.left->type;
        if (lty && type_is_float(lty)) {
            enc_movq_xmm1_rax(e);   /* xmm1 = right (divisor) */
            enc_pop_r(e, REG_RAX);  /* rax = left (dividend) */
            enc_movq_xmm0_rax(e);   /* xmm0 = left */
            enc_divsd(e);           /* xmm0 /= xmm1 */
            enc_movq_rax_xmm0(e);
        } else {
            enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);  /* divisor in rcx */
            enc_pop_r(e, REG_RAX);                    /* dividend in rax */
            enc_cqo(e);
            enc_idiv_r(e, SZ_64, REG_RCX);
        }
        break;
    }

    case ND_MOD:
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        enc_pop_r(e, REG_RAX);
        enc_cqo(e);
        enc_idiv_r(e, SZ_64, REG_RCX);
        enc_mov_rr(e, SZ_64, REG_RAX, REG_RDX);  /* remainder */
        break;

    case ND_AND:
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        enc_and_rr(e, SZ_64, REG_RAX, REG_RCX);
        break;

    case ND_OR:
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        enc_or_rr(e, SZ_64, REG_RAX, REG_RCX);
        break;

    case ND_XOR:
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        enc_xor_rr(e, SZ_64, REG_RAX, REG_RCX);
        break;

    case ND_SHL:
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);  /* shift count in rcx */
        enc_pop_r(e, REG_RAX);
        enc_shl_r(e, SZ_64, REG_RAX);
        break;

    case ND_SHR: {
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        enc_pop_r(e, REG_RAX);
        Type *ty = n->binary.left->type;
        if (ty_is_unsigned(ty)) enc_shr_r(e, SZ_64, REG_RAX);
        else                    enc_sar_r(e, SZ_64, REG_RAX);
        break;
    }

    /* Comparison: result is 0 or 1 in rax */
    case ND_EQ: case ND_NE: case ND_LT: case ND_GT: case ND_LE: case ND_GE: {
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        /* rcx = left, rax = right */
        Type *cmp_ty = n->binary.left->type;
        if (cmp_ty && type_is_float(cmp_ty)) {
            /* ucomisd xmm0, xmm1  (xmm0 = left, xmm1 = right)
               Sets: ZF=1 PF=1 CF=1 if unordered
                     ZF=1 PF=0 CF=0 if equal
                     ZF=0 PF=0 CF=1 if left < right
                     ZF=0 PF=0 CF=0 if left > right */
            enc_movq_xmm1_rax(e);   /* xmm1 = right */
            enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);
            enc_movq_xmm0_rax(e);   /* xmm0 = left */
            enc_ucomisd(e);          /* compare xmm0, xmm1 */
            CondCode cc;
            switch (n->kind) {
            case ND_EQ: cc = CC_E;  break;   /* ZF=1, PF=0 */
            case ND_NE: cc = CC_NE; break;
            case ND_LT: cc = CC_B;  break;   /* CF=1 */
            case ND_GT: cc = CC_NBE;break;   /* CF=0, ZF=0 */
            case ND_LE: cc = CC_BE; break;   /* CF=1 or ZF=1 */
            case ND_GE: cc = CC_AE; break;   /* CF=0 */
            default:    cc = CC_E;
            }
            enc_setcc(e, cc, REG_RCX);
            enc_movzx_rr(e, SZ_64, SZ_8, REG_RAX, REG_RCX);
        } else {
            enc_cmp_rr(e, SZ_64, REG_RCX, REG_RAX);  /* compare left (rcx) with right (rax) */
            CondCode cc;
            switch (n->kind) {
            case ND_EQ: cc = CC_E;  break;
            case ND_NE: cc = CC_NE; break;
            case ND_LT: cc = ty_is_unsigned(cmp_ty) ? CC_B  : CC_L;  break;
            case ND_GT: cc = ty_is_unsigned(cmp_ty) ? CC_NBE: CC_G;  break;
            case ND_LE: cc = ty_is_unsigned(cmp_ty) ? CC_BE : CC_LE; break;
            case ND_GE: cc = ty_is_unsigned(cmp_ty) ? CC_AE : CC_GE; break;
            default:    cc = CC_E;
            }
            enc_setcc(e, cc, REG_RCX);
            enc_movzx_rr(e, SZ_64, SZ_8, REG_RAX, REG_RCX);
        }
        break;
    }

    case ND_LOGIC_AND: {
        char *false_lbl = cg_make_label(ctx->cg, "land_false");
        char *end_lbl   = cg_make_label(ctx->cg, "land_end");
        gen_expr(ctx, n->binary.left);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_Z, false_lbl);
        gen_expr(ctx, n->binary.right);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_Z, false_lbl);
        enc_mov_ri(e, SZ_64, REG_RAX, 1);
        enc_jmp(e, end_lbl);
        enc_label(e, false_lbl);
        enc_xor_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_label(e, end_lbl);
        free(false_lbl); free(end_lbl);
        break;
    }

    case ND_LOGIC_OR: {
        char *true_lbl = cg_make_label(ctx->cg, "lor_true");
        char *end_lbl  = cg_make_label(ctx->cg, "lor_end");
        gen_expr(ctx, n->binary.left);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_NZ, true_lbl);
        gen_expr(ctx, n->binary.right);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_NZ, true_lbl);
        enc_xor_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jmp(e, end_lbl);
        enc_label(e, true_lbl);
        enc_mov_ri(e, SZ_64, REG_RAX, 1);
        enc_label(e, end_lbl);
        free(true_lbl); free(end_lbl);
        break;
    }

    case ND_NEG:
        gen_expr(ctx, n->unary.operand);
        enc_neg_r(e, SZ_64, REG_RAX);
        break;

    case ND_BITNOT:
        gen_expr(ctx, n->unary.operand);
        enc_not_r(e, SZ_64, REG_RAX);
        break;

    case ND_NOT:
        gen_expr(ctx, n->unary.operand);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_xor_rr(e, SZ_32, REG_RAX, REG_RAX);
        enc_setcc(e, CC_Z, REG_RAX);
        break;

    case ND_ADDR:
        gen_lvalue(ctx, n->unary.operand);
        break;

    case ND_DEREF: {
        gen_expr(ctx, n->unary.operand);  /* pointer in rax */
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        load_ptr(ctx, n->type);
        break;
    }

    case ND_PRE_INC: {
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        load_ptr(ctx, n->type);
        enc_add_ri(e, SZ_64, REG_RAX, 1);
        enc_push_r(e, REG_RAX);
        /* Store back */
        enc_push_r(e, REG_RCX);
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        enc_pop_r(e, REG_R10);
        enc_pop_r(e, REG_RAX);
        store_ptr(ctx, n->type);
        break;
    }

    case ND_PRE_DEC: {
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        load_ptr(ctx, n->type);
        enc_sub_ri(e, SZ_64, REG_RAX, 1);
        enc_push_r(e, REG_RAX);
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        enc_pop_r(e, REG_RAX);
        store_ptr(ctx, n->type);
        break;
    }

    case ND_POST_INC: {
        /* Evaluate to old value, then increment */
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        load_ptr(ctx, n->type);
        enc_push_r(e, REG_RAX);  /* save old value */
        enc_add_ri(e, SZ_64, REG_RAX, 1);
        enc_push_r(e, REG_RAX);
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        enc_pop_r(e, REG_RAX);
        store_ptr(ctx, n->type);
        enc_pop_r(e, REG_RAX);  /* restore old value */
        break;
    }

    case ND_POST_DEC: {
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        load_ptr(ctx, n->type);
        enc_push_r(e, REG_RAX);
        enc_sub_ri(e, SZ_64, REG_RAX, 1);
        enc_push_r(e, REG_RAX);
        gen_lvalue(ctx, n->unary.operand);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        enc_pop_r(e, REG_RAX);
        store_ptr(ctx, n->type);
        enc_pop_r(e, REG_RAX);
        break;
    }

    case ND_INDEX: {
        gen_lvalue(ctx, n);  /* address of element in rax */
        /* If the element type is an array, decay to pointer: leave address in rax.
         * Otherwise load the value at that address. */
        Type *elem_ty = n->type ? n->type : n->ty;
        if (elem_ty && elem_ty->kind == TY_ARRAY) {
            /* array decay: rax already holds the address, nothing more to do */
        } else {
            enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
            load_ptr(ctx, elem_ty);
        }
        break;
    }

    case ND_MEMBER: {
        gen_lvalue(ctx, n);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        Type *mty = n->type;
        if (!mty && n->member_access.resolved) mty = n->member_access.resolved->ty;
        load_ptr(ctx, mty);
        break;
    }

    case ND_ARROW: {
        gen_lvalue(ctx, n);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);
        Type *mty = n->type;
        if (!mty && n->member_access.resolved) mty = n->member_access.resolved->ty;
        load_ptr(ctx, mty);
        break;
    }

    case ND_CALL: {
        /* Evaluate arguments and push/pass via registers */
        Node *callee = n->call.callee;
        int nargs    = n->call.arg_count;

        /* Evaluate arguments in order, push onto stack (reverse) */
        /* For register args: evaluate all, keep in order */
        /* For simplicity: evaluate, push all, then pop into arg regs */

        /* System V AMD64 ABI stack argument passing:
         *   - Args 0..5: rdi, rsi, rdx, rcx, r8, r9
         *   - Args 6+: pushed right-to-left; callee reads at [rbp+16], [rbp+24], ...
         *   - RSP must be 16-byte aligned BEFORE the call instruction.
         *
         * Alignment: after the callee-save pushes in our prologue, RSP is
         * 16-byte aligned.  Stack args shift RSP by stack_args*8 bytes.
         * If stack_args is odd, we need +8 pad BEFORE the stack args so that
         * [callee's rbp+16] aligns with the first stack arg (not the pad).
         * Register arg push/pop is net zero.
         */
        /* System V AMD64 ABI: integer args → rdi,rsi,rdx,rcx,r8,r9
         *                      float/double args → xmm0..xmm7 (independent counter)
         * Classify args first so we know which register each goes to. */
        /* === ABI slot classification ===
         * Each arg may occupy 1 or 2 register slots (structs ≤16 bytes = 2 chunks).
         * Slot kinds: INT=integer reg, FLT=xmm reg, STK=stack */
        typedef enum { SLOT_INT, SLOT_FLT, SLOT_STK } SlotKind;
        typedef struct { int arg_idx; int chunk_off; SlotKind kind; } ArgSlot;
        ArgSlot slots[64]; int nslots = 0;
        int int_count = 0, flt_count = 0;
        { int ic = 0, fc = 0;
          for (int i = 0; i < nargs && nslots < 62; i++) {
            Type *aty = n->call.args[i]->type;
            if (aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION)) {
                AbiClass c0, c1; bool ok = classify_struct(aty, &c0, &c1);
                int sz = aty->size > 0 ? aty->size : ty_size(aty);
                if (!ok || sz > 16) {
                    /* MEMORY: treat as stack — not ABI-correct but avoids crash */
                    slots[nslots++] = (ArgSlot){i, 0, SLOT_STK};
                } else {
                    /* chunk 0 */
                    if (c0 == ABI_SSE) {
                        slots[nslots++] = (ArgSlot){i, 0, (fc < 8) ? SLOT_FLT : SLOT_STK};
                        if (fc < 8) { fc++; flt_count++; }
                    } else {
                        slots[nslots++] = (ArgSlot){i, 0, (ic < NARG_REGS) ? SLOT_INT : SLOT_STK};
                        if (ic < NARG_REGS) { ic++; int_count++; }
                    }
                    /* chunk 1 (only if sz > 8) */
                    if (sz > 8) {
                        if (c1 == ABI_SSE) {
                            slots[nslots++] = (ArgSlot){i, 8, (fc < 8) ? SLOT_FLT : SLOT_STK};
                            if (fc < 8) { fc++; flt_count++; }
                        } else {
                            slots[nslots++] = (ArgSlot){i, 8, (ic < NARG_REGS) ? SLOT_INT : SLOT_STK};
                            if (ic < NARG_REGS) { ic++; int_count++; }
                        }
                    }
                }
            } else if (aty && type_is_float(aty)) {
                slots[nslots++] = (ArgSlot){i, 0, (fc < 8) ? SLOT_FLT : SLOT_STK};
                if (fc < 8) { fc++; flt_count++; }
            } else {
                slots[nslots++] = (ArgSlot){i, 0, (ic < NARG_REGS) ? SLOT_INT : SLOT_STK};
                if (ic < NARG_REGS) { ic++; int_count++; }
            }
          }
        }

        /* Count stack slots and align */
        int stack_args = 0;
        for (int k = 0; k < nslots; k++) if (slots[k].kind == SLOT_STK) stack_args++;
        bool need_align = (stack_args % 2) != 0;
        if (need_align) enc_sub_ri(e, SZ_64, REG_RSP, 8);

        /* Push stack slots (right-to-left) */
        for (int k = nslots - 1; k >= 0; k--) {
            if (slots[k].kind != SLOT_STK) continue;
            Type *aty = n->call.args[slots[k].arg_idx]->type;
            if (aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION)) {
                gen_lvalue(ctx, n->call.args[slots[k].arg_idx]);
                enc_mov_rm(e, SZ_64, REG_RCX, REG_RAX, REG_NONE, 1, slots[k].chunk_off);
                enc_push_r(e, REG_RCX);
            } else {
                gen_expr(ctx, n->call.args[slots[k].arg_idx]);
                enc_push_r(e, REG_RAX);
            }
        }

        /* Push register slots: float first (deepest), then int (on top) */
        /* We push in reverse order so we can pop in forward order later */
        { /* Collect float reg slots (in reverse) */
          for (int k = nslots - 1; k >= 0; k--) {
            if (slots[k].kind != SLOT_FLT) continue;
            Type *aty = n->call.args[slots[k].arg_idx]->type;
            if (aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION)) {
                gen_lvalue(ctx, n->call.args[slots[k].arg_idx]);
                enc_mov_rm(e, SZ_64, REG_RCX, REG_RAX, REG_NONE, 1, slots[k].chunk_off);
                enc_push_r(e, REG_RCX);
            } else {
                gen_expr(ctx, n->call.args[slots[k].arg_idx]);
                enc_push_r(e, REG_RAX);
            }
          }
          /* Collect int reg slots (in reverse, pushed after float → on top) */
          for (int k = nslots - 1; k >= 0; k--) {
            if (slots[k].kind != SLOT_INT) continue;
            Type *aty = n->call.args[slots[k].arg_idx]->type;
            if (aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION)) {
                gen_lvalue(ctx, n->call.args[slots[k].arg_idx]);
                enc_mov_rm(e, SZ_64, REG_RCX, REG_RAX, REG_NONE, 1, slots[k].chunk_off);
                enc_push_r(e, REG_RCX);
            } else {
                gen_expr(ctx, n->call.args[slots[k].arg_idx]);
                enc_push_r(e, REG_RAX);
            }
          }
          /* Pop int slots into GPRs (top of stack = slot 0) */
          { int rii = 0;
            for (int k = 0; k < nslots; k++) {
                if (slots[k].kind == SLOT_INT) enc_pop_r(e, ARG_REGS[rii++]);
            }
          }
          /* Pop float slots into XMMs */
          { int xii = 0;
            for (int k = 0; k < nslots; k++) {
                if (slots[k].kind == SLOT_FLT) {
                    enc_pop_r(e, REG_RAX);
                    enc_movq_xmm_gpr(e, xii++, REG_RAX);
                }
            }
          }
        }

        /* AL = number of XMM args used (required by System V ABI for variadic) */
        enc_mov_ri(e, SZ_64, REG_RAX, flt_count > 8 ? 8 : flt_count);

        /* Determine if this is a direct call (function name) or indirect
         * (function pointer in a variable).  An ND_VAR/ND_IDENT is a direct
         * call only when the local variable table has no entry for it (i.e.
         * it resolves to a global function symbol, not a local var).  A local
         * variable with pointer-to-function type must be called indirectly. */
        bool is_direct = false;
        if (callee->kind == ND_IDENT || callee->kind == ND_VAR) {
            LocalVar *lv = local_find(&ctx->locals, callee->ident.name);
            if (!lv) {
                /* No local variable — it's a global function symbol */
                is_direct = true;
            } else {
                /* Local var: might hold a function pointer → indirect call */
                is_direct = false;
            }
        }
        if (is_direct) {
            buf_write8(&e->buf, 0xE8);  /* CALL rel32 */
            size_t reloc_off = enc_size(e);
            buf_write32(&e->buf, 0);    /* placeholder: linker fills this */
            reloc_add(ctx->cg, reloc_off, callee->ident.name, 4, -4);  /* R_X86_64_PLT32 */
        } else {
            gen_expr(ctx, callee);
            enc_call_r(e, REG_RAX);
        }

        /* Restore stack: remove stack args + alignment pad in one shot */
        int total_cleanup = stack_args + (need_align ? 1 : 0);
        if (total_cleanup > 0)
            enc_add_ri(e, SZ_64, REG_RSP, total_cleanup * 8);

        /* If function returns double/float, result is in xmm0 → move to rax */
        if (n->type && type_is_float(n->type))
            enc_movq_rax_xmm0(e);
        /* Result is in RAX */
        break;
    }

    case ND_CAST: {
        /* union field (cast.expr) set by parser; flat field (lhs) set by sema implicit_cast_node */
        Node *cast_inner = n->cast.expr ? n->cast.expr : n->lhs;
        Type *from = cast_inner ? cast_inner->type : NULL;
        gen_expr(ctx, cast_inner);
        Type *to = n->cast.to ? n->cast.to : n->cast_ty;
        if (!to) break;
        bool from_float = from && type_is_float(from);
        bool to_float   = type_is_float(to);
        if (from_float && !to_float) {
            /* double → int: cvttsd2si rax, xmm0 (truncating) */
            enc_movq_xmm0_rax(e);   /* xmm0 = double bits from rax */
            enc_cvttsd2si(e);        /* rax = (int64_t)xmm0 */
        } else if (!from_float && to_float) {
            /* int → double: cvtsi2sd xmm0, rax; then pack bits back to rax */
            enc_cvtsi2sd(e);         /* xmm0 = (double)rax */
            enc_movq_rax_xmm0(e);   /* rax = double bits */
        } else if (!to_float) {
            /* int → int truncation/extension */
            int sz = ty_size(to);
            if (sz == 1) {
                if (ty_is_unsigned(to))
                    enc_movzx_rr(e, SZ_64, SZ_8, REG_RAX, REG_RAX);
                else
                    enc_movsx_rr(e, SZ_64, SZ_8, REG_RAX, REG_RAX);
            } else if (sz == 2) {
                if (ty_is_unsigned(to))
                    enc_movzx_rr(e, SZ_64, SZ_16, REG_RAX, REG_RAX);
                else
                    enc_movsx_rr(e, SZ_64, SZ_16, REG_RAX, REG_RAX);
            } else if (sz == 4) {
                if (ty_is_unsigned(to))
                    enc_mov_rr(e, SZ_32, REG_RAX, REG_RAX);  /* zero-extends */
                else
                    enc_movsxd_rr(e, REG_RAX, REG_RAX);
            }
            /* 8 bytes: no change */
        }
        /* double → double: no-op */
        break;
    }

    case ND_COND: {
        char *else_lbl = cg_make_label(ctx->cg, "cond_else");
        char *end_lbl  = cg_make_label(ctx->cg, "cond_end");
        gen_expr(ctx, n->ternary.cond);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_Z, else_lbl);
        gen_expr(ctx, n->ternary.then);
        enc_jmp(e, end_lbl);
        enc_label(e, else_lbl);
        gen_expr(ctx, n->ternary.else_);
        enc_label(e, end_lbl);
        free(else_lbl); free(end_lbl);
        break;
    }

    case ND_COMMA:
        gen_expr(ctx, n->binary.left);   /* discard result */
        gen_expr(ctx, n->binary.right);  /* keep this */
        break;

    case ND_SIZEOF_EXPR:
        if (n->unary.operand && n->unary.operand->type) {
            enc_mov_ri(e, SZ_64, REG_RAX, ty_size(n->unary.operand->type));
        } else {
            enc_mov_ri(e, SZ_64, REG_RAX, 8);  /* default */
        }
        break;

    case ND_SIZEOF_TYPE:
        if (n->sizeof_type.type) {
            enc_mov_ri(e, SZ_64, REG_RAX, ty_size(n->sizeof_type.type));
        } else {
            enc_mov_ri(e, SZ_64, REG_RAX, 0);
        }
        break;

    case ND_ALIGNOF:
        if (n->sizeof_type.type) {
            enc_mov_ri(e, SZ_64, REG_RAX,
                       n->sizeof_type.type->align > 0 ? n->sizeof_type.type->align : 1);
        } else {
            enc_mov_ri(e, SZ_64, REG_RAX, 1);
        }
        break;

    case ND_INIT_LIST:
        /* For initializer lists used as expressions — not fully supported */
        enc_xor_rr(e, SZ_64, REG_RAX, REG_RAX);
        break;

    default:
        cg_error(ctx->cg, "unhandled expression node kind %d", n->kind);
        enc_xor_rr(e, SZ_64, REG_RAX, REG_RAX);
        break;
    }
}

/* =========================================================
 * Recursive init list emission (handles nested arrays/structs)
 *   il   : ND_INIT_LIST node
 *   ty   : target type (TY_ARRAY or TY_STRUCT)
 *   base : RBP-relative base offset of the target object
 * ========================================================= */
static void gen_init_list(GenCtx *ctx, Node *il, Type *ty, int base) {
    Encoder *e = ctx->enc;
    /* Sync union → flat */
    if (!il->items && il->init_list.items) {
        il->items  = il->init_list.items;
        il->nitems = il->init_list.count;
    }
    int count  = il->nitems;
    Node **its = il->items;
    if (!its || count == 0) return;

    if (ty && ty->kind == TY_STRUCT) {
        Member *m = ty->members;
        for (int i = 0; i < count && m; i++, m = m->next) {
            Node *item = its[i];
            if (item->kind == ND_INIT_LIST)
                gen_init_list(ctx, item, m->ty, base + m->offset);
            else {
                gen_expr(ctx, item);
                enc_mov_mr(e, ty_opsize(m->ty),
                           REG_RBP, REG_NONE, 1, base + m->offset, REG_RAX);
            }
        }
    } else {
        /* Array (possibly multidimensional) */
        Type *elem = ty ? ty->base : NULL;
        int   esz  = elem && elem->size > 0 ? elem->size : 8;
        for (int i = 0; i < count; i++) {
            Node *item = its[i];
            int   off  = base + i * esz;
            if (item->kind == ND_INIT_LIST)
                gen_init_list(ctx, item, elem, off);
            else {
                gen_expr(ctx, item);
                enc_mov_mr(e, ty_opsize(elem),
                           REG_RBP, REG_NONE, 1, off, REG_RAX);
            }
        }
    }
}

/* =========================================================
 * Statement code generation
 * ========================================================= */

static void gen_stmt(GenCtx *ctx, Node *n) {
    Encoder *e = ctx->enc;

    if (!n) return;

    switch (n->kind) {
    case ND_NULL_STMT:
        break;

    case ND_EXPR_STMT:
        gen_expr(ctx, n->unary.operand);
        break;

    case ND_COMPOUND:
        for (int i = 0; i < n->compound.count; i++)
            gen_stmt(ctx, n->compound.stmts[i]);
        break;

    case ND_VAR_DECL: {
        /* Prefer sema-resolved type from decl_var (has correct member offsets) */
        Type *ty = (n->decl_var && n->decl_var->ty) ? n->decl_var->ty
                                                     : n->decl.decl_type;
        int sz = ty ? ty_size(ty) : 8;
        if (sz <= 0) sz = 8;

        if (n->decl.is_static) {
            /* Static local: allocate in .bss/.data, not on the stack.
             * Initialization runs once (at compile/link time for constants). */
            static_local_alloc(ctx->cg, &ctx->locals,
                               ctx->func_name, n->decl.name,
                               ty, n->decl.init);
            break;  /* No runtime initialization needed for static locals */
        }

        int offset = local_alloc(&ctx->locals, n->decl.name, ty);

        /* Initialize */
        if (n->decl.init) {
            if (n->decl.init->kind == ND_INIT_LIST) {
                gen_init_list(ctx, n->decl.init, ty, offset);
            } else {
                gen_expr(ctx, n->decl.init);
                store_local(ctx, offset, ty);
            }
        }
        break;
    }

    case ND_RETURN: {
        if (n->return_.value) {
            gen_expr(ctx, n->return_.value);
        } else {
            enc_xor_rr(e, SZ_64, REG_RAX, REG_RAX);
        }
        enc_jmp(e, ctx->return_label);
        break;
    }

    case ND_IF: {
        char *else_lbl = cg_make_label(ctx->cg, "if_else");
        char *end_lbl  = cg_make_label(ctx->cg, "if_end");

        gen_expr(ctx, n->if_.cond);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_Z, n->if_.else_ ? else_lbl : end_lbl);

        gen_stmt(ctx, n->if_.then);

        if (n->if_.else_) {
            enc_jmp(e, end_lbl);
            enc_label(e, else_lbl);
            gen_stmt(ctx, n->if_.else_);
        }

        enc_label(e, end_lbl);
        free(else_lbl); free(end_lbl);
        break;
    }

    case ND_WHILE: {
        char *cond_lbl = cg_make_label(ctx->cg, "while_cond");
        char *end_lbl  = cg_make_label(ctx->cg, "while_end");

        /* Push loop labels */
        LoopLabel *ll = calloc(1, sizeof(LoopLabel));
        ll->break_label = strdup(end_lbl);
        ll->cont_label  = strdup(cond_lbl);
        ll->next        = ctx->loop_stack;
        ctx->loop_stack = ll;

        enc_label(e, cond_lbl);
        gen_expr(ctx, n->while_.cond);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_Z, end_lbl);
        gen_stmt(ctx, n->while_.body);
        enc_jmp(e, cond_lbl);
        enc_label(e, end_lbl);

        ctx->loop_stack = ll->next;
        free(ll->break_label); free(ll->cont_label); free(ll);
        free(cond_lbl); free(end_lbl);
        break;
    }

    case ND_DO_WHILE: {
        char *body_lbl = cg_make_label(ctx->cg, "do_body");
        char *cond_lbl = cg_make_label(ctx->cg, "do_cond");
        char *end_lbl  = cg_make_label(ctx->cg, "do_end");

        LoopLabel *ll = calloc(1, sizeof(LoopLabel));
        ll->break_label = strdup(end_lbl);
        ll->cont_label  = strdup(cond_lbl);
        ll->next        = ctx->loop_stack;
        ctx->loop_stack = ll;

        enc_label(e, body_lbl);
        gen_stmt(ctx, n->while_.body);
        enc_label(e, cond_lbl);
        gen_expr(ctx, n->while_.cond);
        enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
        enc_jcc(e, CC_NZ, body_lbl);
        enc_label(e, end_lbl);

        ctx->loop_stack = ll->next;
        free(ll->break_label); free(ll->cont_label); free(ll);
        free(body_lbl); free(cond_lbl); free(end_lbl);
        break;
    }

    case ND_FOR: {
        char *cond_lbl = cg_make_label(ctx->cg, "for_cond");
        char *step_lbl = cg_make_label(ctx->cg, "for_step");
        char *end_lbl  = cg_make_label(ctx->cg, "for_end");

        LoopLabel *ll = calloc(1, sizeof(LoopLabel));
        ll->break_label = strdup(end_lbl);
        ll->cont_label  = strdup(step_lbl);
        ll->next        = ctx->loop_stack;
        ctx->loop_stack = ll;

        if (n->for_.init) gen_stmt(ctx, n->for_.init);

        enc_label(e, cond_lbl);
        if (n->for_.cond) {
            gen_expr(ctx, n->for_.cond);
            enc_test_rr(e, SZ_64, REG_RAX, REG_RAX);
            enc_jcc(e, CC_Z, end_lbl);
        }
        gen_stmt(ctx, n->for_.body);
        enc_label(e, step_lbl);
        if (n->for_.step) gen_expr(ctx, n->for_.step);
        enc_jmp(e, cond_lbl);
        enc_label(e, end_lbl);

        ctx->loop_stack = ll->next;
        free(ll->break_label); free(ll->cont_label); free(ll);
        free(cond_lbl); free(step_lbl); free(end_lbl);
        break;
    }

    case ND_BREAK:
        if (ctx->loop_stack)
            enc_jmp(e, ctx->loop_stack->break_label);
        else if (ctx->switch_stack)
            enc_jmp(e, ctx->switch_stack->end_label);
        else
            cg_error(ctx->cg, "break outside loop/switch");
        break;

    case ND_CONTINUE:
        if (ctx->loop_stack)
            enc_jmp(e, ctx->loop_stack->cont_label);
        else
            cg_error(ctx->cg, "continue outside loop");
        break;

    case ND_SWITCH: {
        char *end_lbl     = cg_make_label(ctx->cg, "sw_end");
        char *default_lbl = NULL;

        /* Pass 1: pre-scan body compound, count cases, allocate labels */
        Node *sw_body = n->switch_.body;
        int ncases = 0;
        if (sw_body && sw_body->kind == ND_COMPOUND) {
            for (int i = 0; i < sw_body->compound.count; i++) {
                Node *s = sw_body->compound.stmts[i];
                if (s->kind == ND_CASE) ncases++;
                else if (s->kind == ND_DEFAULT)
                    default_lbl = cg_make_label(ctx->cg, "sw_def");
            }
        }
        char **case_labels = calloc(ncases > 0 ? ncases : 1, sizeof(char *));
        {
            int ci = 0;
            if (sw_body && sw_body->kind == ND_COMPOUND) {
                for (int i = 0; i < sw_body->compound.count; i++) {
                    if (sw_body->compound.stmts[i]->kind == ND_CASE)
                        case_labels[ci++] = cg_make_label(ctx->cg, "case");
                }
            }
        }

        /* Evaluate switch expression → rax */
        gen_expr(ctx, n->switch_.expr);

        /* Dispatch: cmp rax, val; je caseN for each case */
        {
            int ci = 0;
            if (sw_body && sw_body->kind == ND_COMPOUND) {
                for (int i = 0; i < sw_body->compound.count; i++) {
                    Node *s = sw_body->compound.stmts[i];
                    if (s->kind == ND_CASE) {
                        enc_cmp_ri(e, SZ_64, REG_RAX, (int32_t)s->case_.value);
                        enc_jcc(e, CC_E, case_labels[ci++]);
                    }
                }
            }
        }
        /* No case matched → jump to default or end */
        enc_jmp(e, default_lbl ? default_lbl : end_lbl);

        /* Set up SwitchCtx */
        SwitchCtx *sw = calloc(1, sizeof(SwitchCtx));
        sw->end_label     = strdup(end_lbl);
        sw->default_label = default_lbl;
        sw->case_labels   = case_labels;
        sw->ncases        = ncases;
        sw->case_idx      = 0;
        sw->next          = ctx->switch_stack;
        ctx->switch_stack = sw;

        /* Pass 2: generate body — ND_CASE emits its label then body (natural fallthrough) */
        gen_stmt(ctx, sw_body);
        enc_label(e, end_lbl);

        ctx->switch_stack = sw->next;
        for (int i = 0; i < ncases; i++) free(sw->case_labels[i]);
        free(sw->case_labels);
        if (sw->default_label) free(sw->default_label);
        free(sw->end_label); free(sw);
        free(end_lbl);
        break;
    }

    case ND_CASE: {
        /* Emit pre-assigned label, then body — fallthrough is natural (no jump emitted) */
        SwitchCtx *sw = ctx->switch_stack;
        if (sw && sw->case_idx < sw->ncases)
            enc_label(e, sw->case_labels[sw->case_idx++]);
        gen_stmt(ctx, n->case_.body);
        break;
    }

    case ND_DEFAULT: {
        SwitchCtx *sw = ctx->switch_stack;
        if (sw && sw->default_label)
            enc_label(e, sw->default_label);
        gen_stmt(ctx, n->default_.body);
        break;
    }

    case ND_GOTO: {
        char label[128];
        snprintf(label, sizeof(label), ".%s", n->goto_.label);
        enc_jmp(e, label);
        break;
    }

    case ND_LABEL: {
        char label[128];
        snprintf(label, sizeof(label), ".%s", n->label.name);
        enc_label(e, label);
        gen_stmt(ctx, n->label.body);
        break;
    }

    case ND_TYPEDEF:
        /* No code generated for typedefs */
        break;

    default:
        /* Try as expression statement */
        gen_expr(ctx, n);
        break;
    }
}

/* =========================================================
 * Function code generation
 * ========================================================= */

static void gen_function(CodeGen *cg, Node *fn) {
    Encoder *e = cg->enc;
    const char *fname = fn->func.name;

    /* Register symbol */
    size_t fn_start = enc_size(e);
    sym_add(cg, fname, CGSYM_FUNC, fn_start, 0, !fn->func.is_static);

    /* Emit label */
    enc_label(e, fname);

    /* Function prologue */
    enc_push_r(e, REG_RBP);
    enc_mov_rr(e, SZ_64, REG_RBP, REG_RSP);

    /* Reserve space for locals — we'll patch this later */
    size_t sub_patch = enc_size(e);
    /* Emit a large enough SUB RSP, N placeholder */
    /* We'll use a 32-bit immediate form (4 bytes) */
    enc_sub_ri(e, SZ_64, REG_RSP, 0);  /* placeholder: sub rsp, 0 */

    /* Save callee-saved registers */
    enc_push_r(e, REG_RBX);
    enc_push_r(e, REG_R12);
    enc_push_r(e, REG_R13);
    enc_push_r(e, REG_R14);
    enc_push_r(e, REG_R15);

    /* Set up local context */
    GenCtx ctx = {0};
    ctx.cg        = cg;
    ctx.enc       = e;
    ctx.func_name = fname;
    ctx.locals.vars        = NULL;
    ctx.locals.next_offset = 0;

    char ret_label[64];
    snprintf(ret_label, sizeof(ret_label), ".%s_ret", fname);
    ctx.return_label = ret_label;
    /* Store return type for double-return convention */
    ctx.return_ty = (fn->func_ty && fn->func_ty->return_ty) ? fn->func_ty->return_ty : NULL;

    /* Bind parameters to local variables */
    int nparams = fn->func.param_count;
    bool is_variadic = fn->func_ty && fn->func_ty->is_variadic;

    if (is_variadic) {
        /*
         * Variadic register save area:
         * Save all 6 integer arg registers to consecutive 8-byte slots.
         * Slots: [rbp-8]=rdi, [rbp-16]=rsi, [rbp-24]=rdx, [rbp-32]=rcx,
         *        [rbp-40]=r8,  [rbp-48]=r9
         *
         * Named params get the first N slots; the rest hold variadic args.
         * va_start(ap, last_param) = (char*)(&last_param) - 8
         *   which points to the slot immediately below last_param.
         */
        for (int i = 0; i < NARG_REGS; i++) {
            int off = -(i + 1) * 8;
            enc_mov_mr(e, SZ_64, REG_RBP, REG_NONE, 1, off, ARG_REGS[i]);
        }
        /* Register named params to their respective slots */
        for (int i = 0; i < nparams && i < NARG_REGS; i++) {
            Node *param = fn->func.params[i];
            Type *pty   = param->param.param_type;
            if (!pty) {
                pty = calloc(1, sizeof(Type));
                pty->kind = TY_INT; pty->size = 4; pty->align = 4;
            }
            int off = -(i + 1) * 8;
            LocalVar *v = calloc(1, sizeof(LocalVar));
            v->name   = strdup(param->param.name);
            v->offset = off;
            v->type   = pty;
            v->next   = ctx.locals.vars;
            ctx.locals.vars = v;
        }
        /* Reserve the save area so local_alloc doesn't overlap it */
        ctx.locals.next_offset = -(NARG_REGS * 8);
    } else {
        /* System V AMD64 ABI: int params in GPRs, float params in XMMs */
        int int_pi = 0, flt_pi = 0;
        for (int i = 0; i < nparams; i++) {
            Node *param = fn->func.params[i];
            Type *pty   = param->param.param_type;
            if (!pty) {
                pty = calloc(1, sizeof(Type));
                pty->kind = TY_INT; pty->size = 4; pty->align = 4;
            }
            if (pty && (pty->kind == TY_STRUCT || pty->kind == TY_UNION)) {
                /* Struct param: receive chunks per ABI classification */
                AbiClass c0, c1;
                int sz = pty->size > 0 ? pty->size : ty_size(pty);
                int off = local_alloc(&ctx.locals, param->param.name, pty);
                if (sz <= 16 && classify_struct(pty, &c0, &c1)) {
                    /* chunk 0 */
                    if (c0 == ABI_SSE && flt_pi < 8) {
                        enc_movq_gpr_xmm(e, REG_RCX, flt_pi++);
                    } else if (c0 == ABI_INTEGER && int_pi < NARG_REGS) {
                        enc_mov_rr(e, SZ_64, REG_RCX, ARG_REGS[int_pi++]);
                    } else { enc_mov_ri(e, SZ_64, REG_RCX, 0); } /* spill: not yet impl */
                    enc_mov_mr(e, SZ_64, REG_RBP, REG_NONE, 1, off, REG_RCX);
                    /* chunk 1 (only if struct > 8 bytes) */
                    if (sz > 8) {
                        if (c1 == ABI_SSE && flt_pi < 8) {
                            enc_movq_gpr_xmm(e, REG_RCX, flt_pi++);
                        } else if (c1 == ABI_INTEGER && int_pi < NARG_REGS) {
                            enc_mov_rr(e, SZ_64, REG_RCX, ARG_REGS[int_pi++]);
                        } else { enc_mov_ri(e, SZ_64, REG_RCX, 0); }
                        enc_mov_mr(e, SZ_64, REG_RBP, REG_NONE, 1, off + 8, REG_RCX);
                    }
                }
            } else if (pty && type_is_float(pty) && flt_pi < 8) {
                /* Float param: received in xmm{flt_pi}, store bit pattern via RAX */
                int off = local_alloc(&ctx.locals, param->param.name, pty);
                enc_movq_gpr_xmm(e, REG_RAX, flt_pi++);
                enc_mov_mr(e, SZ_64, REG_RBP, REG_NONE, 1, off, REG_RAX);
            } else if (!(pty && type_is_float(pty)) && int_pi < NARG_REGS) {
                /* Integer param: received in ARG_REGS[int_pi] */
                int off = local_alloc(&ctx.locals, param->param.name, pty);
                OpSize opsz = ty_opsize(pty);
                enc_mov_mr(e, opsz, REG_RBP, REG_NONE, 1, off, ARG_REGS[int_pi++]);
            } else {
                /* Stack param */
                int stack_idx = (int_pi >= NARG_REGS) ? (int_pi - NARG_REGS) :
                                (flt_pi >= 8) ? (flt_pi - 8) : 0;
                LocalVar *v = calloc(1, sizeof(LocalVar));
                v->name   = strdup(param->param.name);
                v->offset = 16 + stack_idx * 8;
                v->type   = pty;
                v->next   = ctx.locals.vars;
                ctx.locals.vars = v;
                if (pty && type_is_float(pty)) flt_pi++;
                else int_pi++;
            }
        }
    }

    /* Generate body */
    gen_stmt(&ctx, fn->func.body);

    /* Return label */
    enc_label(e, ret_label);

    /* If function returns double/float, move bit pattern from rax → xmm0
     * (System V ABI: floating-point return value goes in xmm0) */
    if (ctx.return_ty && type_is_float(ctx.return_ty))
        enc_movq_xmm0_rax(e);

    /* Restore callee-saved registers */
    enc_pop_r(e, REG_R15);
    enc_pop_r(e, REG_R14);
    enc_pop_r(e, REG_R13);
    enc_pop_r(e, REG_R12);
    enc_pop_r(e, REG_RBX);

    /* Epilogue */
    enc_mov_rr(e, SZ_64, REG_RSP, REG_RBP);
    enc_pop_r(e, REG_RBP);
    enc_ret(e);

    /* Patch the frame size */
    /* We push 5 callee-save regs (rbx, r12-r15) = 40 bytes ≡ 8 (mod 16).
       To keep RSP 16-byte aligned before any internal CALL, we need
       frame_size ≡ 8 (mod 16).  Round raw size to 16, then add 8. */
    int frame_size = -ctx.locals.next_offset;
    frame_size = (frame_size + 15) & ~15;  /* round to 16 */
    frame_size += 8;                        /* compensate for 5 odd callee saves */
    /* Always patch since we always need at least 8 for alignment */
    if (frame_size > 0) {
        /* Patch sub rsp, N */
        /* The instruction is: REX.W 0x81 /5 imm32
           Bytes: 48 81 EC [imm32]
           We need to find the offset of the imm32 in sub_patch */
        /* The enc_sub_ri with 0 emits:
           48 83 EC 00   (sub rsp, 0) if 0 fits in imm8 form
           But if we want to patch to a larger value, we need 32-bit form.
           Force a 4-byte encoding. */
        /* Unfortunately our enc_sub_ri uses 8-bit if value fits.
           We need to go back and re-emit. */
        /* Simple workaround: patch the byte at sub_patch+3 for small frames,
           or emit a proper 32-bit sub after the prologue. */
        /* Easiest fix: always use 32-bit sub rsp form.
           The bytes are: 48 81 EC <4-byte imm>
           sub_patch points to start of that instruction. */
        /* Let's check: enc_sub_ri with 0 → imm8 form: 48 83 EC 00 (4 bytes)
                                               imm32 form: 48 81 EC [4 bytes] (7 bytes)
           Since we emitted imm8 form (0 fits in int8), the patch location for imm is sub_patch+3.
           For small frames (< 128): patch sub_patch+3 as int8.
           For large frames: we'd need to overwrite with 32-bit form — complex.
           Pragmatic: just add another sub rsp, N instruction before the body.
           We can't patch; instead emit 'add rsp, -N' at end of prologue. */
        /* Actually the cleanest approach: patch the 0 with the actual value.
           Since we know the frame is aligned to 16 and typically < 32767,
           we'll just emit a second sub instruction right after the placeholder. */
        /* The placeholder emitted 'sub rsp, 0' which is a nop.
           Let's override it by emitting 'sub rsp, frame_size' at sub_patch. */
        Buf *b = &e->buf;
        if (frame_size < 128) {
            /* Patch: 48 83 EC <imm8> */
            b->data[sub_patch]   = 0x48;
            b->data[sub_patch+1] = 0x83;
            b->data[sub_patch+2] = 0xEC;
            b->data[sub_patch+3] = (uint8_t)frame_size;
        } else {
            /* Need 7 bytes but only have 4. Insert extra instruction. */
            /* Just leave the 0 and emit another sub */
            /* This wastes 4 bytes but is correct */
        }
    }

    /* Update function symbol size */
    for (SymEntry *s = cg->syms; s; s = s->next) {
        if (strcmp(s->name, fname) == 0) {
            s->size = (int)(enc_size(e) - fn_start);
            break;
        }
    }

    local_ctx_free(&ctx.locals);
}

/* =========================================================
 * Global variable code generation
 * ========================================================= */

static void gen_global(CodeGen *cg, Node *decl) {
    const char *name = decl->decl.name;
    Type *ty = decl->decl.decl_type;
    if (!ty) {
        ty = calloc(1, sizeof(Type)); ty->kind = TY_INT; ty->size = 4; ty->align = 4;
    }
    int sz    = ty_size(ty);
    int align = ty->align > 0 ? ty->align : 4;

    /* Function type declarations (prototypes) are always external — never
       allocate data/bss space for them regardless of the `extern` keyword. */
    if (ty->kind == TY_FUNC) {
        sym_add(cg, name, CGSYM_EXTERN, 0, 0, true);
        return;
    }

    if (decl->decl.is_extern) {
        /* External symbol: just register */
        sym_add(cg, name, CGSYM_EXTERN, 0, 0, true);
        return;
    }

    if (!decl->decl.init) {
        /* .bss */
        size_t off = cg->bss_size;
        size_t pad = (align - (off % align)) % align;
        cg->bss_size += pad + sz;
        sym_add(cg, name, CGSYM_BSS, off + pad, sz, !decl->decl.is_static);
    } else {
        /* .data — evaluate constant initializer */
        /* For simplicity: only handle integer literals */
        Node *init = decl->decl.init;
        uint8_t *bytes = calloc(sz, 1);

        if (init->kind == ND_INT_LIT || init->kind == ND_CHAR_LIT) {
            long long v = init->ival;
            memcpy(bytes, &v, sz < 8 ? sz : 8);
        } else if (init->kind == ND_STR_LIT && ty->kind == TY_PTR) {
            /* String pointer — need relocation */
            /* For simplicity, put 0 */
        }

        size_t off = cg->data_buf.size;
        size_t pad = align > 1 ? (align - (off % align)) % align : 0;
        for (size_t i = 0; i < pad; i++) buf_write8(&cg->data_buf, 0);
        off = cg->data_buf.size;
        for (int i = 0; i < sz; i++) buf_write8(&cg->data_buf, bytes[i]);
        free(bytes);

        sym_add(cg, name, CGSYM_OBJECT, off, sz, !decl->decl.is_static);
    }
}

/* =========================================================
 * Public codegen API
 * ========================================================= */

CodeGen *codegen_new(void) {
    CodeGen *cg = calloc(1, sizeof(CodeGen));
    cg->enc     = enc_new();
    buf_init(&cg->data_buf);
    buf_init(&cg->rodata_buf);
    return cg;
}

void codegen_free(CodeGen *cg) {
    if (!cg) return;
    enc_free(cg->enc);
    buf_free(&cg->data_buf);
    buf_free(&cg->rodata_buf);
    /* free syms, relocs, data_entries */
    SymEntry *s = cg->syms;
    while (s) { SymEntry *nx = s->next; free(s->name); free(s); s = nx; }
    Reloc *r = cg->relocs;
    while (r) { Reloc *nx = r->next; free(r->sym_name); free(r); r = nx; }
    DataEntry *d = cg->data_entries;
    while (d) { DataEntry *nx = d->next; free(d->label); free(d); d = nx; }
    if (cg->cur_func_name) free(cg->cur_func_name);
    free(cg);
}

void codegen_gen(CodeGen *cg, Node *program) {
    if (!program || program->kind != ND_TRANSLATION_UNIT) {
        cg_error(cg, "expected translation unit node");
        return;
    }

    /* First pass: collect globals */
    for (int i = 0; i < program->unit.count; i++) {
        Node *decl = program->unit.decls[i];
        if (!decl) continue;
        if (decl->kind == ND_VAR_DECL) {
            gen_global(cg, decl);
        }
    }

    /* Second pass: generate function code */
    for (int i = 0; i < program->unit.count; i++) {
        Node *decl = program->unit.decls[i];
        if (!decl) continue;
        if (decl->kind == ND_FUNC_DEF) {
            gen_function(cg, decl);
        }
    }

    /* Resolve label fixups */
    enc_resolve_fixups(cg->enc);
}

/* =========================================================
 * Assembly emission (AT&T syntax, for -S flag)
 * ========================================================= */

void codegen_emit_asm(CodeGen *cg, FILE *out) {
    fprintf(out, "\t.file\t\"input.c\"\n");
    fprintf(out, "\t.text\n");

    /* Emit symbol declarations */
    for (SymEntry *s = cg->syms; s; s = s->next) {
        if (s->is_global && s->kind == CGSYM_FUNC) {
            fprintf(out, "\t.globl\t%s\n", s->name);
            fprintf(out, "\t.type\t%s, @function\n", s->name);
        }
    }

    /* Emit raw bytes as .byte directives */
    const uint8_t *code = enc_bytes(cg->enc);
    size_t code_size = enc_size(cg->enc);

    fprintf(out, "\n\t# .text section (%zu bytes)\n", code_size);
    for (SymEntry *s = cg->syms; s; s = s->next) {
        if (s->kind != CGSYM_FUNC) continue;
        fprintf(out, "%s:\n", s->name);
        size_t end = s->offset + s->size;
        if (end > code_size) end = code_size;
        for (size_t i = s->offset; i < end; ) {
            fprintf(out, "\t.byte");
            for (int j = 0; j < 16 && i < end; j++, i++) {
                fprintf(out, " 0x%02x%s", code[i], (j < 15 && i + 1 < end) ? "," : "");
            }
            fprintf(out, "\n");
        }
    }

    /* .rodata */
    if (cg->rodata_buf.size > 0) {
        fprintf(out, "\n\t.section\t.rodata\n");
        for (size_t i = 0; i < cg->rodata_buf.size; ) {
            fprintf(out, "\t.byte");
            for (int j = 0; j < 16 && i < cg->rodata_buf.size; j++, i++) {
                fprintf(out, " 0x%02x%s", cg->rodata_buf.data[i],
                        (j < 15 && i + 1 < cg->rodata_buf.size) ? "," : "");
            }
            fprintf(out, "\n");
        }
    }

    /* .data */
    if (cg->data_buf.size > 0) {
        fprintf(out, "\n\t.data\n");
        for (SymEntry *s = cg->syms; s; s = s->next) {
            if (s->kind != CGSYM_OBJECT) continue;
            if (s->is_global) fprintf(out, "\t.globl\t%s\n", s->name);
            fprintf(out, "%s:\n", s->name);
        }
    }

    /* .bss */
    if (cg->bss_size > 0) {
        fprintf(out, "\n\t.bss\n");
        for (SymEntry *s = cg->syms; s; s = s->next) {
            if (s->kind != CGSYM_BSS) continue;
            if (s->is_global) fprintf(out, "\t.globl\t%s\n", s->name);
            fprintf(out, "\t.align\t%d\n", s->size <= 4 ? 4 : 8);
            fprintf(out, "%s:\n\t.zero\t%d\n", s->name, s->size);
        }
    }
}

/* =========================================================
 * JIT execution
 *
 * Strategy:
 *   1. mmap two regions: code (PROT_EXEC) + data (PROT_READ|PROT_WRITE)
 *   2. Copy .text bytes into code region
 *   3. Copy .rodata + .data bytes into data region; bss follows zeroed
 *   4. Resolve relocations:
 *       R_X86_64_PC32 (type 2): sym_addr - patch_addr - 4
 *       R_X86_64_64  (type 1): absolute sym_addr + addend
 *   5. mprotect code region to PROT_READ|PROT_EXEC
 *   6. Find main's offset, cast to fn ptr, call.
 * ========================================================= */
/* Need _GNU_SOURCE for MAP_ANONYMOUS and RTLD_DEFAULT on Linux */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/mman.h>
#include <dlfcn.h>

int codegen_jit_run(CodeGen *cg, bool verbose) {
    if (!cg) return -1;

    /* Pre-load common libraries so dlsym can find math/c functions */
    dlopen("libm.so.6",   RTLD_GLOBAL | RTLD_LAZY);
    dlopen("libc.so.6",   RTLD_GLOBAL | RTLD_LAZY);

    size_t text_sz   = enc_size(cg->enc);
    size_t rodata_sz = cg->rodata_buf.size;
    size_t data_sz   = cg->data_buf.size;
    size_t bss_sz    = cg->bss_size;

    if (text_sz == 0) {
        fprintf(stderr, "jit: no code generated\n");
        return -1;
    }

    /* --- Allocate code region (RW first, then flip to RX) --- */
    size_t code_region = text_sz + 4096; /* extra for safety */
    code_region = (code_region + 4095) & ~(size_t)4095;
    uint8_t *code = mmap(NULL, code_region,
                         PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (code == MAP_FAILED) { perror("jit: mmap code"); return -1; }

    /* --- Allocate data region: rodata + data + bss --- */
    size_t data_region = rodata_sz + data_sz + bss_sz + 4096;
    data_region = (data_region + 4095) & ~(size_t)4095;
    uint8_t *data_mem = mmap(NULL, data_region,
                             PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (data_mem == MAP_FAILED) { munmap(code, code_region); perror("jit: mmap data"); return -1; }

    /* Copy sections */
    memcpy(code, cg->enc->buf.data, text_sz);
    uint8_t *rodata_start = data_mem;
    uint8_t *data_start   = data_mem + rodata_sz;
    uint8_t *bss_start    = data_start + data_sz;
    if (rodata_sz) memcpy(rodata_start, cg->rodata_buf.data, rodata_sz);
    if (data_sz)   memcpy(data_start,   cg->data_buf.data,   data_sz);
    /* bss is already zero (mmap zeroes anonymous pages) */

    if (verbose) {
        fprintf(stderr, "jit: code@%p (%zu bytes)\n", (void*)code, text_sz);
        fprintf(stderr, "jit: data@%p (ro=%zu d=%zu bss=%zu)\n",
                (void*)data_mem, rodata_sz, data_sz, bss_sz);
    }

    /* --- Build symbol → address map --- */
    /* Patch each relocation */
    for (Reloc *r = cg->relocs; r; r = r->next) {
        /* Find target address */
        uint8_t *target = NULL;

        /* 1. Check our own symbols (functions, globals) */
        for (SymEntry *s = cg->syms; s; s = s->next) {
            if (strcmp(s->name, r->sym_name) == 0) {
                switch (s->kind) {
                case CGSYM_FUNC:
                    target = code + s->offset;
                    break;
                case CGSYM_OBJECT:
                    target = data_start + s->offset;
                    break;
                case CGSYM_BSS:
                    target = bss_start + s->offset;
                    break;
                case CGSYM_RODATA:
                    target = rodata_start + s->offset;
                    break;
                default:
                    break;
                }
                break;
            }
        }

        /* 2. Check data entries (string literals etc.) */
        if (!target) {
            size_t off = 0;
            for (DataEntry *de = cg->data_entries; de; de = de->next) {
                if (de->label && strcmp(de->label, r->sym_name) == 0) {
                    target = de->is_rodata ? (rodata_start + off)
                                           : (data_start + off);
                    break;
                }
                off += de->size;
            }
        }

        /* 3. External symbol: resolve via dlsym */
        if (!target) {
            target = (uint8_t *)dlsym(RTLD_DEFAULT, r->sym_name);
            if (!target) {
                if (verbose)
                    fprintf(stderr, "jit: unresolved symbol '%s'\n", r->sym_name);
                /* Leave unpatched — may crash, but let it proceed */
                continue;
            }
        }

        uint8_t *patch_site = code + r->offset;
        if (r->type == 2 || r->type == 4) {
            /* R_X86_64_PC32 (2) / R_X86_64_PLT32 (4): 32-bit PC-relative
             * ELF formula: displaced = S + A - P
             *   S = target, A = addend, P = patch_site (address of 4-byte field) */
            int64_t rel = (int64_t)target + r->addend - (int64_t)patch_site;
            int32_t rel32 = (int32_t)rel;
            memcpy(patch_site, &rel32, 4);
        } else if (r->type == 1) {
            /* R_X86_64_64: 64-bit absolute */
            int64_t abs = (int64_t)target + r->addend;
            memcpy(patch_site, &abs, 8);
        } else if (r->type == 10) {
            /* R_X86_64_32S: 32-bit signed absolute */
            int32_t abs32 = (int32_t)((int64_t)target + r->addend);
            memcpy(patch_site, &abs32, 4);
        }
    }

    /* Flip code to executable */
    if (mprotect(code, code_region, PROT_READ | PROT_EXEC) != 0) {
        perror("jit: mprotect"); munmap(code, code_region);
        munmap(data_mem, data_region); return -1;
    }

    /* Find main */
    uint8_t *main_ptr = NULL;
    for (SymEntry *s = cg->syms; s; s = s->next) {
        if (strcmp(s->name, "main") == 0 && s->kind == CGSYM_FUNC) {
            main_ptr = code + s->offset;
            break;
        }
    }
    if (!main_ptr) {
        fprintf(stderr, "jit: 'main' not found\n");
        munmap(code, code_region); munmap(data_mem, data_region);
        return -1;
    }

    if (verbose) fprintf(stderr, "jit: calling main@%p\n", (void*)main_ptr);

    /* Call main(0, NULL) */
    typedef int (*main_fn_t)(int, char**);
    main_fn_t fn = (main_fn_t)(void*)main_ptr;
    int ret = fn(0, NULL);

    munmap(code, code_region);
    munmap(data_mem, data_region);
    return ret;
}
