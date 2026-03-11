#pragma once
#include "ast.h"

/* =========================================================
 * symtable.h — Symbol Table for the C compiler
 *
 * Structure:
 *   - Scopes form a stack (linked list via ->parent).
 *   - Each scope holds a hash table of Symbol entries for O(1)
 *     average lookup.
 *   - A "function scope" is pushed once at function entry and
 *     holds parameters.  Nested block scopes are pushed/popped
 *     for every compound statement.
 *   - The global scope is the outermost scope and is never popped.
 * ========================================================= */

/* Symbol kinds */
typedef enum {
    SYM_VAR,         /* object (variable): int x;                 */
    SYM_FUNC,        /* function: int foo(int);                   */
    SYM_TYPEDEF,     /* typedef name: typedef int MyInt;          */
    SYM_ENUM_CONST,  /* enumeration constant: enum E { A=0 };     */
    SYM_STRUCT_TAG,  /* struct tag: struct Point { … }            */
    SYM_UNION_TAG,   /* union tag                                 */
    SYM_ENUM_TAG,    /* enum tag                                  */
} SymKind;

/* A single symbol entry */
typedef struct Symbol {
    SymKind        kind;
    const char    *name;
    Type          *type;

    /* For local variables / parameters: byte offset from %rbp (negative). */
    int            offset;

    /* 1 if declared at file scope (global), 0 for locals. */
    int            is_global;

    /* True if 'static' storage class was used in a local declaration. */
    int            is_static;

    /* True if this is an 'extern' declaration (no storage allocated). */
    int            is_extern;

    /* For SYM_ENUM_CONST: the integer value of the enumerator. */
    long long      enum_val;

    /* For SYM_FUNC: set once the function body has been seen. */
    int            is_defined;

    /* For forward declarations / tentative definitions. */
    int            is_tentative;

    /* ASM label (for globals and static locals). Heap-allocated. */
    char          *asm_label;

    /* Intrusive singly-linked list within a scope's hash bucket. */
    struct Symbol *next;
} Symbol;

/* ---------------------------------------------------------
 * Hash table bucket (internal)
 * --------------------------------------------------------- */
#define SYMTABLE_HASH_BUCKETS 64

typedef struct Scope {
    Symbol        *buckets[SYMTABLE_HASH_BUCKETS]; /* hash table */
    struct Scope  *parent;    /* enclosing scope, or NULL at global */

    /* True if this scope was opened by a function definition
     * (as opposed to an ordinary compound statement).
     * Used to know where parameter symbols live. */
    int            is_func_scope;

    /* Labels are function-scoped; we keep a separate flat list
     * at function scope so goto-label validation is straightforward. */
    Symbol        *labels;   /* only meaningful in function scopes */
} Scope;

/* ---------------------------------------------------------
 * Symbol table handle
 * --------------------------------------------------------- */
typedef struct SymTable {
    Scope  *current;       /* innermost (current) scope            */
    Scope  *global_scope;  /* outermost scope (file scope)         */

    /* Running stack offset for the current function frame.
     * Starts at 0 and grows negatively.
     * Caller saves / restores to the value before enter function. */
    int     stack_offset;

    /* Counter for generating unique internal labels (e.g. .Ltmp0). */
    int     label_counter;

    /* Counter for generating unique names for static locals
     * (e.g. foo.bar.1). */
    int     static_local_id;
} SymTable;

/* =========================================================
 * Public API
 * ========================================================= */

/**
 * Allocate and initialise a new symbol table.
 * The global scope is pre-created.
 */
SymTable *symtable_new(void);

/** Free all resources owned by the symbol table. */
void      symtable_free(SymTable *st);

/* --- Scope management --- */

/**
 * Push a new scope.
 * is_func_scope != 0 marks this as a function's outermost scope.
 */
void      symtable_push_scope(SymTable *st, int is_func_scope);

/**
 * Pop (and free) the current scope.
 * Panics if called when at global scope.
 */
void      symtable_pop_scope(SymTable *st);

/* --- Symbol definition --- */

/**
 * Define a new symbol in the current scope.
 * Returns the new Symbol, or NULL (and emits an error) on redefinition.
 *
 * For SYM_FUNC, re-definition is allowed if the previous declaration was
 * a prototype and the new one is a definition (is_defined becomes 1).
 */
Symbol   *symtable_define(SymTable *st, SymKind kind,
                          const char *name, Type *type);

/**
 * Define a label in the nearest enclosing function scope.
 * Returns the Symbol, or NULL on duplicate label.
 */
Symbol   *symtable_define_label(SymTable *st, const char *name);

/* --- Symbol lookup --- */

/**
 * Search from the current scope outward.
 * Returns the first matching Symbol, or NULL if not found.
 * The lookup respects the distinction between ordinary identifiers
 * (SYM_VAR / SYM_FUNC / SYM_TYPEDEF / SYM_ENUM_CONST) and tag names
 * (SYM_STRUCT_TAG / SYM_UNION_TAG / SYM_ENUM_TAG).
 */
Symbol   *symtable_lookup(SymTable *st, const char *name);

/**
 * Same as symtable_lookup but restricted to the current scope only.
 * Used to detect redeclarations.
 */
Symbol   *symtable_lookup_current(SymTable *st, const char *name);

/**
 * Look up a struct/union/enum tag (searches only SYM_*_TAG kinds).
 * Searches from current scope outward.
 */
Symbol   *symtable_lookup_tag(SymTable *st, const char *tag, SymKind kind);

/**
 * Look up a label in the nearest enclosing function scope.
 */
Symbol   *symtable_lookup_label(SymTable *st, const char *name);

/* --- Stack frame allocation --- */

/**
 * Reserve 'size' bytes on the local stack frame, honouring 'align'.
 * Updates st->stack_offset and returns the (negative) rbp-relative
 * offset of the new slot.
 *
 * Example: symtable_alloc_local(st, 4, 4) → -4 (first call),
 *                                           → -8 (second call), …
 */
int       symtable_alloc_local(SymTable *st, int size, int align);

/**
 * Reset the stack offset (called when entering a new function).
 * Returns the previous value so the caller can restore it.
 */
int       symtable_reset_stack(SymTable *st);

/* --- Unique label / name generation --- */

/**
 * Generate a unique local label string like ".Ltmp3".
 * The returned string is heap-allocated; caller must free().
 */
char     *symtable_gen_label(SymTable *st);

/**
 * Generate a unique ASM label for a static local variable.
 * Result looks like ".Lfoo_bar_2".  Heap-allocated; caller must free().
 */
char     *symtable_gen_static_label(SymTable *st, const char *func,
                                    const char *var);

/* --- Diagnostics helper --- */

/** Print all symbols in the current scope chain to stdout. */
void      symtable_dump(SymTable *st);
