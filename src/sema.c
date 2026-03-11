/*
 * sema.c — Semantic Analysis (type-checking) pass
 *
 * Walks the AST produced by the parser and:
 *   1. Resolves typedef names, struct/union/enum tags.
 *   2. Links ND_VAR references to their Var pointer.
 *   3. Annotates every expression node with its C type (node->ty).
 *   4. Enforces C11 type rules (§6.3 – §6.8).
 *   5. Validates statement constraints (break/continue/goto labels).
 *   6. Computes struct/union member offsets and sizes.
 *   7. Evaluates integer constant expressions (case labels, etc.).
 *   8. Computes the local-variable frame size for each function.
 *
 * Field layout follows the actual ast.h in this project:
 *   - Binary nodes: lhs / rhs
 *   - Body of loops/switch: body
 *   - If: cond / then / els
 *   - For: init / cond / step / body
 *   - Call: func_expr / args[nargs]
 *   - Block: stmts[nstmts]
 *   - Char literals: ival  (not cval)
 *   - Return value: ret_val
 *   - Function def: fname / func_ty / params_var / body / stack_size
 *   - Switch: lhs (expr) / body / cases / default_case / case_val
 *   - Goto/label: goto_label / label_name
 *   - Member: member_name / member
 *   - Cast/sizeof: cast_ty
 *   - Compound-assign: op (underlying NodeKind)
 */

#include "sema.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------
 * Error / warning macros
 *
 * The ##__VA_ARGS__ GNU extension handles zero extra args; for
 * strict C99 portability we add a dummy "" argument.
 * --------------------------------------------------------- */
#define ERR(s, line, ...) \
    do { \
        fprintf(stderr, "%s:%d: error: ", \
                (s)->filename ? (s)->filename : "<unknown>", (line)); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        (s)->error_count++; \
    } while (0)

#define WARN(s, line, ...) \
    do { \
        fprintf(stderr, "%s:%d: warning: ", \
                (s)->filename ? (s)->filename : "<unknown>", (line)); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        (s)->warn_count++; \
    } while (0)

/* ---------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------- */

Sema *sema_new(void)
{
    Sema *s = calloc(1, sizeof(Sema));
    if (!s) { perror("calloc"); exit(1); }
    types_init();
    s->symtable = symtable_new();
    return s;
}

void sema_free(Sema *s)
{
    if (!s) return;
    symtable_free(s->symtable);
    free(s);
}

/* ---------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------- */
static Type *resolve_type(Sema *s, Type *ty);
static void  analyze_func_def(Sema *s, Node *node);
static void  process_var_decl(Sema *s, Node *node);
static void  analyze_stmt(Sema *s, Node *node);
static Type *analyze_expr(Sema *s, Node *node);
static Type *analyze_call(Sema *s, Node *node);
static Node *implicit_cast_node(Node *expr, Type *to);
static int   eval_const(Sema *s, Node *expr, long long *out);

/* Align val up to the nearest multiple of align. */
static int align_up(int v, int a)
{
    if (a <= 1) return v;
    return (v + a - 1) & ~(a - 1);
}

/* =========================================================
 * Public entry points
 * ========================================================= */

int sema_analyze(Sema *s, Node *program)
{
    if (!program) return 1;
    assert(program->kind == ND_PROGRAM);

    /* Sync union → flat for ND_TRANSLATION_UNIT */
    if (program->ndecls == 0 && program->unit.count > 0) {
        program->ndecls = program->unit.count;
        program->decls  = program->unit.decls;
    }

    for (int i = 0; i < program->ndecls; i++)
        sema_toplevel(s, program->decls[i]);

    return s->error_count > 0 ? 1 : 0;
}

void sema_toplevel(Sema *s, Node *decl)
{
    if (!decl) return;
    switch (decl->kind) {
    case ND_FUNC_DEF:
        analyze_func_def(s, decl);
        break;
    case ND_VAR_DECL:
        process_var_decl(s, decl);
        break;
    case ND_STRUCT_DEF:
    case ND_UNION_DEF:
    case ND_ENUM_DEF:
        /* Register the struct/union/enum type in symtable */
        if (decl->tag_def.type) resolve_type(s, decl->tag_def.type);
        break;
    case ND_TYPEDEF:
        /* Handled inside resolve_type; nothing extra needed at top level */
        break;
    default:
        break;
    }
}

/* =========================================================
 * Type resolution
 * ========================================================= */

static Type *resolve_type(Sema *s, Type *ty)
{
    if (!ty) return ty_int;

    switch (ty->kind) {
    /* Primitive types are already complete */
    case TY_VOID:
    case TY_BOOL:
    case TY_CHAR:  case TY_SCHAR:  case TY_UCHAR:
    case TY_SHORT: case TY_USHORT:
    case TY_INT:   case TY_UINT:
    case TY_LONG:  case TY_ULONG:
    case TY_LLONG: case TY_ULLONG:
    case TY_FLOAT: case TY_DOUBLE: case TY_LDOUBLE:
    case TY_ENUM:
        return ty;

    case TY_TYPEDEF_REF: {
        /* Look up the typedef name */
        Symbol *sym = symtable_lookup(s->symtable, ty->typedef_name);
        if (!sym || sym->kind != SYM_TYPEDEF) {
            ERR(s, 0, "unknown type name '%s'", ty->typedef_name);
            return ty_int;
        }
        return resolve_type(s, sym->type);
    }

    case TY_PTR:
        ty->base = resolve_type(s, ty->base);
        ty->size  = 8;
        ty->align = 8;
        ty->is_complete = true;
        return ty;

    case TY_ARRAY:
        ty->base = resolve_type(s, ty->base);
        if (ty->base) ty->align = ty->base->align;
        if (ty->array_len >= 0 && ty->base && ty->base->size > 0) {
            ty->size = ty->base->size * ty->array_len;
            ty->is_complete = true;
        }
        return ty;

    case TY_FUNC:
        ty->return_ty = resolve_type(s, ty->return_ty);
        ty->base      = ty->return_ty;
        for (int i = 0; i < ty->param_count; i++)
            ty->params[i] = resolve_type(s, ty->params[i]);
        ty->is_complete = true;
        return ty;

    case TY_STRUCT:
    case TY_UNION: {
        SymKind tag_kind = (ty->kind == TY_STRUCT) ? SYM_STRUCT_TAG
                                                    : SYM_UNION_TAG;
        if (ty->tag) {
            Symbol *sym = symtable_lookup_tag(s->symtable, ty->tag, tag_kind);
            if (!sym) {
                /* Forward declaration */
                sym = symtable_define(s->symtable, tag_kind, ty->tag, ty);
            } else if (ty->members && !sym->type->members) {
                /* Complete a previously forward-declared struct */
                sym->type->members = ty->members;
            } else if (!ty->members) {
                /* Reference to existing tag — use its type */
                return sym->type;
            }
        }
        /* Resolve member types and recompute layout.
         * The parser may have computed offsets using incomplete member types
         * (e.g., struct A's size was 0 when struct B was parsed).  Always
         * re-lay-out once we have complete member information.            */
        if (ty->members) {
            /* Temporarily clear is_complete to allow relayout */
            ty->is_complete = false;
            for (Member *m = ty->members; m; m = m->next)
                m->ty = resolve_type(s, m->ty);
            type_layout_struct(ty);   /* sets is_complete = true */
        }
        return ty;
    }

    default:
        return ty;
    }
}

Type *sema_resolve_type(Sema *s, Type *ty)
{
    return resolve_type(s, ty);
}

/* =========================================================
 * Variable declaration processing
 * ========================================================= */

static void process_var_decl(Sema *s, Node *node)
{
    if (!node || node->kind != ND_VAR_DECL) return;

    /* Sync union → flat: build Var from decl.* fields if decl_var is missing */
    if (!node->decl_var && node->decl.name) {
        Var *nv = calloc(1, sizeof(Var));
        if (!nv) { perror("calloc"); exit(1); }
        nv->name       = node->decl.name;
        nv->ty         = node->decl.decl_type ? node->decl.decl_type : ty_int;
        nv->is_static  = node->decl.is_static;
        nv->is_extern  = node->decl.is_extern;
        nv->is_register = node->decl.is_register;
        node->decl_var = nv;
    }
    if (!node->decl_init && node->decl.init)
        node->decl_init = node->decl.init;

    Var *v = node->decl_var;
    if (!v) return;

    v->ty = resolve_type(s, v->ty);

    int is_global = (s->current_func_ret == NULL);

    /* Redeclaration check in current scope */
    Symbol *existing = symtable_lookup_current(s->symtable, v->name);
    if (existing && existing->kind == SYM_VAR) {
        /* Tentative definitions: allowed at file scope */
        if (!is_global || !existing->is_tentative) {
            ERR(s, node->line, "redeclaration of '%s'", v->name);
            return;
        }
        if (node->decl_init) existing->is_tentative = 0;
        v->offset    = existing->offset;
        v->is_global = existing->is_global;
        v->is_static = existing->is_static;
        v->is_extern = existing->is_extern;
    } else {
        Symbol *sym = symtable_define(s->symtable, SYM_VAR, v->name, v->ty);
        if (!sym) return;

        sym->is_global    = is_global;
        sym->is_static    = v->is_static;
        sym->is_extern    = v->is_extern;
        sym->is_tentative = (is_global && !node->decl_init && !v->is_extern);

        if (!is_global && !v->is_static && !v->is_extern) {
            int sz  = v->ty->size  > 0 ? v->ty->size  : 1;
            int aln = v->ty->align > 0 ? v->ty->align : 1;
            v->offset   = symtable_alloc_local(s->symtable, sz, aln);
            sym->offset = v->offset;
        }
        v->is_global = is_global;
    }

    /* Analyse initialiser expression */
    if (node->decl_init) {
        Type *init_ty = analyze_expr(s, node->decl_init);
        if (init_ty && !sema_is_assignable(s, v->ty, init_ty)) {
            char tbuf[64], ibuf[64];
            type_to_str(v->ty,   tbuf, sizeof(tbuf));
            type_to_str(init_ty, ibuf, sizeof(ibuf));
            ERR(s, node->line,
                "incompatible initialiser for '%s': cannot convert '%s' to '%s'",
                v->name, ibuf, tbuf);
        }
    }
}

static void analyze_var_decl(Sema *s, Node *node)
{
    process_var_decl(s, node);
}

/* =========================================================
 * Function definition analysis
 * ========================================================= */

static void analyze_func_def(Sema *s, Node *node)
{
    if (!node || node->kind != ND_FUNC_DEF) return;

    /* Sync union → flat fields */
    if (!node->fname && node->func.name)
        node->fname = (char *)node->func.name;
    if (!node->body && node->func.body)
        node->body = node->func.body;
    node->is_static = node->func.is_static;
    node->is_inline = node->func.is_inline;
    node->is_extern = node->func.is_extern;

    /* Build func_ty (TY_FUNC) from union fields if not set.
     * NOTE: type_func_returning stores the ptypes pointer directly (no copy),
     * so we must NOT free ptypes after the call. */
    if (!node->func_ty && node->func.ret_type) {
        int np = node->func.param_count;
        Type **ptypes = NULL;
        if (np > 0) {
            ptypes = malloc(np * sizeof(Type *));
            if (!ptypes) { perror("malloc"); exit(1); }
            for (int i = 0; i < np; i++)
                ptypes[i] = node->func.params[i]->param.param_type
                          ? node->func.params[i]->param.param_type : ty_int;
        }
        /* ptypes ownership transferred to the new TY_FUNC — do NOT free */
        node->func_ty = type_func_returning(node->func.ret_type, ptypes, np, false);
    }

    /* Build params_var (Var* linked list) from func.params[] if not set */
    if (!node->params_var && node->func.param_count > 0) {
        Var *head = NULL, **tail = &head;
        for (int i = 0; i < node->func.param_count; i++) {
            Node *pm = node->func.params[i];
            Var *pv = calloc(1, sizeof(Var));
            if (!pv) { perror("calloc"); exit(1); }
            pv->name = (char *)pm->param.name;
            pv->ty   = pm->param.param_type ? pm->param.param_type : ty_int;
            *tail = pv; tail = &pv->next;
        }
        node->params_var = head;
    }

    Type *ft = node->func_ty ? node->func_ty : ty_int;
    ft = resolve_type(s, ft);
    node->func_ty = ft;

    /* Register/update function symbol at file scope */
    Symbol *fsym = symtable_lookup_current(s->symtable, node->fname);
    if (fsym) {
        if (fsym->kind != SYM_FUNC) {
            ERR(s, node->line,
                "'%s' redeclared as different kind of symbol", node->fname);
            return;
        }
        if (fsym->is_defined) {
            ERR(s, node->line,
                "redefinition of function '%s'", node->fname);
            return;
        }
        fsym->is_defined = 1;
        fsym->type       = ft;
    } else {
        fsym = symtable_define(s->symtable, SYM_FUNC, node->fname, ft);
        if (!fsym) return;
        fsym->is_global  = 1;
        fsym->is_defined = (node->body != NULL);
    }

    /* Only analyse the body if there is one */
    if (!node->body) return;

    /* Save outer context */
    Type       *prev_ret   = s->current_func_ret;
    const char *prev_fn    = s->current_func_name;
    int         prev_loop  = s->in_loop;
    int         prev_sw    = s->in_switch;
    int         prev_stk   = symtable_reset_stack(s->symtable);

    s->current_func_ret  = ft->return_ty ? ft->return_ty : ty_void;
    s->current_func_name = node->fname;
    s->in_loop   = 0;
    s->in_switch = 0;

    symtable_push_scope(s->symtable, 1 /* is_func_scope */);

    /* Register parameters */
    for (Var *p = node->params_var; p; p = p->next) {
        p->ty = resolve_type(s, p->ty);
        /* Array params decay to pointer (C11 §6.7.6.3) */
        if (p->ty->kind == TY_ARRAY) p->ty = type_ptr_to(p->ty->base);

        Symbol *psym = symtable_define(s->symtable, SYM_VAR, p->name, p->ty);
        if (!psym) continue;
        int sz  = p->ty->size  > 0 ? p->ty->size  : 8;
        int aln = p->ty->align > 0 ? p->ty->align : 8;
        p->offset   = symtable_alloc_local(s->symtable, sz, aln);
        psym->offset = p->offset;
    }

    /* Analyse the body */
    analyze_stmt(s, node->body);

    /* Validate goto labels */
    /* (A full two-pass resolution would collect undeclared goto targets here;
       for now we rely on the label symbols defined during analysis.) */

    /* Record total frame size, rounded to 16 for ABI alignment */
    int frame = -(s->symtable->stack_offset);
    frame = align_up(frame, 16);
    node->stack_size = frame;

    symtable_pop_scope(s->symtable);

    /* Restore outer context */
    s->current_func_ret        = prev_ret;
    s->current_func_name       = prev_fn;
    s->in_loop                 = prev_loop;
    s->in_switch               = prev_sw;
    s->symtable->stack_offset  = prev_stk;
}

/* =========================================================
 * Statement analysis
 * ========================================================= */

void sema_stmt(Sema *s, Node *node)
{
    analyze_stmt(s, node);
}

static void analyze_stmt(Sema *s, Node *node)
{
    if (!node) return;

    switch (node->kind) {

    /* --- Compound statement --- */
    case ND_BLOCK: {
        symtable_push_scope(s->symtable, 0);
        for (int i = 0; i < node->nstmts; i++)
            analyze_stmt(s, node->stmts[i]);
        symtable_pop_scope(s->symtable);
        break;
    }

    /* --- Expression statement --- */
    case ND_EXPR_STMT:
        if (!node->lhs) node->lhs = node->unary.operand; /* sync */
        analyze_expr(s, node->lhs);
        break;

    /* --- Variable declaration inside a block --- */
    case ND_VAR_DECL:
        process_var_decl(s, node);
        break;

    /* --- Typedef (record in symbol table, no code) --- */
    case ND_TYPEDEF: {
        Type *ty = resolve_type(s, node->cast_ty);
        /* cast_ty stores the typedef's type in this project */
        if (!ty) break;
        Symbol *sym = symtable_define(s->symtable, SYM_TYPEDEF,
                                      node->fname ? node->fname : "?", ty);
        (void)sym;
        break;
    }

    /* --- Nested function definition (GNU extension) --- */
    case ND_FUNC_DEF:
        analyze_func_def(s, node);
        break;

    /* --- if statement --- */
    case ND_IF: {
        /* Sync union → flat */
        if (!node->cond) node->cond = node->if_.cond;
        if (!node->then) node->then = node->if_.then;
        if (!node->els)  node->els  = node->if_.else_;
        Type *ct = analyze_expr(s, node->cond);
        if (ct && !type_is_scalar(ct))
            ERR(s, node->line, "controlling expression of 'if' must be scalar");
        analyze_stmt(s, node->then);
        if (node->els) analyze_stmt(s, node->els);
        break;
    }

    /* --- while loop --- */
    case ND_WHILE: {
        /* Sync union → flat */
        if (!node->cond) node->cond = node->while_.cond;
        if (!node->body) node->body = node->while_.body;
        Type *ct = analyze_expr(s, node->cond);
        if (ct && !type_is_scalar(ct))
            ERR(s, node->line, "controlling expression of 'while' must be scalar");
        s->in_loop++;
        analyze_stmt(s, node->body);
        s->in_loop--;
        break;
    }

    /* --- do-while loop --- */
    case ND_DO_WHILE: {
        /* Sync union → flat */
        if (!node->cond) node->cond = node->while_.cond;
        if (!node->body) node->body = node->while_.body;
        s->in_loop++;
        analyze_stmt(s, node->body);
        s->in_loop--;
        Type *ct = analyze_expr(s, node->cond);
        if (ct && !type_is_scalar(ct))
            ERR(s, node->line, "controlling expression of 'do-while' must be scalar");
        break;
    }

    /* --- for loop --- */
    case ND_FOR: {
        /* Sync union → flat */
        if (!node->init && node->for_.init) node->init = node->for_.init;
        if (!node->cond && node->for_.cond) node->cond = node->for_.cond;
        if (!node->step && node->for_.step) node->step = node->for_.step;
        if (!node->body)                    node->body = node->for_.body;
        symtable_push_scope(s->symtable, 0);
        if (node->init) analyze_stmt(s, node->init);
        if (node->cond) {
            Type *ct = analyze_expr(s, node->cond);
            if (ct && !type_is_scalar(ct))
                ERR(s, node->line, "controlling expression of 'for' must be scalar");
        }
        if (node->step) analyze_expr(s, node->step);
        s->in_loop++;
        analyze_stmt(s, node->body);
        s->in_loop--;
        symtable_pop_scope(s->symtable);
        break;
    }

    /* --- switch statement --- */
    case ND_SWITCH: {
        /* Sync union → flat */
        if (!node->lhs)  node->lhs  = node->switch_.expr;
        if (!node->body) node->body = node->switch_.body;
        Type *et = analyze_expr(s, node->lhs);
        if (et && !type_is_integer(et))
            ERR(s, node->line, "switch expression must have integer type");
        s->in_switch++;
        s->in_loop++;  /* break exits switch */
        analyze_stmt(s, node->body);
        s->in_loop--;
        s->in_switch--;
        break;
    }

    /* --- case label --- */
    case ND_CASE: {
        /* Sync union → flat: parser stores evaluated value in case_.value */
        if (!node->body) node->body = node->case_.body;
        if (!node->lhs)  node->case_val = node->case_.value; /* already evaluated */
        if (!s->in_switch)
            ERR(s, node->line, "'case' not in switch statement");
        if (node->lhs) {
            long long val;
            if (!eval_const(s, node->lhs, &val))
                ERR(s, node->line, "case label must be an integer constant expression");
            else
                node->case_val = val;
        }
        if (node->body) analyze_stmt(s, node->body);
        break;
    }

    /* --- default label --- */
    case ND_DEFAULT:
        if (!node->body) node->body = node->default_.body; /* sync */
        if (!s->in_switch)
            ERR(s, node->line, "'default' not in switch statement");
        if (node->body) analyze_stmt(s, node->body);
        break;

    /* --- break / continue --- */
    case ND_BREAK:
        if (!s->in_loop && !s->in_switch)
            ERR(s, node->line, "'break' outside loop or switch");
        break;

    case ND_CONTINUE:
        if (!s->in_loop)
            ERR(s, node->line, "'continue' outside loop");
        break;

    /* --- return --- */
    case ND_RETURN: {
        if (!node->ret_val) node->ret_val = node->return_.value; /* sync */
        Type *ret = s->current_func_ret;
        if (!ret) {
            ERR(s, node->line, "'return' outside function");
            break;
        }
        if (node->ret_val) {
            Type *et = analyze_expr(s, node->ret_val);
            if (!et) break;
            if (ret->kind == TY_VOID) {
                ERR(s, node->line, "void function should not return a value");
            } else if (!sema_is_assignable(s, ret, et)) {
                char rbuf[64], ebuf[64];
                type_to_str(ret, rbuf, sizeof(rbuf));
                type_to_str(et,  ebuf, sizeof(ebuf));
                ERR(s, node->line,
                    "returning '%s' from function with return type '%s'",
                    ebuf, rbuf);
            } else if (!type_is_compatible(ret, et)) {
                node->ret_val = implicit_cast_node(node->ret_val, ret);
            }
        } else {
            if (ret->kind != TY_VOID)
                WARN(s, node->line, "non-void function returns no value");
        }
        break;
    }

    /* --- goto --- */
    case ND_GOTO:
        if (!node->goto_label) node->goto_label = (char *)node->goto_.label; /* sync */
        if (!node->goto_label || !node->goto_label[0])
            ERR(s, node->line, "empty goto label");
        break;

    /* --- label --- */
    case ND_LABEL: {
        if (!node->label_name) node->label_name = (char *)node->label.name; /* sync */
        if (!node->body)       node->body        = node->label.body;
        Symbol *lsym = symtable_define_label(s->symtable, node->label_name);
        (void)lsym;
        if (node->body) analyze_stmt(s, node->body);
        break;
    }

    /* --- null statement --- */
    case ND_NULL_STMT:
        break;

    default:
        /* Treat unknown nodes as expressions (covers ND_ASSIGN etc.) */
        analyze_expr(s, node);
        break;
    }
}

/* =========================================================
 * Expression analysis
 * ========================================================= */

static Type *set_ty(Node *node, Type *ty)
{
    node->ty   = ty;
    node->type = ty;   /* keep both aliases in sync */
    return ty;
}

/* Insert an implicit cast node wrapping expr, converting to type 'to'. */
static Node *implicit_cast_node(Node *expr, Type *to)
{
    if (!expr || !to) return expr;
    Node *cast = calloc(1, sizeof(Node));
    if (!cast) { perror("calloc"); exit(1); }
    cast->kind     = ND_CAST;
    cast->line     = expr->line;
    cast->cast_ty  = to;       /* flat field */
    cast->cast.to  = to;       /* union field */
    cast->lhs      = expr;     /* flat field */
    cast->cast.expr = expr;    /* union field */
    cast->ty       = to;
    return cast;
}

/* Apply array/function decay to the type of node. */
static Type *decay_of(Node *node)
{
    if (!node->ty) return ty_int;
    return type_decay(node->ty);
}

Type *sema_expr(Sema *s, Node *expr)
{
    return analyze_expr(s, expr);
}

static Type *analyze_expr(Sema *s, Node *node)
{
    if (!node) return ty_int;

    switch (node->kind) {

    /* --- Integer literal --- */
    case ND_INT_LIT: {
        unsigned long long v = (unsigned long long)node->ival;
        if      (v <= 0x7fffffff)         return set_ty(node, ty_int);
        else if (v <= 0xffffffff)         return set_ty(node, ty_uint);
        else if (v <= 0x7fffffffffffffff) return set_ty(node, ty_long);
        else                              return set_ty(node, ty_ullong);
    }

    /* --- Floating literal --- */
    case ND_FLOAT_LIT:
        return set_ty(node, ty_double);

    /* --- Character literal --- */
    case ND_CHAR_LIT:
        /* char literal has type int in C */
        return set_ty(node, ty_int);

    /* --- String literal --- */
    case ND_STR_LIT: {
        int len = node->slen > 0 ? node->slen : 1;
        return set_ty(node, type_array_of(ty_char, len));
    }

    /* --- Variable reference --- */
    case ND_VAR: {
        Var *v = node->var;
        if (!v) {
            /* Parser only set ident.name; look up in symtable */
            const char *name = node->ident.name;
            if (!name || !name[0]) name = "?";
            Symbol *sym = symtable_lookup(s->symtable, name);
            if (!sym) {
                ERR(s, node->line, "undeclared identifier '%s'", name);
                return set_ty(node, ty_int);
            }
            if (sym->kind == SYM_ENUM_CONST) {
                node->kind = ND_INT_LIT;
                node->ival = sym->enum_val;
                return set_ty(node, ty_int);
            }
            if (sym->kind == SYM_FUNC) {
                /* Function used as value (e.g. function pointer) */
                return set_ty(node, sym->type);
            }
            /* Build a Var from the symbol */
            v = calloc(1, sizeof(Var));
            if (!v) { perror("calloc"); exit(1); }
            v->name      = (char *)sym->name;
            v->ty        = sym->type;
            v->is_global = sym->is_global;
            v->is_static = sym->is_static;
            v->is_extern = sym->is_extern;
            v->offset    = sym->offset;
            node->var    = v;
        }
        v->ty = resolve_type(s, v->ty);
        return set_ty(node, v->ty);
    }

    /* --- Explicit cast --- */
    case ND_CAST: {
        /* Parser writes union fields; sema reads flat — sync both */
        if (!node->cast_ty && node->cast.to)   node->cast_ty  = node->cast.to;
        if (!node->lhs     && node->cast.expr) node->lhs      = node->cast.expr;
        Type *to = resolve_type(s, node->cast_ty);
        node->cast_ty  = to;
        node->cast.to  = to;
        Node *inner = node->lhs ? node->lhs : node->cast.expr;
        if (inner) { analyze_expr(s, inner); node->lhs = inner; node->cast.expr = inner; }
        return set_ty(node, to);
    }

    /* --- sizeof(expr) --- */
    case ND_SIZEOF_EXPR: {
        Type *et = analyze_expr(s, node->lhs);
        Type *dt = type_decay(et);
        int sz   = dt ? type_sizeof_val(dt) : -1;
        if (sz < 0) {
            ERR(s, node->line, "sizeof applied to incomplete type");
            sz = 0;
        }
        node->kind = ND_INT_LIT;
        node->ival = (long long)(unsigned long long)(unsigned)sz;
        node->lhs  = NULL;
        return set_ty(node, ty_ulong);
    }

    /* --- sizeof(type) --- */
    case ND_SIZEOF_TYPE: {
        Type *t = resolve_type(s, node->cast_ty);
        int sz  = t ? type_sizeof_val(t) : -1;
        if (sz < 0) {
            ERR(s, node->line, "sizeof applied to incomplete type");
            sz = 0;
        }
        node->kind = ND_INT_LIT;
        node->ival = (long long)(unsigned long long)(unsigned)sz;
        return set_ty(node, ty_ulong);
    }

    /* --- _Alignof(type) --- */
    case ND_ALIGNOF: {
        Type *t  = resolve_type(s, node->cast_ty);
        int  aln = t ? type_alignof_val(t) : -1;
        if (aln < 0) {
            ERR(s, node->line, "_Alignof applied to incomplete type");
            aln = 1;
        }
        node->kind = ND_INT_LIT;
        node->ival = (long long)aln;
        return set_ty(node, ty_ulong);
    }

    /* --- Unary minus / bitwise NOT --- */
    case ND_NEG:
    case ND_BITNOT: {
        Type *t = analyze_expr(s, node->lhs);
        if (!type_is_arithmetic(t)) {
            ERR(s, node->line, "unary arithmetic operator requires arithmetic type");
            return set_ty(node, ty_int);
        }
        return set_ty(node, type_integer_promote(t));
    }

    /* --- Logical NOT --- */
    case ND_LOGNOT: {
        Type *t = analyze_expr(s, node->lhs);
        if (!type_is_scalar(t))
            ERR(s, node->line, "'!' requires scalar operand");
        return set_ty(node, ty_int);
    }

    /* --- Address-of --- */
    case ND_ADDR: {
        Type *t = analyze_expr(s, node->lhs);
        return set_ty(node, type_ptr_to(t));
    }

    /* --- Dereference --- */
    case ND_DEREF: {
        Type *t  = analyze_expr(s, node->lhs);
        Type *dt = type_decay(t);
        if (dt->kind != TY_PTR) {
            ERR(s, node->line, "dereference of non-pointer type");
            return set_ty(node, ty_int);
        }
        return set_ty(node, dt->base);
    }

    /* --- Increment / Decrement --- */
    case ND_PRE_INC:
    case ND_PRE_DEC:
    case ND_POST_INC:
    case ND_POST_DEC: {
        Type *t = analyze_expr(s, node->lhs);
        if (!type_is_scalar(t))
            ERR(s, node->line, "increment/decrement requires scalar type");
        return set_ty(node, t);
    }

    /* --- Addition --- */
    case ND_ADD: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        Type *ld = decay_of(node->lhs);
        Type *rd = decay_of(node->rhs);

        if (ld->kind == TY_PTR && type_is_integer(rt))
            return set_ty(node, ld);
        if (rd->kind == TY_PTR && type_is_integer(lt))
            return set_ty(node, rd);
        if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
            ERR(s, node->line, "invalid operands to binary '+'");
            return set_ty(node, ty_int);
        }
        return set_ty(node, type_usual_arith_conv(lt, rt));
    }

    /* --- Subtraction --- */
    case ND_SUB: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        Type *ld = decay_of(node->lhs);
        Type *rd = decay_of(node->rhs);

        if (ld->kind == TY_PTR && type_is_integer(rt))
            return set_ty(node, ld);
        /* pointer - pointer → ptrdiff_t == long on x86-64 */
        if (ld->kind == TY_PTR && rd->kind == TY_PTR)
            return set_ty(node, ty_long);
        if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
            ERR(s, node->line, "invalid operands to binary '-'");
            return set_ty(node, ty_int);
        }
        return set_ty(node, type_usual_arith_conv(lt, rt));
    }

    /* --- Multiplication / Division --- */
    case ND_MUL:
    case ND_DIV: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
            ERR(s, node->line, "invalid operands to binary '%s'",
                node->kind == ND_MUL ? "*" : "/");
            return set_ty(node, ty_int);
        }
        return set_ty(node, type_usual_arith_conv(lt, rt));
    }

    /* --- Modulo --- */
    case ND_MOD: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        if (!type_is_integer(lt) || !type_is_integer(rt)) {
            ERR(s, node->line, "'%%' requires integer operands");
            return set_ty(node, ty_int);
        }
        return set_ty(node, type_usual_arith_conv(lt, rt));
    }

    /* --- Bitwise operators --- */
    case ND_BITAND:
    case ND_BITOR:
    case ND_BITXOR: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        if (!type_is_integer(lt) || !type_is_integer(rt)) {
            ERR(s, node->line, "bitwise operator requires integer operands");
            return set_ty(node, ty_int);
        }
        return set_ty(node, type_usual_arith_conv(lt, rt));
    }

    /* --- Shift operators --- */
    case ND_SHL:
    case ND_SHR: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        if (!type_is_integer(lt) || !type_is_integer(rt)) {
            ERR(s, node->line, "shift operator requires integer operands");
            return set_ty(node, ty_int);
        }
        return set_ty(node, type_integer_promote(lt));
    }

    /* --- Equality comparisons --- */
    case ND_EQ:
    case ND_NEQ: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        Type *ld = decay_of(node->lhs);
        Type *rd = decay_of(node->rhs);
        int ok = (type_is_arithmetic(lt) && type_is_arithmetic(rt)) ||
                 (ld->kind == TY_PTR    && rd->kind == TY_PTR);
        if (!ok)
            ERR(s, node->line, "invalid operands to equality operator");
        return set_ty(node, ty_int);
    }

    /* --- Relational comparisons --- */
    case ND_LT:
    case ND_LE:
    case ND_GT:
    case ND_GE: {
        Type *lt = analyze_expr(s, node->lhs); /* for arithmetic check */
        Type *rt = analyze_expr(s, node->rhs);
        Type *ld = decay_of(node->lhs);
        Type *rd = decay_of(node->rhs);
        int ok = (type_is_arithmetic(lt) && type_is_arithmetic(rt)) ||
                 (ld->kind == TY_PTR && rd->kind == TY_PTR);
        if (!ok)
            ERR(s, node->line, "invalid operands to relational operator");
        return set_ty(node, ty_int);
    }

    /* --- Logical operators --- */
    case ND_LOGAND:
    case ND_LOGOR: {
        Type *lt = analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        if (!type_is_scalar(lt) || !type_is_scalar(rt))
            ERR(s, node->line, "'%s' requires scalar operands",
                node->kind == ND_LOGAND ? "&&" : "||");
        return set_ty(node, ty_int);
    }

    /* --- Assignment --- */
    case ND_ASSIGN: {
        analyze_expr(s, node->lhs);
        Type *rt = analyze_expr(s, node->rhs);
        Type *ld = decay_of(node->lhs);
        if (!sema_is_assignable(s, ld, rt)) {
            char lbuf[64], rbuf[64];
            type_to_str(ld, lbuf, sizeof(lbuf));
            type_to_str(rt, rbuf, sizeof(rbuf));
            ERR(s, node->line,
                "incompatible types in assignment: '%s' = '%s'",
                lbuf, rbuf);
        } else if (!type_is_compatible(ld, rt)) {
            node->rhs = implicit_cast_node(node->rhs, ld);
        }
        return set_ty(node, ld);
    }

    /* --- Compound assignment --- */
    case ND_COMPOUND_ASSIGN:
    case ND_ASSIGN_ADD: case ND_ASSIGN_SUB: case ND_ASSIGN_MUL:
    case ND_ASSIGN_DIV: case ND_ASSIGN_MOD: case ND_ASSIGN_AND:
    case ND_ASSIGN_OR:  case ND_ASSIGN_XOR: case ND_ASSIGN_SHL:
    case ND_ASSIGN_SHR: {
        analyze_expr(s, node->lhs);
        analyze_expr(s, node->rhs);
        return set_ty(node, decay_of(node->lhs));
    }

    /* --- Comma --- */
    case ND_COMMA: {
        analyze_expr(s, node->lhs);
        Type *t = analyze_expr(s, node->rhs);
        return set_ty(node, t);
    }

    /* --- Ternary --- */
    case ND_TERNARY: {
        /* Sync union → flat */
        if (!node->cond) node->cond = node->ternary.cond;
        if (!node->then) node->then = node->ternary.then;
        if (!node->els)  node->els  = node->ternary.else_;
        Type *ct = analyze_expr(s, node->cond);
        if (!type_is_scalar(ct))
            ERR(s, node->line, "ternary condition must be scalar");
        Type *tt = analyze_expr(s, node->then);
        Type *et = analyze_expr(s, node->els);
        tt = decay_of(node->then);
        et = decay_of(node->els);

        if (type_is_arithmetic(tt) && type_is_arithmetic(et))
            return set_ty(node, type_usual_arith_conv(tt, et));
        if (tt->kind == TY_VOID && et->kind == TY_VOID)
            return set_ty(node, ty_void);
        if (tt->kind == TY_PTR  && et->kind == TY_PTR)
            return set_ty(node, tt);
        return set_ty(node, tt);
    }

    /* --- Array subscript --- */
    case ND_INDEX: {
        /* Parser stores operands in union fields; also sync flat fields */
        Node *arr_node = node->index.arr ? node->index.arr : node->lhs;
        Node *idx_node = node->index.idx ? node->index.idx : node->rhs;
        node->lhs = arr_node;
        node->rhs = idx_node;
        node->index.arr = arr_node;
        node->index.idx = idx_node;

        analyze_expr(s, arr_node);
        Type *it = analyze_expr(s, idx_node);
        Type *ld = decay_of(arr_node);
        Type *rd = decay_of(idx_node);

        Type *ptr_ty = NULL;
        if (ld && ld->kind == TY_PTR)      ptr_ty = ld;
        else if (rd && rd->kind == TY_PTR) { ptr_ty = rd; it = ld; }

        if (!ptr_ty) {
            ERR(s, node->line, "subscript of non-array/pointer type");
            return set_ty(node, ty_int);
        }
        if (!it || !type_is_integer(it))
            ERR(s, node->line, "array subscript is not an integer");
        return set_ty(node, ptr_ty->base);
    }

    /* --- Member access: expr.member --- */
    case ND_MEMBER: {
        /* Sync union → flat */
        if (!node->lhs)         node->lhs         = node->member_access.obj;
        if (!node->member_name) node->member_name  = (char *)node->member_access.member;
        Type *obj_ty = analyze_expr(s, node->lhs);
        if (obj_ty->kind != TY_STRUCT && obj_ty->kind != TY_UNION) {
            ERR(s, node->line,
                "request for member '%s' in non-struct/union type",
                node->member_name ? node->member_name : "?");
            return set_ty(node, ty_int);
        }
        /* Re-resolve obj_ty in case it's a forward-declared struct */
        obj_ty = resolve_type(s, obj_ty);
        for (Member *m = obj_ty->members; m; m = m->next) {
            if (m->name && node->member_name &&
                strcmp(m->name, node->member_name) == 0) {
                node->member = m;
                node->member_access.resolved = m; /* sync back for codegen */
                return set_ty(node, m->ty);
            }
        }
        ERR(s, node->line, "struct/union has no member named '%s'",
            node->member_name ? node->member_name : "?");
        return set_ty(node, ty_int);
    }

    /* --- Pointer member access: expr->member --- */
    case ND_ARROW: {
        /* Sync union → flat */
        if (!node->lhs)         node->lhs        = node->member_access.obj;
        if (!node->member_name) node->member_name = (char *)node->member_access.member;
        analyze_expr(s, node->lhs);
        Type *pd = decay_of(node->lhs);
        if (pd->kind != TY_PTR ||
            (pd->base->kind != TY_STRUCT && pd->base->kind != TY_UNION)) {
            ERR(s, node->line,
                "request for member '%s' in non-pointer-to-struct/union",
                node->member_name ? node->member_name : "?");
            return set_ty(node, ty_int);
        }
        Type *struct_ty = pd->base;
        for (Member *m = struct_ty->members; m; m = m->next) {
            if (m->name && node->member_name &&
                strcmp(m->name, node->member_name) == 0) {
                node->member = m;
                node->member_access.resolved = m; /* sync back for codegen */
                return set_ty(node, m->ty);
            }
        }
        ERR(s, node->line, "struct/union has no member named '%s'",
            node->member_name ? node->member_name : "?");
        return set_ty(node, ty_int);
    }

    /* --- Function call --- */
    case ND_CALL:
        return analyze_call(s, node);

    /* --- Initializer list --- */
    case ND_INIT_LIST: {
        for (int i = 0; i < node->nitems; i++)
            analyze_expr(s, node->items[i]);
        return set_ty(node, ty_void); /* type set later by context */
    }

    default:
        ERR(s, node->line,
            "internal: unknown expression node kind %d", (int)node->kind);
        return set_ty(node, ty_int);
    }
}

/* =========================================================
 * Function call type-checking
 * ========================================================= */

static Type *analyze_call(Sema *s, Node *node)
{
    /* Sync union → flat: parser sets call.* fields */
    if (!node->func_expr) {
        node->func_expr = node->call.callee;
        node->nargs     = node->call.arg_count;
        node->args      = node->call.args;
    }
    Type *callee_ty = analyze_expr(s, node->func_expr);
    callee_ty = type_decay(callee_ty);

    if (callee_ty->kind == TY_PTR && callee_ty->base &&
        callee_ty->base->kind == TY_FUNC)
        callee_ty = callee_ty->base;

    if (callee_ty->kind != TY_FUNC) {
        ERR(s, node->line,
            "called object is not a function or function pointer");
        for (int i = 0; i < node->nargs; i++)
            analyze_expr(s, node->args[i]);
        return set_ty(node, ty_int);
    }

    /* Match actual args against parameters */
    int i = 0;
    for (; i < node->nargs && i < callee_ty->param_count; i++) {
        Type *arg_ty = analyze_expr(s, node->args[i]);
        arg_ty = decay_of(node->args[i]);
        Type *param_ty = callee_ty->params[i];
        if (!sema_is_assignable(s, param_ty, arg_ty)) {
            char pbuf[64], abuf[64];
            type_to_str(param_ty, pbuf, sizeof(pbuf));
            type_to_str(arg_ty,   abuf, sizeof(abuf));
            ERR(s, node->line,
                "argument %d: cannot convert '%s' to parameter type '%s'",
                i + 1, abuf, pbuf);
        } else if (!type_is_compatible(param_ty, arg_ty)) {
            node->args[i] = implicit_cast_node(node->args[i], param_ty);
        }
    }

    /* Too few */
    if (i < callee_ty->param_count && !callee_ty->is_variadic)
        ERR(s, node->line, "too few arguments to function call");

    /* Variadic / extra args: default promotions (§6.5.2.2 p7) */
    for (; i < node->nargs; i++) {
        Type *arg_ty = analyze_expr(s, node->args[i]);
        arg_ty = type_integer_promote(arg_ty);
        if (arg_ty->kind == TY_FLOAT) arg_ty = ty_double;
        node->args[i] = implicit_cast_node(node->args[i], arg_ty);
    }

    /* Too many (non-variadic) */
    if (i < node->nargs && !callee_ty->is_variadic)
        ERR(s, node->line, "too many arguments to function call");

    Type *ret = callee_ty->return_ty ? callee_ty->return_ty : ty_void;
    return set_ty(node, ret);
}

/* =========================================================
 * Constant expression evaluator
 * ========================================================= */

static int eval_const(Sema *s, Node *expr, long long *out)
{
    if (!expr) return 0;

    switch (expr->kind) {
    case ND_INT_LIT:
        *out = expr->ival;
        return 1;

    case ND_CHAR_LIT:
        *out = expr->ival; /* char literal value is stored in ival */
        return 1;

    case ND_NEG: { long long v; if (!eval_const(s,expr->lhs,&v)) return 0; *out=-v; return 1; }
    case ND_BITNOT: { long long v; if (!eval_const(s,expr->lhs,&v)) return 0; *out=~v; return 1; }
    case ND_LOGNOT: { long long v; if (!eval_const(s,expr->lhs,&v)) return 0; *out=!v; return 1; }

#define BINOP(op) \
    { long long l,r; \
      if (!eval_const(s,expr->lhs,&l)) return 0; \
      if (!eval_const(s,expr->rhs,&r)) return 0; \
      *out = (l op r); return 1; }

    case ND_ADD:    BINOP(+)
    case ND_SUB:    BINOP(-)
    case ND_MUL:    BINOP(*)
    case ND_BITAND: BINOP(&)
    case ND_BITOR:  BINOP(|)
    case ND_BITXOR: BINOP(^)
    case ND_SHL:    BINOP(<<)
    case ND_SHR:    BINOP(>>)
    case ND_EQ:     BINOP(==)
    case ND_NEQ:    BINOP(!=)
    case ND_LT:     BINOP(<)
    case ND_LE:     BINOP(<=)
    case ND_GT:     BINOP(>)
    case ND_GE:     BINOP(>=)
    case ND_LOGAND: BINOP(&&)
    case ND_LOGOR:  BINOP(||)
#undef BINOP

    case ND_DIV: {
        long long l, r;
        if (!eval_const(s, expr->lhs, &l)) return 0;
        if (!eval_const(s, expr->rhs, &r)) return 0;
        if (r == 0) { ERR(s, expr->line, "division by zero in constant expression"); return 0; }
        *out = l / r; return 1;
    }
    case ND_MOD: {
        long long l, r;
        if (!eval_const(s, expr->lhs, &l)) return 0;
        if (!eval_const(s, expr->rhs, &r)) return 0;
        if (r == 0) { ERR(s, expr->line, "modulo by zero in constant expression"); return 0; }
        *out = l % r; return 1;
    }

    case ND_TERNARY: {
        long long c;
        if (!eval_const(s, expr->cond, &c)) return 0;
        return eval_const(s, c ? expr->then : expr->els, out);
    }

    case ND_CAST: {
        long long v;
        if (!eval_const(s, expr->lhs, &v)) return 0;
        if (expr->cast_ty) {
            int sz = type_sizeof_val(expr->cast_ty);
            if (sz == 1) v = (long long)(signed char)v;
            else if (sz == 2) v = (long long)(short)v;
            else if (sz == 4) v = (long long)(int)v;
        }
        *out = v;
        return 1;
    }

    /* Enum constant */
    case ND_VAR: {
        if (!expr->var) return 0;
        Symbol *sym = symtable_lookup(s->symtable, expr->var->name);
        if (sym && sym->kind == SYM_ENUM_CONST) {
            *out = sym->enum_val;
            return 1;
        }
        return 0;
    }

    default:
        return 0;
    }
}

int sema_eval_const_int(Sema *s, Node *expr, long long *out)
{
    return eval_const(s, expr, out);
}

/* =========================================================
 * Assignment compatibility (C11 §6.5.16)
 * ========================================================= */

int sema_is_assignable(Sema *s, Type *to, Type *from)
{
    (void)s;
    if (!to || !from) return 0;
    if (type_is_compatible(to, from)) return 1;
    if (type_is_arithmetic(to) && type_is_arithmetic(from)) return 1;

    /* void* ↔ any pointer */
    if (to->kind == TY_PTR && from->kind == TY_PTR) {
        if (to->base->kind == TY_VOID || from->base->kind == TY_VOID)
            return 1;
        return type_is_compatible(to->base, from->base);
    }

    /* integer → pointer (e.g. NULL = 0) */
    if (to->kind == TY_PTR && type_is_integer(from)) return 1;

    /* array decays to pointer */
    if (to->kind == TY_PTR && from->kind == TY_ARRAY)
        return type_is_compatible(to->base, from->base);

    /* function decays to pointer-to-function */
    if (to->kind == TY_PTR && from->kind == TY_FUNC)
        return 1;
    if (to->kind == TY_FUNC && from->kind == TY_PTR && from->base && from->base->kind == TY_FUNC)
        return 1;

    /* enum ↔ integer */
    if (to->kind == TY_ENUM && type_is_integer(from)) return 1;
    if (type_is_integer(to) && from->kind == TY_ENUM) return 1;

    /* _Bool: any scalar */
    if (to->kind == TY_BOOL && type_is_scalar(from)) return 1;

    return 0;
}

Node *sema_coerce(Sema *s, Node *expr, Type *to)
{
    (void)s;
    if (!expr || !to) return expr;
    if (type_is_compatible(expr->ty, to)) return expr;
    return implicit_cast_node(expr, to);
}
