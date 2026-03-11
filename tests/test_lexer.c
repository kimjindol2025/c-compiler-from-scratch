/*
 * tests/test_lexer.c — Comprehensive test suite for the C11 lexer
 *
 * Tests are self-contained; no external framework required.
 * Build & run:
 *   gcc -std=c11 -Wall -Wextra -I../include ../src/lexer.c test_lexer.c -o test_lexer
 *   ./test_lexer
 */

#include "../include/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>   /* fabs */
#include <assert.h>

/* =========================================================
 *  Mini test harness
 * ========================================================= */

static int g_pass = 0;
static int g_fail = 0;
static const char *g_suite = "";

#define SUITE(name) do { g_suite = (name); printf("\n=== %s ===\n", g_suite); } while(0)

#define CHECK(expr) do {                                    \
    if (expr) {                                             \
        g_pass++;                                           \
    } else {                                                \
        g_fail++;                                           \
        fprintf(stderr, "  FAIL %s:%d  %s\n",              \
                __FILE__, __LINE__, #expr);                 \
    }                                                       \
} while(0)

#define CHECK_MSG(expr, ...) do {                           \
    if (expr) {                                             \
        g_pass++;                                           \
    } else {                                                \
        g_fail++;                                           \
        fprintf(stderr, "  FAIL %s:%d  ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                       \
        fprintf(stderr, "\n");                              \
    }                                                       \
} while(0)

/* Lex a single token from a source string */
static Token one_token(const char *src) {
    Lexer *l = lexer_new(src, "<test>");
    Token t  = lexer_next(l);
    lexer_free(l);
    return t;
}

/* Lex all tokens from src into out[], return count (excludes TK_EOF) */
static int all_tokens(const char *src, Token *out, int max) {
    Lexer *l = lexer_new(src, "<test>");
    int n = 0;
    for (;;) {
        Token t = lexer_next(l);
        if (t.kind == TK_EOF || t.kind == TK_ERROR) break;
        if (n < max) out[n] = t;
        n++;
    }
    lexer_free(l);
    return n;
}

/* Free an array of tokens (those that own heap data) */
static void free_tokens(Token *arr, int n) {
    for (int i = 0; i < n; i++) token_free(arr[i]);
}

/* =========================================================
 *  Test: EOF on empty input
 * ========================================================= */
static void test_empty(void) {
    SUITE("Empty input");
    Token t = one_token("");
    CHECK(t.kind == TK_EOF);

    t = one_token("   \t\n\r\n  ");
    CHECK(t.kind == TK_EOF);
}

/* =========================================================
 *  Test: Keywords
 * ========================================================= */
static void test_keywords(void) {
    SUITE("Keywords");

    struct { const char *src; TokenKind kind; } kw[] = {
        { "auto",           TK_AUTO           },
        { "break",          TK_BREAK          },
        { "case",           TK_CASE           },
        { "char",           TK_CHAR_KW        },
        { "const",          TK_CONST          },
        { "continue",       TK_CONTINUE       },
        { "default",        TK_DEFAULT        },
        { "do",             TK_DO             },
        { "double",         TK_DOUBLE         },
        { "else",           TK_ELSE           },
        { "enum",           TK_ENUM           },
        { "extern",         TK_EXTERN         },
        { "float",          TK_FLOAT_KW       },
        { "for",            TK_FOR            },
        { "goto",           TK_GOTO           },
        { "if",             TK_IF             },
        { "inline",         TK_INLINE         },
        { "int",            TK_INT_KW         },
        { "long",           TK_LONG           },
        { "register",       TK_REGISTER       },
        { "restrict",       TK_RESTRICT       },
        { "return",         TK_RETURN         },
        { "short",          TK_SHORT          },
        { "signed",         TK_SIGNED         },
        { "sizeof",         TK_SIZEOF         },
        { "static",         TK_STATIC         },
        { "struct",         TK_STRUCT         },
        { "switch",         TK_SWITCH         },
        { "typedef",        TK_TYPEDEF        },
        { "union",          TK_UNION          },
        { "unsigned",       TK_UNSIGNED       },
        { "void",           TK_VOID           },
        { "volatile",       TK_VOLATILE       },
        { "while",          TK_WHILE          },
        { "_Alignas",       TK_ALIGNAS        },
        { "_Alignof",       TK_ALIGNOF        },
        { "_Atomic",        TK_ATOMIC         },
        { "_Bool",          TK_BOOL           },
        { "_Complex",       TK_COMPLEX        },
        { "_Generic",       TK_GENERIC        },
        { "_Imaginary",     TK_IMAGINARY      },
        { "_Noreturn",      TK_NORETURN       },
        { "_Static_assert", TK_STATIC_ASSERT  },
        { "_Thread_local",  TK_THREAD_LOCAL   },
    };
    int n = (int)(sizeof(kw)/sizeof(kw[0]));
    for (int i = 0; i < n; i++) {
        Token t = one_token(kw[i].src);
        CHECK_MSG(t.kind == kw[i].kind,
                  "'%s' expected kind=%d got=%d",
                  kw[i].src, kw[i].kind, t.kind);
    }

    /* Keywords must NOT match as prefix of longer identifiers */
    Token t;
    t = one_token("integer");  /* starts with 'int' but is identifier */
    CHECK(t.kind == TK_IDENT && strcmp(t.sval, "integer") == 0);
    token_free(t);

    t = one_token("forall");   /* starts with 'for' */
    CHECK(t.kind == TK_IDENT && strcmp(t.sval, "forall") == 0);
    token_free(t);

    t = one_token("_Bool2");   /* _Bool followed by digit */
    CHECK(t.kind == TK_IDENT);
    token_free(t);
}

/* =========================================================
 *  Test: Identifiers
 * ========================================================= */
static void test_identifiers(void) {
    SUITE("Identifiers");

    const char *ids[] = {
        "x", "foo", "bar123", "_priv", "__stdcall",
        "CamelCase", "snake_case", "a1b2c3", "_",
        "_Z12mangled_namev"
    };
    for (int i = 0; i < (int)(sizeof(ids)/sizeof(ids[0])); i++) {
        Token t = one_token(ids[i]);
        CHECK_MSG(t.kind == TK_IDENT,
                  "'%s' should be TK_IDENT, got %d", ids[i], t.kind);
        if (t.kind == TK_IDENT) {
            CHECK(strcmp(t.sval, ids[i]) == 0);
        }
        token_free(t);
    }

    /* line/col tracking */
    Lexer *l = lexer_new("   foo", "<test>");
    Token t2 = lexer_next(l);
    CHECK(t2.line == 1 && t2.col == 4);
    token_free(t2);
    lexer_free(l);
}

/* =========================================================
 *  Test: Integer literals
 * ========================================================= */
static void test_integers(void) {
    SUITE("Integer literals");

    /* Decimal */
    Token t;
    t = one_token("0");       CHECK(t.kind == TK_INT && t.ival == 0);
    t = one_token("42");      CHECK(t.kind == TK_INT && t.ival == 42);
    t = one_token("1234567"); CHECK(t.kind == TK_INT && t.ival == 1234567);
    t = one_token("0000");    CHECK(t.kind == TK_INT && t.ival == 0);

    /* Hexadecimal */
    t = one_token("0x0");     CHECK(t.kind == TK_INT && t.ival == 0);
    t = one_token("0xFF");    CHECK(t.kind == TK_INT && t.ival == 255);
    t = one_token("0XDEADBEEF"); CHECK(t.kind == TK_INT && t.ival == 0xDEADBEEF);
    t = one_token("0x10");    CHECK(t.kind == TK_INT && t.ival == 16);
    t = one_token("0xABCDEF"); CHECK(t.kind == TK_INT && t.ival == 0xABCDEF);

    /* Octal */
    t = one_token("0755");    CHECK(t.kind == TK_INT && t.ival == 0755);
    t = one_token("0644");    CHECK(t.kind == TK_INT && t.ival == 0644);
    t = one_token("00");      CHECK(t.kind == TK_INT && t.ival == 0);
    t = one_token("07");      CHECK(t.kind == TK_INT && t.ival == 7);
    t = one_token("010");     CHECK(t.kind == TK_INT && t.ival == 8);

    /* Binary (GCC extension) */
    t = one_token("0b0");     CHECK(t.kind == TK_INT && t.ival == 0);
    t = one_token("0b1");     CHECK(t.kind == TK_INT && t.ival == 1);
    t = one_token("0b1010");  CHECK(t.kind == TK_INT && t.ival == 10);
    t = one_token("0B11001100"); CHECK(t.kind == TK_INT && t.ival == 0xCC);

    /* Suffixes */
    t = one_token("42u");     CHECK(t.kind == TK_INT && t.ival == 42 && (t.isuf & INT_SUF_UNSIGNED));
    t = one_token("42U");     CHECK(t.kind == TK_INT && t.ival == 42 && (t.isuf & INT_SUF_UNSIGNED));
    t = one_token("42l");     CHECK(t.kind == TK_INT && t.ival == 42 && (t.isuf & INT_SUF_LONG));
    t = one_token("42L");     CHECK(t.kind == TK_INT && t.ival == 42 && (t.isuf & INT_SUF_LONG));
    t = one_token("42ll");    CHECK(t.kind == TK_INT && t.ival == 42 && (t.isuf & INT_SUF_LONGLONG));
    t = one_token("42LL");    CHECK(t.kind == TK_INT && t.ival == 42 && (t.isuf & INT_SUF_LONGLONG));
    t = one_token("42ul");    CHECK(t.kind == TK_INT && t.ival == 42 &&
                                    (t.isuf & INT_SUF_UNSIGNED) && (t.isuf & INT_SUF_LONG));
    t = one_token("42ULL");   CHECK(t.kind == TK_INT && t.ival == 42 &&
                                    (t.isuf & INT_SUF_UNSIGNED) && (t.isuf & INT_SUF_LONGLONG));
    t = one_token("42llu");   CHECK(t.kind == TK_INT && t.ival == 42 &&
                                    (t.isuf & INT_SUF_UNSIGNED) && (t.isuf & INT_SUF_LONGLONG));
    t = one_token("0xFFFFFFFFu"); CHECK(t.kind == TK_INT && t.ival == 0xFFFFFFFF &&
                                        (t.isuf & INT_SUF_UNSIGNED));

    /* Large values */
    t = one_token("18446744073709551615ULL");
    CHECK(t.kind == TK_INT && t.ival == 18446744073709551615ULL);
}

/* =========================================================
 *  Test: Float literals
 * ========================================================= */
#define NEAR(a, b)  (fabs((double)(a) - (double)(b)) < 1e-9 * fabs((double)(b) + 1e-300))

static void test_floats(void) {
    SUITE("Float literals");

    Token t;

    /* Basic */
    t = one_token("3.14");     CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 3.14));
    t = one_token("0.0");      CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 0.0));
    t = one_token("1.0");      CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1.0));
    t = one_token("1.");       CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1.0));
    t = one_token(".5");       CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 0.5));
    t = one_token(".25");      CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 0.25));

    /* Exponent */
    t = one_token("1e3");      CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1000.0));
    t = one_token("1E3");      CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1000.0));
    t = one_token("1.5e2");    CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 150.0));
    t = one_token("1e-3");     CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 0.001));
    t = one_token("2.5e+2");   CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 250.0));
    t = one_token("1.0e0");    CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1.0));

    /* Suffixes */
    t = one_token("3.14f");    CHECK(t.kind == TK_FLOAT && t.fsuf == FLOAT_SUF_FLOAT);
    t = one_token("3.14F");    CHECK(t.kind == TK_FLOAT && t.fsuf == FLOAT_SUF_FLOAT);
    t = one_token("3.14l");    CHECK(t.kind == TK_FLOAT && t.fsuf == FLOAT_SUF_LONG);
    t = one_token("3.14L");    CHECK(t.kind == TK_FLOAT && t.fsuf == FLOAT_SUF_LONG);
    t = one_token("1e3f");     CHECK(t.kind == TK_FLOAT && t.fsuf == FLOAT_SUF_FLOAT);

    /* Hex float */
    t = one_token("0x1p0");    CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1.0));
    t = one_token("0x1p4");    CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 16.0));
    t = one_token("0x1.8p1");  CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 3.0));
}

/* =========================================================
 *  Test: Character literals
 * ========================================================= */
static void test_chars(void) {
    SUITE("Character literals");

    Token t;
    t = one_token("'a'");    CHECK(t.kind == TK_CHAR && t.cval == 'a');
    t = one_token("'A'");    CHECK(t.kind == TK_CHAR && t.cval == 'A');
    t = one_token("'0'");    CHECK(t.kind == TK_CHAR && t.cval == '0');
    t = one_token("' '");    CHECK(t.kind == TK_CHAR && t.cval == ' ');
    t = one_token("'z'");    CHECK(t.kind == TK_CHAR && t.cval == 'z');

    /* Standard escape sequences */
    t = one_token("'\\n'");  CHECK(t.kind == TK_CHAR && t.cval == '\n');
    t = one_token("'\\t'");  CHECK(t.kind == TK_CHAR && t.cval == '\t');
    t = one_token("'\\r'");  CHECK(t.kind == TK_CHAR && t.cval == '\r');
    t = one_token("'\\0'");  CHECK(t.kind == TK_CHAR && t.cval == '\0');
    t = one_token("'\\a'");  CHECK(t.kind == TK_CHAR && t.cval == '\a');
    t = one_token("'\\b'");  CHECK(t.kind == TK_CHAR && t.cval == '\b');
    t = one_token("'\\f'");  CHECK(t.kind == TK_CHAR && t.cval == '\f');
    t = one_token("'\\v'");  CHECK(t.kind == TK_CHAR && t.cval == '\v');
    t = one_token("'\\\\'"); CHECK(t.kind == TK_CHAR && t.cval == '\\');
    t = one_token("'\\''");  CHECK(t.kind == TK_CHAR && t.cval == '\'');
    t = one_token("'\\\"'"); CHECK(t.kind == TK_CHAR && t.cval == '"');
    t = one_token("'\\?'");  CHECK(t.kind == TK_CHAR && t.cval == '?');

    /* Hex escapes */
    t = one_token("'\\x41'"); CHECK(t.kind == TK_CHAR && t.cval == 0x41); /* 'A' */
    t = one_token("'\\xFF'"); CHECK(t.kind == TK_CHAR && t.cval == 0xFF);
    t = one_token("'\\x0'");  CHECK(t.kind == TK_CHAR && t.cval == 0);

    /* Octal escapes */
    t = one_token("'\\101'"); CHECK(t.kind == TK_CHAR && t.cval == 0101); /* 'A' */
    t = one_token("'\\7'");   CHECK(t.kind == TK_CHAR && t.cval == 7);
    t = one_token("'\\077'"); CHECK(t.kind == TK_CHAR && t.cval == 077);
}

/* =========================================================
 *  Test: String literals
 * ========================================================= */
static void test_strings(void) {
    SUITE("String literals");

    Token t;

    t = one_token("\"\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "") == 0);
    token_free(t);

    t = one_token("\"hello\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "hello") == 0);
    token_free(t);

    t = one_token("\"hello world\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "hello world") == 0);
    token_free(t);

    t = one_token("\"line1\\nline2\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "line1\nline2") == 0);
    token_free(t);

    t = one_token("\"tab\\there\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "tab\there") == 0);
    token_free(t);

    t = one_token("\"quote: \\\"hi\\\"\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "quote: \"hi\"") == 0);
    token_free(t);

    t = one_token("\"backslash: \\\\\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "backslash: \\") == 0);
    token_free(t);

    t = one_token("\"hex: \\x41\\x42\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "hex: AB") == 0);
    token_free(t);

    t = one_token("\"octal: \\101\\102\"");
    CHECK(t.kind == TK_STRING && strcmp(t.sval, "octal: AB") == 0);
    token_free(t);

    t = one_token("\"null: \\0 end\"");
    /* The string should contain an embedded NUL after "null: " */
    CHECK(t.kind == TK_STRING);
    if (t.kind == TK_STRING) {
        CHECK(t.sval[6] == '\0');  /* NUL at position 6 */
    }
    token_free(t);

    /* All standard escapes in one string */
    t = one_token("\"\\a\\b\\f\\n\\r\\t\\v\\\\\\'\\\"\\?\"");
    CHECK(t.kind == TK_STRING);
    if (t.kind == TK_STRING) {
        CHECK(t.sval[0] == '\a');
        CHECK(t.sval[1] == '\b');
        CHECK(t.sval[2] == '\f');
        CHECK(t.sval[3] == '\n');
        CHECK(t.sval[4] == '\r');
        CHECK(t.sval[5] == '\t');
        CHECK(t.sval[6] == '\v');
        CHECK(t.sval[7] == '\\');
        CHECK(t.sval[8] == '\'');
        CHECK(t.sval[9] == '"');
        CHECK(t.sval[10] == '?');
    }
    token_free(t);
}

/* =========================================================
 *  Test: Operators
 * ========================================================= */
static void test_operators(void) {
    SUITE("Operators");

    struct { const char *src; TokenKind kind; } ops[] = {
        /* arithmetic */
        { "+",   TK_PLUS        },
        { "-",   TK_MINUS       },
        { "*",   TK_STAR        },
        { "/",   TK_SLASH       },
        { "%",   TK_PERCENT     },
        /* bitwise */
        { "&",   TK_AMP         },
        { "|",   TK_PIPE        },
        { "^",   TK_CARET       },
        { "~",   TK_TILDE       },
        { "<<",  TK_LSHIFT      },
        { ">>",  TK_RSHIFT      },
        /* logical */
        { "!",   TK_BANG        },
        { "&&",  TK_AMP_AMP     },
        { "||",  TK_PIPE_PIPE   },
        /* comparison */
        { "==",  TK_EQ_EQ       },
        { "!=",  TK_BANG_EQ     },
        { "<",   TK_LT          },
        { ">",   TK_GT          },
        { "<=",  TK_LT_EQ       },
        { ">=",  TK_GT_EQ       },
        /* assignment */
        { "=",   TK_EQ          },
        { "+=",  TK_PLUS_EQ     },
        { "-=",  TK_MINUS_EQ    },
        { "*=",  TK_STAR_EQ     },
        { "/=",  TK_SLASH_EQ    },
        { "%=",  TK_PERCENT_EQ  },
        { "&=",  TK_AMP_EQ      },
        { "|=",  TK_PIPE_EQ     },
        { "^=",  TK_CARET_EQ    },
        { "<<=", TK_LSHIFT_EQ   },
        { ">>=", TK_RSHIFT_EQ   },
        /* increment/decrement */
        { "++",  TK_PLUS_PLUS   },
        { "--",  TK_MINUS_MINUS },
        /* access */
        { "->",  TK_ARROW       },
        { ".",   TK_DOT         },
        /* misc */
        { "?",   TK_QUESTION    },
        { ":",   TK_COLON       },
        { ";",   TK_SEMICOLON   },
        { ",",   TK_COMMA       },
        { "...", TK_ELLIPSIS    },
        /* brackets */
        { "(",   TK_LPAREN      },
        { ")",   TK_RPAREN      },
        { "[",   TK_LBRACKET    },
        { "]",   TK_RBRACKET    },
        { "{",   TK_LBRACE      },
        { "}",   TK_RBRACE      },
        /* preprocessor */
        { "#",   TK_HASH        },
        { "##",  TK_HASH_HASH   },
    };

    for (int i = 0; i < (int)(sizeof(ops)/sizeof(ops[0])); i++) {
        Token t = one_token(ops[i].src);
        CHECK_MSG(t.kind == ops[i].kind,
                  "'%s' expected %s got %s",
                  ops[i].src,
                  token_kind_name(ops[i].kind),
                  token_kind_name(t.kind));
    }
}

/* =========================================================
 *  Test: Comments
 * ========================================================= */
static void test_comments(void) {
    SUITE("Comments");

    Token t;

    /* Line comment: nothing visible, just whitespace after */
    t = one_token("// this is a comment\n42");
    CHECK(t.kind == TK_INT && t.ival == 42);

    /* Line comment at end of file */
    t = one_token("// eof comment");
    CHECK(t.kind == TK_EOF);

    /* Block comment */
    t = one_token("/* block */ 99");
    CHECK(t.kind == TK_INT && t.ival == 99);

    /* Block comment spanning multiple lines */
    t = one_token("/* line1\n   line2\n   line3 */ 7");
    CHECK(t.kind == TK_INT && t.ival == 7);

    /* Nested-looking block comment: ends at the first closing marker */
    t = one_token("/* a /* b */ 3");
    CHECK(t.kind == TK_INT && t.ival == 3);

    /* Multiple comments */
    Lexer *l = lexer_new("// c1\n/* c2 */\n// c3\nfoo", "<test>");
    t = lexer_next(l);
    CHECK(t.kind == TK_IDENT && strcmp(t.sval, "foo") == 0);
    token_free(t);
    lexer_free(l);
}

/* =========================================================
 *  Test: Line/column tracking
 * ========================================================= */
static void test_linecol(void) {
    SUITE("Line/column tracking");

    const char *src =
        "int x;\n"          /* line 1 */
        "float y;\n"        /* line 2 */
        "return 0;\n";      /* line 3 */

    Lexer *l = lexer_new(src, "<test>");

    Token t0 = lexer_next(l);  /* int   */
    CHECK(t0.line == 1 && t0.col == 1);

    Token t1 = lexer_next(l);  /* x     */
    CHECK(t1.line == 1 && t1.col == 5);
    token_free(t1);

    Token t2 = lexer_next(l);  /* ;     */
    CHECK(t2.line == 1 && t2.col == 6);

    Token t3 = lexer_next(l);  /* float */
    CHECK(t3.line == 2 && t3.col == 1);

    Token t4 = lexer_next(l);  /* y     */
    CHECK(t4.line == 2 && t4.col == 7);
    token_free(t4);

    lexer_free(l);
}

/* =========================================================
 *  Test: Preprocessor directives (tokenised raw)
 * ========================================================= */
static void test_preprocessor(void) {
    SUITE("Preprocessor tokens");

    /* #include should tokenise as TK_HASH then TK_IDENT */
    Token toks[16];
    int n = all_tokens("#include <stdio.h>", toks, 16);

    /* Expect at least: # include < stdio . h > */
    CHECK(n >= 2);
    if (n >= 2) {
        CHECK(toks[0].kind == TK_HASH);
        CHECK(toks[1].kind == TK_IDENT && strcmp(toks[1].sval, "include") == 0);
    }
    free_tokens(toks, n);

    /* ## operator */
    n = all_tokens("a##b", toks, 16);
    CHECK(n == 3);
    if (n >= 3) {
        CHECK(toks[0].kind == TK_IDENT);
        CHECK(toks[1].kind == TK_HASH_HASH);
        CHECK(toks[2].kind == TK_IDENT);
    }
    free_tokens(toks, n);
}

/* =========================================================
 *  Test: Lookahead (peek)
 * ========================================================= */
static void test_peek(void) {
    SUITE("Lookahead (peek)");

    Lexer *l = lexer_new("42 hello", "<test>");

    Token p1 = lexer_peek(l);
    Token p2 = lexer_peek(l);   /* should be idempotent */
    CHECK(p1.kind == TK_INT && p1.ival == 42);
    CHECK(p2.kind == TK_INT && p2.ival == 42);

    Token n1 = lexer_next(l);   /* consume 42 */
    CHECK(n1.kind == TK_INT && n1.ival == 42);

    Token p3 = lexer_peek(l);   /* now peek at "hello" */
    CHECK(p3.kind == TK_IDENT);

    Token n2 = lexer_next(l);   /* consume "hello" */
    CHECK(n2.kind == TK_IDENT && strcmp(n2.sval, "hello") == 0);
    token_free(n2);

    Token eof = lexer_next(l);
    CHECK(eof.kind == TK_EOF);

    lexer_free(l);
}

/* =========================================================
 *  Test: A realistic C snippet
 * ========================================================= */
static void test_c_snippet(void) {
    SUITE("Realistic C snippet");

    const char *src =
        "/* Fibonacci */\n"
        "int fib(int n) {\n"
        "    if (n <= 1) return n;\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "}\n";

    Token toks[64];
    int n = all_tokens(src, toks, 64);

    /* Spot-check a few key tokens */
    CHECK(n > 10);
    if (n > 0) CHECK(toks[0].kind == TK_INT_KW);          /* int */
    if (n > 1) {
        CHECK(toks[1].kind == TK_IDENT &&
              strcmp(toks[1].sval, "fib") == 0);            /* fib */
    }
    if (n > 2) CHECK(toks[2].kind == TK_LPAREN);          /* (   */
    if (n > 3) CHECK(toks[3].kind == TK_INT_KW);          /* int */

    free_tokens(toks, n);
}

/* =========================================================
 *  Test: token_kind_name() covers every kind
 * ========================================================= */
static void test_kind_names(void) {
    SUITE("token_kind_name");

    for (int k = 0; k < TK_KIND_COUNT; k++) {
        const char *name = token_kind_name((TokenKind)k);
        CHECK_MSG(name != NULL && strcmp(name, "<unknown>") != 0,
                  "kind %d has no name", k);
    }

    /* Out-of-range should not crash */
    const char *bad = token_kind_name((TokenKind)-1);
    CHECK(bad != NULL);
    bad = token_kind_name(TK_KIND_COUNT);
    CHECK(bad != NULL);
}

/* =========================================================
 *  Test: Multiple consecutive tokens / token stream
 * ========================================================= */
static void test_token_stream(void) {
    SUITE("Token stream");

    /* Typical variable declaration */
    const char *src = "unsigned long long x = 0xDEADull;";
    Token toks[16];
    int n = all_tokens(src, toks, 16);

    CHECK(n == 7);
    if (n >= 1) CHECK(toks[0].kind == TK_UNSIGNED);
    if (n >= 2) CHECK(toks[1].kind == TK_LONG);
    if (n >= 3) CHECK(toks[2].kind == TK_LONG);
    if (n >= 4) {
        CHECK(toks[3].kind == TK_IDENT);
        if (toks[3].kind == TK_IDENT)
            CHECK(strcmp(toks[3].sval, "x") == 0);
    }
    if (n >= 5) CHECK(toks[4].kind == TK_EQ);
    if (n >= 6) {
        CHECK(toks[5].kind == TK_INT);
        CHECK(toks[5].ival == 0xDEAD);
        CHECK(toks[5].isuf & INT_SUF_UNSIGNED);
        CHECK(toks[5].isuf & INT_SUF_LONGLONG);
    }
    if (n >= 7) CHECK(toks[6].kind == TK_SEMICOLON);

    free_tokens(toks, n);

    /* Chained comparison (not valid C, but must tokenise correctly) */
    const char *src2 = "a <= b >= c == d != e";
    int n2 = all_tokens(src2, toks, 16);
    CHECK(n2 == 9);
    if (n2 >= 9) {
        CHECK(toks[1].kind == TK_LT_EQ);
        CHECK(toks[3].kind == TK_GT_EQ);
        CHECK(toks[5].kind == TK_EQ_EQ);
        CHECK(toks[7].kind == TK_BANG_EQ);
    }
    free_tokens(toks, n2);

    /* <<= and >>= */
    const char *src3 = "x <<= 2; y >>= 3;";
    int n3 = all_tokens(src3, toks, 16);
    CHECK(n3 >= 6);
    if (n3 >= 3) CHECK(toks[1].kind == TK_LSHIFT_EQ);
    if (n3 >= 7) CHECK(toks[5].kind == TK_RSHIFT_EQ);
    free_tokens(toks, n3);
}

/* =========================================================
 *  Test: Ellipsis
 * ========================================================= */
static void test_ellipsis(void) {
    SUITE("Ellipsis");

    Token t;
    t = one_token("...");
    CHECK(t.kind == TK_ELLIPSIS);

    /* In context: void f(int x, ...)  => void f ( int x , ... ) = 8 tokens */
    Token toks[16];
    int n = all_tokens("void f(int x, ...)", toks, 16);
    CHECK(n == 8);
    if (n >= 7) CHECK(toks[6].kind == TK_ELLIPSIS);
    free_tokens(toks, n);
}

/* =========================================================
 *  Test: Struct/enum/typedef snippet
 * ========================================================= */
static void test_struct_snippet(void) {
    SUITE("Struct / enum / typedef");

    const char *src =
        "typedef struct Point {\n"
        "    int x, y;\n"
        "} Point;\n"
        "\n"
        "enum Color { RED = 0, GREEN, BLUE };\n";

    Token toks[32];
    int n = all_tokens(src, toks, 32);
    CHECK(n > 5);
    if (n >= 1) CHECK(toks[0].kind == TK_TYPEDEF);
    if (n >= 2) CHECK(toks[1].kind == TK_STRUCT);

    free_tokens(toks, n);
}

/* =========================================================
 *  Test: Pointer / dereference operators
 * ========================================================= */
static void test_pointer_ops(void) {
    SUITE("Pointer operators");

    /* int *p;  — star between type and name */
    Token toks[8];
    int n = all_tokens("int *p;", toks, 8);
    CHECK(n == 4);
    if (n >= 2) CHECK(toks[1].kind == TK_STAR);
    free_tokens(toks, n);

    /* p->x */
    n = all_tokens("p->x", toks, 8);
    CHECK(n == 3);
    if (n >= 2) CHECK(toks[1].kind == TK_ARROW);
    free_tokens(toks, n);

    /* (*p).x  => ( * p ) . x  = 6 tokens */
    n = all_tokens("(*p).x", toks, 8);
    CHECK(n == 6);
    if (n >= 5) CHECK(toks[4].kind == TK_DOT);
    free_tokens(toks, n);
}

/* =========================================================
 *  Test: _Atomic / _Bool / _Complex
 * ========================================================= */
static void test_c11_keywords(void) {
    SUITE("C11 _Keywords");

    Token toks[8];
    int n;

    n = all_tokens("_Atomic int x;", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_ATOMIC);
    free_tokens(toks, n);

    n = all_tokens("_Bool flag;", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_BOOL);
    free_tokens(toks, n);

    n = all_tokens("_Complex double z;", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_COMPLEX);
    free_tokens(toks, n);

    n = all_tokens("_Noreturn void die(void);", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_NORETURN);
    free_tokens(toks, n);

    n = all_tokens("_Static_assert(sizeof(int)==4, \"int must be 4 bytes\");", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_STATIC_ASSERT);
    free_tokens(toks, n);

    n = all_tokens("_Thread_local int tls;", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_THREAD_LOCAL);
    free_tokens(toks, n);

    n = all_tokens("_Alignas(16) char buf[64];", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_ALIGNAS);
    free_tokens(toks, n);

    n = all_tokens("_Alignof(double)", toks, 8);
    CHECK(n >= 1 && toks[0].kind == TK_ALIGNOF);
    free_tokens(toks, n);
}

/* =========================================================
 *  Test: Special float forms
 * ========================================================= */
static void test_float_edge_cases(void) {
    SUITE("Float edge cases");

    Token t;

    /* Starts with digit, no leading zero required in exponent */
    t = one_token("0e0");   CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 0.0));
    t = one_token("1e0");   CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1.0));
    t = one_token("1e10");  CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1e10));
    t = one_token("1e-10"); CHECK(t.kind == TK_FLOAT && NEAR(t.fval, 1e-10));

    /* Very small float suffix — just checks kind, not value precision */
    t = one_token("1.0f");
    CHECK(t.kind == TK_FLOAT && t.fsuf == FLOAT_SUF_FLOAT);
    t = one_token("0.0L");
    CHECK(t.kind == TK_FLOAT && t.fsuf == FLOAT_SUF_LONG);
}

/* =========================================================
 *  Test: Separator between tokens
 * ========================================================= */
static void test_no_separator_needed(void) {
    SUITE("No separator needed between tokens");

    /* In C, >> can be two separate > > tokens in template context,
       but the lexer always produces TK_RSHIFT — that is correct
       for a C compiler. */
    Token toks[8];
    int n = all_tokens("a>>b", toks, 8);
    CHECK(n == 3);
    if (n >= 2) CHECK(toks[1].kind == TK_RSHIFT);
    free_tokens(toks, n);

    /* a+b — no spaces */
    n = all_tokens("a+b", toks, 8);
    CHECK(n == 3);
    if (n >= 2) CHECK(toks[1].kind == TK_PLUS);
    free_tokens(toks, n);

    /* a++b parses as a ++ b */
    n = all_tokens("a++b", toks, 8);
    CHECK(n == 3);
    if (n >= 2) CHECK(toks[1].kind == TK_PLUS_PLUS);
    free_tokens(toks, n);
}

/* =========================================================
 *  Test: Unicode escape in string
 * ========================================================= */
static void test_unicode_escapes(void) {
    SUITE("Unicode escapes");

    Token t;

    /* \u0041 == 'A' (low byte) */
    t = one_token("\"\\u0041\"");
    CHECK(t.kind == TK_STRING);
    if (t.kind == TK_STRING) {
        CHECK((unsigned char)t.sval[0] == 0x41);
    }
    token_free(t);

    /* \U0000004B == 'K' */
    t = one_token("\"\\U0000004B\"");
    CHECK(t.kind == TK_STRING);
    if (t.kind == TK_STRING) {
        CHECK((unsigned char)t.sval[0] == 0x4B);
    }
    token_free(t);
}

/* =========================================================
 *  Test: Macro-like compound usage
 * ========================================================= */
static void test_macro_usage(void) {
    SUITE("Macro-like usage");

    const char *src = "#define MAX(a,b) ((a)>(b)?(a):(b))";
    Token toks[32];
    int n = all_tokens(src, toks, 32);

    CHECK(n > 3);
    if (n >= 1) CHECK(toks[0].kind == TK_HASH);
    if (n >= 2) {
        CHECK(toks[1].kind == TK_IDENT &&
              strcmp(toks[1].sval, "define") == 0);
    }
    if (n >= 3) {
        CHECK(toks[2].kind == TK_IDENT &&
              strcmp(toks[2].sval, "MAX") == 0);
    }
    free_tokens(toks, n);
}

/* =========================================================
 *  Main
 * ========================================================= */
int main(void) {
    printf("=== C11 Lexer Test Suite ===\n");

    test_empty();
    test_keywords();
    test_identifiers();
    test_integers();
    test_floats();
    test_chars();
    test_strings();
    test_operators();
    test_comments();
    test_linecol();
    test_preprocessor();
    test_peek();
    test_c_snippet();
    test_kind_names();
    test_token_stream();
    test_ellipsis();
    test_struct_snippet();
    test_pointer_ops();
    test_c11_keywords();
    test_float_edge_cases();
    test_no_separator_needed();
    test_unicode_escapes();
    test_macro_usage();

    printf("\n=== Results: %d passed, %d failed ===\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
