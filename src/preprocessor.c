/*
 * preprocessor.c — text-based C preprocessor
 *
 * Pipeline: source text → preprocessed text
 * The preprocessed text is then fed into the lexer.
 */

#define _POSIX_C_SOURCE 200809L
#include "../include/preprocessor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

/* =========================================================
 * Virtual (built-in) headers
 * ========================================================= */

static const char *VIRTUAL_STDARG_H =
    "/* stdarg.h - built-in */\n"
    "#ifndef _STDARG_H\n"
    "#define _STDARG_H\n"
    "typedef char* va_list;\n"
    /* Register save area: rdi at [rbp-8], rsi at [rbp-16], etc.
     * va_start(ap, last): ap = &last - 8  (next slot down the stack)
     * va_arg(ap, type):   subtract 8 first, then read at ap+8 (old ap)
     *                     so: (ap) -= 8, then *(type*)((ap) + 8) */
    "#define va_start(ap, last) ((ap) = (char*)(&(last)) - 8)\n"
    "#define va_arg(ap, type)   (*(type*)((ap) -= 8, (ap) + 8))\n"
    "#define va_end(ap)         ((ap) = 0)\n"
    "#define va_copy(dst, src)  ((dst) = (src))\n"
    "#endif\n";

static const char *VIRTUAL_STDBOOL_H =
    "/* stdbool.h - built-in */\n"
    "#ifndef _STDBOOL_H\n"
    "#define _STDBOOL_H\n"
    "#define bool  _Bool\n"
    "#define true  1\n"
    "#define false 0\n"
    "#endif\n";

static const char *VIRTUAL_STDDEF_H =
    "/* stddef.h - built-in */\n"
    "#ifndef _STDDEF_H\n"
    "#define _STDDEF_H\n"
    "typedef long ptrdiff_t;\n"
    "typedef unsigned long size_t;\n"
    "#define NULL ((void*)0)\n"
    "#define offsetof(type, member) __builtin_offsetof(type, member)\n"
    "#endif\n";

static const char *VIRTUAL_STDIO_H =
    "/* stdio.h - minimal built-in */\n"
    "#ifndef _STDIO_H\n"
    "#define _STDIO_H\n"
    "typedef void FILE;\n"
    "extern FILE *stdout;\n"
    "extern FILE *stderr;\n"
    "extern FILE *stdin;\n"
    "int printf(const char *fmt, ...);\n"
    "int fprintf(FILE *f, const char *fmt, ...);\n"
    "int sprintf(char *buf, const char *fmt, ...);\n"
    "int snprintf(char *buf, unsigned long n, const char *fmt, ...);\n"
    "int scanf(const char *fmt, ...);\n"
    "int puts(const char *s);\n"
    "int putchar(int c);\n"
    "int getchar(void);\n"
    "void *fopen(const char *path, const char *mode);\n"
    "int fclose(FILE *f);\n"
    "unsigned long fread(void *ptr, unsigned long sz, unsigned long n, FILE *f);\n"
    "unsigned long fwrite(const void *ptr, unsigned long sz, unsigned long n, FILE *f);\n"
    "#endif\n";

static const char *VIRTUAL_STDLIB_H =
    "/* stdlib.h - minimal built-in */\n"
    "#ifndef _STDLIB_H\n"
    "#define _STDLIB_H\n"
    "void *malloc(unsigned long size);\n"
    "void *calloc(unsigned long n, unsigned long size);\n"
    "void *realloc(void *ptr, unsigned long size);\n"
    "void  free(void *ptr);\n"
    "void  exit(int status);\n"
    "int   abs(int n);\n"
    "long  atol(const char *s);\n"
    "int   atoi(const char *s);\n"
    "double atof(const char *s);\n"
    "#endif\n";

static const char *VIRTUAL_STRING_H =
    "/* string.h - minimal built-in */\n"
    "#ifndef _STRING_H\n"
    "#define _STRING_H\n"
    "unsigned long strlen(const char *s);\n"
    "char *strcpy(char *dst, const char *src);\n"
    "char *strncpy(char *dst, const char *src, unsigned long n);\n"
    "char *strcat(char *dst, const char *src);\n"
    "int   strcmp(const char *a, const char *b);\n"
    "int   strncmp(const char *a, const char *b, unsigned long n);\n"
    "char *strchr(const char *s, int c);\n"
    "char *strrchr(const char *s, int c);\n"
    "char *strstr(const char *haystack, const char *needle);\n"
    "void *memcpy(void *dst, const void *src, unsigned long n);\n"
    "void *memmove(void *dst, const void *src, unsigned long n);\n"
    "void *memset(void *dst, int c, unsigned long n);\n"
    "int   memcmp(const void *a, const void *b, unsigned long n);\n"
    "#endif\n";

/* =========================================================
 * Internal helpers
 * ========================================================= */

static unsigned pp_hash(const char *name) {
    unsigned h = 5381;
    for (; *name; name++) h = ((h << 5) + h) ^ (unsigned char)*name;
    return h % PP_MACRO_BUCKETS;
}

static void pp_out_char(Preprocessor *pp, char c) {
    if (pp->out_len + 1 >= pp->out_cap) {
        pp->out_cap = pp->out_cap ? pp->out_cap * 2 : 4096;
        pp->output  = realloc(pp->output, pp->out_cap);
    }
    pp->output[pp->out_len++] = c;
    pp->output[pp->out_len]   = '\0';
}

static void pp_out_str(Preprocessor *pp, const char *s) {
    for (; *s; s++) pp_out_char(pp, *s);
}

static MacroDef *pp_lookup(Preprocessor *pp, const char *name) {
    unsigned h = pp_hash(name);
    for (MacroDef *m = pp->macros[h]; m; m = m->next)
        if (strcmp(m->name, name) == 0) return m;
    return NULL;
}

static void pp_define_macro(Preprocessor *pp, const char *name,
                             const char *body, MacroParam *params, bool is_func, bool variadic) {
    unsigned h = pp_hash(name);
    /* Remove existing definition */
    MacroDef **p = &pp->macros[h];
    while (*p) {
        if (strcmp((*p)->name, name) == 0) {
            MacroDef *old = *p;
            *p = old->next;
            free(old->name); free(old->body);
            /* free param list */
            MacroParam *pm = old->params;
            while (pm) { MacroParam *nx = pm->next; free(pm->name); free(pm); pm = nx; }
            free(old);
            break;
        }
        p = &(*p)->next;
    }
    MacroDef *m = calloc(1, sizeof(MacroDef));
    m->name       = strdup(name);
    m->body       = body ? strdup(body) : strdup("");
    m->params     = params;
    m->is_func    = is_func;
    m->is_variadic = variadic;
    m->next       = pp->macros[h];
    pp->macros[h] = m;
}

static void pp_undef(Preprocessor *pp, const char *name) {
    unsigned h = pp_hash(name);
    MacroDef **p = &pp->macros[h];
    while (*p) {
        if (strcmp((*p)->name, name) == 0) {
            MacroDef *old = *p; *p = old->next;
            free(old->name); free(old->body);
            MacroParam *pm = old->params;
            while (pm) { MacroParam *nx = pm->next; free(pm->name); free(pm); pm = nx; }
            free(old);
            return;
        }
        p = &(*p)->next;
    }
}

/* Skip whitespace within a line (not newline) */
static const char *skip_hspace(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read identifier into buf (NUL-terminated), return pointer after it */
static const char *read_ident(const char *p, char *buf, int maxlen) {
    int n = 0;
    while (n < maxlen - 1 && (isalnum((unsigned char)*p) || *p == '_')) {
        buf[n++] = *p++;
    }
    buf[n] = '\0';
    return p;
}

/* Skip to end of line (not consuming the newline) */
static const char *skip_to_eol(const char *p) {
    while (*p && *p != '\n') {
        if (*p == '\\' && *(p+1) == '\n') { p += 2; continue; } /* line splice */
        p++;
    }
    return p;
}

/* Read a line (handling line splices) into buf. Returns pointer after newline. */
static const char *read_line(const char *p, char *buf, int maxlen) {
    int n = 0;
    while (*p && *p != '\n' && n < maxlen - 1) {
        if (*p == '\\' && *(p+1) == '\n') { p += 2; continue; }
        buf[n++] = *p++;
    }
    buf[n] = '\0';
    if (*p == '\n') p++;
    return p;
}

/* =========================================================
 * Macro expansion engine
 * ========================================================= */

/* Forward declaration */
static char *expand_text(Preprocessor *pp, const char *text,
                         const char **hide, int nhide);

/* Collect function-like macro arguments. Returns pointer after ')' */
static const char *collect_args(const char *p, char ***args_out, int *nargs_out) {
    /* p points to '(' */
    p++; /* skip '(' */
    int cap = 4, n = 0;
    char **args = malloc(cap * sizeof(char*));

    /* Handle empty arg list */
    const char *q = skip_hspace(p);
    if (*q == ')') {
        *args_out = args; *nargs_out = 0;
        return q + 1;
    }

    int depth = 0;
    char tmp[4096]; int ti = 0;
    while (*p) {
        if (*p == '(' ) depth++;
        else if (*p == ')') {
            if (depth == 0) {
                /* end of args */
                tmp[ti] = '\0';
                /* trim trailing spaces */
                while (ti > 0 && (tmp[ti-1]==' ' || tmp[ti-1]=='\t')) tmp[--ti]='\0';
                if (n >= cap) { cap *= 2; args = realloc(args, cap * sizeof(char*)); }
                args[n++] = strdup(tmp);
                p++;
                break;
            }
            depth--;
        } else if (*p == ',' && depth == 0) {
            tmp[ti] = '\0';
            while (ti > 0 && (tmp[ti-1]==' ' || tmp[ti-1]=='\t')) tmp[--ti]='\0';
            if (n >= cap) { cap *= 2; args = realloc(args, cap * sizeof(char*)); }
            args[n++] = strdup(tmp);
            ti = 0; p++;
            p = (const char*)skip_hspace(p);
            continue;
        } else if (*p == '"') {
            /* string literal — don't split on commas inside */
            tmp[ti++] = *p++;
            while (*p && *p != '"') {
                if (*p == '\\') tmp[ti++] = *p++;
                if (ti < (int)sizeof(tmp)-1) tmp[ti++] = *p++;
            }
            if (*p == '"') tmp[ti++] = *p++;
            continue;
        }
        if (ti < (int)sizeof(tmp)-1) tmp[ti++] = *p;
        p++;
    }
    *args_out = args; *nargs_out = n;
    return p;
}

/* Substitute params in body with args. Returns heap string. */
static char *subst_params(const char *body, MacroParam *params, char **args, int nargs,
                           bool is_variadic) {
    char *out = malloc(4096); int olen = 0, ocap = 4096;
    out[0] = '\0';
#define OUTC(c) do { if (olen+1>=ocap){ocap*=2;out=realloc(out,ocap);} out[olen++]=(c); out[olen]='\0'; } while(0)
#define OUTS(s) do { for(const char*_s=(s);*_s;_s++) OUTC(*_s); } while(0)

    const char *p = body;
    while (*p) {
        /* Handle # stringify and ## paste */
        if (*p == '#' && *(p+1) == '#') {
            /* token paste — remove surrounding whitespace already done */
            /* just skip ## */
            while (olen > 0 && (out[olen-1]==' '||out[olen-1]=='\t')) { olen--; out[olen]='\0'; }
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            continue;
        }
        if (*p == '#' && *(p+1) != '#') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            char name[256]; p = read_ident(p, name, sizeof(name));
            /* find param */
            int idx = 0; MacroParam *pm = params;
            while (pm && strcmp(pm->name, name)!=0) { pm=pm->next; idx++; }
            if (pm && idx < nargs) {
                /* stringify */
                OUTC('"');
                for (const char *s = args[idx]; *s; s++) {
                    if (*s == '"' || *s == '\\') OUTC('\\');
                    OUTC(*s);
                }
                OUTC('"');
            } else if (is_variadic && strcmp(name,"__VA_ARGS__")==0) {
                OUTC('"');
                for (int i = nargs; i < nargs; i++) {} /* variadic stringify - simplified */
                OUTC('"');
            } else {
                OUTC('#'); OUTS(name);
            }
            continue;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            char name[256]; const char *after = read_ident(p, name, sizeof(name));
            /* __VA_ARGS__ */
            if (is_variadic && strcmp(name, "__VA_ARGS__")==0) {
                /* find where named params end */
                int np = 0; for (MacroParam *pm = params; pm; pm=pm->next) np++;
                for (int i = np; i < nargs; i++) {
                    if (i > np) OUTS(", ");
                    OUTS(args[i]);
                }
                p = after; continue;
            }
            /* find in params */
            int idx = 0; MacroParam *pm = params;
            while (pm && strcmp(pm->name, name)!=0) { pm=pm->next; idx++; }
            if (pm && idx < nargs) {
                OUTS(args[idx]);
                p = after; continue;
            }
            OUTS(name); p = after; continue;
        }
        OUTC(*p++);
    }
    return out;
#undef OUTC
#undef OUTS
}

/* Expand text with macro substitution. hide[] = names currently being expanded (HideSet).
 * Returns heap-allocated string. */
static char *expand_text(Preprocessor *pp, const char *text,
                         const char **hide, int nhide) {
    char *out = malloc(4096); int olen = 0, ocap = 4096;
    out[0] = '\0';
#define OUTC(c) do { if (olen+1>=ocap){ocap*=2;out=realloc(out,ocap);} out[olen++]=(c); out[olen]='\0'; } while(0)
#define OUTS(s) do { for(const char*_s=(s);*_s;_s++) OUTC(*_s); } while(0)

    const char *p = text;
    while (*p) {
        /* String literals — pass through */
        if (*p == '"') {
            OUTC(*p++);
            while (*p && *p != '"') {
                if (*p == '\\') OUTC(*p++);
                OUTC(*p++);
            }
            if (*p) OUTC(*p++);
            continue;
        }
        /* Char literals */
        if (*p == '\'') {
            OUTC(*p++);
            while (*p && *p != '\'') {
                if (*p == '\\') OUTC(*p++);
                OUTC(*p++);
            }
            if (*p) OUTC(*p++);
            continue;
        }
        /* Comments (C style) */
        if (*p == '/' && *(p+1) == '*') {
            OUTC(' ');
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        /* Comments (C++ style) */
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        /* Identifier */
        if (isalpha((unsigned char)*p) || *p == '_') {
            char name[256]; const char *after = read_ident(p, name, sizeof(name));

            /* Check hide set */
            bool hidden = false;
            for (int i = 0; i < nhide; i++)
                if (strcmp(hide[i], name) == 0) { hidden = true; break; }

            if (hidden) { OUTS(name); p = after; continue; }

            MacroDef *m = pp_lookup(pp, name);
            if (!m) { OUTS(name); p = after; continue; }

            /* Object-like macro */
            if (!m->is_func) {
                /* Extend hide set */
                const char **new_hide = malloc((nhide+1) * sizeof(char*));
                if (nhide) memcpy(new_hide, hide, nhide * sizeof(char*));
                new_hide[nhide] = name;
                char *expanded = expand_text(pp, m->body, new_hide, nhide+1);
                free(new_hide);
                OUTS(expanded); free(expanded);
                p = after; continue;
            }

            /* Function-like: must be followed by '(' */
            const char *q = skip_hspace(after);
            if (*q != '(') { OUTS(name); p = after; continue; }

            /* Collect args */
            char **args; int nargs;
            const char *rest = collect_args(q, &args, &nargs);

            /* Substitute */
            char *body_subst = subst_params(m->body, m->params, args, nargs, m->is_variadic);

            /* Extend hide set and expand */
            const char **new_hide = malloc((nhide+1) * sizeof(char*));
            if (nhide) memcpy(new_hide, hide, nhide * sizeof(char*));
            new_hide[nhide] = name;
            char *expanded = expand_text(pp, body_subst, new_hide, nhide+1);
            free(new_hide); free(body_subst);

            OUTS(expanded); free(expanded);

            for (int i = 0; i < nargs; i++) free(args[i]);
            free(args);
            p = rest; continue;
        }
        OUTC(*p++);
    }
    return out;
#undef OUTC
#undef OUTS
}

/* =========================================================
 * Expression evaluator for #if
 * ========================================================= */

typedef struct { const char *p; Preprocessor *pp; } Eval;

static long eval_expr(Eval *e);

static void eval_skip_ws(Eval *e) {
    while (*e->p == ' ' || *e->p == '\t') e->p++;
}

static long eval_defined(Eval *e) {
    eval_skip_ws(e);
    bool paren = *e->p == '(';
    if (paren) e->p++;
    eval_skip_ws(e);
    char name[256];
    e->p = read_ident(e->p, name, sizeof(name));
    if (paren) { eval_skip_ws(e); if (*e->p == ')') e->p++; }
    return pp_lookup(e->pp, name) ? 1L : 0L;
}

static long eval_primary(Eval *e) {
    eval_skip_ws(e);
    if (*e->p == '(') {
        e->p++;
        long v = eval_expr(e);
        eval_skip_ws(e);
        if (*e->p == ')') e->p++;
        return v;
    }
    if (*e->p == '!') { e->p++; return !eval_primary(e); }
    if (*e->p == '-') { e->p++; return -eval_primary(e); }
    if (*e->p == '~') { e->p++; return ~eval_primary(e); }
    /* defined(...) */
    if (strncmp(e->p, "defined", 7)==0 && !isalnum((unsigned char)e->p[7]) && e->p[7]!='_') {
        e->p += 7;
        return eval_defined(e);
    }
    /* number */
    if (isdigit((unsigned char)*e->p)) {
        long v = 0; int base = 10;
        if (*e->p == '0' && (*(e->p+1)=='x'||*(e->p+1)=='X')) {
            base = 16; e->p += 2;
            while (isxdigit((unsigned char)*e->p)) {
                int d = isdigit((unsigned char)*e->p) ? *e->p-'0' : tolower(*e->p)-'a'+10;
                v = v*16 + d; e->p++;
            }
        } else {
            while (isdigit((unsigned char)*e->p)) v = v*10 + (*e->p++ - '0');
        }
        /* skip L/U suffixes */
        while (*e->p == 'L' || *e->p == 'U' || *e->p == 'l' || *e->p == 'u') e->p++;
        return v;
    }
    /* identifier → 0 (undefined) */
    if (isalpha((unsigned char)*e->p) || *e->p == '_') {
        char name[256]; e->p = read_ident(e->p, name, sizeof(name));
        MacroDef *m = pp_lookup(e->pp, name);
        if (m) {
            /* try to evaluate its body as a number */
            const char *q = m->body;
            while (*q == ' ' || *q == '\t') q++;
            if (isdigit((unsigned char)*q)) {
                Eval sub; sub.p = m->body; sub.pp = e->pp;
                return eval_primary(&sub);
            }
        }
        return 0L;
    }
    return 0L;
}

static long eval_mul(Eval *e) {
    long v = eval_primary(e);
    eval_skip_ws(e);
    while (*e->p == '*' || *e->p == '/' || *e->p == '%') {
        char op = *e->p++; long r = eval_primary(e);
        if (op == '*') v *= r;
        else if (op == '/') v = r ? v/r : 0;
        else v = r ? v%r : 0;
        eval_skip_ws(e);
    }
    return v;
}

static long eval_add(Eval *e) {
    long v = eval_mul(e);
    eval_skip_ws(e);
    while (*e->p == '+' || *e->p == '-') {
        char op = *e->p++; v = op=='+' ? v+eval_mul(e) : v-eval_mul(e);
        eval_skip_ws(e);
    }
    return v;
}

static long eval_shift(Eval *e) {
    long v = eval_add(e);
    eval_skip_ws(e);
    while ((*e->p=='<' && *(e->p+1)=='<') || (*e->p=='>' && *(e->p+1)=='>')) {
        bool left = *e->p == '<'; e->p += 2;
        long r = eval_add(e);
        v = left ? v<<r : v>>r;
        eval_skip_ws(e);
    }
    return v;
}

static long eval_rel(Eval *e) {
    long v = eval_shift(e);
    eval_skip_ws(e);
    while (*e->p=='<' || *e->p=='>') {
        bool eq = *(e->p+1) == '=';
        bool lt = *e->p == '<';
        e->p += eq ? 2 : 1;
        long r = eval_shift(e);
        if (lt) v = eq ? v<=r : v<r;
        else    v = eq ? v>=r : v>r;
        eval_skip_ws(e);
    }
    return v;
}

static long eval_eq(Eval *e) {
    long v = eval_rel(e);
    eval_skip_ws(e);
    while ((*e->p=='='&&*(e->p+1)=='=') || (*e->p=='!'&&*(e->p+1)=='=')) {
        bool neq = *e->p == '!'; e->p += 2;
        long r = eval_rel(e);
        v = neq ? v!=r : v==r;
        eval_skip_ws(e);
    }
    return v;
}

static long eval_band(Eval *e) {
    long v = eval_eq(e);
    eval_skip_ws(e);
    while (*e->p=='&' && *(e->p+1)!='&') { e->p++; v &= eval_eq(e); eval_skip_ws(e); }
    return v;
}

static long eval_bxor(Eval *e) {
    long v = eval_band(e);
    eval_skip_ws(e);
    while (*e->p=='^') { e->p++; v ^= eval_band(e); eval_skip_ws(e); }
    return v;
}

static long eval_bor(Eval *e) {
    long v = eval_bxor(e);
    eval_skip_ws(e);
    while (*e->p=='|' && *(e->p+1)!='|') { e->p++; v |= eval_bxor(e); eval_skip_ws(e); }
    return v;
}

static long eval_and(Eval *e) {
    long v = eval_bor(e);
    eval_skip_ws(e);
    while (*e->p=='&' && *(e->p+1)=='&') {
        e->p += 2; long r = eval_bor(e); v = v && r; eval_skip_ws(e);
    }
    return v;
}

static long eval_or(Eval *e) {
    long v = eval_and(e);
    eval_skip_ws(e);
    while (*e->p=='|' && *(e->p+1)=='|') {
        e->p += 2; long r = eval_and(e); v = v || r; eval_skip_ws(e);
    }
    return v;
}

static long eval_ternary(Eval *e) {
    long v = eval_or(e);
    eval_skip_ws(e);
    if (*e->p == '?') {
        e->p++;
        long t = eval_expr(e);
        eval_skip_ws(e);
        if (*e->p == ':') e->p++;
        long f = eval_expr(e);
        return v ? t : f;
    }
    return v;
}

static long eval_expr(Eval *e) { return eval_ternary(e); }

static long pp_eval_if(Preprocessor *pp, const char *expr_str) {
    /* Expand macros first */
    char *expanded = expand_text(pp, expr_str, NULL, 0);
    Eval e; e.p = expanded; e.pp = pp;
    long v = eval_expr(&e);
    free(expanded);
    return v;
}

/* =========================================================
 * Directive processing
 * ========================================================= */

static bool pp_is_active(Preprocessor *pp) {
    CondFrame *f = pp->cond_stack;
    while (f) { if (!f->active) return false; f = f->next; }
    return true;
}

/* Read a virtual header or real file, return heap string or NULL */
static char *pp_read_include(Preprocessor *pp, const char *name, bool system) {
    (void)system;
    /* Virtual headers */
    if (strcmp(name, "stdarg.h")==0) return strdup(VIRTUAL_STDARG_H);
    if (strcmp(name, "stdbool.h")==0) return strdup(VIRTUAL_STDBOOL_H);
    if (strcmp(name, "stddef.h")==0)  return strdup(VIRTUAL_STDDEF_H);
    if (strcmp(name, "stdio.h")==0)   return strdup(VIRTUAL_STDIO_H);
    if (strcmp(name, "stdlib.h")==0)  return strdup(VIRTUAL_STDLIB_H);
    if (strcmp(name, "string.h")==0)  return strdup(VIRTUAL_STRING_H);

    /* Search include paths */
    for (IncPath *ip = pp->include_paths; ip; ip = ip->next) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", ip->path, name);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            char *buf = malloc((size_t)sz + 1);
            fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);
            return buf;
        }
    }
    return NULL;
}

static void pp_push_cond(Preprocessor *pp, bool active) {
    CondFrame *f = calloc(1, sizeof(CondFrame));
    f->active     = active;
    f->ever_active = active;
    f->done       = active;
    f->next       = pp->cond_stack;
    pp->cond_stack = f;
}

static void pp_process_directive(Preprocessor *pp, const char *line) {
    const char *p = skip_hspace(line);
    if (*p == '#') p++;
    p = skip_hspace(p);

    char directive[64];
    p = read_ident(p, directive, sizeof(directive));
    p = skip_hspace(p);

    /* Always process #if/#ifdef/#ifndef/#else/#elif/#endif regardless of active state */
    if (strcmp(directive, "ifdef")==0 || strcmp(directive, "ifndef")==0) {
        char name[256]; read_ident(p, name, sizeof(name));
        bool defined_val = pp_lookup(pp, name) != NULL;
        bool should_enter = (strcmp(directive,"ifdef")==0) ? defined_val : !defined_val;
        bool parent_active = pp_is_active(pp);
        pp_push_cond(pp, parent_active && should_enter);
        return;
    }
    if (strcmp(directive, "if")==0) {
        bool parent_active = pp_is_active(pp);
        bool cond = parent_active ? (pp_eval_if(pp, p) != 0) : false;
        pp_push_cond(pp, cond);
        return;
    }
    if (strcmp(directive, "elif")==0) {
        CondFrame *f = pp->cond_stack;
        if (!f) { pp->error_count++; return; }
        if (!f->done) {
            bool parent_active = true;
            /* Check parent frames */
            for (CondFrame *pf = f->next; pf; pf = pf->next)
                if (!pf->active) { parent_active = false; break; }
            bool cond = parent_active ? (pp_eval_if(pp, p) != 0) : false;
            f->active = cond;
            if (cond) { f->ever_active = true; f->done = true; }
        } else {
            f->active = false;
        }
        return;
    }
    if (strcmp(directive, "else")==0) {
        CondFrame *f = pp->cond_stack;
        if (!f) { pp->error_count++; return; }
        f->active = !f->done;
        return;
    }
    if (strcmp(directive, "endif")==0) {
        CondFrame *f = pp->cond_stack;
        if (!f) { pp->error_count++; return; }
        pp->cond_stack = f->next;
        free(f);
        return;
    }

    /* Only process other directives in active sections */
    if (!pp_is_active(pp)) return;

    if (strcmp(directive, "define")==0) {
        char name[256];
        p = read_ident(p, name, sizeof(name));
        if (!name[0]) { pp->error_count++; return; }

        MacroParam *params = NULL;
        bool is_func = false, is_variadic = false;

        /* Function-like: no space before '(' */
        if (*p == '(') {
            is_func = true;
            p++; /* skip '(' */
            p = skip_hspace(p);
            MacroParam **tail = &params;
            while (*p && *p != ')') {
                if (*p == '.' && *(p+1)=='.' && *(p+2)=='.') {
                    is_variadic = true; p += 3;
                    p = skip_hspace(p);
                    /* Add __VA_ARGS__ placeholder - not a real param */
                    break;
                }
                char pname[256]; p = read_ident(p, pname, sizeof(pname));
                if (pname[0]) {
                    MacroParam *pm = calloc(1, sizeof(MacroParam));
                    pm->name = strdup(pname); *tail = pm; tail = &pm->next;
                }
                p = skip_hspace(p);
                if (*p == ',') { p++; p = skip_hspace(p); }
            }
            if (*p == ')') p++;
        }

        p = skip_hspace(p);
        /* Body is rest of line (without trailing whitespace) */
        char body[4096];
        int bi = 0;
        while (*p && bi < (int)sizeof(body)-1) body[bi++] = *p++;
        body[bi] = '\0';
        /* Trim trailing whitespace */
        while (bi > 0 && (body[bi-1]==' '||body[bi-1]=='\t')) body[--bi]='\0';

        pp_define_macro(pp, name, body, params, is_func, is_variadic);
        return;
    }

    if (strcmp(directive, "undef")==0) {
        char name[256]; read_ident(p, name, sizeof(name));
        pp_undef(pp, name);
        return;
    }

    if (strcmp(directive, "include")==0) {
        if (pp->include_depth > 32) {
            fprintf(stderr, "preprocessor: include depth limit exceeded\n");
            pp->error_count++; return;
        }
        char inc_name[512]; bool system = false;
        if (*p == '"') {
            p++;
            int i = 0; while (*p && *p != '"' && i < (int)sizeof(inc_name)-1) inc_name[i++] = *p++;
            inc_name[i] = '\0'; if (*p == '"') p++;
        } else if (*p == '<') {
            p++; system = true;
            int i = 0; while (*p && *p != '>' && i < (int)sizeof(inc_name)-1) inc_name[i++] = *p++;
            inc_name[i] = '\0'; if (*p == '>') p++;
        } else {
            /* Macro-expanded include */
            char *expanded = expand_text(pp, p, NULL, 0);
            const char *q = skip_hspace(expanded);
            if (*q == '"') { q++; int i=0; while(*q&&*q!='"'&&i<(int)sizeof(inc_name)-1) inc_name[i++]=*q++; inc_name[i]='\0'; }
            else if (*q == '<') { q++; system=true; int i=0; while(*q&&*q!='>'&&i<(int)sizeof(inc_name)-1) inc_name[i++]=*q++; inc_name[i]='\0'; }
            free(expanded);
        }

        char *inc_src = pp_read_include(pp, inc_name, system);
        if (!inc_src) {
            /* Non-fatal: just warn for unknown headers */
            fprintf(stderr, "preprocessor: warning: cannot find '%s'\n", inc_name);
            return;
        }

        /* Save and restore state */
        char *saved_file = pp->input_file;
        int   saved_line = pp->line;
        pp->input_file = strdup(inc_name);
        pp->include_depth++;

        /* Process included file */
        char *processed = pp_process_string(pp, inc_src, inc_name);
        free(inc_src);
        pp->include_depth--;
        free(pp->input_file);
        pp->input_file = saved_file;
        pp->line = saved_line;

        if (processed) {
            pp_out_str(pp, processed);
            free(processed);
        }
        return;
    }

    if (strcmp(directive, "pragma")==0 || strcmp(directive, "line")==0 ||
        strcmp(directive, "error")==0  || strcmp(directive, "warning")==0) {
        /* Ignore pragmas, handle #error/#warning as warnings */
        if (strcmp(directive, "error")==0) {
            fprintf(stderr, "preprocessor: #error %s\n", p);
        }
        return;
    }

    /* Unknown directive — ignore */
}

/* =========================================================
 * Main processing loop
 * ========================================================= */

char *pp_process_string(Preprocessor *pp, const char *src, const char *input_file) {
    /* Save/restore output buffer when recursing (includes) */
    char  *saved_out     = pp->output;
    size_t saved_len     = pp->out_len;
    size_t saved_cap     = pp->out_cap;

    pp->output  = NULL;
    pp->out_len = 0;
    pp->out_cap = 0;

    char *saved_file = pp->input_file;
    int   saved_line = pp->line;
    pp->input_file = (char*)input_file;
    pp->line = 1;

    const char *p = src;
    char line_buf[8192];

    while (*p) {
        /* Collect a logical line (handling line splices) */
        const char *line_start = p;
        int llen = 0;
        bool is_directive = false;

        /* Check if line starts with # (after optional whitespace) */
        {
            const char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '#') is_directive = true;
        }

        /* Read entire logical line */
        while (*p && *p != '\n') {
            if (*p == '\\' && *(p+1) == '\n') {
                if (llen < (int)sizeof(line_buf)-1) line_buf[llen++] = ' ';
                p += 2; pp->line++; continue;
            }
            /* Skip string contents */
            if (*p == '"' && !is_directive) {
                if (llen < (int)sizeof(line_buf)-1) line_buf[llen++] = *p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p+1)) {
                        if (llen < (int)sizeof(line_buf)-2) { line_buf[llen++]=*p++; line_buf[llen++]=*p++; }
                        else p += 2;
                    } else {
                        if (llen < (int)sizeof(line_buf)-1) line_buf[llen++] = *p++;
                        else p++;
                    }
                }
                if (*p) { if (llen < (int)sizeof(line_buf)-1) line_buf[llen++] = *p++; else p++; }
                continue;
            }
            if (llen < (int)sizeof(line_buf)-1) line_buf[llen++] = *p;
            p++;
        }
        line_buf[llen] = '\0';
        if (*p == '\n') { p++; pp->line++; }

        if (is_directive) {
            pp_process_directive(pp, line_buf);
            pp_out_char(pp, '\n');
        } else {
            if (!pp_is_active(pp)) {
                pp_out_char(pp, '\n');
                continue;
            }
            /* Expand macros in line */
            char *expanded = expand_text(pp, line_buf, NULL, 0);
            pp_out_str(pp, expanded);
            free(expanded);
            pp_out_char(pp, '\n');
        }
        (void)line_start;
    }

    char *result = pp->output ? pp->output : strdup("");

    /* Restore outer output buffer */
    pp->output   = saved_out;
    pp->out_len  = saved_len;
    pp->out_cap  = saved_cap;
    pp->input_file = saved_file;
    pp->line       = saved_line;

    return result;
}

/* =========================================================
 * Public API
 * ========================================================= */

Preprocessor *pp_new(void) {
    Preprocessor *pp = calloc(1, sizeof(Preprocessor));
    pp->line = 1;

    /* Predefined macros */
    pp_define_macro(pp, "__STDC__",       "1",    NULL, false, false);
    pp_define_macro(pp, "__STDC_VERSION__","201112L", NULL, false, false);
    pp_define_macro(pp, "__x86_64__",     "1",    NULL, false, false);
    pp_define_macro(pp, "__linux__",      "1",    NULL, false, false);

    /* Date/time (static for simplicity) */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char date_buf[32], time_buf[32];
    strftime(date_buf, sizeof(date_buf), "\"%b %e %Y\"", tm);
    strftime(time_buf, sizeof(time_buf), "\"%H:%M:%S\"", tm);
    pp_define_macro(pp, "__DATE__", date_buf, NULL, false, false);
    pp_define_macro(pp, "__TIME__", time_buf, NULL, false, false);

    return pp;
}

void pp_free(Preprocessor *pp) {
    if (!pp) return;
    for (int i = 0; i < PP_MACRO_BUCKETS; i++) {
        MacroDef *m = pp->macros[i];
        while (m) {
            MacroDef *nx = m->next;
            free(m->name); free(m->body);
            MacroParam *pm = m->params;
            while (pm) { MacroParam *nx2 = pm->next; free(pm->name); free(pm); pm = nx2; }
            free(m); m = nx;
        }
    }
    IncPath *ip = pp->include_paths;
    while (ip) { IncPath *nx = ip->next; free(ip->path); free(ip); ip = nx; }
    CondFrame *f = pp->cond_stack;
    while (f) { CondFrame *nx = f->next; free(f); f = nx; }
    free(pp->output);
    free(pp);
}

void pp_add_include_path(Preprocessor *pp, const char *path) {
    IncPath *ip = calloc(1, sizeof(IncPath));
    ip->path = strdup(path);
    ip->next = pp->include_paths;
    pp->include_paths = ip;
}

void pp_define(Preprocessor *pp, const char *name, const char *value) {
    /* name may be "NAME" or "NAME=VALUE" */
    char *eq = strchr((char*)name, '=');
    char n[256];
    if (eq) {
        int len = (int)(eq - name);
        memcpy(n, name, (size_t)len); n[len] = '\0';
        pp_define_macro(pp, n, eq+1, NULL, false, false);
    } else {
        pp_define_macro(pp, name, "1", NULL, false, false);
    }
}

char *pp_process_file(Preprocessor *pp, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "preprocessor: cannot open '%s': %s\n", path, strerror(errno));
        pp->error_count++;
        return NULL;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)sz + 1);
    fread(src, 1, (size_t)sz, f); src[sz] = '\0'; fclose(f);

    char *result = pp_process_string(pp, src, path);
    free(src);
    return result;
}
