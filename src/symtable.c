/*
 * symtable.c — Symbol Table implementation
 *
 * Uses a per-scope hash table of SYMTABLE_HASH_BUCKETS buckets.
 * Collisions are resolved with chaining (linked list per bucket).
 *
 * The tag namespace (struct/union/enum) is kept separate from the
 * ordinary identifier namespace by filtering on SymKind in lookup.
 */

#include "symtable.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------- */

/* djb2 hash (Dan Bernstein) — fast, reasonable distribution */
static unsigned hash_str(const char *s)
{
    unsigned long h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) ^ c;   /* h * 33 XOR c */
    return (unsigned)(h & 0xffffffff);
}

static unsigned bucket_for(const char *name)
{
    return hash_str(name) % SYMTABLE_HASH_BUCKETS;
}

/* True for the "ordinary" identifier namespace */
static int is_ordinary_ns(SymKind k)
{
    return k == SYM_VAR || k == SYM_FUNC ||
           k == SYM_TYPEDEF || k == SYM_ENUM_CONST;
}

/* True for the tag namespace */
static int is_tag_ns(SymKind k)
{
    return k == SYM_STRUCT_TAG || k == SYM_UNION_TAG || k == SYM_ENUM_TAG;
}

/* Allocate and zero a fresh Symbol */
static Symbol *sym_alloc(SymKind kind, const char *name, Type *type)
{
    Symbol *s = calloc(1, sizeof(Symbol));
    if (!s) { perror("calloc"); exit(1); }
    s->kind = kind;
    s->name = name;  /* borrowed — must outlive symbol */
    s->type = type;
    return s;
}

/* Allocate and zero a fresh Scope */
static Scope *scope_alloc(Scope *parent, int is_func_scope)
{
    Scope *sc = calloc(1, sizeof(Scope));
    if (!sc) { perror("calloc"); exit(1); }
    sc->parent        = parent;
    sc->is_func_scope = is_func_scope;
    return sc;
}

/* Free all Symbol entries in a scope (does NOT free the Type they point to) */
static void scope_free(Scope *sc)
{
    for (int i = 0; i < SYMTABLE_HASH_BUCKETS; i++) {
        Symbol *sym = sc->buckets[i];
        while (sym) {
            Symbol *next = sym->next;
            free(sym->asm_label);
            free(sym);
            sym = next;
        }
    }
    /* Free label list */
    Symbol *lbl = sc->labels;
    while (lbl) {
        Symbol *next = lbl->next;
        free(lbl->asm_label);
        free(lbl);
        lbl = next;
    }
    free(sc);
}

/* ---------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------- */

SymTable *symtable_new(void)
{
    SymTable *st = calloc(1, sizeof(SymTable));
    if (!st) { perror("calloc"); exit(1); }

    /* Create the global / file scope */
    Scope *global = scope_alloc(NULL, 0);
    st->current      = global;
    st->global_scope = global;
    st->stack_offset  = 0;
    st->label_counter = 0;
    st->static_local_id = 0;
    return st;
}

void symtable_free(SymTable *st)
{
    if (!st) return;
    /* Pop and free all scopes */
    Scope *sc = st->current;
    while (sc) {
        Scope *parent = sc->parent;
        scope_free(sc);
        sc = parent;
    }
    free(st);
}

/* ---------------------------------------------------------
 * Scope management
 * --------------------------------------------------------- */

void symtable_push_scope(SymTable *st, int is_func_scope)
{
    Scope *sc = scope_alloc(st->current, is_func_scope);
    st->current = sc;
}

void symtable_pop_scope(SymTable *st)
{
    assert(st->current != st->global_scope &&
           "Cannot pop global scope");
    Scope *sc = st->current;
    st->current = sc->parent;
    scope_free(sc);
}

/* ---------------------------------------------------------
 * Symbol definition
 * --------------------------------------------------------- */

Symbol *symtable_define(SymTable *st, SymKind kind,
                        const char *name, Type *type)
{
    Scope *sc  = st->current;
    unsigned b = bucket_for(name);

    /* Check for redefinition in current scope (same namespace) */
    for (Symbol *s = sc->buckets[b]; s; s = s->next) {
        if (strcmp(s->name, name) != 0) continue;

        /* Same namespace? */
        int both_tag      = is_tag_ns(kind) && is_tag_ns(s->kind);
        int both_ordinary = is_ordinary_ns(kind) && is_ordinary_ns(s->kind);
        if (!both_tag && !both_ordinary) continue;

        /* Allow function prototype → definition upgrade */
        if (kind == SYM_FUNC && s->kind == SYM_FUNC && !s->is_defined) {
            s->type = type;  /* update prototype type to definition type */
            return s;
        }

        /* Allow struct/union/enum forward declaration → completion */
        if (both_tag && s->type && s->type->members == NULL && type->members) {
            s->type = type;
            return s;
        }

        fprintf(stderr, "error: redefinition of '%s'\n", name);
        return NULL;
    }

    Symbol *s = sym_alloc(kind, name, type);
    s->is_global = (sc == st->global_scope);

    /* Prepend to bucket chain */
    s->next       = sc->buckets[b];
    sc->buckets[b] = s;
    return s;
}

Symbol *symtable_define_label(SymTable *st, const char *name)
{
    /* Find the nearest function scope */
    Scope *sc = st->current;
    while (sc && !sc->is_func_scope) sc = sc->parent;
    if (!sc) {
        fprintf(stderr, "error: label '%s' outside function\n", name);
        return NULL;
    }

    /* Check for duplicate label */
    for (Symbol *s = sc->labels; s; s = s->next) {
        if (strcmp(s->name, name) == 0) {
            fprintf(stderr, "error: duplicate label '%s'\n", name);
            return NULL;
        }
    }

    Symbol *s = sym_alloc(SYM_VAR, name, NULL);
    /* prepend */
    s->next    = sc->labels;
    sc->labels = s;
    return s;
}

/* ---------------------------------------------------------
 * Lookup
 * --------------------------------------------------------- */

Symbol *symtable_lookup(SymTable *st, const char *name)
{
    unsigned b = bucket_for(name);
    for (Scope *sc = st->current; sc; sc = sc->parent) {
        for (Symbol *s = sc->buckets[b]; s; s = s->next) {
            if (strcmp(s->name, name) == 0 && is_ordinary_ns(s->kind))
                return s;
        }
    }
    return NULL;
}

Symbol *symtable_lookup_current(SymTable *st, const char *name)
{
    Scope   *sc = st->current;
    unsigned b  = bucket_for(name);
    for (Symbol *s = sc->buckets[b]; s; s = s->next) {
        if (strcmp(s->name, name) == 0)
            return s;
    }
    return NULL;
}

Symbol *symtable_lookup_tag(SymTable *st, const char *tag, SymKind kind)
{
    unsigned b = bucket_for(tag);
    for (Scope *sc = st->current; sc; sc = sc->parent) {
        for (Symbol *s = sc->buckets[b]; s; s = s->next) {
            if (s->kind == kind && strcmp(s->name, tag) == 0)
                return s;
        }
    }
    return NULL;
}

Symbol *symtable_lookup_label(SymTable *st, const char *name)
{
    /* Labels are function-scoped */
    Scope *sc = st->current;
    while (sc && !sc->is_func_scope) sc = sc->parent;
    if (!sc) return NULL;

    for (Symbol *s = sc->labels; s; s = s->next) {
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

/* ---------------------------------------------------------
 * Stack frame allocation
 * --------------------------------------------------------- */

/*
 * Align value DOWN to the nearest lower multiple of align.
 * On x86-64, locals grow downward (rbp - offset), so we round
 * offset DOWN (make it more negative) to satisfy alignment.
 */
static int align_down(int val, int align)
{
    if (align <= 1) return val;
    /* val is negative; we want to round to -align boundary */
    /* e.g. val=-5, align=4 → -8 */
    return -((-val + align - 1) & ~(align - 1));
}

int symtable_alloc_local(SymTable *st, int size, int align)
{
    if (size  <= 0) size  = 1;
    if (align <= 0) align = 1;

    st->stack_offset -= size;
    /* Align down (toward more-negative) */
    if (align > 1) {
        st->stack_offset = align_down(st->stack_offset, align);
    }
    return st->stack_offset;
}

int symtable_reset_stack(SymTable *st)
{
    int prev = st->stack_offset;
    st->stack_offset = 0;
    return prev;
}

/* ---------------------------------------------------------
 * Label generation
 * --------------------------------------------------------- */

char *symtable_gen_label(SymTable *st)
{
    /* ".Ltmp<N>" */
    int  n = st->label_counter++;
    /* Max length: ".Ltmp" (5) + up to 10 digits + NUL */
    char *buf = malloc(32);
    if (!buf) { perror("malloc"); exit(1); }
    snprintf(buf, 32, ".Ltmp%d", n);
    return buf;
}

char *symtable_gen_static_label(SymTable *st,
                                const char *func, const char *var)
{
    /* ".L<func>_<var>_<N>" */
    int  n   = st->static_local_id++;
    int  len = 64 + (func ? (int)strlen(func) : 4) + (int)strlen(var);
    char *buf = malloc(len);
    if (!buf) { perror("malloc"); exit(1); }
    snprintf(buf, len, ".L%s_%s_%d",
             func ? func : "anon", var, n);
    return buf;
}

/* ---------------------------------------------------------
 * Debug dump
 * --------------------------------------------------------- */

static const char *symkind_str(SymKind k)
{
    switch (k) {
    case SYM_VAR:        return "var";
    case SYM_FUNC:       return "func";
    case SYM_TYPEDEF:    return "typedef";
    case SYM_ENUM_CONST: return "enum_const";
    case SYM_STRUCT_TAG: return "struct_tag";
    case SYM_UNION_TAG:  return "union_tag";
    case SYM_ENUM_TAG:   return "enum_tag";
    default:             return "?";
    }
}

void symtable_dump(SymTable *st)
{
    int depth = 0;
    for (Scope *sc = st->current; sc; sc = sc->parent, depth++) {
        printf("=== Scope depth %d (func=%d) ===\n",
               depth, sc->is_func_scope);
        for (int i = 0; i < SYMTABLE_HASH_BUCKETS; i++) {
            for (Symbol *s = sc->buckets[i]; s; s = s->next) {
                char tybuf[128] = "<no-type>";
                if (s->type) type_to_str(s->type, tybuf, sizeof(tybuf));
                printf("  [%s] %s : %s  offset=%d global=%d\n",
                       symkind_str(s->kind), s->name, tybuf,
                       s->offset, s->is_global);
            }
        }
    }
}
