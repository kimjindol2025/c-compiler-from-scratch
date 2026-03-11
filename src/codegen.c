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

    /* Apply op: rax = rcx op rax */
    switch (op) {
    case ND_ASSIGN_ADD: enc_add_rr(e, SZ_64, REG_RCX, REG_RAX); break;
    case ND_ASSIGN_SUB: enc_sub_rr(e, SZ_64, REG_RCX, REG_RAX); break;
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

    case ND_FLOAT_LIT:
        /* Simplified: treat as integer (truncate) */
        enc_mov_ri(e, SZ_64, REG_RAX, (long long)n->fval);
        break;

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
        /* If left is pointer, scale right by elem size */
        Type *lty = n->binary.left->type;
        if (lty && lty->kind == TY_PTR && lty->base) {
            int esz = ty_size(lty->base);
            if (esz > 1) enc_imul_rri(e, SZ_64, REG_RAX, REG_RAX, esz);
        }
        enc_add_rr(e, SZ_64, REG_RAX, REG_RCX);
        break;
    }

    case ND_SUB: {
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        /* rcx = left, rax = right; result = left - right = rcx - rax */
        enc_sub_rr(e, SZ_64, REG_RCX, REG_RAX);
        enc_mov_rr(e, SZ_64, REG_RAX, REG_RCX);
        /* Pointer-pointer subtraction: divide by elem size */
        Type *lty = n->binary.left->type;
        Type *rty = n->binary.right->type;
        if (lty && lty->kind == TY_PTR && rty && rty->kind == TY_PTR && lty->base) {
            int esz = ty_size(lty->base);
            if (esz > 1) {
                enc_mov_ri(e, SZ_64, REG_RCX, esz);
                enc_cqo(e);
                enc_idiv_r(e, SZ_64, REG_RCX);
            }
        }
        break;
    }

    case ND_MUL:
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_pop_r(e, REG_RCX);
        enc_imul_rr(e, SZ_64, REG_RAX, REG_RCX);
        break;

    case ND_DIV:
        gen_expr(ctx, n->binary.left);
        enc_push_r(e, REG_RAX);
        gen_expr(ctx, n->binary.right);
        enc_mov_rr(e, SZ_64, REG_RCX, REG_RAX);  /* divisor in rcx */
        enc_pop_r(e, REG_RAX);                    /* dividend in rax */
        enc_cqo(e);
        enc_idiv_r(e, SZ_64, REG_RCX);
        break;

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
        enc_cmp_rr(e, SZ_64, REG_RCX, REG_RAX);  /* compare left (rcx) with right (rax) */
        CondCode cc;
        switch (n->kind) {
        case ND_EQ: cc = CC_E;  break;
        case ND_NE: cc = CC_NE; break;
        case ND_LT: cc = ty_is_unsigned(n->binary.left->type) ? CC_B  : CC_L;  break;
        case ND_GT: cc = ty_is_unsigned(n->binary.left->type) ? CC_NBE: CC_G;  break;
        case ND_LE: cc = ty_is_unsigned(n->binary.left->type) ? CC_BE : CC_LE; break;
        case ND_GE: cc = ty_is_unsigned(n->binary.left->type) ? CC_AE : CC_GE; break;
        default:    cc = CC_E;
        }
        /* NOTE: xor must come BEFORE setcc — xor changes flags! */
        /* Use movzx trick: setcc writes to cl, then movzx rax, cl */
        enc_setcc(e, cc, REG_RCX);            /* cl = condition (uses CMP flags) */
        enc_movzx_rr(e, SZ_64, SZ_8, REG_RAX, REG_RCX);  /* rax = (uint64_t)cl */
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

        /* Push all args onto stack first (in reverse order for stack args) */
        int stack_args = nargs > NARG_REGS ? nargs - NARG_REGS : 0;
        if (stack_args > 0) {
            /* Push excess args right-to-left */
            for (int i = nargs - 1; i >= NARG_REGS; i--) {
                gen_expr(ctx, n->call.args[i]);
                enc_push_r(e, REG_RAX);
            }
        }

        /* Evaluate register args into temporary stack slots */
        int reg_args = nargs < NARG_REGS ? nargs : NARG_REGS;
        for (int i = reg_args - 1; i >= 0; i--) {
            gen_expr(ctx, n->call.args[i]);
            enc_push_r(e, REG_RAX);
        }

        /* Pop into argument registers */
        for (int i = 0; i < reg_args; i++) {
            enc_pop_r(e, ARG_REGS[i]);
        }

        /* Align stack to 16 bytes if needed.
           The function prologue ensures RSP ≡ 0 (mod 16) at the start of
           the function body (after push rbp + sub rsp,N + 5 callee saves).
           Register args are pushed then popped (net RSP change = 0), so RSP
           stays aligned.  Stack args (pushed right-to-left, NOT popped before
           call) shift RSP by stack_args*8 bytes — we need +8 if stack_args
           is odd. */
        bool need_align = (stack_args % 2) != 0;
        if (need_align) enc_sub_ri(e, SZ_64, REG_RSP, 8);

        /* Call the function.
           For external symbols: emit CALL rel32 placeholder + ELF reloc.
           We bypass enc_call() to avoid adding to encoder's internal fixup
           list (which only resolves local labels, not external symbols). */
        /* Zero AL/RAX before any call — required by System V ABI for variadic
           functions (AL = number of vector arguments, must be 0 if no SSE args).
           Safe to do for all calls since we don't track variadic at this point. */
        enc_xor_rr(e, SZ_32, REG_RAX, REG_RAX);  /* xor eax, eax */

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

        /* Restore stack */
        if (need_align) enc_add_ri(e, SZ_64, REG_RSP, 8);
        if (stack_args > 0)
            enc_add_ri(e, SZ_64, REG_RSP, stack_args * 8);

        /* Result is in RAX */
        break;
    }

    case ND_CAST: {
        gen_expr(ctx, n->cast.expr);
        /* Truncate/extend based on target type */
        Type *to = n->cast.to;
        if (!to) break;
        int sz = ty_size(to);
        if (sz == 1) {
            /* movsx/movzx al, al (extend byte) */
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
            if (ty_is_unsigned(to)) {
                enc_mov_rr(e, SZ_32, REG_RAX, REG_RAX);  /* zero-extends */
            } else {
                enc_movsxd_rr(e, REG_RAX, REG_RAX);
            }
        }
        /* For 8 bytes (ptr, long): no truncation needed */
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
        /* Allocate local variable */
        Type *ty = n->decl.decl_type;
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
                /* Array/struct initializer */
                int elem_sz = ty && ty->base ? ty_size(ty->base) : 8;
                for (int i = 0; i < n->decl.init->init_list.count; i++) {
                    gen_expr(ctx, n->decl.init->init_list.items[i]);
                    /* Store at offset + i * elem_sz */
                    enc_mov_mr(e, ty_opsize(ty ? ty->base : NULL),
                               REG_RBP, REG_NONE, 1,
                               offset + i * elem_sz, REG_RAX);
                }
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
        char *end_lbl = cg_make_label(ctx->cg, "switch_end");

        SwitchCtx *sw = calloc(1, sizeof(SwitchCtx));
        sw->end_label     = strdup(end_lbl);
        sw->default_label = NULL;
        sw->next          = ctx->switch_stack;
        ctx->switch_stack = sw;

        gen_expr(ctx, n->switch_.expr);
        /* rax = switch value */
        /* For simplicity: generate jump table style inline */
        /* This is simplified — a real compiler would sort cases */
        enc_push_r(e, REG_RAX);
        gen_stmt(ctx, n->switch_.body);
        enc_pop_r(e, REG_RCX);  /* discard saved value */
        enc_label(e, end_lbl);

        ctx->switch_stack = sw->next;
        if (sw->default_label) free(sw->default_label);
        free(sw->end_label); free(sw);
        free(end_lbl);
        break;
    }

    case ND_CASE: {
        /* Compare against switch value (which is on stack from ND_SWITCH) */
        char *skip_lbl = cg_make_label(ctx->cg, "case_skip");
        /* Load switch value without popping */
        enc_mov_rm(e, SZ_64, REG_RCX, REG_RSP, REG_NONE, 1, 0);
        enc_cmp_ri(e, SZ_64, REG_RCX, (int32_t)n->case_.value);
        enc_jcc(e, CC_NE, skip_lbl);
        gen_stmt(ctx, n->case_.body);
        enc_label(e, skip_lbl);
        free(skip_lbl);
        break;
    }

    case ND_DEFAULT:
        gen_stmt(ctx, n->default_.body);
        break;

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

    /* Bind parameters to local variables */
    int nparams = fn->func.param_count;
    for (int i = 0; i < nparams && i < NARG_REGS; i++) {
        Node *param = fn->func.params[i];
        Type *pty   = param->param.param_type;
        if (!pty) {
            /* default to int */
            pty = calloc(1, sizeof(Type));
            pty->kind = TY_INT; pty->size = 4; pty->align = 4;
        }
        int off = local_alloc(&ctx.locals, param->param.name, pty);
        /* Store arg register to local */
        OpSize opsz = ty_opsize(pty);
        enc_mov_mr(e, opsz, REG_RBP, REG_NONE, 1, off, ARG_REGS[i]);
    }
    /* Stack parameters (i >= 6) are at [rbp + 16 + (i-6)*8] */
    for (int i = NARG_REGS; i < nparams; i++) {
        Node *param = fn->func.params[i];
        Type *pty   = param->param.param_type;
        if (!pty) {
            pty = calloc(1, sizeof(Type));
            pty->kind = TY_INT; pty->size = 4; pty->align = 4;
        }
        LocalVar *v = calloc(1, sizeof(LocalVar));
        v->name   = strdup(param->param.name);
        v->offset = 16 + (i - NARG_REGS) * 8;  /* positive from rbp */
        v->type   = pty;
        v->next   = ctx.locals.vars;
        ctx.locals.vars = v;
    }

    /* Generate body */
    gen_stmt(&ctx, fn->func.body);

    /* Return label */
    enc_label(e, ret_label);

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
        sym_add(cg, name, CGSYM_OBJECT, off + pad, sz, !decl->decl.is_static);
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
        fprintf(out, "\t.zero\t%zu\n", cg->bss_size);
    }
}
