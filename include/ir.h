/*
 * ir.h — Three-address IR for ccc
 *
 * Pipeline: AST+Sema → ir_lower() → opt_const_fold() → ir_codegen()
 *
 * Each IrInstr is: dst = op(src1, src2)   [typed 3-address]
 * Values are typed from the start → no boxing overhead in later passes.
 */
#pragma once
#include "ast.h"   /* Type */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ── Value kinds ──────────────────────────────────────────── */
typedef enum {
    IV_NONE,
    IV_TEMP,    /* virtual register (temp ID)         */
    IV_IMM,     /* integer immediate                  */
    IV_FIMM,    /* floating-point immediate            */
    IV_GLOBAL,  /* global symbol name                 */
    IV_LABEL,   /* branch target label                */
} IrValKind;

typedef struct IrVal {
    IrValKind  kind;
    Type      *ty;          /* attached C type (always set)    */
    union {
        int         temp;   /* IV_TEMP: virtual reg id         */
        long long   imm;    /* IV_IMM                          */
        double      fimm;   /* IV_FIMM                         */
        const char *name;   /* IV_GLOBAL | IV_LABEL            */
    };
} IrVal;

/* Convenience constructors */
static inline IrVal iv_none(void)                  { IrVal v={0}; v.kind=IV_NONE; return v; }
static inline IrVal iv_temp(int t, Type *ty)       { IrVal v={0}; v.kind=IV_TEMP; v.temp=t; v.ty=ty; return v; }
static inline IrVal iv_imm(long long i, Type *ty)  { IrVal v={0}; v.kind=IV_IMM;  v.imm=i;  v.ty=ty; return v; }
static inline IrVal iv_global(const char *n,Type*t){ IrVal v={0}; v.kind=IV_GLOBAL;v.name=n;v.ty=t; return v; }
static inline IrVal iv_label(const char *n)        { IrVal v={0}; v.kind=IV_LABEL; v.name=n; return v; }
static inline bool  iv_is_imm(IrVal v)             { return v.kind==IV_IMM; }

/* ── Opcodes ──────────────────────────────────────────────── */
typedef enum {
    /*  dst = src1 op src2  */
    IR_ADD,   IR_SUB,   IR_MUL,   IR_DIV,   IR_MOD,
    IR_AND,   IR_OR,    IR_XOR,   IR_SHL,   IR_SHR,
    /*  dst = op src1  */
    IR_NEG,   IR_NOT,   IR_BOOL_NOT,
    /*  dst = (type)src1   */
    IR_CAST,
    /*  dst = src1          (copy / move) */
    IR_MOV,
    /*  dst = *[src1 + imm]  typed load  */
    IR_LOAD,
    /*  *[dst + imm] = src1  typed store  */
    IR_STORE,
    /*  dst = &local[name]   / &global[name] */
    IR_ADDR,
    /*  dst = src1 + src2 * imm    (array element address, scale=imm) */
    IR_INDEX,
    /*  dst = src1 + imm           (struct member address, offset=imm) */
    IR_MEMBER,
    /*  dst = call src1 (args[0..nargs-1]) */
    IR_CALL,
    /*  return src1  (void if src1.kind==IV_NONE) */
    IR_RET,
    /*  goto label   */
    IR_JMP,
    /*  if (src1 CMP 0) goto label  */
    IR_JZ,    /* jump if zero    */
    IR_JNZ,   /* jump if nonzero */
    /*  define label at this point */
    IR_LABEL,
    /*  dst = alloca(size, align)  — stack slot */
    IR_ALLOCA,
    /*  comparisons → dst is 0 or 1 */
    IR_EQ,    IR_NE,
    IR_LT,    IR_LE,
    IR_GT,    IR_GE,
    IR_ULT,   IR_ULE,   /* unsigned */
    IR_UGT,   IR_UGE,
    /*  nop / end of function */
    IR_NOP,
    IR_FUNC_BEGIN,  /* marks start of a function */
    IR_FUNC_END,
} IrOpcode;

/* ── Instruction ──────────────────────────────────────────── */
typedef struct IrInstr {
    IrOpcode    op;
    IrVal       dst;    /* result register / target address for STORE */
    IrVal       src1;
    IrVal       src2;
    int         imm;    /* LOAD/STORE: byte offset; INDEX: scale; ALLOCA: size */
    int         imm2;   /* ALLOCA: align */
    Type       *ty;     /* operation granularity (for LOAD/STORE sizing) */

    /* call */
    IrVal      *args;
    int         nargs;

    /* label (IR_LABEL, IR_JMP, IR_JZ, IR_JNZ) */
    const char *label;

    /* function boundary */
    const char *func_name;
    int         func_frame_size; /* reserved stack (filled by regalloc / codegen) */

    struct IrInstr *next;
} IrInstr;

/* ── Function ─────────────────────────────────────────────── */
typedef struct IrFunc {
    const char *name;
    Type       *func_ty;        /* TY_FUNC type                */
    IrInstr    *head;           /* instruction list            */
    IrInstr    *tail;
    int         ninstr;
    int         next_temp;      /* next virtual register ID    */
    int         frame_size;     /* bytes to reserve on stack   */
    bool        is_static;
    struct IrFunc *next;
} IrFunc;

/* ── Module ───────────────────────────────────────────────── */
typedef struct IrModule {
    IrFunc *funcs;              /* linked list of functions    */
    IrFunc *funcs_tail;
    int     nfuncs;
    /* arena for allocations */
    void   *arena;
    size_t  arena_used;
    size_t  arena_cap;
} IrModule;

/* ── Public API ───────────────────────────────────────────── */
IrModule *ir_module_new(void);
void      ir_module_free(IrModule *m);

/* AST → IR lowering (runs after sema) */
void      ir_lower(IrModule *m, Node *translation_unit);

/* Optimizations */
void      ir_opt_const_fold(IrModule *m);    /* constant folding       */
void      ir_opt_dce(IrModule *m);           /* dead code elimination  */

/* Print IR (for -ir flag) */
void      ir_print(IrModule *m, FILE *out);
