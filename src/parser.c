/*
 * parser.c — C11 Recursive-Descent Parser
 *
 * Implements the full C11 expression/statement/declaration grammar.
 * Produces an AST using the node types defined in ast.h.
 */

#include "../include/parser.h"
#include "../include/ast.h"
#include "../include/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

/* =========================================================
 * Internal helpers
 * ========================================================= */

/* ----- error reporting ----- */
static void parse_error(Parser *p, int line, int col, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", p->filename ? p->filename : "<input>", line, col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    p->error_count++;
}

/* ----- token helpers ----- */
static Token peek_tok(Parser *p) {
    if (!p->peek_valid) {
        p->peek       = lexer_next(p->lexer);
        p->peek_valid = true;
    }
    return p->peek;
}

static Token advance_tok(Parser *p) {
    if (p->peek_valid) {
        p->cur       = p->peek;
        p->peek_valid = false;
    } else {
        p->cur = lexer_next(p->lexer);
    }
    return p->cur;
}

static bool check(Parser *p, TokenKind k) {
    return peek_tok(p).kind == k;
}

static bool match(Parser *p, TokenKind k) {
    if (check(p, k)) { advance_tok(p); return true; }
    return false;
}

static Token expect(Parser *p, TokenKind k) {
    Token t = peek_tok(p);
    if (t.kind != k) {
        parse_error(p, t.line, t.col,
                    "expected '%s', got '%s'",
                    token_kind_name(k), token_kind_name(t.kind));
    }
    return advance_tok(p);
}

/* ----- node allocation ----- */
static Node *alloc_node(Parser *p, NodeKind kind, int line, int col) {
    Node *n = arena_alloc_node(p->arena);
    n->kind = kind;
    n->line = line;
    n->col  = col;
    return n;
}

static Type *alloc_type(Parser *p, TypeKind kind) {
    Type *t = arena_alloc_type(p->arena);
    t->kind = kind;
    return t;
}

/* ----- dynamic arrays for children (use arena for storage) ----- */
static void node_array_push(Arena *a, Node ***arr, int *count, int *cap, Node *n) {
    if (*count >= *cap) {
        int new_cap = (*cap == 0) ? 4 : *cap * 2;
        Node **new_arr = arena_alloc(a, (size_t)new_cap * sizeof(Node *));
        if (*arr) memcpy(new_arr, *arr, (size_t)*count * sizeof(Node *));
        *arr = new_arr;
        *cap = new_cap;
    }
    (*arr)[(*count)++] = n;
}

/* =========================================================
 * Typedef / scope management
 * ========================================================= */

static void scope_push(Parser *p) {
    ParseScope *s = arena_alloc(p->arena, sizeof(ParseScope));
    s->entries = NULL;
    s->outer   = p->scope;
    p->scope   = s;
}

static void scope_pop(Parser *p) {
    if (p->scope) p->scope = p->scope->outer;
}

static void typedef_register(Parser *p, const char *name, Type *ty) {
    if (!p->scope) { scope_push(p); }
    TypedefEntry *e = arena_alloc(p->arena, sizeof(TypedefEntry));
    e->name    = arena_strdup(p->arena, name);
    e->ty      = ty;
    e->next    = p->scope->entries;
    p->scope->entries = e;
}

static Type *typedef_lookup(Parser *p, const char *name) {
    for (ParseScope *s = p->scope; s; s = s->outer) {
        for (TypedefEntry *e = s->entries; e; e = e->next) {
            if (strcmp(e->name, name) == 0) return e->ty;
        }
    }
    return NULL;
}

static bool is_typedef_name(Parser *p, const char *name) {
    return typedef_lookup(p, name) != NULL;
}

/* =========================================================
 * Forward declarations
 * ========================================================= */

static Node *parse_expr(Parser *p);
static Node *parse_assign_expr(Parser *p);
static Node *parse_initializer(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_compound_stmt(Parser *p);
static Type *parse_declaration_specifiers(Parser *p, bool *is_typedef_out,
                                           bool *is_extern_out, bool *is_static_out,
                                           bool *is_inline_out);
static Type *parse_declarator(Parser *p, Type *base, char **name_out);
static Node *parse_external_decl(Parser *p);

/* =========================================================
 * Type helpers
 * ========================================================= */

static int type_size_of(TypeKind k) {
    switch (k) {
    case TY_VOID:   return 0;
    case TY_BOOL:   return 1;
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR: return 1;
    case TY_SHORT: case TY_USHORT: return 2;
    case TY_INT: case TY_UINT: case TY_ENUM: return 4;
    case TY_LONG: case TY_ULONG: return 8;   /* LP64 */
    case TY_LLONG: case TY_ULLONG: return 8;
    case TY_FLOAT:  return 4;
    case TY_DOUBLE: return 8;
    case TY_LDOUBLE: return 16;
    case TY_PTR:    return 8;
    default: return 0;
    }
}

static int type_align_of(TypeKind k) {
    switch (k) {
    case TY_VOID:   return 1;
    case TY_BOOL:   return 1;
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR: return 1;
    case TY_SHORT: case TY_USHORT: return 2;
    case TY_INT: case TY_UINT: case TY_ENUM: return 4;
    case TY_LONG: case TY_ULONG: return 8;
    case TY_LLONG: case TY_ULLONG: return 8;
    case TY_FLOAT:  return 4;
    case TY_DOUBLE: return 8;
    case TY_LDOUBLE: return 16;
    case TY_PTR:    return 8;
    default: return 1;
    }
}

static Type *make_prim(Parser *p, TypeKind k) {
    Type *t  = alloc_type(p, k);
    t->size  = type_size_of(k);
    t->align = type_align_of(k);
    return t;
}

static Type *make_ptr(Parser *p, Type *base) {
    Type *t  = alloc_type(p, TY_PTR);
    t->base  = base;
    t->size  = 8;
    t->align = 8;
    return t;
}

static Type *make_array(Parser *p, Type *elem, int len) {
    Type *t        = alloc_type(p, TY_ARRAY);
    t->base        = elem;
    t->array_len   = len;
    t->size        = elem->size * (len >= 0 ? len : 0);
    t->align       = elem->align;
    return t;
}

/* =========================================================
 * Declaration specifier parsing
 * ========================================================= */

static Type *parse_struct_or_union(Parser *p, bool is_struct);
static Type *parse_enum(Parser *p);

static Type *parse_declaration_specifiers(Parser *p,
                                           bool *is_typedef_out,
                                           bool *is_extern_out,
                                           bool *is_static_out,
                                           bool *is_inline_out)
{
    if (is_typedef_out) *is_typedef_out = false;
    if (is_extern_out)  *is_extern_out  = false;
    if (is_static_out)  *is_static_out  = false;
    if (is_inline_out)  *is_inline_out  = false;

    /* Accumulated type specifiers */
    int n_void = 0, n_char = 0, n_short = 0, n_int = 0, n_long = 0;
    int n_float = 0, n_double = 0, n_signed = 0, n_unsigned = 0;
    int n_bool = 0;
    bool seen_specifier = false;
    bool seen_storage   = false;
    Type *struct_ty = NULL;  /* for struct/union/enum types */

    bool is_const = false, is_volatile = false;

    while (true) {
        Token t = peek_tok(p);
        switch (t.kind) {
        /* storage class */
        case TK_TYPEDEF:   advance_tok(p); if (is_typedef_out) *is_typedef_out = true; seen_storage = true; break;
        case TK_EXTERN:    advance_tok(p); if (is_extern_out)  *is_extern_out  = true; seen_storage = true; break;
        case TK_STATIC:    advance_tok(p); if (is_static_out)  *is_static_out  = true; seen_storage = true; break;
        case TK_REGISTER:  advance_tok(p); seen_storage = true; break;
        case TK_AUTO:      advance_tok(p); seen_storage = true; break;

        /* function specifiers */
        case TK_INLINE:   advance_tok(p); if (is_inline_out) *is_inline_out = true; break;
        case TK_NORETURN: advance_tok(p); break;

        /* qualifiers */
        case TK_CONST:    advance_tok(p); is_const    = true; break;
        case TK_VOLATILE: advance_tok(p); is_volatile = true; break;
        case TK_RESTRICT: advance_tok(p); break;
        case TK_ATOMIC:   advance_tok(p); break;

        /* type specifiers */
        case TK_VOID:     advance_tok(p); n_void++;     seen_specifier = true; break;
        case TK_CHAR_KW:  advance_tok(p); n_char++;     seen_specifier = true; break;
        case TK_SHORT:    advance_tok(p); n_short++;    seen_specifier = true; break;
        case TK_INT_KW:   advance_tok(p); n_int++;      seen_specifier = true; break;
        case TK_LONG:     advance_tok(p); n_long++;     seen_specifier = true; break;
        case TK_FLOAT_KW: advance_tok(p); n_float++;    seen_specifier = true; break;
        case TK_DOUBLE:   advance_tok(p); n_double++;   seen_specifier = true; break;
        case TK_SIGNED:   advance_tok(p); n_signed++;   seen_specifier = true; break;
        case TK_UNSIGNED: advance_tok(p); n_unsigned++; seen_specifier = true; break;
        case TK_BOOL:     advance_tok(p); n_bool++;     seen_specifier = true; break;

        case TK_STRUCT:
            advance_tok(p);
            struct_ty = parse_struct_or_union(p, true);
            seen_specifier = true;
            goto done;

        case TK_UNION:
            advance_tok(p);
            struct_ty = parse_struct_or_union(p, false);
            seen_specifier = true;
            goto done;

        case TK_ENUM:
            advance_tok(p);
            struct_ty = parse_enum(p);
            seen_specifier = true;
            goto done;

        case TK_IDENT:
            /* Could be a typedef name */
            if (!seen_specifier && is_typedef_name(p, t.sval)) {
                advance_tok(p);
                /* p->cur is now the consumed ident token */
                Type *td = typedef_lookup(p, p->cur.sval ? p->cur.sval : "");
                if (!td) td = make_prim(p, TY_INT); /* fallback */
                struct_ty      = td;
                seen_specifier = true;
                goto done;
            }
            goto done;

        default:
            goto done;
        }
        (void)seen_storage;
    }

done: ;
    /* Combine specifiers into a TypeKind */
    TypeKind kind;
    if (struct_ty) {
        struct_ty->is_const    = is_const;
        struct_ty->is_volatile = is_volatile;
        return struct_ty;
    }

    if (n_void)   { kind = TY_VOID; }
    else if (n_bool) { kind = TY_BOOL; }
    else if (n_float) { kind = TY_FLOAT; }
    else if (n_double) {
        kind = (n_long >= 1) ? TY_LDOUBLE : TY_DOUBLE;
    }
    else if (n_char) {
        kind = n_unsigned ? TY_UCHAR : (n_signed ? TY_SCHAR : TY_CHAR);
    }
    else if (n_short) {
        kind = n_unsigned ? TY_USHORT : TY_SHORT;
    }
    else if (n_long >= 2) {
        kind = n_unsigned ? TY_ULLONG : TY_LLONG;
    }
    else if (n_long == 1) {
        kind = n_unsigned ? TY_ULONG : TY_LONG;
    }
    else if (n_unsigned) { kind = TY_UINT; }
    else if (n_signed)   { kind = TY_INT; }
    else if (n_int || seen_specifier) { kind = TY_INT; }
    else {
        /* No specifier seen (could be just a qualifier) */
        kind = TY_INT; /* implicit int */
    }

    Type *ty         = make_prim(p, kind);
    ty->is_const     = is_const;
    ty->is_volatile  = is_volatile;
    return ty;
}

/* =========================================================
 * Struct / union parsing
 * ========================================================= */

static Type *parse_struct_or_union(Parser *p, bool is_struct) {
    Token t = peek_tok(p);
    const char *tag = NULL;

    if (t.kind == TK_IDENT) {
        tag = arena_strdup(p->arena, t.sval);
        advance_tok(p);
        t   = peek_tok(p);
    }

    Type *ty    = alloc_type(p, is_struct ? TY_STRUCT : TY_UNION);
    ty->tag     = tag;
    ty->size    = 0;
    ty->align   = 1;

    if (t.kind != TK_LBRACE) {
        /* Forward reference */
        ty->is_complete = false;
        return ty;
    }

    advance_tok(p); /* consume '{' */
    ty->is_complete = true;

    Member *head = NULL, *tail = NULL;
    int offset = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        bool dummy1, dummy2, dummy3, dummy4;
        Type *base = parse_declaration_specifiers(p, &dummy1, &dummy2, &dummy3, &dummy4);

        /* May have multiple declarators: int a, b; */
        do {
            char *mname = NULL;
            Type *mty   = parse_declarator(p, base, &mname);

            Member *m = arena_alloc(p->arena, sizeof(Member));
            m->name      = arena_strdup(p->arena, mname ? mname : "");
            m->ty        = mty;
            m->bit_width = -1;
            m->next      = NULL;

            /* Compute alignment padding */
            int malign = mty->align > 0 ? mty->align : 1;
            if (!is_struct) {
                /* union: all at offset 0 */
                m->offset = 0;
                if (mty->size > ty->size) ty->size = mty->size;
            } else {
                offset = ((offset + malign - 1) / malign) * malign;
                m->offset = offset;
                offset   += mty->size;
            }
            if (malign > ty->align) ty->align = malign;

            if (!head) head = tail = m;
            else { tail->next = m; tail = m; }

            if (check(p, TK_COMMA)) advance_tok(p);
            else break;
        } while (!check(p, TK_SEMICOLON) && !check(p, TK_RBRACE));

        expect(p, TK_SEMICOLON);
    }

    expect(p, TK_RBRACE);

    ty->members = head;
    if (is_struct) {
        /* Final size with trailing padding */
        int align = ty->align > 0 ? ty->align : 1;
        ty->size  = ((offset + align - 1) / align) * align;
    }
    return ty;
}

/* =========================================================
 * Enum parsing
 * ========================================================= */

static Type *parse_enum(Parser *p) {
    Token t = peek_tok(p);
    const char *tag = NULL;

    if (t.kind == TK_IDENT) {
        tag = arena_strdup(p->arena, t.sval);
        advance_tok(p);
        t   = peek_tok(p);
    }

    Type *ty    = alloc_type(p, TY_ENUM);
    ty->tag     = tag;
    ty->size    = 4;
    ty->align   = 4;

    if (t.kind != TK_LBRACE) {
        ty->is_complete = false;
        return ty;
    }

    advance_tok(p); /* '{' */
    ty->is_complete = true;

    long long next_val = 0;
    EnumConst *head = NULL, *tail = NULL;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Token name_tok = expect(p, TK_IDENT);
        long long val  = next_val;

        if (match(p, TK_EQ)) {
            /* constant expression — simplified: only integer literal */
            Token vt = advance_tok(p);
            if (vt.kind == TK_INT) val = (long long)vt.ival;
            else if (vt.kind == TK_MINUS) {
                Token vt2 = advance_tok(p);
                if (vt2.kind == TK_INT) val = -(long long)vt2.ival;
            }
        }
        next_val = val + 1;

        EnumConst *ec = arena_alloc(p->arena, sizeof(EnumConst));
        ec->name  = arena_strdup(p->arena, name_tok.sval ? name_tok.sval : "");
        ec->value = val;
        ec->next  = NULL;

        if (!head) head = tail = ec;
        else { tail->next = ec; tail = ec; }

        /* Register enum constant as a typedef? No — we'd need a symbol table.
           For now just note it. */

        if (!match(p, TK_COMMA)) break;
        if (check(p, TK_RBRACE)) break; /* trailing comma */
    }

    expect(p, TK_RBRACE);
    ty->enumerators = head;
    return ty;
}

/* =========================================================
 * Declarator parsing
 *
 * declarator: pointer* direct-declarator
 * direct-declarator: IDENT | '(' declarator ')' | ... '[' ... ']' | '(' ... ')'
 * ========================================================= */

static Type *parse_abstract_declarator(Parser *p, Type *base);

/* Parse pointer prefix: returns the outermost type wrapping `base` */
static Type *parse_pointer_prefix(Parser *p, Type *base) {
    while (check(p, TK_STAR)) {
        advance_tok(p); /* consume '*' */
        bool is_const = false, is_volatile = false;
        while (true) {
            if      (match(p, TK_CONST))    { is_const    = true; }
            else if (match(p, TK_VOLATILE)) { is_volatile = true; }
            else if (match(p, TK_RESTRICT)) { }
            else break;
        }
        base = make_ptr(p, base);
        base->is_const    = is_const;
        base->is_volatile = is_volatile;
    }
    return base;
}

/* Parse suffix: array or function parameter list */
static Type *parse_declarator_suffix(Parser *p, Type *ty) {
    while (true) {
        if (check(p, TK_LBRACKET)) {
            advance_tok(p); /* '[' */
            int len = -1;
            if (!check(p, TK_RBRACKET)) {
                /* Constant expression expected */
                Token nt = peek_tok(p);
                if (nt.kind == TK_INT) {
                    len = (int)nt.ival;
                    advance_tok(p);
                } else if (nt.kind == TK_STAR) {
                    advance_tok(p); /* VLA '*' */
                } else {
                    /* Skip expression for simplicity */
                    Node *e = parse_assign_expr(p);
                    (void)e;
                    /* Try to evaluate constant expressions */
                    if (e && e->kind == ND_INT_LIT) len = (int)e->ival;
                }
            }
            expect(p, TK_RBRACKET);
            ty = make_array(p, ty, len);
        } else if (check(p, TK_LPAREN)) {
            /* Function declarator */
            advance_tok(p); /* '(' */
            Type *func_ty = alloc_type(p, TY_FUNC);
            func_ty->base = ty; /* return type */
            func_ty->size  = 8; /* function pointer size */
            func_ty->align = 8;

            /* Parse parameters */
            int   pcap   = 4;
            int   pcount = 0;
            Type  **ptypes  = arena_alloc(p->arena, (size_t)pcap * sizeof(Type *));
            char  **pnames  = arena_alloc(p->arena, (size_t)pcap * sizeof(char *));
            bool variadic = false;

            if (!check(p, TK_RPAREN)) {
                /* Check for (void) */
                if (peek_tok(p).kind == TK_VOID) {
                    Token v = advance_tok(p);
                    (void)v;
                    if (!check(p, TK_RPAREN)) {
                        /* It's a parameter of type void* etc */
                        /* handle below */
                    }
                } else {
                    do {
                        if (check(p, TK_ELLIPSIS)) {
                            advance_tok(p);
                            variadic = true;
                            break;
                        }
                        bool d1, d2, d3, d4;
                        Type *pbase = parse_declaration_specifiers(p, &d1, &d2, &d3, &d4);
                        char *pname = NULL;
                        Type *pty   = parse_declarator(p, pbase, &pname);

                        if (pcount >= pcap) {
                            pcap *= 2;
                            Type **np = arena_alloc(p->arena, (size_t)pcap * sizeof(Type *));
                            char **nn = arena_alloc(p->arena, (size_t)pcap * sizeof(char *));
                            memcpy(np, ptypes, (size_t)pcount * sizeof(Type *));
                            memcpy(nn, pnames, (size_t)pcount * sizeof(char *));
                            ptypes = np; pnames = nn;
                        }
                        ptypes[pcount]   = pty;
                        pnames[pcount++] = pname ? arena_strdup(p->arena, pname) : NULL;
                    } while (match(p, TK_COMMA));
                }
            }

            expect(p, TK_RPAREN);
            func_ty->params       = ptypes;
            func_ty->param_names  = pnames;
            func_ty->param_count  = pcount;
            func_ty->is_variadic  = variadic;
            ty = func_ty;
        } else {
            break;
        }
    }
    return ty;
}

static Type *parse_declarator(Parser *p, Type *base, char **name_out) {
    if (name_out) *name_out = NULL;

    /* pointer prefix */
    Type *ptr_type = parse_pointer_prefix(p, base);

    /* grouped declarator: ( declarator ) */
    if (check(p, TK_LPAREN)) {
        advance_tok(p);
        /* We need to handle this carefully.
           Parse inner declarator, but defer the base type connection. */
        /* For simplicity: parse direct declarator name */
        Type *inner_base = alloc_type(p, TY_INT); /* placeholder */
        char *inner_name = NULL;
        Type *inner      = parse_declarator(p, inner_base, &inner_name);
        expect(p, TK_RPAREN);
        if (name_out) *name_out = inner_name;
        /* Apply suffixes to ptr_type, then reconnect inner */
        Type *suffix = parse_declarator_suffix(p, ptr_type);
        /* Reconnect: replace inner_base with suffix in the inner chain */
        /* Simple approach: if inner == inner_base, just use suffix */
        if (inner == inner_base) return suffix;
        /* Walk inner chain to find inner_base and replace */
        Type *cur_ty = inner;
        while (cur_ty) {
            if (cur_ty->base == inner_base) { cur_ty->base = suffix; break; }
            cur_ty = cur_ty->base;
        }
        return inner;
    }

    /* IDENT */
    if (peek_tok(p).kind == TK_IDENT) {
        Token nt = advance_tok(p);
        if (name_out) *name_out = arena_strdup(p->arena, nt.sval ? nt.sval : "");
    }

    return parse_declarator_suffix(p, ptr_type);
}

static Type *parse_abstract_declarator(Parser *p, Type *base) {
    char *dummy = NULL;
    return parse_declarator(p, base, &dummy);
}

/* =========================================================
 * Type-name (used in casts, sizeof, _Alignof)
 * ========================================================= */

Type *parser_parse_type_name(Parser *p) {
    bool d1, d2, d3, d4;
    Type *base = parse_declaration_specifiers(p, &d1, &d2, &d3, &d4);
    return parse_abstract_declarator(p, base);
}

/* =========================================================
 * Expression parsing (precedence climbing)
 * ========================================================= */

static Node *parse_primary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_cast(Parser *p);
static Node *parse_mul(Parser *p);
static Node *parse_add(Parser *p);
static Node *parse_shift(Parser *p);
static Node *parse_relational(Parser *p);
static Node *parse_equality(Parser *p);
static Node *parse_bitand(Parser *p);
static Node *parse_bitxor(Parser *p);
static Node *parse_bitor(Parser *p);
static Node *parse_logand(Parser *p);
static Node *parse_logor(Parser *p);
static Node *parse_conditional(Parser *p);

/* Helpers to make binary/unary nodes — set BOTH union and flat fields */
static Node *binop(Parser *p, NodeKind kind, Node *left, Node *right) {
    Node *n         = alloc_node(p, kind, left->line, left->col);
    n->binary.left  = left;
    n->binary.right = right;
    n->lhs          = left;   /* flat alias */
    n->rhs          = right;  /* flat alias */
    return n;
}

static Node *unop(Parser *p, NodeKind kind, Node *operand, int line, int col) {
    Node *n          = alloc_node(p, kind, line, col);
    n->unary.operand = operand;
    n->lhs           = operand; /* flat alias */
    return n;
}

static Node *parse_primary(Parser *p) {
    Token t = peek_tok(p);

    if (t.kind == TK_INT) {
        advance_tok(p);
        Node *n  = alloc_node(p, ND_INT_LIT, t.line, t.col);
        n->ival  = (long long)t.ival;
        return n;
    }

    if (t.kind == TK_FLOAT) {
        advance_tok(p);
        Node *n  = alloc_node(p, ND_FLOAT_LIT, t.line, t.col);
        n->fval  = t.fval;
        return n;
    }

    if (t.kind == TK_CHAR) {
        advance_tok(p);
        Node *n  = alloc_node(p, ND_CHAR_LIT, t.line, t.col);
        n->ival  = t.cval;
        return n;
    }

    if (t.kind == TK_STRING) {
        advance_tok(p);
        /* Adjacent string literal concatenation: "foo" "bar" → "foobar" */
        const char *s = t.sval ? t.sval : "";
        size_t slen = strlen(s);
        while (check(p, TK_STRING)) {
            Token next = peek_tok(p);
            advance_tok(p);
            const char *s2 = next.sval ? next.sval : "";
            size_t s2len = strlen(s2);
            char *buf = arena_alloc(p->arena, slen + s2len + 1);
            memcpy(buf, s, slen);
            memcpy(buf + slen, s2, s2len + 1);
            s    = buf;
            slen = slen + s2len;
        }
        Node *n  = alloc_node(p, ND_STR_LIT, t.line, t.col);
        n->sval  = (char *)s;
        n->slen  = (int)slen + 1; /* include NUL */
        return n;
    }

    if (t.kind == TK_IDENT) {
        advance_tok(p);
        /* Check for NULL macro */
        if (strcmp(t.sval ? t.sval : "", "NULL") == 0) {
            Node *n = alloc_node(p, ND_INT_LIT, t.line, t.col);
            n->ival = 0;
            return n;
        }
        Node *n         = alloc_node(p, ND_VAR, t.line, t.col);
        n->ident.name     = arena_strdup(p->arena, t.sval ? t.sval : "");
        return n;
    }

    if (t.kind == TK_LPAREN) {
        advance_tok(p);
        Node *n = parse_expr(p);
        expect(p, TK_RPAREN);
        return n;
    }

    /* Error recovery */
    parse_error(p, t.line, t.col, "unexpected token '%s' in expression",
                token_kind_name(t.kind));
    advance_tok(p);
    Node *n = alloc_node(p, ND_INT_LIT, t.line, t.col);
    n->ival = 0;
    return n;
}

static Node *apply_postfix_ops(Parser *p, Node *n) {
    while (true) {
        Token t = peek_tok(p);

        if (t.kind == TK_LPAREN) {
            /* Function call */
            advance_tok(p);
            Node *call = alloc_node(p, ND_CALL, t.line, t.col);
            call->call.callee    = n;
            call->call.args      = NULL;
            call->call.arg_count = 0;
            call->call.arg_cap   = 0;

            if (!check(p, TK_RPAREN)) {
                do {
                    Node *arg = parse_assign_expr(p);
                    node_array_push(p->arena, &call->call.args,
                                    &call->call.arg_count, &call->call.arg_cap, arg);
                } while (match(p, TK_COMMA));
            }
            expect(p, TK_RPAREN);
            n = call;

        } else if (t.kind == TK_LBRACKET) {
            /* Array index */
            advance_tok(p);
            Node *idx  = parse_expr(p);
            expect(p, TK_RBRACKET);
            Node *node      = alloc_node(p, ND_INDEX, t.line, t.col);
            node->index.arr = n;
            node->index.idx = idx;
            node->lhs       = n;   /* flat alias */
            node->rhs       = idx; /* flat alias */
            n = node;

        } else if (t.kind == TK_DOT) {
            advance_tok(p);
            Token mname = expect(p, TK_IDENT);
            Node *node          = alloc_node(p, ND_MEMBER, t.line, t.col);
            node->member_access.obj    = n;
            node->member_access.member = arena_strdup(p->arena, mname.sval ? mname.sval : "");
            n = node;

        } else if (t.kind == TK_ARROW) {
            advance_tok(p);
            Token mname         = expect(p, TK_IDENT);
            Node *node          = alloc_node(p, ND_ARROW, t.line, t.col);
            node->member_access.obj    = n;
            node->member_access.member = arena_strdup(p->arena, mname.sval ? mname.sval : "");
            n = node;

        } else if (t.kind == TK_PLUS_PLUS) {
            advance_tok(p);
            n = unop(p, ND_POST_INC, n, t.line, t.col);

        } else if (t.kind == TK_MINUS_MINUS) {
            advance_tok(p);
            n = unop(p, ND_POST_DEC, n, t.line, t.col);

        } else {
            break;
        }
    }
    return n;
}

static Node *parse_postfix(Parser *p) {
    return apply_postfix_ops(p, parse_primary(p));
}

/* ---- check if current tokens start a type-name ---- */
static bool is_type_start(Parser *p) {
    Token t = peek_tok(p);
    switch (t.kind) {
    case TK_VOID: case TK_CHAR_KW: case TK_SHORT: case TK_INT_KW:
    case TK_LONG: case TK_FLOAT_KW: case TK_DOUBLE: case TK_SIGNED:
    case TK_UNSIGNED: case TK_BOOL: case TK_STRUCT: case TK_UNION:
    case TK_ENUM: case TK_CONST: case TK_VOLATILE: case TK_RESTRICT:
    case TK_ATOMIC: case TK_COMPLEX:
        return true;
    case TK_IDENT:
        return is_typedef_name(p, t.sval ? t.sval : "");
    default:
        return false;
    }
}

static Node *parse_unary(Parser *p) {
    Token t = peek_tok(p);

    if (t.kind == TK_PLUS_PLUS) {
        advance_tok(p);
        return unop(p, ND_PRE_INC, parse_unary(p), t.line, t.col);
    }
    if (t.kind == TK_MINUS_MINUS) {
        advance_tok(p);
        return unop(p, ND_PRE_DEC, parse_unary(p), t.line, t.col);
    }
    if (t.kind == TK_AMP) {
        advance_tok(p);
        return unop(p, ND_ADDR, parse_cast(p), t.line, t.col);
    }
    if (t.kind == TK_STAR) {
        advance_tok(p);
        return unop(p, ND_DEREF, parse_cast(p), t.line, t.col);
    }
    if (t.kind == TK_PLUS) {
        advance_tok(p);
        return parse_cast(p); /* unary + is a no-op */
    }
    if (t.kind == TK_MINUS) {
        advance_tok(p);
        return unop(p, ND_NEG, parse_cast(p), t.line, t.col);
    }
    if (t.kind == TK_TILDE) {
        advance_tok(p);
        return unop(p, ND_BITNOT, parse_cast(p), t.line, t.col);
    }
    if (t.kind == TK_BANG) {
        advance_tok(p);
        return unop(p, ND_NOT, parse_cast(p), t.line, t.col);
    }
    if (t.kind == TK_SIZEOF) {
        advance_tok(p);
        if (check(p, TK_LPAREN)) {
            advance_tok(p); /* consume '(' */
            if (is_type_start(p)) {
                /* sizeof(type-name) */
                Type *ty = parser_parse_type_name(p);
                expect(p, TK_RPAREN);
                Node *n           = alloc_node(p, ND_SIZEOF_TYPE, t.line, t.col);
                n->sizeof_type.type = ty;
                return n;
            } else {
                /* sizeof(expr) — '(' already consumed, parse expr and ')' */
                Node *inner = parse_expr(p);
                expect(p, TK_RPAREN);
                /* Apply postfix just in case */
                inner = apply_postfix_ops(p, inner);
                return unop(p, ND_SIZEOF_EXPR, inner, t.line, t.col);
            }
        } else {
            Node *operand = parse_unary(p);
            return unop(p, ND_SIZEOF_EXPR, operand, t.line, t.col);
        }
    }
    if (t.kind == TK_ALIGNOF) {
        advance_tok(p);
        expect(p, TK_LPAREN);
        Type *ty = parser_parse_type_name(p);
        expect(p, TK_RPAREN);
        Node *n           = alloc_node(p, ND_ALIGNOF, t.line, t.col);
        n->sizeof_type.type = ty;
        return n;
    }

    return parse_postfix(p);
}

static Node *parse_cast(Parser *p) {
    /* Look for (type-name) */
    if (check(p, TK_LPAREN)) {
        /* Distinguish (type) expr from (expr) */
        /* Save lexer position — simplified: try parsing type name */
        Token saved_peek = peek_tok(p);
        /* We peek past '(' to see if it's a type start */
        /* This requires 2-token lookahead; use a heuristic */
        advance_tok(p); /* consume '(' */
        if (is_type_start(p)) {
            Type *ty = parser_parse_type_name(p);
            if (check(p, TK_RPAREN)) {
                advance_tok(p);
                /* It might be a compound literal: (T){...} */
                if (check(p, TK_LBRACE)) {
                    /* Compound literal — parse as init list */
                    Node *cast = alloc_node(p, ND_CAST, p->cur.line, p->cur.col);
                    cast->cast.to   = ty;
                    cast->cast.expr = parse_unary(p);
                    return cast;
                }
                Node *expr = parse_cast(p);
                Node *cast = alloc_node(p, ND_CAST, saved_peek.line, saved_peek.col);
                cast->cast.to   = ty;
                cast->cast.expr = expr;
                return cast;
            }
            /* Not a cast — restore. Can't easily restore; fall through to error */
            parse_error(p, saved_peek.line, saved_peek.col,
                        "expected ')' after type name in cast");
            return parse_unary(p);
        } else {
            /* Not a type → it's a parenthesised expression */
            /* But we consumed '(' already.  Parse expr and ')' */
            Node *expr = parse_expr(p);
            expect(p, TK_RPAREN);
            /* Apply postfix operators (e.g. (*p)++, (arr)[i], (fn)() ) */
            return apply_postfix_ops(p, expr);
        }
    }
    return parse_unary(p);
}

static Node *parse_mul(Parser *p) {
    Node *n = parse_cast(p);
    while (true) {
        Token t = peek_tok(p);
        NodeKind k;
        if      (t.kind == TK_STAR)    k = ND_MUL;
        else if (t.kind == TK_SLASH)   k = ND_DIV;
        else if (t.kind == TK_PERCENT) k = ND_MOD;
        else break;
        advance_tok(p);
        n = binop(p, k, n, parse_cast(p));
    }
    return n;
}

static Node *parse_add(Parser *p) {
    Node *n = parse_mul(p);
    while (true) {
        Token t = peek_tok(p);
        if      (t.kind == TK_PLUS)  { advance_tok(p); n = binop(p, ND_ADD, n, parse_mul(p)); }
        else if (t.kind == TK_MINUS) { advance_tok(p); n = binop(p, ND_SUB, n, parse_mul(p)); }
        else break;
    }
    return n;
}

static Node *parse_shift(Parser *p) {
    Node *n = parse_add(p);
    while (true) {
        Token t = peek_tok(p);
        if      (t.kind == TK_LSHIFT) { advance_tok(p); n = binop(p, ND_SHL, n, parse_add(p)); }
        else if (t.kind == TK_RSHIFT) { advance_tok(p); n = binop(p, ND_SHR, n, parse_add(p)); }
        else break;
    }
    return n;
}

static Node *parse_relational(Parser *p) {
    Node *n = parse_shift(p);
    while (true) {
        Token t = peek_tok(p);
        NodeKind k;
        if      (t.kind == TK_LT)    k = ND_LT;
        else if (t.kind == TK_GT)    k = ND_GT;
        else if (t.kind == TK_LT_EQ) k = ND_LE;
        else if (t.kind == TK_GT_EQ) k = ND_GE;
        else break;
        advance_tok(p);
        n = binop(p, k, n, parse_shift(p));
    }
    return n;
}

static Node *parse_equality(Parser *p) {
    Node *n = parse_relational(p);
    while (true) {
        Token t = peek_tok(p);
        if      (t.kind == TK_EQ_EQ)   { advance_tok(p); n = binop(p, ND_EQ, n, parse_relational(p)); }
        else if (t.kind == TK_BANG_EQ)  { advance_tok(p); n = binop(p, ND_NE, n, parse_relational(p)); }
        else break;
    }
    return n;
}

static Node *parse_bitand(Parser *p) {
    Node *n = parse_equality(p);
    while (check(p, TK_AMP)) {
        advance_tok(p);
        n = binop(p, ND_AND, n, parse_equality(p));
    }
    return n;
}

static Node *parse_bitxor(Parser *p) {
    Node *n = parse_bitand(p);
    while (check(p, TK_CARET)) {
        advance_tok(p);
        n = binop(p, ND_XOR, n, parse_bitand(p));
    }
    return n;
}

static Node *parse_bitor(Parser *p) {
    Node *n = parse_bitxor(p);
    while (check(p, TK_PIPE)) {
        advance_tok(p);
        n = binop(p, ND_OR, n, parse_bitxor(p));
    }
    return n;
}

static Node *parse_logand(Parser *p) {
    Node *n = parse_bitor(p);
    while (check(p, TK_AMP_AMP)) {
        advance_tok(p);
        n = binop(p, ND_LOGIC_AND, n, parse_bitor(p));
    }
    return n;
}

static Node *parse_logor(Parser *p) {
    Node *n = parse_logand(p);
    while (check(p, TK_PIPE_PIPE)) {
        advance_tok(p);
        n = binop(p, ND_LOGIC_OR, n, parse_logand(p));
    }
    return n;
}

static Node *parse_conditional(Parser *p) {
    Node *n = parse_logor(p);
    if (check(p, TK_QUESTION)) {
        Token t = advance_tok(p);
        Node *then = parse_expr(p);
        expect(p, TK_COLON);
        Node *else_ = parse_conditional(p);
        Node *cond_node       = alloc_node(p, ND_COND, t.line, t.col);
        cond_node->ternary.cond  = n;
        cond_node->ternary.then  = then;
        cond_node->ternary.else_ = else_;
        return cond_node;
    }
    return n;
}

static NodeKind assign_op_kind(TokenKind tk) {
    switch (tk) {
    case TK_EQ:         return ND_ASSIGN;
    case TK_PLUS_EQ:    return ND_ASSIGN_ADD;
    case TK_MINUS_EQ:   return ND_ASSIGN_SUB;
    case TK_STAR_EQ:    return ND_ASSIGN_MUL;
    case TK_SLASH_EQ:   return ND_ASSIGN_DIV;
    case TK_PERCENT_EQ: return ND_ASSIGN_MOD;
    case TK_AMP_EQ:     return ND_ASSIGN_AND;
    case TK_PIPE_EQ:    return ND_ASSIGN_OR;
    case TK_CARET_EQ:   return ND_ASSIGN_XOR;
    case TK_LSHIFT_EQ:  return ND_ASSIGN_SHL;
    case TK_RSHIFT_EQ:  return ND_ASSIGN_SHR;
    default:            return -1;
    }
}

static Node *parse_assign_expr(Parser *p) {
    Node *n = parse_conditional(p);
    Token t = peek_tok(p);
    NodeKind kind = assign_op_kind(t.kind);
    if ((int)kind >= 0) {
        advance_tok(p);
        Node *rhs = parse_assign_expr(p);
        return binop(p, kind, n, rhs);
    }
    return n;
}

static Node *parse_expr(Parser *p) {
    Node *n = parse_assign_expr(p);
    while (check(p, TK_COMMA)) {
        advance_tok(p);
        n = binop(p, ND_COMMA, n, parse_assign_expr(p));
    }
    return n;
}

/* =========================================================
 * Statement parsing
 * ========================================================= */

static Node *parse_local_decl(Parser *p);

static Node *parse_compound_stmt(Parser *p) {
    Token lbrace = expect(p, TK_LBRACE);
    Node *n = alloc_node(p, ND_COMPOUND, lbrace.line, lbrace.col);
    n->compound.stmts = NULL;
    n->compound.count = 0;
    n->compound.cap   = 0;

    scope_push(p);

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Node *stmt;
        if (is_type_start(p)) {
            stmt = parse_local_decl(p);
        } else {
            stmt = parse_stmt(p);
        }
        if (stmt) {
            node_array_push(p->arena, &n->compound.stmts,
                            &n->compound.count, &n->compound.cap, stmt);
        }
    }

    scope_pop(p);
    /* Sync flat fields used by sema */
    n->stmts     = n->compound.stmts;
    n->nstmts    = n->compound.count;
    n->cap_stmts = n->compound.cap;
    expect(p, TK_RBRACE);
    return n;
}

/* Parse a single initializer: either a braced init-list or an assign-expr */
static Node *parse_initializer(Parser *p) {
    if (check(p, TK_LBRACE)) {
        Token lb = peek_tok(p);
        advance_tok(p); /* consume '{' */
        Node *il = alloc_node(p, ND_INIT_LIST, lb.line, lb.col);
        il->init_list.items = NULL;
        il->init_list.count = 0;
        il->init_list.cap   = 0;
        while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
            Node *item = parse_initializer(p); /* recursive for nested */
            node_array_push(p->arena, &il->init_list.items,
                            &il->init_list.count, &il->init_list.cap, item);
            if (!match(p, TK_COMMA)) break;
            /* allow trailing comma */
        }
        expect(p, TK_RBRACE);
        return il;
    }
    return parse_assign_expr(p);
}

static Node *parse_local_decl(Parser *p) {
    bool is_typedef, is_extern, is_static, is_inline;
    Type *base = parse_declaration_specifiers(p, &is_typedef, &is_extern, &is_static, &is_inline);
    (void)is_extern; (void)is_inline;

    /* May be just "struct S { ... };" with no declarator */
    if (check(p, TK_SEMICOLON)) {
        advance_tok(p);
        return alloc_node(p, ND_NULL_STMT, p->cur.line, p->cur.col);
    }

    /* Parse multiple declarators */
    Node *first = NULL, *last = NULL;
    do {
        char *name = NULL;
        Type *ty   = parse_declarator(p, base, &name);

        if (is_typedef) {
            if (name) typedef_register(p, name, ty);
            /* emit a typedef node */
            Node *td = alloc_node(p, ND_TYPEDEF, p->cur.line, p->cur.col);
            td->typedef_.alias = arena_strdup(p->arena, name ? name : "");
            td->typedef_.type  = ty;
            if (!first) first = last = td;
            else { last->next = td; last = td; }
        } else {
            Node *decl = alloc_node(p, ND_VAR_DECL, p->cur.line, p->cur.col);
            decl->decl.name      = arena_strdup(p->arena, name ? name : "");
            decl->decl.decl_type = ty;
            decl->decl.is_static = is_static;

            if (check(p, TK_EQ)) {
                advance_tok(p);
                decl->decl.init = parse_initializer(p);
            }

            if (!first) first = last = decl;
            else { last->next = decl; last = decl; }
        }
    } while (match(p, TK_COMMA));

    expect(p, TK_SEMICOLON);

    /* If multiple declarators, wrap in compound */
    if (first && first->next) {
        Node *blk = alloc_node(p, ND_COMPOUND, first->line, first->col);
        blk->compound.stmts = NULL;
        blk->compound.count = 0;
        blk->compound.cap   = 0;
        for (Node *nd = first; nd; nd = nd->next) {
            node_array_push(p->arena, &blk->compound.stmts,
                            &blk->compound.count, &blk->compound.cap, nd);
        }
        return blk;
    }
    return first;
}

static Node *parse_stmt(Parser *p) {
    Token t = peek_tok(p);

    /* Labeled statement */
    if (t.kind == TK_IDENT) {
        /* Check if next-next is ':' (label) */
        /* Simple approach: save state */
        Token saved = t;
        /* We need 2-token lookahead. Consume ident, check for ':' */
        /* If yes → label; else → expression statement */
        /* NOTE: this is a limitation; real parser needs more lookahead */
        (void)saved;
    }

    switch (t.kind) {
    case TK_LBRACE:
        return parse_compound_stmt(p);

    case TK_IF: {
        advance_tok(p);
        expect(p, TK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TK_RPAREN);
        Node *then = parse_stmt(p);
        Node *else_ = NULL;
        if (match(p, TK_ELSE)) else_ = parse_stmt(p);

        Node *n       = alloc_node(p, ND_IF, t.line, t.col);
        n->if_.cond   = cond;
        n->if_.then   = then;
        n->if_.else_  = else_;
        return n;
    }

    case TK_WHILE: {
        advance_tok(p);
        expect(p, TK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TK_RPAREN);
        Node *body = parse_stmt(p);

        Node *n           = alloc_node(p, ND_WHILE, t.line, t.col);
        n->while_.cond    = cond;
        n->while_.body    = body;
        return n;
    }

    case TK_DO: {
        advance_tok(p);
        Node *body = parse_stmt(p);
        expect(p, TK_WHILE);
        expect(p, TK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TK_RPAREN);
        expect(p, TK_SEMICOLON);

        Node *n        = alloc_node(p, ND_DO_WHILE, t.line, t.col);
        n->while_.cond = cond;
        n->while_.body = body;
        return n;
    }

    case TK_FOR: {
        advance_tok(p);
        expect(p, TK_LPAREN);
        scope_push(p);

        Node *init = NULL;
        if (!check(p, TK_SEMICOLON)) {
            if (is_type_start(p)) {
                init = parse_local_decl(p); /* includes the ';' */
            } else {
                Node *e = parse_expr(p);
                init = alloc_node(p, ND_EXPR_STMT, e->line, e->col);
                init->unary.operand = e;
                expect(p, TK_SEMICOLON);
            }
        } else {
            advance_tok(p); /* consume ';' */
        }

        Node *cond = NULL;
        if (!check(p, TK_SEMICOLON)) cond = parse_expr(p);
        expect(p, TK_SEMICOLON);

        Node *step = NULL;
        if (!check(p, TK_RPAREN)) step = parse_expr(p);
        expect(p, TK_RPAREN);

        Node *body = parse_stmt(p);
        scope_pop(p);

        Node *n       = alloc_node(p, ND_FOR, t.line, t.col);
        n->for_.init  = init;
        n->for_.cond  = cond;
        n->for_.step  = step;
        n->for_.body  = body;
        return n;
    }

    case TK_SWITCH: {
        advance_tok(p);
        expect(p, TK_LPAREN);
        Node *expr = parse_expr(p);
        expect(p, TK_RPAREN);
        Node *body = parse_stmt(p);

        Node *n           = alloc_node(p, ND_SWITCH, t.line, t.col);
        n->switch_.expr   = expr;
        n->switch_.body   = body;
        return n;
    }

    case TK_CASE: {
        advance_tok(p);
        Token vt = advance_tok(p);
        long long val = 0;
        if (vt.kind == TK_INT) val = (long long)vt.ival;
        else if (vt.kind == TK_CHAR) val = vt.cval;
        expect(p, TK_COLON);
        Node *body = parse_stmt(p);

        Node *n          = alloc_node(p, ND_CASE, t.line, t.col);
        n->case_.value   = val;
        n->case_.body    = body;
        return n;
    }

    case TK_DEFAULT: {
        advance_tok(p);
        expect(p, TK_COLON);
        Node *body = parse_stmt(p);
        Node *n           = alloc_node(p, ND_DEFAULT, t.line, t.col);
        n->default_.body  = body;
        return n;
    }

    case TK_BREAK: {
        advance_tok(p);
        expect(p, TK_SEMICOLON);
        return alloc_node(p, ND_BREAK, t.line, t.col);
    }

    case TK_CONTINUE: {
        advance_tok(p);
        expect(p, TK_SEMICOLON);
        return alloc_node(p, ND_CONTINUE, t.line, t.col);
    }

    case TK_RETURN: {
        advance_tok(p);
        Node *val = NULL;
        if (!check(p, TK_SEMICOLON)) val = parse_expr(p);
        expect(p, TK_SEMICOLON);
        Node *n          = alloc_node(p, ND_RETURN, t.line, t.col);
        n->return_.value = val;
        return n;
    }

    case TK_GOTO: {
        advance_tok(p);
        Token lbl = expect(p, TK_IDENT);
        expect(p, TK_SEMICOLON);
        Node *n         = alloc_node(p, ND_GOTO, t.line, t.col);
        n->goto_.label  = arena_strdup(p->arena, lbl.sval ? lbl.sval : "");
        return n;
    }

    case TK_SEMICOLON:
        advance_tok(p);
        return alloc_node(p, ND_NULL_STMT, t.line, t.col);

    default: {
        /* Check for label: IDENT ':' */
        if (t.kind == TK_IDENT) {
            /* Peek 2 ahead — we already have 1-token lookahead.
               Use a manual look-ahead trick. */
            advance_tok(p); /* consume ident */
            if (check(p, TK_COLON)) {
                advance_tok(p); /* consume ':' */
                Node *body = parse_stmt(p);
                Node *n         = alloc_node(p, ND_LABEL, t.line, t.col);
                n->label.name   = arena_strdup(p->arena, t.sval ? t.sval : "");
                n->label.body   = body;
                return n;
            }
            /* It was an expression starting with an ident.
               We've consumed the ident — need to synthesise it. */
            Node *ident = alloc_node(p, ND_VAR, t.line, t.col);
            ident->ident.name = arena_strdup(p->arena, t.sval ? t.sval : "");

            /* Continue parsing the expression with the ident as LHS */
            /* Re-parse from postfix */
            /* Handle postfix on already-parsed ident */
            Node *left = ident;
            /* Apply postfix operators */
            while (true) {
                Token pt = peek_tok(p);
                if (pt.kind == TK_LPAREN) {
                    advance_tok(p);
                    Node *call = alloc_node(p, ND_CALL, pt.line, pt.col);
                    call->call.callee    = left;
                    call->call.args      = NULL;
                    call->call.arg_count = 0;
                    call->call.arg_cap   = 0;
                    if (!check(p, TK_RPAREN)) {
                        do {
                            Node *arg = parse_assign_expr(p);
                            node_array_push(p->arena, &call->call.args,
                                            &call->call.arg_count, &call->call.arg_cap, arg);
                        } while (match(p, TK_COMMA));
                    }
                    expect(p, TK_RPAREN);
                    left = call;
                } else if (pt.kind == TK_LBRACKET) {
                    advance_tok(p);
                    Node *idx = parse_expr(p);
                    expect(p, TK_RBRACKET);
                    Node *nd = alloc_node(p, ND_INDEX, pt.line, pt.col);
                    nd->index.arr = left;
                    nd->index.idx = idx;
                    nd->lhs       = left; /* flat alias */
                    nd->rhs       = idx;  /* flat alias */
                    left = nd;
                } else if (pt.kind == TK_DOT) {
                    advance_tok(p);
                    Token mn = expect(p, TK_IDENT);
                    Node *nd = alloc_node(p, ND_MEMBER, pt.line, pt.col);
                    nd->member_access.obj    = left;
                    nd->member_access.member = arena_strdup(p->arena, mn.sval ? mn.sval : "");
                    left = nd;
                } else if (pt.kind == TK_ARROW) {
                    advance_tok(p);
                    Token mn = expect(p, TK_IDENT);
                    Node *nd = alloc_node(p, ND_ARROW, pt.line, pt.col);
                    nd->member_access.obj    = left;
                    nd->member_access.member = arena_strdup(p->arena, mn.sval ? mn.sval : "");
                    left = nd;
                } else if (pt.kind == TK_PLUS_PLUS) {
                    advance_tok(p);
                    left = unop(p, ND_POST_INC, left, pt.line, pt.col);
                } else if (pt.kind == TK_MINUS_MINUS) {
                    advance_tok(p);
                    left = unop(p, ND_POST_DEC, left, pt.line, pt.col);
                } else break;
            }

            /* Now check for mul/div/add/sub etc — continue with binary ops */
            /* Reconstruct: use left as the LHS for the rest of the expression chain */
            /* We need to re-enter parse_mul etc with 'left' already parsed.
               Simplest: parse_add style continuation. */
            /* This is a simplified approach: just handle assignment and binary ops inline */
            while (true) {
                Token bt = peek_tok(p);
                NodeKind ak = assign_op_kind(bt.kind);
                if ((int)ak >= 0) {
                    advance_tok(p);
                    Node *rhs = parse_assign_expr(p);
                    left = binop(p, ak, left, rhs);
                    break;
                }
                /* mul ops */
                NodeKind mk = -1;
                if      (bt.kind == TK_STAR)    mk = ND_MUL;
                else if (bt.kind == TK_SLASH)   mk = ND_DIV;
                else if (bt.kind == TK_PERCENT) mk = ND_MOD;
                else if (bt.kind == TK_PLUS)    mk = ND_ADD;
                else if (bt.kind == TK_MINUS)   mk = ND_SUB;
                else if (bt.kind == TK_LSHIFT)  mk = ND_SHL;
                else if (bt.kind == TK_RSHIFT)  mk = ND_SHR;
                else if (bt.kind == TK_LT)      mk = ND_LT;
                else if (bt.kind == TK_GT)      mk = ND_GT;
                else if (bt.kind == TK_LT_EQ)   mk = ND_LE;
                else if (bt.kind == TK_GT_EQ)   mk = ND_GE;
                else if (bt.kind == TK_EQ_EQ)   mk = ND_EQ;
                else if (bt.kind == TK_BANG_EQ) mk = ND_NE;
                else if (bt.kind == TK_AMP)     mk = ND_AND;
                else if (bt.kind == TK_CARET)   mk = ND_XOR;
                else if (bt.kind == TK_PIPE)    mk = ND_OR;
                else if (bt.kind == TK_AMP_AMP) mk = ND_LOGIC_AND;
                else if (bt.kind == TK_PIPE_PIPE) mk = ND_LOGIC_OR;
                else if (bt.kind == TK_QUESTION) {
                    advance_tok(p);
                    Node *then = parse_expr(p);
                    expect(p, TK_COLON);
                    Node *else_ = parse_conditional(p);
                    Node *cn = alloc_node(p, ND_COND, bt.line, bt.col);
                    cn->ternary.cond  = left;
                    cn->ternary.then  = then;
                    cn->ternary.else_ = else_;
                    left = cn;
                    break;
                }
                else if (bt.kind == TK_COMMA) {
                    advance_tok(p);
                    left = binop(p, ND_COMMA, left, parse_assign_expr(p));
                }
                else break;

                if ((int)mk >= 0) {
                    advance_tok(p);
                    left = binop(p, mk, left, parse_cast(p));
                }
            }

            expect(p, TK_SEMICOLON);
            Node *stmt = alloc_node(p, ND_EXPR_STMT, t.line, t.col);
            stmt->unary.operand = left;
            return stmt;
        }

        /* Expression statement */
        Node *expr = parse_expr(p);
        expect(p, TK_SEMICOLON);
        Node *stmt         = alloc_node(p, ND_EXPR_STMT, t.line, t.col);
        stmt->unary.operand = expr;
        return stmt;
    }
    }
}

/* =========================================================
 * External declaration parsing
 * ========================================================= */

static Node *parse_external_decl(Parser *p) {
    Token t = peek_tok(p);

    /* Handle preprocessor # lines (skip) */
    if (t.kind == TK_HASH) {
        while (!check(p, TK_EOF) && peek_tok(p).kind != TK_SEMICOLON) {
            advance_tok(p);
            if (p->cur.kind == TK_IDENT && strcmp(p->cur.sval ? p->cur.sval : "", "include") == 0) {
                /* Skip rest of line */
                /* Simplified: just skip to next newline / ; */
                break;
            }
        }
        /* For now just return null stmt */
        return alloc_node(p, ND_NULL_STMT, t.line, t.col);
    }

    bool is_typedef, is_extern, is_static, is_inline;
    Type *base = parse_declaration_specifiers(p, &is_typedef, &is_extern, &is_static, &is_inline);

    /* Just a tag declaration: struct S { ... }; */
    if (check(p, TK_SEMICOLON)) {
        advance_tok(p);
        return alloc_node(p, ND_NULL_STMT, t.line, t.col);
    }

    char *name = NULL;
    Type *ty   = parse_declarator(p, base, &name);

    if (is_typedef) {
        if (name) typedef_register(p, name, ty);
        do {
            if (match(p, TK_COMMA)) {
                char *n2 = NULL;
                Type *t2 = parse_declarator(p, base, &n2);
                if (n2) typedef_register(p, n2, t2);
            } else break;
        } while (true);
        expect(p, TK_SEMICOLON);
        Node *td = alloc_node(p, ND_TYPEDEF, t.line, t.col);
        td->typedef_.alias = arena_strdup(p->arena, name ? name : "");
        td->typedef_.type  = ty;
        return td;
    }

    /* Function definition */
    if (ty->kind == TY_FUNC && check(p, TK_LBRACE)) {
        Node *fn          = alloc_node(p, ND_FUNC_DEF, t.line, t.col);
        fn->func.name     = arena_strdup(p->arena, name ? name : "");
        fn->func.ret_type = ty->base; /* return type */

        /* Build parameter nodes */
        fn->func.param_count = ty->param_count;
        fn->func.params      = NULL;
        if (ty->param_count > 0) {
            fn->func.params = arena_alloc(p->arena, (size_t)ty->param_count * sizeof(Node *));
        }
        fn->func.is_static  = is_static;
        fn->func.is_inline  = is_inline;
        fn->func.is_extern  = is_extern;

        /* Build parameter nodes using names captured during type parsing */
        for (int i = 0; i < ty->param_count; i++) {
            Node *pm = alloc_node(p, ND_PARAM, t.line, t.col);
            const char *pname = (ty->param_names && ty->param_names[i])
                                ? ty->param_names[i] : NULL;
            if (!pname) {
                /* Fallback: synthesize name */
                char pbuf[16];
                snprintf(pbuf, sizeof(pbuf), ".p%d", i);
                pname = arena_strdup(p->arena, pbuf);
            }
            pm->param.name       = pname;
            pm->param.param_type = ty->params[i];
            fn->func.params[i]   = pm;
        }

        scope_push(p);
        fn->func.body = parse_compound_stmt(p);
        scope_pop(p);
        return fn;
    }

    /* Global variable declaration */
    Node *decl = alloc_node(p, ND_VAR_DECL, t.line, t.col);
    decl->decl.name      = arena_strdup(p->arena, name ? name : "");
    decl->decl.decl_type = ty;
    decl->decl.is_static = is_static;
    decl->decl.is_extern = is_extern;

    if (match(p, TK_EQ))
        decl->decl.init = parse_initializer(p);

    /* Multiple declarators — keep first for return, chain via ->next */
    Node *first_decl = decl;
    while (match(p, TK_COMMA)) {
        char *n2   = NULL;
        Type *ty2  = parse_declarator(p, base, &n2);
        Node *decl2 = alloc_node(p, ND_VAR_DECL, t.line, t.col);
        decl2->decl.name      = arena_strdup(p->arena, n2 ? n2 : "");
        decl2->decl.decl_type = ty2;
        decl2->decl.is_static = is_static;
        decl2->decl.is_extern = is_extern;
        if (match(p, TK_EQ))
            decl2->decl.init = parse_initializer(p);
        decl->next = decl2;
        decl = decl2;
    }

    expect(p, TK_SEMICOLON);
    return first_decl;
}

/* =========================================================
 * Public API
 * ========================================================= */

Parser *parser_new(Lexer *lexer, const char *filename) {
    Parser *p       = calloc(1, sizeof(Parser));
    p->lexer        = lexer;
    p->arena        = arena_new();
    p->filename     = filename;
    p->peek_valid   = false;
    p->error_count  = 0;
    p->scope        = NULL;
    scope_push(p); /* global scope */

    /* Pre-register built-in types (some compilers treat these as typedefs) */
    /* e.g. __builtin_va_list etc. — skip for now */
    return p;
}

void parser_free(Parser *p) {
    if (!p) return;
    arena_free(p->arena);
    free(p);
}

Node *parser_parse(Parser *p) {
    int line = peek_tok(p).line;
    Node *root = alloc_node(p, ND_TRANSLATION_UNIT, line, 1);
    root->unit.decls = NULL;
    root->unit.count = 0;
    root->unit.cap   = 0;

    while (!check(p, TK_EOF)) {
        Node *decl = parse_external_decl(p);
        if (decl) {
            /* Flatten linked lists from multiple declarators */
            for (Node *nd = decl; nd; nd = nd->next) {
                node_array_push(p->arena, &root->unit.decls,
                                &root->unit.count, &root->unit.cap, nd);
            }
        }
    }
    return root;
}

Node *parser_parse_external_decl(Parser *p) {
    return parse_external_decl(p);
}

Node *parser_parse_stmt(Parser *p) {
    return parse_stmt(p);
}

Node *parser_parse_expr(Parser *p) {
    return parse_expr(p);
}
