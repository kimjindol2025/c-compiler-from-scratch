#pragma once
#include "ast.h"
#include "x86_encode.h"
#include <stdio.h>

/* =========================================================
 * x86-64 Code Generator
 * System V AMD64 ABI
 * ========================================================= */

/* ---- Code-generator symbol kinds (different from symtable.h SymKind) ---- */
typedef enum {
    CGSYM_FUNC,
    CGSYM_OBJECT,    /* .data or .bss */
    CGSYM_RODATA,    /* .rodata (string literals, const data) */
    CGSYM_EXTERN,
} CGSymKind;

/* ---- Relocation record (for ELF writer) ---- */
typedef struct Reloc {
    size_t   offset;      /* byte offset in .text where patching needed */
    char    *sym_name;    /* target symbol */
    int      type;        /* R_X86_64_* */
    int64_t  addend;
    struct Reloc *next;
} Reloc;

/* ---- Data section entry ---- */
typedef struct DataEntry {
    char        *label;
    uint8_t     *data;
    int          size;
    int          align;
    bool         is_bss;     /* zero-initialized → .bss */
    bool         is_rodata;  /* read-only → .rodata */
    struct DataEntry *next;
} DataEntry;

/* ---- Exported symbol ---- */
typedef struct SymEntry {
    char        *name;
    CGSymKind    kind;
    size_t       offset;     /* offset in .text or .data */
    int          size;
    bool         is_global;
    struct SymEntry *next;
} SymEntry;

/* ---- Break/continue label stack ---- */
typedef struct LoopLabel {
    char *break_label;
    char *cont_label;
    struct LoopLabel *next;
} LoopLabel;

/* ---- Switch context ---- */
typedef struct SwitchCtx {
    char  *end_label;
    char  *default_label;
    struct SwitchCtx *next;
} SwitchCtx;

/* ---- Code generator ---- */
typedef struct CodeGen {
    Encoder    *enc;          /* x86 instruction encoder */

    /* Sections */
    Buf         data_buf;     /* .data bytes */
    Buf         rodata_buf;   /* .rodata bytes */
    size_t      bss_size;     /* .bss byte count */

    /* Symbol and relocation tables */
    SymEntry   *syms;
    Reloc      *relocs;
    DataEntry  *data_entries;

    /* Current function state */
    char       *cur_func_name;
    int         cur_stack_size;
    char       *cur_return_label;

    /* Control flow stack */
    LoopLabel  *loop_stack;
    SwitchCtx  *switch_stack;

    /* label counter for unique labels */
    int         label_count;

    /* Diagnostics */
    int         error_count;
} CodeGen;

/* ---- Public API ---- */

CodeGen *codegen_new(void);
void     codegen_free(CodeGen *cg);

/**
 * Generate code for the entire program (ND_PROGRAM node).
 * After this call:
 *   - enc->buf contains .text machine code
 *   - data_buf / rodata_buf / bss_size are filled
 *   - syms / relocs are populated
 */
void codegen_gen(CodeGen *cg, Node *program);

/**
 * Write a relocatable ELF .o file.
 * Requires codegen_gen() to have been called.
 */
void codegen_emit_elf(CodeGen *cg, const char *outfile);

/**
 * Emit AT&T syntax assembly to `out`.
 * Useful with -S flag (human-readable output).
 */
void codegen_emit_asm(CodeGen *cg, FILE *out);
