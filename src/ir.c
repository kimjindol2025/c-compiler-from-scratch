/*
 * ir.c — AST → 3-address IR lowering + const folding + DCE
 *
 * Design choices:
 *  - Every IrVal carries a Type* so later passes never guess widths.
 *  - Array/struct operations are pre-lowered to IR_INDEX / IR_MEMBER so
 *    the backend can emit scale/offset without re-querying the type system.
 *  - Const folding is a single-pass over the instruction list; no DAG needed
 *    for the C subset we target.
 */
#define _POSIX_C_SOURCE 200809L
#include "../include/ir.h"
#include "../include/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* =========================================================
 * Arena allocator (simple bump-ptr)
 * ========================================================= */
#define ARENA_INIT  (256 * 1024)  /* 256 KB initial */

static void *ir_arena_alloc(IrModule *m, size_t sz) {
    sz = (sz + 7) & ~(size_t)7; /* 8-byte align */
    if (m->arena_used + sz > m->arena_cap) {
        size_t newcap = m->arena_cap ? m->arena_cap * 2 : ARENA_INIT;
        while (newcap < m->arena_used + sz) newcap *= 2;
        m->arena = realloc(m->arena, newcap);
        if (!m->arena) { perror("ir_arena_alloc"); exit(1); }
        m->arena_cap = newcap;
    }
    void *p = (char *)m->arena + m->arena_used;
    memset(p, 0, sz);
    m->arena_used += sz;
    return p;
}
static char *ir_arena_strdup(IrModule *m, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = ir_arena_alloc(m, n);
    memcpy(d, s, n);
    return d;
}

/* =========================================================
 * Module / function helpers
 * ========================================================= */
IrModule *ir_module_new(void) {
    IrModule *m = calloc(1, sizeof(IrModule));
    return m;
}
void ir_module_free(IrModule *m) {
    if (!m) return;
    free(m->arena);
    free(m);
}

static IrFunc *func_new(IrModule *m, const char *name, Type *fty) {
    IrFunc *f = ir_arena_alloc(m, sizeof(IrFunc));
    f->name    = ir_arena_strdup(m, name);
    f->func_ty = fty;
    if (m->funcs_tail) { m->funcs_tail->next = f; m->funcs_tail = f; }
    else               { m->funcs = m->funcs_tail = f; }
    m->nfuncs++;
    return f;
}

static IrInstr *instr_new(IrModule *m, IrOpcode op) {
    IrInstr *i = ir_arena_alloc(m, sizeof(IrInstr));
    i->op = op;
    return i;
}

static void emit(IrFunc *f, IrInstr *i) {
    if (f->tail) { f->tail->next = i; f->tail = i; }
    else         { f->head = f->tail = i; }
    f->ninstr++;
}

static IrVal new_temp(IrFunc *f, Type *ty) {
    return iv_temp(f->next_temp++, ty);
}

/* =========================================================
 * Lowering context
 * ========================================================= */
typedef struct {
    IrModule *m;
    IrFunc   *f;
    int       label_seq;   /* unique label counter */
    /* for break/continue */
    const char *break_lbl;
    const char *cont_lbl;
    /* return label */
    const char *ret_lbl;
    IrVal       ret_val;   /* temp holding return value, or IV_NONE */
} LCtx;

static char *make_label(LCtx *lc, const char *prefix) {
    char buf[64];
    snprintf(buf, sizeof(buf), ".L%s%d", prefix, lc->label_seq++);
    return ir_arena_strdup(lc->m, buf);
}

/* Emit a label instruction */
static void emit_label(LCtx *lc, const char *lbl) {
    IrInstr *i = instr_new(lc->m, IR_LABEL);
    i->label = lbl;
    emit(lc->f, i);
}

/* Emit a binary op: dst = src1 op src2 */
static IrVal emit_binop(LCtx *lc, IrOpcode op, IrVal s1, IrVal s2, Type *ty) {
    IrVal dst = new_temp(lc->f, ty);
    IrInstr *i = instr_new(lc->m, op);
    i->dst = dst; i->src1 = s1; i->src2 = s2; i->ty = ty;
    emit(lc->f, i);
    return dst;
}

/* Emit a unary op: dst = op src */
static IrVal emit_unop(LCtx *lc, IrOpcode op, IrVal s1, Type *ty) {
    IrVal dst = new_temp(lc->f, ty);
    IrInstr *i = instr_new(lc->m, op);
    i->dst = dst; i->src1 = s1; i->ty = ty;
    emit(lc->f, i);
    return dst;
}

/* Emit a typed load from address: dst = *[addr + offset] */
static IrVal emit_load(LCtx *lc, IrVal addr, int off, Type *ty) {
    IrVal dst = new_temp(lc->f, ty);
    IrInstr *i = instr_new(lc->m, IR_LOAD);
    i->dst = dst; i->src1 = addr; i->imm = off; i->ty = ty;
    emit(lc->f, i);
    return dst;
}

/* Emit a typed store to address: *[addr + offset] = val */
static void emit_store(LCtx *lc, IrVal addr, int off, IrVal val, Type *ty) {
    IrInstr *i = instr_new(lc->m, IR_STORE);
    i->dst = addr; i->src1 = val; i->imm = off; i->ty = ty;
    emit(lc->f, i);
}

/* =========================================================
 * Helper: get integer element/pointee size
 * ========================================================= */
static int base_size(Type *ty) {
    if (!ty) return 1;
    if (ty->kind == TY_PTR   && ty->base) return ty->base->size > 0 ? ty->base->size : 1;
    if (ty->kind == TY_ARRAY && ty->base) return ty->base->size > 0 ? ty->base->size : 1;
    return 1;
}

/* =========================================================
 * Forward declarations for mutual recursion
 * ========================================================= */
static IrVal lower_expr(LCtx *lc, Node *n);
static IrVal lower_lvalue(LCtx *lc, Node *n); /* returns address of n */
static void  lower_stmt(LCtx *lc, Node *n);

/* =========================================================
 * lvalue lowering: returns IR_ADDR / IR_INDEX / IR_MEMBER val
 * ========================================================= */
static IrVal lower_lvalue(LCtx *lc, Node *n) {
    if (!n) return iv_none();

    switch (n->kind) {
    case ND_IDENT: {
        /* local or global */
        const char *name = n->ident.name;
        if (n->var && n->var->name) name = n->var->name;
        Type *ty = n->type ? n->type : n->var ? n->var->ty : NULL;
        /* Check if it's a local (we don't have offset info here, use name) */
        IrVal dst = new_temp(lc->f, ty);
        IrInstr *ai = instr_new(lc->m, IR_ADDR);
        ai->dst = dst;
        ai->label = ir_arena_strdup(lc->m, name);
        ai->ty = ty;
        emit(lc->f, ai);
        return dst;
    }
    case ND_INDEX: {
        /* base[idx]: address = base_addr + idx * elem_size */
        Node *base_n = n->index.arr ? n->index.arr : n->binary.left;
        Node *idx_n  = n->index.idx ? n->index.idx : n->binary.right;
        IrVal base_addr = lower_lvalue(lc, base_n);
        IrVal idx_val   = lower_expr(lc, idx_n);
        Type *elem_ty   = base_n->type ? base_n->type->base : NULL;
        int   scale     = elem_ty ? (elem_ty->size > 0 ? elem_ty->size : 1) : 1;

        IrVal dst = new_temp(lc->f, elem_ty);
        IrInstr *xi = instr_new(lc->m, IR_INDEX);
        xi->dst = dst; xi->src1 = base_addr; xi->src2 = idx_val;
        xi->imm = scale; xi->ty = elem_ty;
        emit(lc->f, xi);
        return dst;
    }
    case ND_MEMBER:
    case ND_ARROW: {
        Node *obj = n->member_access.obj;
        Member *m  = n->member_access.resolved
                   ? n->member_access.resolved
                   : n->member;
        IrVal base_addr;
        if (n->kind == ND_ARROW) {
            /* ptr->member: base_addr = *ptr */
            base_addr = lower_expr(lc, obj);
        } else {
            base_addr = lower_lvalue(lc, obj);
        }
        int off = m ? m->offset : 0;
        IrVal dst = new_temp(lc->f, m ? m->ty : NULL);
        IrInstr *mi = instr_new(lc->m, IR_MEMBER);
        mi->dst = dst; mi->src1 = base_addr; mi->imm = off;
        mi->ty = m ? m->ty : NULL;
        emit(lc->f, mi);
        return dst;
    }
    case ND_DEREF: {
        /* *ptr: the lvalue address is just the pointer value */
        return lower_expr(lc, n->unary.operand);
    }
    default:
        return iv_none();
    }
}

/* =========================================================
 * Get the node type (with array→ptr decay for arithmetic)
 * ========================================================= */
static Type *node_type(Node *n) {
    if (!n) return NULL;
    return n->type;
}

/* =========================================================
 * Expression lowering: returns IrVal holding the value
 * ========================================================= */
static IrVal lower_expr(LCtx *lc, Node *n) {
    if (!n) return iv_imm(0, NULL);

    switch (n->kind) {

    /* ---- literals ---- */
    case ND_INT_LIT:
        return iv_imm(n->ival, n->type);
    case ND_CHAR_LIT:
        return iv_imm(n->ival, n->type);

    /* ---- string literal ---- */
    case ND_STR_LIT: {
        const char *nm = n->sval ? n->sval : "";
        /* treat as IV_GLOBAL pointing to .rodata label — codegen handles */
        return iv_global(ir_arena_strdup(lc->m, nm), n->type);
    }

    /* ---- identifier: load from lvalue address ---- */
    case ND_IDENT: {
        Type *ty = node_type(n);
        if (ty && (ty->kind == TY_ARRAY || ty->kind == TY_FUNC)) {
            /* Array/function: yield address directly (decay) */
            return lower_lvalue(lc, n);
        }
        IrVal addr = lower_lvalue(lc, n);
        return emit_load(lc, addr, 0, ty);
    }

    /* ---- assign ---- */
    case ND_ASSIGN: {
        IrVal rval = lower_expr(lc, n->binary.right);
        IrVal addr = lower_lvalue(lc, n->binary.left);
        emit_store(lc, addr, 0, rval, node_type(n->binary.left));
        return rval;
    }

    /* ---- compound assign ---- */
#define LOWER_COMPOUND(ND_OP, IR_OP) \
    case ND_OP: { \
        IrVal addr = lower_lvalue(lc, n->binary.left); \
        Type *lty  = node_type(n->binary.left); \
        IrVal old  = emit_load(lc, addr, 0, lty); \
        IrVal rhs  = lower_expr(lc, n->binary.right); \
        /* pointer arithmetic scaling */ \
        if (lty && (lty->kind==TY_PTR||lty->kind==TY_ARRAY) && lty->base) { \
            int esz = lty->base->size > 0 ? lty->base->size : 1; \
            if (esz > 1) rhs = emit_binop(lc, IR_MUL, rhs, iv_imm(esz,NULL), rhs.ty); \
        } \
        IrVal res = emit_binop(lc, IR_OP, old, rhs, lty); \
        emit_store(lc, addr, 0, res, lty); \
        return res; \
    }
    LOWER_COMPOUND(ND_ASSIGN_ADD, IR_ADD)
    LOWER_COMPOUND(ND_ASSIGN_SUB, IR_SUB)
    LOWER_COMPOUND(ND_ASSIGN_MUL, IR_MUL)
    LOWER_COMPOUND(ND_ASSIGN_DIV, IR_DIV)
    LOWER_COMPOUND(ND_ASSIGN_MOD, IR_MOD)
    LOWER_COMPOUND(ND_ASSIGN_AND, IR_AND)
    LOWER_COMPOUND(ND_ASSIGN_OR,  IR_OR)
    LOWER_COMPOUND(ND_ASSIGN_XOR, IR_XOR)
    LOWER_COMPOUND(ND_ASSIGN_SHL, IR_SHL)
    LOWER_COMPOUND(ND_ASSIGN_SHR, IR_SHR)
#undef LOWER_COMPOUND

    /* ---- arithmetic ---- */
    case ND_ADD: {
        IrVal l = lower_expr(lc, n->binary.left);
        IrVal r = lower_expr(lc, n->binary.right);
        Type *lty = node_type(n->binary.left);
        /* pointer/array arithmetic: scale r by element size */
        if (lty && (lty->kind==TY_PTR||lty->kind==TY_ARRAY) && lty->base) {
            int esz = base_size(lty);
            if (esz > 1) r = emit_binop(lc, IR_MUL, r, iv_imm(esz, NULL), r.ty);
        }
        return emit_binop(lc, IR_ADD, l, r, node_type(n));
    }
    case ND_SUB: {
        IrVal l = lower_expr(lc, n->binary.left);
        IrVal r = lower_expr(lc, n->binary.right);
        Type *lty = node_type(n->binary.left);
        Type *rty = node_type(n->binary.right);
        IrVal res = emit_binop(lc, IR_SUB, l, r, node_type(n));
        /* ptr - ptr: divide by element size */
        if (lty && rty &&
            (lty->kind==TY_PTR||lty->kind==TY_ARRAY) &&
            (rty->kind==TY_PTR||rty->kind==TY_ARRAY) && lty->base) {
            int esz = base_size(lty);
            if (esz > 1) res = emit_binop(lc, IR_DIV, res, iv_imm(esz, NULL), node_type(n));
        }
        return res;
    }
    case ND_MUL: return emit_binop(lc, IR_MUL,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_DIV: return emit_binop(lc, IR_DIV,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_MOD: return emit_binop(lc, IR_MOD,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_BITAND: return emit_binop(lc, IR_AND,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_BITOR:  return emit_binop(lc, IR_OR,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_BITXOR: return emit_binop(lc, IR_XOR,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_SHL:    return emit_binop(lc, IR_SHL,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_SHR:    return emit_binop(lc, IR_SHR,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_NEG:    return emit_unop(lc, IR_NEG,
                     lower_expr(lc, n->unary.operand), node_type(n));
    case ND_BITNOT: return emit_unop(lc, IR_NOT,
                     lower_expr(lc, n->unary.operand), node_type(n));
    case ND_NOT:    return emit_unop(lc, IR_BOOL_NOT,
                     lower_expr(lc, n->unary.operand), node_type(n));

    /* ---- comparisons ---- */
    case ND_EQ:  return emit_binop(lc, IR_EQ,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_NE:  return emit_binop(lc, IR_NE,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_LT:  return emit_binop(lc, IR_LT,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_LE:  return emit_binop(lc, IR_LE,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_GT:  return emit_binop(lc, IR_GT,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));
    case ND_GE:  return emit_binop(lc, IR_GE,
                     lower_expr(lc,n->binary.left),
                     lower_expr(lc,n->binary.right), node_type(n));

    /* ---- logical and/or (short-circuit) ---- */
    case ND_LOGIC_AND: {
        char *false_lbl = make_label(lc, "land_f");
        char *end_lbl   = make_label(lc, "land_e");
        IrVal dst = new_temp(lc->f, node_type(n));

        IrVal l = lower_expr(lc, n->binary.left);
        IrInstr *jz1 = instr_new(lc->m, IR_JZ);
        jz1->src1 = l; jz1->label = false_lbl;
        emit(lc->f, jz1);

        IrVal r = lower_expr(lc, n->binary.right);
        IrInstr *jz2 = instr_new(lc->m, IR_JZ);
        jz2->src1 = r; jz2->label = false_lbl;
        emit(lc->f, jz2);

        /* true branch */
        IrInstr *mv1 = instr_new(lc->m, IR_MOV);
        mv1->dst = dst; mv1->src1 = iv_imm(1, node_type(n)); emit(lc->f, mv1);
        IrInstr *jmp = instr_new(lc->m, IR_JMP);
        jmp->label = end_lbl; emit(lc->f, jmp);

        /* false branch */
        emit_label(lc, false_lbl);
        IrInstr *mv0 = instr_new(lc->m, IR_MOV);
        mv0->dst = dst; mv0->src1 = iv_imm(0, node_type(n)); emit(lc->f, mv0);

        emit_label(lc, end_lbl);
        return dst;
    }
    case ND_LOGIC_OR: {
        char *true_lbl = make_label(lc, "lor_t");
        char *end_lbl  = make_label(lc, "lor_e");
        IrVal dst = new_temp(lc->f, node_type(n));

        IrVal l = lower_expr(lc, n->binary.left);
        IrInstr *jnz1 = instr_new(lc->m, IR_JNZ);
        jnz1->src1 = l; jnz1->label = true_lbl;
        emit(lc->f, jnz1);

        IrVal r = lower_expr(lc, n->binary.right);
        IrInstr *jnz2 = instr_new(lc->m, IR_JNZ);
        jnz2->src1 = r; jnz2->label = true_lbl;
        emit(lc->f, jnz2);

        IrInstr *mv0 = instr_new(lc->m, IR_MOV);
        mv0->dst = dst; mv0->src1 = iv_imm(0, node_type(n)); emit(lc->f, mv0);
        IrInstr *jmp = instr_new(lc->m, IR_JMP);
        jmp->label = end_lbl; emit(lc->f, jmp);

        emit_label(lc, true_lbl);
        IrInstr *mv1 = instr_new(lc->m, IR_MOV);
        mv1->dst = dst; mv1->src1 = iv_imm(1, node_type(n)); emit(lc->f, mv1);
        emit_label(lc, end_lbl);
        return dst;
    }

    /* ---- address-of ---- */
    case ND_ADDR:
        return lower_lvalue(lc, n->unary.operand);

    /* ---- dereference ---- */
    case ND_DEREF: {
        IrVal ptr = lower_expr(lc, n->unary.operand);
        return emit_load(lc, ptr, 0, node_type(n));
    }

    /* ---- array subscript ---- */
    case ND_INDEX: {
        IrVal addr = lower_lvalue(lc, n);
        return emit_load(lc, addr, 0, node_type(n));
    }

    /* ---- struct member ---- */
    case ND_MEMBER:
    case ND_ARROW: {
        Type *mty = node_type(n);
        if (mty && (mty->kind == TY_ARRAY || mty->kind == TY_STRUCT)) {
            return lower_lvalue(lc, n);  /* yield address for aggregate types */
        }
        IrVal addr = lower_lvalue(lc, n);
        return emit_load(lc, addr, 0, mty);
    }

    /* ---- pre-increment / pre-decrement ---- */
    case ND_PRE_INC: case ND_PRE_DEC: {
        IrVal addr = lower_lvalue(lc, n->unary.operand);
        Type *ty   = node_type(n->unary.operand);
        IrVal old  = emit_load(lc, addr, 0, ty);
        long long delta = 1;
        if (ty && (ty->kind==TY_PTR||ty->kind==TY_ARRAY) && ty->base)
            delta = ty->base->size > 0 ? ty->base->size : 1;
        IrOpcode op = (n->kind == ND_PRE_INC) ? IR_ADD : IR_SUB;
        IrVal res = emit_binop(lc, op, old, iv_imm(delta, NULL), ty);
        emit_store(lc, addr, 0, res, ty);
        return res;
    }

    /* ---- post-increment / post-decrement ---- */
    case ND_POST_INC: case ND_POST_DEC: {
        IrVal addr = lower_lvalue(lc, n->unary.operand);
        Type *ty   = node_type(n->unary.operand);
        IrVal old  = emit_load(lc, addr, 0, ty);
        long long delta = 1;
        if (ty && (ty->kind==TY_PTR||ty->kind==TY_ARRAY) && ty->base)
            delta = ty->base->size > 0 ? ty->base->size : 1;
        IrOpcode op = (n->kind == ND_POST_INC) ? IR_ADD : IR_SUB;
        IrVal res = emit_binop(lc, op, old, iv_imm(delta, NULL), ty);
        emit_store(lc, addr, 0, res, ty);
        return old;  /* post: return OLD value */
    }

    /* ---- cast ---- */
    case ND_CAST: {
        Node *inner = n->lhs ? n->lhs : n->cast.expr;
        Type *to    = n->cast_ty ? n->cast_ty : n->cast.to;
        IrVal val   = lower_expr(lc, inner);
        return emit_unop(lc, IR_CAST, val, to);
    }

    /* ---- ternary ---- */
    case ND_TERNARY: {
        char *else_lbl = make_label(lc, "ter_e");
        char *end_lbl  = make_label(lc, "ter_d");
        IrVal dst = new_temp(lc->f, node_type(n));
        IrVal cond = lower_expr(lc, n->ternary.cond);
        IrInstr *jz = instr_new(lc->m, IR_JZ);
        jz->src1 = cond; jz->label = else_lbl;
        emit(lc->f, jz);
        IrVal tv = lower_expr(lc, n->ternary.then);
        IrInstr *mvt = instr_new(lc->m, IR_MOV);
        mvt->dst = dst; mvt->src1 = tv; emit(lc->f, mvt);
        IrInstr *jmp = instr_new(lc->m, IR_JMP);
        jmp->label = end_lbl; emit(lc->f, jmp);
        emit_label(lc, else_lbl);
        IrVal fv = lower_expr(lc, n->ternary.else_);
        IrInstr *mvf = instr_new(lc->m, IR_MOV);
        mvf->dst = dst; mvf->src1 = fv; emit(lc->f, mvf);
        emit_label(lc, end_lbl);
        return dst;
    }

    /* ---- comma ---- */
    case ND_COMMA:
        lower_expr(lc, n->binary.left);
        return lower_expr(lc, n->binary.right);

    /* ---- sizeof ---- */
    case ND_SIZEOF_EXPR: case ND_SIZEOF_TYPE:
        return iv_imm(n->ival, n->type);

    /* ---- function call ---- */
    case ND_CALL: {
        Node *callee_n = n->func_expr ? n->func_expr : n->call.callee;
        int   nargs    = n->nargs > 0 ? n->nargs : n->call.arg_count;
        Node **args_n  = n->args ? n->args : n->call.args;

        IrVal callee = lower_expr(lc, callee_n);
        IrVal *args  = nargs > 0 ? ir_arena_alloc(lc->m, sizeof(IrVal) * nargs) : NULL;
        for (int i = 0; i < nargs; i++)
            args[i] = lower_expr(lc, args_n[i]);

        IrVal dst = new_temp(lc->f, node_type(n));
        IrInstr *ci = instr_new(lc->m, IR_CALL);
        ci->dst = dst; ci->src1 = callee;
        ci->args = args; ci->nargs = nargs;
        ci->ty = node_type(n);
        emit(lc->f, ci);
        return dst;
    }

    default:
        return iv_imm(0, node_type(n));
    }
}

/* =========================================================
 * Statement lowering
 * ========================================================= */
static void lower_stmt(LCtx *lc, Node *n) {
    if (!n) return;

    switch (n->kind) {
    case ND_NULL_STMT:
        break;

    case ND_EXPR_STMT:
        lower_expr(lc, n->unary.operand);
        break;

    case ND_COMPOUND:
        for (int i = 0; i < n->compound.count; i++)
            lower_stmt(lc, n->compound.stmts[i]);
        break;

    case ND_VAR_DECL: {
        /* Emit ALLOCA for the variable */
        Type *ty = n->decl_var ? n->decl_var->ty : n->decl.decl_type;
        if (!ty) ty = n->decl.decl_type;
        int sz  = ty && ty->size > 0 ? ty->size : 8;
        int aln = ty && ty->align > 0 ? ty->align : 8;
        const char *nm = n->decl_var ? n->decl_var->name : n->decl.name;
        IrVal slot = new_temp(lc->f, ty);
        IrInstr *ai = instr_new(lc->m, IR_ALLOCA);
        ai->dst = slot; ai->imm = sz; ai->imm2 = aln;
        ai->label = ir_arena_strdup(lc->m, nm);
        ai->ty = ty;
        emit(lc->f, ai);

        /* Initialize */
        Node *init = n->decl_init ? n->decl_init : n->decl.init;
        if (init) {
            if (init->kind == ND_INIT_LIST) {
                /* Struct / array: emit per-member stores */
                /* Sync flat fields */
                if (!init->items && init->init_list.items) {
                    init->items  = init->init_list.items;
                    init->nitems = init->init_list.count;
                }
                if (ty && ty->kind == TY_STRUCT) {
                    Member *m = ty->members;
                    for (int i = 0; i < init->nitems && m; i++, m = m->next) {
                        IrVal maddr = new_temp(lc->f, m->ty);
                        IrInstr *mi = instr_new(lc->m, IR_MEMBER);
                        mi->dst = maddr; mi->src1 = slot;
                        mi->imm = m->offset; mi->ty = m->ty;
                        emit(lc->f, mi);
                        IrVal val = lower_expr(lc, init->items[i]);
                        emit_store(lc, maddr, 0, val, m->ty);
                    }
                } else {
                    /* Array */
                    Type *elem = ty ? ty->base : NULL;
                    int   esz  = elem && elem->size > 0 ? elem->size : 8;
                    for (int i = 0; i < init->nitems; i++) {
                        IrVal eaddr = new_temp(lc->f, elem);
                        IrInstr *xi = instr_new(lc->m, IR_MEMBER);
                        xi->dst = eaddr; xi->src1 = slot;
                        xi->imm = i * esz; xi->ty = elem;
                        emit(lc->f, xi);
                        IrVal val = lower_expr(lc, init->items[i]);
                        emit_store(lc, eaddr, 0, val, elem);
                    }
                }
            } else {
                IrVal val  = lower_expr(lc, init);
                emit_store(lc, slot, 0, val, ty);
            }
        }
        break;
    }

    case ND_RETURN: {
        Node *rv = n->return_.value;
        if (rv) {
            IrVal v = lower_expr(lc, rv);
            IrInstr *ri = instr_new(lc->m, IR_RET);
            ri->src1 = v; ri->ty = node_type(rv);
            emit(lc->f, ri);
        } else {
            IrInstr *ri = instr_new(lc->m, IR_RET);
            emit(lc->f, ri);
        }
        break;
    }

    case ND_IF: {
        char *else_lbl = make_label(lc, "if_e");
        char *end_lbl  = make_label(lc, "if_d");
        IrVal cond = lower_expr(lc, n->if_.cond);
        IrInstr *jz = instr_new(lc->m, IR_JZ);
        jz->src1 = cond; jz->label = n->if_.else_ ? else_lbl : end_lbl;
        emit(lc->f, jz);
        lower_stmt(lc, n->if_.then);
        if (n->if_.else_) {
            IrInstr *j = instr_new(lc->m, IR_JMP);
            j->label = end_lbl; emit(lc->f, j);
            emit_label(lc, else_lbl);
            lower_stmt(lc, n->if_.else_);
        }
        emit_label(lc, end_lbl);
        break;
    }

    case ND_WHILE: {
        char *hd = make_label(lc, "wh");
        char *ex = make_label(lc, "wx");
        const char *saved_brk = lc->break_lbl;
        const char *saved_cnt = lc->cont_lbl;
        lc->break_lbl = ex; lc->cont_lbl = hd;
        emit_label(lc, hd);
        IrVal cond = lower_expr(lc, n->while_.cond);
        IrInstr *jz = instr_new(lc->m, IR_JZ);
        jz->src1 = cond; jz->label = ex; emit(lc->f, jz);
        lower_stmt(lc, n->while_.body);
        IrInstr *jmp = instr_new(lc->m, IR_JMP);
        jmp->label = hd; emit(lc->f, jmp);
        emit_label(lc, ex);
        lc->break_lbl = saved_brk; lc->cont_lbl = saved_cnt;
        break;
    }

    case ND_DO_WHILE: {
        char *hd = make_label(lc, "do");
        char *ex = make_label(lc, "dx");
        const char *saved_brk = lc->break_lbl;
        const char *saved_cnt = lc->cont_lbl;
        lc->break_lbl = ex; lc->cont_lbl = hd;
        emit_label(lc, hd);
        lower_stmt(lc, n->while_.body);
        IrVal cond = lower_expr(lc, n->while_.cond);
        IrInstr *jnz = instr_new(lc->m, IR_JNZ);
        jnz->src1 = cond; jnz->label = hd; emit(lc->f, jnz);
        emit_label(lc, ex);
        lc->break_lbl = saved_brk; lc->cont_lbl = saved_cnt;
        break;
    }

    case ND_FOR: {
        char *hd = make_label(lc, "fo");
        char *ex = make_label(lc, "fx");
        char *ct = make_label(lc, "fc");
        const char *saved_brk = lc->break_lbl;
        const char *saved_cnt = lc->cont_lbl;
        lc->break_lbl = ex; lc->cont_lbl = ct;
        lower_stmt(lc, n->for_.init);
        emit_label(lc, hd);
        if (n->for_.cond) {
            IrVal cond = lower_expr(lc, n->for_.cond);
            IrInstr *jz = instr_new(lc->m, IR_JZ);
            jz->src1 = cond; jz->label = ex; emit(lc->f, jz);
        }
        lower_stmt(lc, n->for_.body);
        emit_label(lc, ct);
        if (n->for_.step) lower_expr(lc, n->for_.step);
        IrInstr *jmp = instr_new(lc->m, IR_JMP);
        jmp->label = hd; emit(lc->f, jmp);
        emit_label(lc, ex);
        lc->break_lbl = saved_brk; lc->cont_lbl = saved_cnt;
        break;
    }

    case ND_BREAK: {
        if (lc->break_lbl) {
            IrInstr *j = instr_new(lc->m, IR_JMP);
            j->label = lc->break_lbl; emit(lc->f, j);
        }
        break;
    }
    case ND_CONTINUE: {
        if (lc->cont_lbl) {
            IrInstr *j = instr_new(lc->m, IR_JMP);
            j->label = lc->cont_lbl; emit(lc->f, j);
        }
        break;
    }

    case ND_LABEL: {
        emit_label(lc, n->label.name);
        lower_stmt(lc, n->label.body);
        break;
    }
    case ND_GOTO: {
        IrInstr *j = instr_new(lc->m, IR_JMP);
        j->label = n->goto_.label; emit(lc->f, j);
        break;
    }

    default:
        /* Expression statement fallthrough */
        lower_expr(lc, n);
        break;
    }
}

/* =========================================================
 * Function lowering
 * ========================================================= */
static void lower_func(LCtx *lc, Node *n) {
    const char *fname = n->func.name ? n->func.name : n->fname;
    if (!fname) return;

    IrFunc *f = func_new(lc->m, fname, n->func_ty);
    lc->f = f;

    /* FUNC_BEGIN marker */
    IrInstr *fb = instr_new(lc->m, IR_FUNC_BEGIN);
    fb->func_name = f->name;
    emit(f, fb);

    /* Parameters: emit ALLOCA + STORE for each param */
    int param_count = n->func.param_count;
    for (int i = 0; i < param_count; i++) {
        Node *pm = n->func.params[i];
        if (!pm) continue;
        const char *pname = pm->param.name;
        Type *pty = pm->param.param_type;
        if (!pty) pty = pm->type;
        int sz  = pty && pty->size  > 0 ? pty->size  : 8;
        int aln = pty && pty->align > 0 ? pty->align : 8;
        IrVal slot = new_temp(f, pty);
        IrInstr *ai = instr_new(lc->m, IR_ALLOCA);
        ai->dst = slot; ai->imm = sz; ai->imm2 = aln;
        ai->label = ir_arena_strdup(lc->m, pname ? pname : "_param");
        ai->ty = pty;
        emit(f, ai);
    }

    /* Body */
    if (n->func.body)
        lower_stmt(lc, n->func.body);
    else if (n->body)
        lower_stmt(lc, n->body);

    /* Implicit void return */
    IrInstr *fe = instr_new(lc->m, IR_FUNC_END);
    fe->func_name = f->name;
    emit(f, fe);
}

/* =========================================================
 * Top-level: walk translation unit
 * ========================================================= */
void ir_lower(IrModule *m, Node *tu) {
    if (!tu) return;

    Node **decls = tu->decls;
    int    ndecls = tu->ndecls;
    if (!decls && tu->compound.stmts) {
        decls  = tu->compound.stmts;
        ndecls = tu->compound.count;
    }

    for (int i = 0; i < ndecls; i++) {
        Node *d = decls[i];
        if (!d) continue;
        if (d->kind == ND_FUNC_DEF) {
            LCtx lc = {0};
            lc.m = m;
            lower_func(&lc, d);
        }
        /* Global var decls are handled by codegen separately */
    }
}

/* =========================================================
 * Constant folding
 * ========================================================= */
static long long fold_binop(IrOpcode op, long long a, long long b) {
    switch (op) {
    case IR_ADD: return a + b;
    case IR_SUB: return a - b;
    case IR_MUL: return a * b;
    case IR_DIV: return b ? a / b : 0;
    case IR_MOD: return b ? a % b : 0;
    case IR_AND: return a & b;
    case IR_OR:  return a | b;
    case IR_XOR: return a ^ b;
    case IR_SHL: return a << b;
    case IR_SHR: return a >> b;
    case IR_EQ:  return a == b;
    case IR_NE:  return a != b;
    case IR_LT:  return a <  b;
    case IR_LE:  return a <= b;
    case IR_GT:  return a >  b;
    case IR_GE:  return a >= b;
    default: return 0;
    }
}

void ir_opt_const_fold(IrModule *m) {
    for (IrFunc *f = m->funcs; f; f = f->next) {
        for (IrInstr *i = f->head; i; i = i->next) {
            /* Fold binary op on two immediates */
            if (i->src1.kind == IV_IMM && i->src2.kind == IV_IMM) {
                switch (i->op) {
                case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
                case IR_AND: case IR_OR:  case IR_XOR: case IR_SHL: case IR_SHR:
                case IR_EQ:  case IR_NE:  case IR_LT:  case IR_LE:
                case IR_GT:  case IR_GE: {
                    long long res = fold_binop(i->op, i->src1.imm, i->src2.imm);
                    i->op   = IR_MOV;
                    i->src1 = iv_imm(res, i->dst.ty);
                    i->src2 = iv_none();
                    break;
                }
                default: break;
                }
            }
            /* Fold unary on immediate */
            if (i->src1.kind == IV_IMM) {
                if (i->op == IR_NEG) {
                    i->op = IR_MOV; i->src1.imm = -i->src1.imm;
                } else if (i->op == IR_NOT) {
                    i->op = IR_MOV; i->src1.imm = ~i->src1.imm;
                } else if (i->op == IR_BOOL_NOT) {
                    i->op = IR_MOV; i->src1.imm = !i->src1.imm;
                }
            }
            /* Strength reduction: x * 1 → x, x * 0 → 0, x + 0 → x, etc. */
            if (i->op == IR_MUL) {
                if (i->src2.kind == IV_IMM && i->src2.imm == 1)
                    { i->op = IR_MOV; i->src2 = iv_none(); }
                else if (i->src2.kind == IV_IMM && i->src2.imm == 0)
                    { i->op = IR_MOV; i->src1 = iv_imm(0, i->dst.ty); i->src2 = iv_none(); }
                else if (i->src1.kind == IV_IMM && i->src1.imm == 1)
                    { i->op = IR_MOV; i->src1 = i->src2; i->src2 = iv_none(); }
            }
            if ((i->op == IR_ADD || i->op == IR_SUB) &&
                i->src2.kind == IV_IMM && i->src2.imm == 0) {
                i->op = IR_MOV; i->src2 = iv_none();
            }
        }
    }
}

/* =========================================================
 * Dead code elimination (after const fold: remove JZ/JNZ on constant)
 * ========================================================= */
void ir_opt_dce(IrModule *m) {
    for (IrFunc *f = m->funcs; f; f = f->next) {
        for (IrInstr *i = f->head; i; i = i->next) {
            if (i->op == IR_JZ && i->src1.kind == IV_IMM) {
                if (i->src1.imm != 0) i->op = IR_NOP;   /* never taken */
                else                  i->op = IR_JMP;   /* always taken */
            }
            if (i->op == IR_JNZ && i->src1.kind == IV_IMM) {
                if (i->src1.imm != 0) i->op = IR_JMP;   /* always taken */
                else                  i->op = IR_NOP;   /* never taken */
            }
        }
    }
}

/* =========================================================
 * IR printer
 * ========================================================= */
static const char *opname(IrOpcode op) {
    switch (op) {
    case IR_ADD: return "add";  case IR_SUB: return "sub";
    case IR_MUL: return "mul";  case IR_DIV: return "div";
    case IR_MOD: return "mod";  case IR_AND: return "and";
    case IR_OR:  return "or";   case IR_XOR: return "xor";
    case IR_SHL: return "shl";  case IR_SHR: return "shr";
    case IR_NEG: return "neg";  case IR_NOT: return "not";
    case IR_BOOL_NOT: return "bnot"; case IR_CAST: return "cast";
    case IR_MOV: return "mov";  case IR_LOAD: return "load";
    case IR_STORE: return "store"; case IR_ADDR: return "addr";
    case IR_INDEX: return "index"; case IR_MEMBER: return "member";
    case IR_CALL: return "call"; case IR_RET: return "ret";
    case IR_JMP: return "jmp";  case IR_JZ: return "jz";
    case IR_JNZ: return "jnz"; case IR_LABEL: return "label";
    case IR_ALLOCA: return "alloca";
    case IR_EQ: return "eq"; case IR_NE: return "ne";
    case IR_LT: return "lt"; case IR_LE: return "le";
    case IR_GT: return "gt"; case IR_GE: return "ge";
    case IR_NOP: return "nop";
    case IR_FUNC_BEGIN: return "func_begin";
    case IR_FUNC_END:   return "func_end";
    default: return "???";
    }
}

static void print_val(IrVal v, FILE *out) {
    switch (v.kind) {
    case IV_NONE:   fprintf(out, "_"); break;
    case IV_TEMP:   fprintf(out, "%%t%d", v.temp); break;
    case IV_IMM:    fprintf(out, "%lld", v.imm); break;
    case IV_FIMM:   fprintf(out, "%g", v.fimm); break;
    case IV_GLOBAL: fprintf(out, "@%s", v.name ? v.name : "?"); break;
    case IV_LABEL:  fprintf(out, "%%%s", v.name ? v.name : "?"); break;
    }
}

void ir_print(IrModule *m, FILE *out) {
    for (IrFunc *f = m->funcs; f; f = f->next) {
        fprintf(out, "\nfn %s {\n", f->name);
        for (IrInstr *i = f->head; i; i = i->next) {
            if (i->op == IR_LABEL) {
                fprintf(out, "%s:\n", i->label ? i->label : "?");
                continue;
            }
            if (i->op == IR_FUNC_BEGIN || i->op == IR_FUNC_END) {
                fprintf(out, "  ; %s %s\n", opname(i->op), i->func_name ? i->func_name : "");
                continue;
            }
            fprintf(out, "  ");
            if (i->dst.kind != IV_NONE) {
                print_val(i->dst, out);
                fprintf(out, " = ");
            }
            fprintf(out, "%s", opname(i->op));
            if (i->op == IR_ALLOCA)
                fprintf(out, "(%d, align %d, \"%s\")", i->imm, i->imm2, i->label ? i->label : "");
            else if (i->op == IR_LOAD || i->op == IR_MEMBER)
                { fprintf(out, " "); print_val(i->src1, out); fprintf(out, "[%d]", i->imm); }
            else if (i->op == IR_STORE)
                { fprintf(out, " "); print_val(i->dst, out);
                  fprintf(out, "[%d] = "); print_val(i->src1, out); }
            else if (i->op == IR_INDEX)
                { fprintf(out, " "); print_val(i->src1, out);
                  fprintf(out, "["); print_val(i->src2, out);
                  fprintf(out, "*%d]", i->imm); }
            else if (i->op == IR_ADDR)
                fprintf(out, " &%s", i->label ? i->label : "?");
            else if (i->op == IR_JMP || i->op == IR_JZ || i->op == IR_JNZ) {
                if (i->src1.kind != IV_NONE)
                    { fprintf(out, " "); print_val(i->src1, out); fprintf(out, ", "); }
                fprintf(out, "%s", i->label ? i->label : "?");
            } else if (i->op == IR_CALL) {
                fprintf(out, " "); print_val(i->src1, out); fprintf(out, "(");
                for (int a = 0; a < i->nargs; a++) {
                    if (a) fprintf(out, ", ");
                    print_val(i->args[a], out);
                }
                fprintf(out, ")");
            } else if (i->op == IR_RET) {
                if (i->src1.kind != IV_NONE)
                    { fprintf(out, " "); print_val(i->src1, out); }
            } else {
                if (i->src1.kind != IV_NONE)
                    { fprintf(out, " "); print_val(i->src1, out); }
                if (i->src2.kind != IV_NONE)
                    { fprintf(out, ", "); print_val(i->src2, out); }
            }
            fprintf(out, "\n");
        }
        fprintf(out, "}\n");
    }
}
