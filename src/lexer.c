/*
 * lexer.c — C11 Lexer Implementation
 *
 * Handles:
 *   - All C11 keywords (including _Alignas, _Bool, _Complex, etc.)
 *   - Integer literals: decimal, hexadecimal (0x), octal (0...), binary (0b)
 *   - Integer suffixes: u/U, l/L, ll/LL, and combinations
 *   - Float literals: decimal, hex (0x...p...), exponent notation
 *   - Float suffixes: f/F, l/L
 *   - Character literals with full escape-sequence support
 *   - String literals with full escape-sequence support
 *   - All C11 operators and punctuators (including <<= >>= etc.)
 *   - Line comments (//) and block comments
 *   - Preprocessor tokens (# ##) — tokenised but not processed
 *   - Accurate line/col tracking including across escaped newlines
 *   - Friendly error messages with source location
 */

#include "../include/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>

/* =========================================================
 *  Internal helpers
 * ========================================================= */

#define INITIAL_SBUF  256   /* initial string-buffer capacity */

/* Growable string buffer used while decoding escape sequences */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sbuf_init(StrBuf *b) {
    b->cap  = INITIAL_SBUF;
    b->len  = 0;
    b->data = malloc(b->cap);
    if (!b->data) { perror("malloc"); exit(1); }
}

static void sbuf_push(StrBuf *b, char c) {
    if (b->len + 1 >= b->cap) {
        b->cap *= 2;
        b->data = realloc(b->data, b->cap);
        if (!b->data) { perror("realloc"); exit(1); }
    }
    b->data[b->len++] = c;
}

static char *sbuf_finish(StrBuf *b) {
    sbuf_push(b, '\0');
    return b->data;   /* caller owns the allocation */
}

/* ---- character classification ---- */
static inline bool is_id_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}
static inline bool is_id_cont(char c) {
    return isalnum((unsigned char)c) || c == '_';
}
static inline bool is_hex_digit(char c) {
    return isxdigit((unsigned char)c);
}
static inline bool is_oct_digit(char c) {
    return c >= '0' && c <= '7';
}
static inline bool is_bin_digit(char c) {
    return c == '0' || c == '1';
}

/* ---- peek/advance ---- */
static inline char cur(const Lexer *l) {
    return l->src[l->pos];
}
static inline char peek1(const Lexer *l) {
    return l->src[l->pos] ? l->src[l->pos + 1] : '\0';
}
static inline char peek2(const Lexer *l) {
    if (!l->src[l->pos]) return '\0';
    if (!l->src[l->pos + 1]) return '\0';
    return l->src[l->pos + 2];
}

static void advance(Lexer *l) {
    if (l->src[l->pos] == '\0') return;

    /* Handle Windows-style CRLF as a single newline */
    if (l->src[l->pos] == '\r' && l->src[l->pos + 1] == '\n') {
        l->pos++;
    }

    if (l->src[l->pos] == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    l->pos++;
}

/* Advance n characters (line/col track through each) */
static void advance_n(Lexer *l, int n) {
    for (int i = 0; i < n; i++) advance(l);
}

static void lex_error(const Lexer *l, int line, int col, const char *msg) {
    fprintf(stderr, "%s:%d:%d: lexer error: %s\n",
            l->filename ? l->filename : "<input>", line, col, msg);
}

/* =========================================================
 *  Keyword table
 * ========================================================= */

typedef struct {
    const char *word;
    TokenKind   kind;
} KwEntry;

static const KwEntry KEYWORDS[] = {
    { "auto",            TK_AUTO           },
    { "break",           TK_BREAK          },
    { "case",            TK_CASE           },
    { "char",            TK_CHAR_KW        },
    { "const",           TK_CONST          },
    { "continue",        TK_CONTINUE       },
    { "default",         TK_DEFAULT        },
    { "do",              TK_DO             },
    { "double",          TK_DOUBLE         },
    { "else",            TK_ELSE           },
    { "enum",            TK_ENUM           },
    { "extern",          TK_EXTERN         },
    { "float",           TK_FLOAT_KW       },
    { "for",             TK_FOR            },
    { "goto",            TK_GOTO           },
    { "if",              TK_IF             },
    { "inline",          TK_INLINE         },
    { "int",             TK_INT_KW         },
    { "long",            TK_LONG           },
    { "register",        TK_REGISTER       },
    { "restrict",        TK_RESTRICT       },
    { "return",          TK_RETURN         },
    { "short",           TK_SHORT          },
    { "signed",          TK_SIGNED         },
    { "sizeof",          TK_SIZEOF         },
    { "static",          TK_STATIC         },
    { "struct",          TK_STRUCT         },
    { "switch",          TK_SWITCH         },
    { "typedef",         TK_TYPEDEF        },
    { "union",           TK_UNION          },
    { "unsigned",        TK_UNSIGNED       },
    { "void",            TK_VOID           },
    { "volatile",        TK_VOLATILE       },
    { "while",           TK_WHILE          },
    /* C11 _Keyword forms */
    { "_Alignas",        TK_ALIGNAS        },
    { "_Alignof",        TK_ALIGNOF        },
    { "_Atomic",         TK_ATOMIC         },
    { "_Bool",           TK_BOOL           },
    { "_Complex",        TK_COMPLEX        },
    { "_Generic",        TK_GENERIC        },
    { "_Imaginary",      TK_IMAGINARY      },
    { "_Noreturn",       TK_NORETURN       },
    { "_Static_assert",  TK_STATIC_ASSERT  },
    { "_Thread_local",   TK_THREAD_LOCAL   },
};

#define KW_COUNT  (int)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static TokenKind lookup_keyword(const char *buf, int len) {
    for (int i = 0; i < KW_COUNT; i++) {
        if ((int)strlen(KEYWORDS[i].word) == len &&
            memcmp(KEYWORDS[i].word, buf, (size_t)len) == 0)
        {
            return KEYWORDS[i].kind;
        }
    }
    return TK_IDENT;
}

/* =========================================================
 *  Escape-sequence decoder
 *  Returns the decoded byte value, or -1 on error.
 *  Advances l past the escape.
 * ========================================================= */

static long long decode_escape(Lexer *l) {
    int esc_line = l->line, esc_col = l->col;

    /* caller already consumed the backslash */
    char c = cur(l);
    if (c == '\0') {
        lex_error(l, esc_line, esc_col, "unterminated escape sequence");
        return -1;
    }
    advance(l); /* consume the escape character */

    switch (c) {
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case 'v':  return '\v';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case '?':  return '?';
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* octal escape: 1–3 octal digits, first was already c */
            unsigned int val = (unsigned int)(c - '0');
            for (int i = 0; i < 2; i++) {
                if (!is_oct_digit(cur(l))) break;
                val = val * 8 + (unsigned int)(cur(l) - '0');
                advance(l);
            }
            return (long long)val;
        }
        case 'x': {
            /* hex escape: \xHH... */
            if (!is_hex_digit(cur(l))) {
                lex_error(l, esc_line, esc_col, "invalid hex escape sequence");
                return -1;
            }
            unsigned long long val = 0;
            while (is_hex_digit(cur(l))) {
                char hc = cur(l);
                int  dv = isdigit((unsigned char)hc) ? hc - '0'
                        : tolower((unsigned char)hc)  - 'a' + 10;
                val = val * 16 + (unsigned long long)dv;
                advance(l);
            }
            return (long long)val;
        }
        case 'u': {
            /* universal character: \uHHHH */
            unsigned int val = 0;
            for (int i = 0; i < 4; i++) {
                if (!is_hex_digit(cur(l))) {
                    lex_error(l, esc_line, esc_col,
                              "\\u requires 4 hex digits");
                    return -1;
                }
                char hc = cur(l);
                int  dv = isdigit((unsigned char)hc) ? hc - '0'
                        : tolower((unsigned char)hc)  - 'a' + 10;
                val = val * 16 + (unsigned int)dv;
                advance(l);
            }
            return (long long)val;
        }
        case 'U': {
            /* universal character: \UHHHHHHHH */
            unsigned long long val = 0;
            for (int i = 0; i < 8; i++) {
                if (!is_hex_digit(cur(l))) {
                    lex_error(l, esc_line, esc_col,
                              "\\U requires 8 hex digits");
                    return -1;
                }
                char hc = cur(l);
                int  dv = isdigit((unsigned char)hc) ? hc - '0'
                        : tolower((unsigned char)hc)  - 'a' + 10;
                val = val * 16 + (unsigned long long)dv;
                advance(l);
            }
            return (long long)val;
        }
        default: {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "unknown escape sequence '\\%c'", c);
            lex_error(l, esc_line, esc_col, msg);
            return (long long)c; /* best-effort recovery */
        }
    }
}

/* =========================================================
 *  Token scanners
 * ========================================================= */

/* --- identifier / keyword --- */
static Token scan_ident(Lexer *l) {
    Token t;
    t.start  = l->src + l->pos;
    t.line   = l->line;
    t.col    = l->col;

    /* Collect all id-continuation chars */
    int start_pos = l->pos;
    while (is_id_cont(cur(l))) advance(l);
    t.length = l->pos - start_pos;

    /* Check keyword table */
    TokenKind kw = lookup_keyword(t.start, t.length);
    t.kind = kw;

    if (kw == TK_IDENT) {
        /* heap-copy for identifiers */
        t.sval = malloc((size_t)t.length + 1);
        if (!t.sval) { perror("malloc"); exit(1); }
        memcpy(t.sval, t.start, (size_t)t.length);
        t.sval[t.length] = '\0';
    } else {
        t.sval = NULL;
    }
    return t;
}

/* --- integer / float literal --- */

/*
 * scan_number: handles all numeric literals:
 *   decimal     42  42u  42ul  42ull
 *   hexadecimal 0xFF  0xDEADBEEFull
 *   octal       0755
 *   binary      0b1010  (GCC extension, widely supported)
 *   float       3.14  3.14f  1e-3  1.5e+2L  0x1.fp10
 */
static Token scan_number(Lexer *l) {
    Token t;
    t.start  = l->src + l->pos;
    t.line   = l->line;
    t.col    = l->col;
    int start_pos = l->pos;

    bool is_float  = false;
    int  radix     = 10;

    unsigned long long ival = 0;
    double             fval = 0.0;
    IntSuffix          isuf = INT_SUF_NONE;
    FloatSuffix        fsuf = FLOAT_SUF_NONE;

    /* --- determine radix --- */
    if (cur(l) == '0' && (peek1(l) == 'x' || peek1(l) == 'X')) {
        /* hexadecimal */
        radix = 16;
        advance_n(l, 2);
    } else if (cur(l) == '0' && (peek1(l) == 'b' || peek1(l) == 'B')) {
        /* binary (GCC extension) */
        radix = 2;
        advance_n(l, 2);
    } else if (cur(l) == '0' && isdigit((unsigned char)peek1(l))) {
        /* octal */
        radix = 8;
        advance(l); /* consume leading 0 */
    }

    /* --- scan digits / body --- */
    if (radix == 16) {
        /* hex integer or hex float (0x1.8p+1) */
        if (!is_hex_digit(cur(l)) && cur(l) != '.') {
            lex_error(l, t.line, t.col,
                      "expected hex digit after '0x'");
            t.kind = TK_ERROR;
            t.length = l->pos - start_pos;
            return t;
        }
        while (is_hex_digit(cur(l))) {
            int dv = isdigit((unsigned char)cur(l)) ? cur(l) - '0'
                   : tolower((unsigned char)cur(l)) - 'a' + 10;
            ival = ival * 16 + (unsigned long long)dv;
            advance(l);
        }
        /* hex float: optional fractional part */
        if (cur(l) == '.') {
            is_float = true;
            advance(l);
            double frac  = 1.0 / 16.0;
            double fpart = 0.0;
            while (is_hex_digit(cur(l))) {
                int dv = isdigit((unsigned char)cur(l)) ? cur(l) - '0'
                       : tolower((unsigned char)cur(l)) - 'a' + 10;
                fpart += dv * frac;
                frac  /= 16.0;
                advance(l);
            }
            fval = (double)ival + fpart;
        }
        /* hex float requires binary exponent 'p' */
        if (cur(l) == 'p' || cur(l) == 'P') {
            /* If we had no fractional dot, fval hasn't been set yet */
            if (!is_float) fval = (double)ival;
            is_float = true;
            advance(l);
            int  exp_sign = 1;
            long exp_val  = 0;
            if (cur(l) == '+')      { advance(l); }
            else if (cur(l) == '-') { exp_sign = -1; advance(l); }
            while (isdigit((unsigned char)cur(l))) {
                exp_val = exp_val * 10 + (cur(l) - '0');
                advance(l);
            }
            /* fval * 2^(exp_sign * exp_val) */
            double scale = 1.0;
            for (long i = 0; i < exp_val; i++) scale *= 2.0;
            if (exp_sign > 0) fval *= scale;
            else              fval /= scale;
        }
    } else if (radix == 2) {
        if (!is_bin_digit(cur(l))) {
            lex_error(l, t.line, t.col,
                      "expected binary digit after '0b'");
            t.kind = TK_ERROR;
            t.length = l->pos - start_pos;
            return t;
        }
        while (is_bin_digit(cur(l))) {
            ival = ival * 2 + (unsigned long long)(cur(l) - '0');
            advance(l);
        }
    } else if (radix == 8) {
        while (is_oct_digit(cur(l))) {
            ival = ival * 8 + (unsigned long long)(cur(l) - '0');
            advance(l);
        }
        /* sanity: if next char is 8 or 9, that is invalid octal */
        if (cur(l) == '8' || cur(l) == '9') {
            lex_error(l, l->line, l->col,
                      "invalid digit in octal literal");
        }
    } else {
        /* decimal */
        while (isdigit((unsigned char)cur(l))) {
            ival = ival * 10 + (unsigned long long)(cur(l) - '0');
            advance(l);
        }
        /* decimal float: optional fractional or exponent */
        if (cur(l) == '.' && peek1(l) != '.') {
            is_float = true;
            fval = (double)ival;
            advance(l); /* consume '.' */
            double frac = 0.1;
            while (isdigit((unsigned char)cur(l))) {
                fval += (cur(l) - '0') * frac;
                frac /= 10.0;
                advance(l);
            }
        }
        if (cur(l) == 'e' || cur(l) == 'E') {
            /* If we had no fractional dot, initialize fval from ival */
            if (!is_float) fval = (double)ival;
            is_float = true;
            advance(l);
            int  exp_sign = 1;
            long exp_val  = 0;
            if (cur(l) == '+')      { advance(l); }
            else if (cur(l) == '-') { exp_sign = -1; advance(l); }
            if (!isdigit((unsigned char)cur(l))) {
                lex_error(l, l->line, l->col,
                          "expected digits after exponent");
            }
            while (isdigit((unsigned char)cur(l))) {
                exp_val = exp_val * 10 + (cur(l) - '0');
                advance(l);
            }
            double scale = 1.0;
            for (long i = 0; i < exp_val; i++) scale *= 10.0;
            if (exp_sign > 0) fval *= scale;
            else              fval /= scale;
        }
    }

    /* --- suffixes --- */
    if (is_float) {
        if (cur(l) == 'f' || cur(l) == 'F') {
            fsuf = FLOAT_SUF_FLOAT;
            advance(l);
        } else if (cur(l) == 'l' || cur(l) == 'L') {
            fsuf = FLOAT_SUF_LONG;
            advance(l);
        }
        t.kind   = TK_FLOAT;
        t.fval   = fval;
        t.fsuf   = fsuf;
    } else {
        /* integer suffixes: u/U l/L ll/LL in any order */
        bool got_u  = false;
        bool got_l  = false;
        bool got_ll = false;

        for (int pass = 0; pass < 3; pass++) {
            if (!got_u && (cur(l) == 'u' || cur(l) == 'U')) {
                got_u = true;
                isuf |= INT_SUF_UNSIGNED;
                advance(l);
            } else if (!got_l && !got_ll &&
                       (cur(l) == 'l' || cur(l) == 'L')) {
                char first = cur(l);
                advance(l);
                if (cur(l) == first) {   /* ll / LL */
                    got_ll = true;
                    isuf |= INT_SUF_LONGLONG;
                    advance(l);
                } else {
                    got_l = true;
                    isuf |= INT_SUF_LONG;
                }
            } else {
                break;
            }
        }
        t.kind = TK_INT;
        t.ival = ival;
        t.isuf = isuf;
    }

    t.length = l->pos - start_pos;
    return t;
}

/* --- character literal --- */
static Token scan_char(Lexer *l) {
    Token t;
    t.start = l->src + l->pos;
    t.line  = l->line;
    t.col   = l->col;
    int start_pos = l->pos;

    advance(l); /* consume opening ' */

    long long val = 0;
    int count = 0;

    while (cur(l) != '\'' && cur(l) != '\0' && cur(l) != '\n') {
        if (cur(l) == '\\') {
            advance(l); /* consume backslash */
            long long ev = decode_escape(l);
            if (ev < 0) { val = 0; break; }
            val = ev;
        } else {
            val = (unsigned char)cur(l);
            advance(l);
        }
        count++;
    }

    if (cur(l) == '\'') {
        advance(l); /* consume closing ' */
    } else {
        lex_error(l, t.line, t.col, "unterminated character literal");
    }

    if (count == 0) {
        lex_error(l, t.line, t.col, "empty character literal");
    } else if (count > 1) {
        /* multi-character constant: allowed but implementation-defined */
        /* we just keep the last character */
    }

    t.kind   = TK_CHAR;
    t.cval   = val;
    t.length = l->pos - start_pos;
    return t;
}

/* --- string literal --- */
static Token scan_string(Lexer *l) {
    Token t;
    t.start = l->src + l->pos;
    t.line  = l->line;
    t.col   = l->col;
    int start_pos = l->pos;

    advance(l); /* consume opening " */

    StrBuf sb;
    sbuf_init(&sb);

    while (cur(l) != '"' && cur(l) != '\0') {
        if (cur(l) == '\\') {
            advance(l); /* consume backslash */
            long long ev = decode_escape(l);
            if (ev < 0) {
                /* error already reported; insert replacement char */
                sbuf_push(&sb, '?');
            } else {
                /*
                 * For simplicity we store the low byte.
                 * A production compiler would encode as UTF-8 for \u/\U.
                 */
                sbuf_push(&sb, (char)(ev & 0xFF));
            }
        } else if (cur(l) == '\n') {
            lex_error(l, l->line, l->col,
                      "unterminated string literal (newline)");
            break;
        } else {
            sbuf_push(&sb, cur(l));
            advance(l);
        }
    }

    if (cur(l) == '"') {
        advance(l); /* consume closing " */
    } else {
        lex_error(l, t.line, t.col, "unterminated string literal");
    }

    t.kind   = TK_STRING;
    t.sval   = sbuf_finish(&sb);
    t.length = l->pos - start_pos;
    return t;
}

/* --- skip whitespace and comments --- */
static void skip_whitespace_and_comments(Lexer *l) {
again:
    /* Skip plain whitespace */
    while (cur(l) == ' ' || cur(l) == '\t' ||
           cur(l) == '\n' || cur(l) == '\r' ||
           cur(l) == '\v' || cur(l) == '\f') {
        advance(l);
    }

    /* Line comment */
    if (cur(l) == '/' && peek1(l) == '/') {
        advance_n(l, 2);
        while (cur(l) != '\n' && cur(l) != '\0') advance(l);
        goto again;
    }

    /* Block comment */
    if (cur(l) == '/' && peek1(l) == '*') {
        int cl = l->line, cc = l->col;
        advance_n(l, 2);
        while (!(cur(l) == '*' && peek1(l) == '/')) {
            if (cur(l) == '\0') {
                lex_error(l, cl, cc, "unterminated block comment");
                return;
            }
            advance(l);
        }
        advance_n(l, 2); /* consume *\/ */
        goto again;
    }

    /* Line continuation (escaped newline — used inside preprocessor lines) */
    if (cur(l) == '\\' && peek1(l) == '\n') {
        advance_n(l, 2);
        goto again;
    }
    if (cur(l) == '\\' && peek1(l) == '\r' && peek2(l) == '\n') {
        advance_n(l, 3);
        goto again;
    }
}

/* =========================================================
 *  Main tokeniser
 * ========================================================= */

static Token scan_one(Lexer *l) {
    skip_whitespace_and_comments(l);

    Token t;
    t.start  = l->src + l->pos;
    t.line   = l->line;
    t.col    = l->col;
    t.sval   = NULL;

    char c = cur(l);

    if (c == '\0') {
        t.kind   = TK_EOF;
        t.length = 0;
        return t;
    }

    /* ---- identifier or keyword ---- */
    if (is_id_start(c)) {
        return scan_ident(l);
    }

    /* ---- numeric literal ---- */
    if (isdigit((unsigned char)c) ||
        (c == '.' && isdigit((unsigned char)peek1(l))))
    {
        /* Handle ".5f" style float */
        if (c == '.') {
            /* treat it like "0.5f" */
            int start_pos = l->pos;
            t.fval  = 0.0;
            t.fsuf  = FLOAT_SUF_NONE;
            advance(l); /* consume '.' */
            double frac = 0.1;
            while (isdigit((unsigned char)cur(l))) {
                t.fval += (cur(l) - '0') * frac;
                frac   /= 10.0;
                advance(l);
            }
            if (cur(l) == 'e' || cur(l) == 'E') {
                advance(l);
                int exp_sign = 1; long exp_val = 0;
                if (cur(l) == '+')      advance(l);
                else if (cur(l) == '-') { exp_sign = -1; advance(l); }
                while (isdigit((unsigned char)cur(l))) {
                    exp_val = exp_val * 10 + (cur(l) - '0');
                    advance(l);
                }
                double scale = 1.0;
                for (long i = 0; i < exp_val; i++) scale *= 10.0;
                if (exp_sign > 0) t.fval *= scale;
                else              t.fval /= scale;
            }
            if (cur(l) == 'f' || cur(l) == 'F') {
                t.fsuf = FLOAT_SUF_FLOAT; advance(l);
            } else if (cur(l) == 'l' || cur(l) == 'L') {
                t.fsuf = FLOAT_SUF_LONG; advance(l);
            }
            t.kind   = TK_FLOAT;
            t.length = l->pos - start_pos;
            return t;
        }
        return scan_number(l);
    }

    /* ---- character literal ---- */
    if (c == '\'') return scan_char(l);

    /* ---- string literal ---- */
    if (c == '"')  return scan_string(l);

    /* ---- wide / UTF string / char prefixes: L"..." u"..." u8"..." U"..." --- */
    if ((c == 'L' || c == 'u' || c == 'U') &&
        (peek1(l) == '"' || peek1(l) == '\''))
    {
        /* skip prefix, then scan as normal string/char */
        advance(l);
        if (peek1(l) == '8' && peek2(l) == '"') {
            /* u8"..." */
            advance(l); /* skip '8' */
        }
        if (cur(l) == '"')  return scan_string(l);
        if (cur(l) == '\'') return scan_char(l);
    }
    /* u8"..." where peek1='8' checked above; also handle 'u' before '8' */
    if (c == 'u' && peek1(l) == '8' &&
        (peek2(l) == '"' || peek2(l) == '\'')) {
        advance_n(l, 2);
        if (cur(l) == '"')  return scan_string(l);
        if (cur(l) == '\'') return scan_char(l);
    }

    /* ---- operators & punctuation ---- */
    advance(l); /* consume first char of operator */

    switch (c) {
        case '+':
            if (cur(l) == '+')  { advance(l); t.kind = TK_PLUS_PLUS;  break; }
            if (cur(l) == '=')  { advance(l); t.kind = TK_PLUS_EQ;    break; }
            t.kind = TK_PLUS; break;

        case '-':
            if (cur(l) == '-')  { advance(l); t.kind = TK_MINUS_MINUS; break; }
            if (cur(l) == '=')  { advance(l); t.kind = TK_MINUS_EQ;    break; }
            if (cur(l) == '>')  { advance(l); t.kind = TK_ARROW;        break; }
            t.kind = TK_MINUS; break;

        case '*':
            if (cur(l) == '=')  { advance(l); t.kind = TK_STAR_EQ;    break; }
            t.kind = TK_STAR; break;

        case '/':
            if (cur(l) == '=')  { advance(l); t.kind = TK_SLASH_EQ;   break; }
            t.kind = TK_SLASH; break;

        case '%':
            if (cur(l) == '=')  { advance(l); t.kind = TK_PERCENT_EQ; break; }
            t.kind = TK_PERCENT; break;

        case '&':
            if (cur(l) == '&')  { advance(l); t.kind = TK_AMP_AMP;    break; }
            if (cur(l) == '=')  { advance(l); t.kind = TK_AMP_EQ;     break; }
            t.kind = TK_AMP; break;

        case '|':
            if (cur(l) == '|')  { advance(l); t.kind = TK_PIPE_PIPE;  break; }
            if (cur(l) == '=')  { advance(l); t.kind = TK_PIPE_EQ;    break; }
            t.kind = TK_PIPE; break;

        case '^':
            if (cur(l) == '=')  { advance(l); t.kind = TK_CARET_EQ;   break; }
            t.kind = TK_CARET; break;

        case '~': t.kind = TK_TILDE;  break;
        case '!':
            if (cur(l) == '=')  { advance(l); t.kind = TK_BANG_EQ;    break; }
            t.kind = TK_BANG; break;

        case '<':
            if (cur(l) == '<') {
                advance(l);
                if (cur(l) == '=') { advance(l); t.kind = TK_LSHIFT_EQ; break; }
                t.kind = TK_LSHIFT; break;
            }
            if (cur(l) == '=')  { advance(l); t.kind = TK_LT_EQ;      break; }
            t.kind = TK_LT; break;

        case '>':
            if (cur(l) == '>') {
                advance(l);
                if (cur(l) == '=') { advance(l); t.kind = TK_RSHIFT_EQ; break; }
                t.kind = TK_RSHIFT; break;
            }
            if (cur(l) == '=')  { advance(l); t.kind = TK_GT_EQ;      break; }
            t.kind = TK_GT; break;

        case '=':
            if (cur(l) == '=')  { advance(l); t.kind = TK_EQ_EQ;      break; }
            t.kind = TK_EQ; break;

        case '.':
            if (cur(l) == '.' && peek1(l) == '.') {
                advance_n(l, 2);
                t.kind = TK_ELLIPSIS;
                break;
            }
            t.kind = TK_DOT; break;

        case '?': t.kind = TK_QUESTION;  break;
        case ':': t.kind = TK_COLON;     break;
        case ';': t.kind = TK_SEMICOLON; break;
        case ',': t.kind = TK_COMMA;     break;
        case '(': t.kind = TK_LPAREN;    break;
        case ')': t.kind = TK_RPAREN;    break;
        case '[': t.kind = TK_LBRACKET;  break;
        case ']': t.kind = TK_RBRACKET;  break;
        case '{': t.kind = TK_LBRACE;    break;
        case '}': t.kind = TK_RBRACE;    break;
        case '#':
            if (cur(l) == '#')  { advance(l); t.kind = TK_HASH_HASH;  break; }
            t.kind = TK_HASH; break;

        default: {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "unexpected character '%c' (0x%02x)", c, (unsigned char)c);
            lex_error(l, t.line, t.col, msg);
            t.kind = TK_ERROR;
            break;
        }
    }

    t.length = (int)((l->src + l->pos) - t.start);
    return t;
}

/* =========================================================
 *  Public API
 * ========================================================= */

Lexer *lexer_new(const char *src, const char *filename) {
    Lexer *l = calloc(1, sizeof(Lexer));
    if (!l) { perror("calloc"); exit(1); }
    l->src      = src;
    l->filename = filename ? filename : "<input>";
    l->pos      = 0;
    l->line     = 1;
    l->col      = 1;
    l->peeked   = false;
    return l;
}

void lexer_free(Lexer *l) {
    if (!l) return;
    if (l->peeked) token_free(l->peek_tok);
    free(l);
}

Token lexer_next(Lexer *l) {
    if (l->peeked) {
        l->peeked = false;
        return l->peek_tok;
    }
    return scan_one(l);
}

Token lexer_peek(Lexer *l) {
    if (!l->peeked) {
        l->peek_tok = scan_one(l);
        l->peeked   = true;
    }
    return l->peek_tok;
}

void token_free(Token t) {
    if (t.kind == TK_STRING || t.kind == TK_IDENT) {
        free(t.sval);
    }
}

/* =========================================================
 *  Diagnostics helpers
 * ========================================================= */

static const char *KIND_NAMES[TK_KIND_COUNT] = {
    /* literals */
    [TK_INT]          = "int-literal",
    [TK_FLOAT]        = "float-literal",
    [TK_CHAR]         = "char-literal",
    [TK_STRING]       = "string-literal",
    [TK_IDENT]        = "identifier",
    /* keywords */
    [TK_AUTO]         = "auto",
    [TK_BREAK]        = "break",
    [TK_CASE]         = "case",
    [TK_CHAR_KW]      = "char",
    [TK_CONST]        = "const",
    [TK_CONTINUE]     = "continue",
    [TK_DEFAULT]      = "default",
    [TK_DO]           = "do",
    [TK_DOUBLE]       = "double",
    [TK_ELSE]         = "else",
    [TK_ENUM]         = "enum",
    [TK_EXTERN]       = "extern",
    [TK_FLOAT_KW]     = "float",
    [TK_FOR]          = "for",
    [TK_GOTO]         = "goto",
    [TK_IF]           = "if",
    [TK_INLINE]       = "inline",
    [TK_INT_KW]       = "int",
    [TK_LONG]         = "long",
    [TK_REGISTER]     = "register",
    [TK_RESTRICT]     = "restrict",
    [TK_RETURN]       = "return",
    [TK_SHORT]        = "short",
    [TK_SIGNED]       = "signed",
    [TK_SIZEOF]       = "sizeof",
    [TK_STATIC]       = "static",
    [TK_STRUCT]       = "struct",
    [TK_SWITCH]       = "switch",
    [TK_TYPEDEF]      = "typedef",
    [TK_UNION]        = "union",
    [TK_UNSIGNED]     = "unsigned",
    [TK_VOID]         = "void",
    [TK_VOLATILE]     = "volatile",
    [TK_WHILE]        = "while",
    [TK_ALIGNAS]      = "_Alignas",
    [TK_ALIGNOF]      = "_Alignof",
    [TK_ATOMIC]       = "_Atomic",
    [TK_BOOL]         = "_Bool",
    [TK_COMPLEX]      = "_Complex",
    [TK_GENERIC]      = "_Generic",
    [TK_IMAGINARY]    = "_Imaginary",
    [TK_NORETURN]     = "_Noreturn",
    [TK_STATIC_ASSERT]= "_Static_assert",
    [TK_THREAD_LOCAL] = "_Thread_local",
    /* operators */
    [TK_PLUS]         = "+",
    [TK_MINUS]        = "-",
    [TK_STAR]         = "*",
    [TK_SLASH]        = "/",
    [TK_PERCENT]      = "%",
    [TK_AMP]          = "&",
    [TK_PIPE]         = "|",
    [TK_CARET]        = "^",
    [TK_TILDE]        = "~",
    [TK_BANG]         = "!",
    [TK_LT]           = "<",
    [TK_GT]           = ">",
    [TK_LSHIFT]       = "<<",
    [TK_RSHIFT]       = ">>",
    [TK_PLUS_PLUS]    = "++",
    [TK_MINUS_MINUS]  = "--",
    [TK_PLUS_EQ]      = "+=",
    [TK_MINUS_EQ]     = "-=",
    [TK_STAR_EQ]      = "*=",
    [TK_SLASH_EQ]     = "/=",
    [TK_PERCENT_EQ]   = "%=",
    [TK_AMP_EQ]       = "&=",
    [TK_PIPE_EQ]      = "|=",
    [TK_CARET_EQ]     = "^=",
    [TK_LSHIFT_EQ]    = "<<=",
    [TK_RSHIFT_EQ]    = ">>=",
    [TK_EQ_EQ]        = "==",
    [TK_BANG_EQ]      = "!=",
    [TK_LT_EQ]        = "<=",
    [TK_GT_EQ]        = ">=",
    [TK_AMP_AMP]      = "&&",
    [TK_PIPE_PIPE]    = "||",
    [TK_EQ]           = "=",
    [TK_ARROW]        = "->",
    [TK_DOT]          = ".",
    [TK_ELLIPSIS]     = "...",
    [TK_QUESTION]     = "?",
    [TK_COLON]        = ":",
    [TK_SEMICOLON]    = ";",
    [TK_COMMA]        = ",",
    [TK_LPAREN]       = "(",
    [TK_RPAREN]       = ")",
    [TK_LBRACKET]     = "[",
    [TK_RBRACKET]     = "]",
    [TK_LBRACE]       = "{",
    [TK_RBRACE]       = "}",
    [TK_HASH]         = "#",
    [TK_HASH_HASH]    = "##",
    /* special */
    [TK_EOF]          = "<eof>",
    [TK_ERROR]        = "<error>",
};

const char *token_kind_name(TokenKind k) {
    if (k < 0 || k >= TK_KIND_COUNT) return "<unknown>";
    const char *n = KIND_NAMES[k];
    return n ? n : "<unknown>";
}

void token_print(Token t) {
    printf("[%3d:%-3d] %-16s", t.line, t.col, token_kind_name(t.kind));
    switch (t.kind) {
        case TK_INT:
            printf("  %llu", (unsigned long long)t.ival);
            if (t.isuf & INT_SUF_UNSIGNED) printf("u");
            if (t.isuf & INT_SUF_LONGLONG) printf("ll");
            else if (t.isuf & INT_SUF_LONG) printf("l");
            break;
        case TK_FLOAT:
            printf("  %g", t.fval);
            if (t.fsuf == FLOAT_SUF_FLOAT) printf("f");
            else if (t.fsuf == FLOAT_SUF_LONG) printf("L");
            break;
        case TK_CHAR:
            if (t.cval >= 32 && t.cval < 127)
                printf("  '%c' (0x%llx)", (char)t.cval, (unsigned long long)t.cval);
            else
                printf("  0x%llx", (unsigned long long)t.cval);
            break;
        case TK_STRING:
            printf("  \"%s\"", t.sval);
            break;
        case TK_IDENT:
            printf("  %s", t.sval);
            break;
        default:
            /* raw lexeme */
            if (t.length > 0)
                printf("  %.*s", t.length, t.start);
            break;
    }
    printf("\n");
}
