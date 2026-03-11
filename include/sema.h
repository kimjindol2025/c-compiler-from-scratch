#pragma once
#include "ast.h"
#include "symtable.h"

/* =========================================================
 * sema.h — Semantic Analysis (type-checking) pass
 *
 * The semantic pass walks the AST produced by the parser and:
 *   1. Resolves typedef names, struct/union/enum tags.
 *   2. Links ND_VAR references to their Symbol entries.
 *   3. Computes and annotates every expression's Type.
 *   4. Enforces C11 type rules (promotions, arithmetic conversions,
 *      pointer arithmetic, assignment compatibility, …).
 *   5. Validates statement constraints (break/continue/goto).
 *   6. Computes struct/union member offsets and sizes.
 *   7. Evaluates integer constant expressions where required.
 * ========================================================= */

/* ---------------------------------------------------------
 * Context passed through the analysis
 * --------------------------------------------------------- */
typedef struct Sema {
    SymTable  *symtable;

    /* Return type of the function currently being analysed.
     * NULL when outside any function (file scope). */
    Type      *current_func_ret;

    /* Name of the current function (for diagnostics). */
    const char *current_func_name;

    /* Nesting depth of loop/switch contexts — for break/continue. */
    int        in_loop;
    int        in_switch;

    /* Total number of semantic errors encountered. */
    int        error_count;

    /* Number of warnings (non-fatal). */
    int        warn_count;

    /* Source filename (for error messages). */
    const char *filename;
} Sema;

/* =========================================================
 * Public API
 * ========================================================= */

/** Allocate and initialise a new semantic-analysis context. */
Sema *sema_new(void);

/** Free all resources owned by the semantic context. */
void  sema_free(Sema *s);

/**
 * Run the full semantic analysis pass on the translation unit.
 *
 * @param s        semantic context (reset between translation units)
 * @param program  ND_PROGRAM node produced by the parser
 * @return 0 on success (no errors), non-zero if errors were found
 *
 * Side effects:
 *   - All ND_VAR nodes get their ->var pointer set.
 *   - All expression nodes get their ->ty pointer set.
 *   - All struct/union types get member offsets computed.
 *   - Enum constants get their numeric values assigned.
 *   - Function definitions get their ->stack_size computed.
 */
int   sema_analyze(Sema *s, Node *program);

/* ---------------------------------------------------------
 * Fine-grained entry points (useful for unit testing)
 * --------------------------------------------------------- */

/** Analyse a single top-level declaration. */
void  sema_toplevel(Sema *s, Node *decl);

/** Analyse a statement (fills types, checks constraints). */
void  sema_stmt(Sema *s, Node *stmt);

/**
 * Analyse an expression and return its resolved type.
 * Also annotates node->ty.
 */
Type *sema_expr(Sema *s, Node *expr);

/**
 * Analyse a declaration-specifier list and resolve any typedef
 * names or tag references.  Returns the resolved Type.
 */
Type *sema_resolve_type(Sema *s, Type *ty);

/* ---------------------------------------------------------
 * Utility helpers exposed for codegen convenience
 * --------------------------------------------------------- */

/**
 * Evaluate an integer constant expression.
 * Returns 1 on success and sets *out, or 0 if the expression is
 * not an integer constant.
 */
int   sema_eval_const_int(Sema *s, Node *expr, long long *out);

/**
 * Insert an implicit cast node if 'expr' doesn't already have
 * type 'to'.  Returns either expr unchanged or a new ND_CAST wrapping it.
 */
Node *sema_coerce(Sema *s, Node *expr, Type *to);

/** True if 'from' can be implicitly converted to 'to'. */
int   sema_is_assignable(Sema *s, Type *to, Type *from);
