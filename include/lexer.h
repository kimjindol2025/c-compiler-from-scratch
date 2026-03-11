#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* =========================================================
 * C11 Lexer — Token Types and Lexer API
 * ========================================================= */

typedef enum {
    /* --- Literals --- */
    TK_INT,        /* 42, 0xff, 0777, 0b1010               */
    TK_FLOAT,      /* 3.14, 1.0f, 2.5e-3                   */
    TK_CHAR,       /* 'a', '\n', '\x41'                     */
    TK_STRING,     /* "hello\n"                             */
    TK_IDENT,      /* identifier                            */

    /* --- C11 Keywords --- */
    TK_AUTO,
    TK_BREAK,
    TK_CASE,
    TK_CHAR_KW,        /* char      (distinct from TK_CHAR literal) */
    TK_CONST,
    TK_CONTINUE,
    TK_DEFAULT,
    TK_DO,
    TK_DOUBLE,
    TK_ELSE,
    TK_ENUM,
    TK_EXTERN,
    TK_FLOAT_KW,       /* float     */
    TK_FOR,
    TK_GOTO,
    TK_IF,
    TK_INLINE,
    TK_INT_KW,         /* int       */
    TK_LONG,
    TK_REGISTER,
    TK_RESTRICT,
    TK_RETURN,
    TK_SHORT,
    TK_SIGNED,
    TK_SIZEOF,
    TK_STATIC,
    TK_STRUCT,
    TK_SWITCH,
    TK_TYPEDEF,
    TK_UNION,
    TK_UNSIGNED,
    TK_VOID,
    TK_VOLATILE,
    TK_WHILE,
    /* C11 _Keyword forms */
    TK_ALIGNAS,        /* _Alignas  */
    TK_ALIGNOF,        /* _Alignof  */
    TK_ATOMIC,         /* _Atomic   */
    TK_BOOL,           /* _Bool     */
    TK_COMPLEX,        /* _Complex  */
    TK_GENERIC,        /* _Generic  */
    TK_IMAGINARY,      /* _Imaginary */
    TK_NORETURN,       /* _Noreturn */
    TK_STATIC_ASSERT,  /* _Static_assert */
    TK_THREAD_LOCAL,   /* _Thread_local */

    /* --- Operators & Punctuation --- */
    TK_PLUS,           /* +   */
    TK_MINUS,          /* -   */
    TK_STAR,           /* *   */
    TK_SLASH,          /* /   */
    TK_PERCENT,        /* %   */
    TK_AMP,            /* &   */
    TK_PIPE,           /* |   */
    TK_CARET,          /* ^   */
    TK_TILDE,          /* ~   */
    TK_BANG,           /* !   */
    TK_LT,             /* <   */
    TK_GT,             /* >   */
    TK_LSHIFT,         /* <<  */
    TK_RSHIFT,         /* >>  */
    TK_PLUS_PLUS,      /* ++  */
    TK_MINUS_MINUS,    /* --  */
    TK_PLUS_EQ,        /* +=  */
    TK_MINUS_EQ,       /* -=  */
    TK_STAR_EQ,        /* *=  */
    TK_SLASH_EQ,       /* /=  */
    TK_PERCENT_EQ,     /* %=  */
    TK_AMP_EQ,         /* &=  */
    TK_PIPE_EQ,        /* |=  */
    TK_CARET_EQ,       /* ^=  */
    TK_LSHIFT_EQ,      /* <<= */
    TK_RSHIFT_EQ,      /* >>= */
    TK_EQ_EQ,          /* ==  */
    TK_BANG_EQ,        /* !=  */
    TK_LT_EQ,          /* <=  */
    TK_GT_EQ,          /* >=  */
    TK_AMP_AMP,        /* &&  */
    TK_PIPE_PIPE,      /* ||  */
    TK_EQ,             /* =   */
    TK_ARROW,          /* ->  */
    TK_DOT,            /* .   */
    TK_ELLIPSIS,       /* ... */
    TK_QUESTION,       /* ?   */
    TK_COLON,          /* :   */
    TK_SEMICOLON,      /* ;   */
    TK_COMMA,          /* ,   */
    TK_LPAREN,         /* (   */
    TK_RPAREN,         /* )   */
    TK_LBRACKET,       /* [   */
    TK_RBRACKET,       /* ]   */
    TK_LBRACE,         /* {   */
    TK_RBRACE,         /* }   */
    TK_HASH,           /* #   */
    TK_HASH_HASH,      /* ##  */

    /* --- Special --- */
    TK_EOF,
    TK_ERROR,

    TK_KIND_COUNT      /* sentinel — keep last */
} TokenKind;

/* Integer suffix flags (bit-field) */
typedef enum {
    INT_SUF_NONE     = 0,
    INT_SUF_UNSIGNED = 1 << 0,   /* u / U          */
    INT_SUF_LONG     = 1 << 1,   /* l / L          */
    INT_SUF_LONGLONG = 1 << 2,   /* ll / LL        */
} IntSuffix;

/* Float suffix */
typedef enum {
    FLOAT_SUF_NONE   = 0,
    FLOAT_SUF_FLOAT  = 1,        /* f / F          */
    FLOAT_SUF_LONG   = 2,        /* l / L          */
} FloatSuffix;

typedef struct {
    TokenKind  kind;
    const char *start;    /* pointer into original source buffer     */
    int        length;    /* byte length of raw lexeme               */
    int        line;      /* 1-based                                  */
    int        col;       /* 1-based                                  */

    union {
        /* TK_INT */
        struct {
            unsigned long long ival;
            IntSuffix          isuf;
        };
        /* TK_FLOAT */
        struct {
            double      fval;
            FloatSuffix fsuf;
        };
        /* TK_CHAR  — value is the decoded codepoint / byte */
        long long cval;
        /* TK_STRING / TK_IDENT — heap-allocated, NUL-terminated */
        char *sval;
    };
} Token;

typedef struct {
    const char *src;          /* full source text (NUL-terminated)   */
    const char *filename;
    int         pos;          /* current read position in src         */
    int         line;         /* line of src[pos]                     */
    int         col;          /* col  of src[pos]                     */

    bool        peeked;       /* is peek valid?                       */
    Token       peek_tok;     /* 1-token lookahead cache              */

    /* diagnostics */
    int         error_count;
} Lexer;

/* ---- Public API ---- */

/** Create a new lexer.  src must outlive the lexer. */
Lexer      *lexer_new(const char *src, const char *filename);

/** Free the lexer (does NOT free src). */
void        lexer_free(Lexer *l);

/**
 * Advance and return the next token.
 * The returned Token owns its sval (if any); caller must free it when done.
 */
Token       lexer_next(Lexer *l);

/**
 * Return the next token without consuming it.
 * The returned Token is owned by the lexer's peek buffer — do NOT free it.
 */
Token       lexer_peek(Lexer *l);

/** Human-readable name for a token kind (e.g. "TK_INT" → "int-literal"). */
const char *token_kind_name(TokenKind k);

/** Print a token to stdout (for debugging). */
void        token_print(Token t);

/** Free any heap memory held by a token (sval for TK_STRING/TK_IDENT). */
void        token_free(Token t);
