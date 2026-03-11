#pragma once
#include "lexer.h"
#include "ast.h"

/* =========================================================
 * C11 Recursive-Descent Parser — Public API
 *
 * Usage
 * -----
 *   Lexer  *lex = lexer_new(src, "file.c");
 *   Parser *p   = parser_new(lex, "file.c");
 *   Node   *ast = parser_parse(p);        // ND_PROGRAM node
 *   // ... use ast ...
 *   parser_free(p);     // releases arena; all AST nodes gone
 *   lexer_free(lex);
 *
 * Error handling
 * --------------
 *   On a syntax error the parser prints a message to stderr, increments
 *   p->error_count, then attempts panic-mode synchronisation so it can
 *   keep going and report further errors.  Check p->error_count after
 *   parser_parse() returns; a non-zero value means the AST is partial.
 *
 * Typedef tracking
 * ----------------
 *   The parser maintains a scope-aware typedef table so it can resolve
 *   the ambiguity between a declaration and a multiplication expression
 *   without a separate pre-pass.
 * ========================================================= */

/* ---- typedef scope (for disambiguation during parse) ---- */

typedef struct TypedefEntry {
    char               *name;
    Type               *ty;
    struct TypedefEntry *next;   /* hash-chain or flat list         */
} TypedefEntry;

typedef struct ParseScope {
    TypedefEntry      *entries;
    struct ParseScope *outer;
} ParseScope;

/* ---- Parser struct ---- */

typedef struct Parser {
    Lexer      *lexer;
    Arena      *arena;           /* all nodes/types allocated here  */
    Token       cur;             /* most-recently consumed token    */
    Token       peek;            /* one-token lookahead             */
    bool        peek_valid;

    /* typedef name → Type mapping, scoped */
    ParseScope *scope;

    /* diagnostics */

    int         error_count;
    const char *filename;
} Parser;

/* ---- lifecycle ---- */
Parser *parser_new(Lexer *lexer, const char *filename);
void    parser_free(Parser *p);

/* ---- main entry point ---- */

/**
 * Parse the entire translation unit.
 * Returns an ND_PROGRAM node (never NULL, but may be incomplete if
 * error_count > 0 after the call).
 */
Node *parser_parse(Parser *p);

/* ---- sub-parsers exposed for unit testing ---- */

/** Parse one external declaration (function definition or declaration). */
Node *parser_parse_external_decl(Parser *p);

/** Parse one statement. */
Node *parser_parse_stmt(Parser *p);

/** Parse a full comma-expression. */
Node *parser_parse_expr(Parser *p);

/** Parse a type-name (used in casts, sizeof, _Alignof). */
Type *parser_parse_type_name(Parser *p);
