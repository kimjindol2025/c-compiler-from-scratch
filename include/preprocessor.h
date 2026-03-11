#pragma once
#include <stdbool.h>
#include <stddef.h>

/* =========================================================
 * C Preprocessor — text-based, single-pass
 *
 * Supports:
 *   #define NAME [value]           (object-like)
 *   #define NAME(params) body      (function-like)
 *   #undef NAME
 *   #include "file" / <file>
 *   #ifdef / #ifndef / #if / #elif / #else / #endif
 *   #pragma (ignored)
 *   __FILE__, __LINE__, __DATE__, __TIME__
 * ========================================================= */

/* ---- Macro parameter list ---- */
typedef struct MacroParam {
    char              *name;
    struct MacroParam *next;
} MacroParam;

/* ---- Macro definition ---- */
typedef struct MacroDef {
    char       *name;
    char       *body;          /* replacement text (NULL = predefined/empty) */
    MacroParam *params;        /* NULL → object-like */
    bool        is_func;       /* has parameter list */
    bool        is_variadic;   /* last param is ... */
    struct MacroDef *next;     /* hash chain */
} MacroDef;

/* ---- Conditional compilation frame ---- */
typedef struct CondFrame {
    bool active;           /* currently in active branch */
    bool ever_active;      /* any branch was active (suppress else/elif) */
    bool done;             /* already found true branch — skip rest */
    struct CondFrame *next;
} CondFrame;

/* ---- Include path entry ---- */
typedef struct IncPath {
    char          *path;
    struct IncPath *next;
} IncPath;

#define PP_MACRO_BUCKETS 256

/* ---- Preprocessor state ---- */
typedef struct Preprocessor {
    MacroDef  *macros[PP_MACRO_BUCKETS];   /* macro hash table */
    CondFrame *cond_stack;                  /* #if nesting stack */
    IncPath   *include_paths;               /* -I search dirs */
    char      *input_file;                  /* current input filename */
    int        line;                        /* current line number */
    char      *output;                      /* output buffer (heap) */
    size_t     out_len;
    size_t     out_cap;
    int        error_count;
    int        include_depth;              /* prevent infinite recursion */
} Preprocessor;

/* ---- Public API ---- */

Preprocessor *pp_new(void);
void          pp_free(Preprocessor *pp);

/** Add an include search path (system or -I). */
void          pp_add_include_path(Preprocessor *pp, const char *path);

/** Define a macro from the command line (-DNAME[=VALUE]). */
void          pp_define(Preprocessor *pp, const char *name, const char *value);

/**
 * Preprocess a source file.
 * Returns heap-allocated processed source (caller must free).
 * Returns NULL on fatal error.
 */
char         *pp_process_file(Preprocessor *pp, const char *path);

/**
 * Preprocess an in-memory string (input_file used for error messages and __FILE__).
 * Returns heap-allocated processed string (caller must free).
 */
char         *pp_process_string(Preprocessor *pp, const char *src,
                                const char *input_file);
