#pragma once
#include "lexer.h"
#include <stddef.h>
#include <stdbool.h>

/* =========================================================
 * C11 AST  —  Node and Type Definitions
 *
 * Allocation model
 * ----------------
 *   All nodes and types are bump-allocated from an Arena owned by the
 *   Parser.  Nothing is individually freed; arena_free() releases all.
 *
 * Field design
 * ------------
 *   Node carries BOTH a set of flat fields (lhs, rhs, body, cond …)
 *   AND an anonymous union of named sub-structs (binary.left, if_.cond
 *   …).  The union members are provided as convenience aliases; they do
 *   NOT share the same storage as the flat fields (the struct is simply
 *   larger).  Code may use either naming convention.
 * ========================================================= */

typedef struct Node      Node;
typedef struct Type      Type;
typedef struct Member    Member;
typedef struct EnumConst EnumConst;
typedef struct Symbol    Symbol;   /* opaque; defined in sema layer */
typedef struct Var       Var;
typedef struct Param     Param;

/* =========================================================
 * 1.  Node kinds
 *
 * Aliases are provided so that both old (flat-field) code and new
 * (union-field) code compile against this header.
 * ========================================================= */
typedef enum {
    /* top-level */
    ND_TRANSLATION_UNIT,

    /* declarations */
    ND_FUNC_DEF,
    ND_VAR_DECL,
    ND_PARAM,
    ND_STRUCT_DEF,
    ND_UNION_DEF,
    ND_ENUM_DEF,
    ND_TYPEDEF,

    /* statements */
    ND_COMPOUND,
    ND_IF,
    ND_WHILE,
    ND_DO_WHILE,
    ND_FOR,
    ND_SWITCH,
    ND_CASE,
    ND_DEFAULT,
    ND_BREAK,
    ND_CONTINUE,
    ND_RETURN,
    ND_GOTO,
    ND_LABEL,
    ND_EXPR_STMT,
    ND_NULL_STMT,

    /* literals */
    ND_INT_LIT,
    ND_FLOAT_LIT,
    ND_CHAR_LIT,
    ND_STR_LIT,

    /* primary / identifier */
    ND_IDENT,
    ND_VAR,        /* alias for IDENT in sema/codegen context     */

    /* binary: arithmetic */
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
    /* binary: bitwise */
    ND_AND, ND_OR, ND_XOR, ND_SHL, ND_SHR,
    /* binary: logical */
    ND_LOGIC_AND,
    ND_LOGIC_OR,
    /* relational */
    ND_EQ, ND_NE, ND_LT, ND_GT, ND_LE, ND_GE,
    /* assignment */
    ND_ASSIGN,
    ND_COMPOUND_ASSIGN,  /* lhs op= rhs  (specific op in ->op field) */
    ND_ASSIGN_ADD, ND_ASSIGN_SUB, ND_ASSIGN_MUL, ND_ASSIGN_DIV,
    ND_ASSIGN_MOD, ND_ASSIGN_AND, ND_ASSIGN_OR,  ND_ASSIGN_XOR,
    ND_ASSIGN_SHL, ND_ASSIGN_SHR,
    /* comma */
    ND_COMMA,

    /* unary */
    ND_NEG,
    ND_NOT,        /* logical not ! */
    ND_BITNOT,
    ND_ADDR,
    ND_DEREF,
    ND_PRE_INC,
    ND_PRE_DEC,
    ND_POST_INC,
    ND_POST_DEC,
    ND_SIZEOF_EXPR,
    ND_SIZEOF_TYPE,
    ND_ALIGNOF,

    /* postfix / complex */
    ND_CALL,
    ND_INDEX,
    ND_MEMBER,
    ND_ARROW,
    ND_CAST,
    ND_COND,
    ND_INIT_LIST,
} NodeKind;

/* Aliases — macro form avoids duplicate case-value errors in switches */
#define ND_PROGRAM        ND_TRANSLATION_UNIT
#define ND_BLOCK          ND_COMPOUND
#define ND_BITAND         ND_AND
#define ND_BITOR          ND_OR
#define ND_BITXOR         ND_XOR
#define ND_LOGAND         ND_LOGIC_AND
#define ND_LOGOR          ND_LOGIC_OR
#define ND_NEQ            ND_NE
#define ND_LOGNOT         ND_NOT
#define ND_TERNARY        ND_COND

/* =========================================================
 * 2.  Type system
 * ========================================================= */
typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,   TY_SCHAR,  TY_UCHAR,
    TY_SHORT,  TY_USHORT,
    TY_INT,    TY_UINT,
    TY_LONG,   TY_ULONG,
    TY_LLONG,  TY_ULLONG,
    TY_FLOAT,  TY_DOUBLE, TY_LDOUBLE,
    TY_PTR,
    TY_ARRAY,
    TY_FUNC,
    TY_STRUCT,
    TY_UNION,
    TY_ENUM,
    TY_TYPEDEF_REF,
} TypeKind;

/* Struct / union field */
struct Member {
    const char *name;
    Type       *ty;      /* field type                             */
    int         offset;  /* byte offset (sema fill)               */
    int         bit_width; /* -1 = not a bitfield                 */
    Member     *next;
};

/* Enum constant */
struct EnumConst {
    const char  *name;
    long long    value;
    EnumConst   *next;
};

/* Function parameter (linked list used by sema / types) */
struct Param {
    const char *name;
    Type       *ty;
    Param      *next;
};

/* Variable / symbol (used by parser and sema) */
struct Var {
    const char *name;
    Type       *ty;
    bool        is_global;
    bool        is_static;
    bool        is_extern;
    bool        is_register;
    int         offset;    /* stack frame offset for locals        */
    char       *init_data; /* initial bytes for globals            */
    int         init_len;
    char       *asm_label;
    Var        *next;
};

struct Type {
    TypeKind    kind;
    int         size;       /* sizeof (0 = unknown)                */
    int         align;

    bool        is_const;
    bool        is_volatile;
    bool        is_restrict;
    bool        is_atomic;
    bool        is_complete;
    bool        is_resolving; /* 재귀 guard: resolve_type 중복 진입 방지 */

    /* TY_PTR   → pointee
       TY_ARRAY → element
       TY_FUNC  → return type                                     */
    Type       *base;
    Type       *return_ty;  /* alias for base for TY_FUNC         */

    /* TY_ARRAY */
    int         array_len;  /* -1 = incomplete / VLA              */
    bool        is_vla;
    Node       *vla_len;

    /* TY_STRUCT / TY_UNION / TY_ENUM */
    const char *tag;
    Member     *members;
    EnumConst  *enumerators;

    /* TY_FUNC — params stored as a Type** array                  */
    Type      **params;      /* array[param_count] of Type*       */
    char      **param_names; /* array[param_count] of name strings (may be NULL) */
    int         param_count;
    bool        is_variadic;

    /* TY_TYPEDEF_REF */
    const char *typedef_name;
};

/* =========================================================
 * 3.  AST Node
 *
 * Fields are provided in two layers:
 *   a) Flat fields  (lhs, rhs, body, cond, then, els, …)
 *      Used by sema.c, codegen.c, and older code.
 *   b) Named sub-structs in an anonymous union
 *      (binary.left, if_.cond, func.name, …)
 *      Used by parser.c and ast.c.
 *
 * Both sets are present in the struct; they occupy separate storage.
 * ========================================================= */
struct Node {
    NodeKind    kind;
    int         line, col;
    Type       *type;    /* resolved type (sema fill)              */
    Type       *ty;      /* alias for type                         */

    /* ---- literal values ---- */
    long long   ival;    /* ND_INT_LIT, ND_CHAR_LIT                */
    double      fval;    /* ND_FLOAT_LIT                           */
    char       *sval;    /* ND_STR_LIT                             */
    int         slen;    /* ND_STR_LIT byte length incl. NUL       */

    /* ---- flat fields (sema / codegen layer) ---- */
    Node       *lhs;        /* left operand / callee / object      */
    Node       *rhs;        /* right operand                       */
    Node       *body;       /* loop / label / switch body          */
    Node       *cond;       /* condition                           */
    Node       *then;       /* then-branch                         */
    Node       *els;        /* else-branch                         */
    Node       *init;       /* for-loop init                       */
    Node       *step;       /* for-loop step                       */
    Node       *ret_val;    /* ND_RETURN value                     */

    /* function definition (flat) */
    char       *fname;
    Type       *func_ty;
    Node      **params_flat; /* formal param Node* array (sema)   */
    int         nparam_flat;
    Var        *locals;
    Var        *params_var;
    int         stack_size;
    bool        is_static;
    bool        is_inline;
    bool        is_extern;

    /* function call (flat) */
    Node       *func_expr;
    Node      **args;
    int         nargs;
    int         cap_args;

    /* block / compound (flat) */
    Node      **stmts;
    int         nstmts;
    int         cap_stmts;

    /* translation unit (flat) */
    Node      **decls;
    int         ndecls;
    int         cap_decls;

    /* initializer list (flat) */
    Node      **items;
    int         nitems;
    int         cap_items;

    /* variable decl (flat) */
    Var        *decl_var;
    Node       *decl_init;
    bool        decl_is_static;
    bool        decl_is_extern;

    /* member access (flat) */
    char       *member_name;
    Member     *member;         /* resolved member pointer (sema fill)  */

    /* case / switch */
    long long   case_val;
    Node       *default_case;
    Node       *cases_list;

    /* goto / label */
    char       *goto_label;
    char       *label_name;

    /* cast / sizeof */
    Type       *cast_ty;

    /* compound-assign underlying op */
    NodeKind    op;

    /* var reference (flat) — used by sema as node->var */
    Var        *var;

    /* ---- union of named sub-structs (parser / ast printer layer) ---- */
    union {
        struct { const char *name; Symbol *sym; } ident;       /* ND_IDENT  */
        struct { Node *left;  Node *right; }       binary;
        struct { Node *operand; }                  unary;
        struct { Node *cond; Node *then; Node *else_; } if_;
        struct { Node *cond; Node *body; }         while_;
        struct { Node *init; Node *cond; Node *step; Node *body; } for_;
        struct { Node *expr; Node *body; }         switch_;
        struct { long long value; Node *body; }    case_;
        struct { Node *body; }                     default_;
        struct { Node *value; }                    return_;
        struct { const char *label; }              goto_;
        struct { const char *name; Node *body; }   label;
        struct { Node **stmts; int count; int cap; } compound;
        struct {
            const char *name;
            Type       *ret_type;
            Node      **params;
            int         param_count;
            Node       *body;
            bool        is_static;
            bool        is_inline;
            bool        is_extern;
            bool        is_noreturn;
        } func;
        struct {
            const char *name;
            Type       *decl_type;
            Node       *init;
            bool        is_static;
            bool        is_extern;
            bool        is_register;
            bool        is_auto;
        } decl;
        struct { const char *name; Type *param_type; } param;
        struct { Node *callee; Node **args; int arg_count; int arg_cap; } call;
        struct { Node *arr; Node *idx; }           index;
        struct { Node *obj; const char *member; Member *resolved; } member_access;
        struct { Type *to; Node *expr; }           cast;
        struct { Node *cond; Node *then; Node *else_; } ternary;
        struct { Node **items; int count; int cap; } init_list;
        struct { Type *type; }                     sizeof_type;
        struct { Node **decls; int count; int cap; } unit;
        struct { const char *tag; Type *type; }    tag_def;
        struct { const char *alias; Type *type; }  typedef_;
    };

    Node *next;   /* intrusive linked list                         */
};

/* =========================================================
 * 4.  Arena allocator
 * ========================================================= */

#define ARENA_DEFAULT_BLOCK  (1u << 20)   /* 1 MiB per block      */

typedef struct ArenaBlock {
    struct ArenaBlock *prev;
    size_t             cap;
    size_t             used;
} ArenaBlock;

typedef struct Arena {
    ArenaBlock *current;
    size_t      total_allocated;
} Arena;

Arena *arena_new(void);
void   arena_free(Arena *a);
void  *arena_alloc(Arena *a, size_t size);
char  *arena_strdup(Arena *a, const char *s);
char  *arena_strndup(Arena *a, const char *s, size_t n);

static inline Node *arena_alloc_node(Arena *a)
{
    return (Node *)arena_alloc(a, sizeof(Node));
}
static inline Type *arena_alloc_type(Arena *a)
{
    return (Type *)arena_alloc(a, sizeof(Type));
}

/* =========================================================
 * 5.  Type helpers (implemented in types.c)
 * ========================================================= */
Type *type_new(TypeKind kind);
Type *type_ptr_to(Type *base);
Type *type_array_of(Type *base, int len);
Type *type_func_returning(Type *ret, Type **params, int np, bool variadic);
Type *type_copy(Arena *a, Type *ty);
bool  type_is_integer(Type *ty);
bool  type_is_unsigned(Type *ty);
bool  type_is_float(Type *ty);
bool  type_is_arithmetic(Type *ty);
bool  type_is_scalar(Type *ty);
bool  type_is_pointer(Type *ty);
int   type_sizeof_val(Type *ty);
int   type_alignof_val(Type *ty);

/* =========================================================
 * 6.  Node helpers (implemented in ast.c)
 * ========================================================= */
void        node_print(Node *n, int indent);
void        type_print(Type *t);
const char *node_kind_name(NodeKind k);
const char *type_kind_name(TypeKind k);
