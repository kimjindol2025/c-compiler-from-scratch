/*
 * ast.c — Arena allocator + AST helpers
 */

#include "../include/ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* =========================================================
 * Arena allocator
 * ========================================================= */

Arena *arena_new(void) {
    Arena *a = calloc(1, sizeof(Arena));
    if (!a) { perror("calloc"); exit(1); }
    return a;
}

static ArenaBlock *block_new(size_t cap) {
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
    if (!b) { perror("malloc"); exit(1); }
    b->prev = NULL;
    b->cap  = cap;
    b->used = 0;
    return b;
}

void *arena_alloc(Arena *a, size_t size) {
    /* align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    if (!a->current || a->current->used + size > a->current->cap) {
        size_t block_cap = size > ARENA_DEFAULT_BLOCK ? size * 2 : ARENA_DEFAULT_BLOCK;
        ArenaBlock *b = block_new(block_cap);
        b->prev      = a->current;
        a->current   = b;
    }

    void *ptr = (char *)(a->current + 1) + a->current->used;
    a->current->used       += size;
    a->total_allocated     += size;
    memset(ptr, 0, size);
    return ptr;
}

char *arena_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char  *p = arena_alloc(a, n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    if (!s) return NULL;
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void arena_free(Arena *a) {
    if (!a) return;
    ArenaBlock *b = a->current;
    while (b) {
        ArenaBlock *prev = b->prev;
        free(b);
        b = prev;
    }
    free(a);
}

/* =========================================================
 * Node / Type helpers
 * ========================================================= */

const char *node_kind_name(NodeKind k) {
    switch (k) {
    case ND_TRANSLATION_UNIT: return "translation_unit";
    case ND_FUNC_DEF:         return "func_def";
    case ND_VAR_DECL:         return "var_decl";
    case ND_PARAM:            return "param";
    case ND_STRUCT_DEF:       return "struct_def";
    case ND_UNION_DEF:        return "union_def";
    case ND_ENUM_DEF:         return "enum_def";
    case ND_TYPEDEF:          return "typedef";
    case ND_COMPOUND:         return "compound";
    case ND_IF:               return "if";
    case ND_WHILE:            return "while";
    case ND_DO_WHILE:         return "do_while";
    case ND_FOR:              return "for";
    case ND_SWITCH:           return "switch";
    case ND_CASE:             return "case";
    case ND_DEFAULT:          return "default";
    case ND_BREAK:            return "break";
    case ND_CONTINUE:         return "continue";
    case ND_RETURN:           return "return";
    case ND_GOTO:             return "goto";
    case ND_LABEL:            return "label";
    case ND_EXPR_STMT:        return "expr_stmt";
    case ND_NULL_STMT:        return "null_stmt";
    case ND_INT_LIT:          return "int_lit";
    case ND_FLOAT_LIT:        return "float_lit";
    case ND_CHAR_LIT:         return "char_lit";
    case ND_STR_LIT:          return "str_lit";
    case ND_IDENT:            return "ident";
    case ND_VAR:              return "var";
    case ND_ADD:              return "add";
    case ND_SUB:              return "sub";
    case ND_MUL:              return "mul";
    case ND_DIV:              return "div";
    case ND_MOD:              return "mod";
    case ND_AND:              return "and";
    case ND_OR:               return "or";
    case ND_XOR:              return "xor";
    case ND_SHL:              return "shl";
    case ND_SHR:              return "shr";
    case ND_LOGIC_AND:        return "logic_and";
    case ND_LOGIC_OR:         return "logic_or";
    case ND_EQ:               return "eq";
    case ND_NE:               return "ne";
    case ND_LT:               return "lt";
    case ND_GT:               return "gt";
    case ND_LE:               return "le";
    case ND_GE:               return "ge";
    case ND_ASSIGN:           return "assign";
    case ND_ASSIGN_ADD:       return "assign_add";
    case ND_ASSIGN_SUB:       return "assign_sub";
    case ND_ASSIGN_MUL:       return "assign_mul";
    case ND_ASSIGN_DIV:       return "assign_div";
    case ND_ASSIGN_MOD:       return "assign_mod";
    case ND_ASSIGN_AND:       return "assign_and";
    case ND_ASSIGN_OR:        return "assign_or";
    case ND_ASSIGN_XOR:       return "assign_xor";
    case ND_ASSIGN_SHL:       return "assign_shl";
    case ND_ASSIGN_SHR:       return "assign_shr";
    case ND_COMMA:            return "comma";
    case ND_NEG:              return "neg";
    case ND_NOT:              return "not";
    case ND_BITNOT:           return "bitnot";
    case ND_ADDR:             return "addr";
    case ND_DEREF:            return "deref";
    case ND_PRE_INC:          return "pre_inc";
    case ND_PRE_DEC:          return "pre_dec";
    case ND_POST_INC:         return "post_inc";
    case ND_POST_DEC:         return "post_dec";
    case ND_SIZEOF_EXPR:      return "sizeof_expr";
    case ND_SIZEOF_TYPE:      return "sizeof_type";
    case ND_ALIGNOF:          return "alignof";
    case ND_CALL:             return "call";
    case ND_INDEX:            return "index";
    case ND_MEMBER:           return "member";
    case ND_ARROW:            return "arrow";
    case ND_CAST:             return "cast";
    case ND_COND:             return "cond";
    case ND_INIT_LIST:        return "init_list";
    default:                  return "<unknown>";
    }
}

const char *type_kind_name(TypeKind k) {
    switch (k) {
    case TY_VOID:        return "void";
    case TY_BOOL:        return "_Bool";
    case TY_CHAR:        return "char";
    case TY_SCHAR:       return "signed char";
    case TY_UCHAR:       return "unsigned char";
    case TY_SHORT:       return "short";
    case TY_USHORT:      return "unsigned short";
    case TY_INT:         return "int";
    case TY_UINT:        return "unsigned int";
    case TY_LONG:        return "long";
    case TY_ULONG:       return "unsigned long";
    case TY_LLONG:       return "long long";
    case TY_ULLONG:      return "unsigned long long";
    case TY_FLOAT:       return "float";
    case TY_DOUBLE:      return "double";
    case TY_LDOUBLE:     return "long double";
    case TY_PTR:         return "ptr";
    case TY_ARRAY:       return "array";
    case TY_FUNC:        return "func";
    case TY_STRUCT:      return "struct";
    case TY_UNION:       return "union";
    case TY_ENUM:        return "enum";
    case TY_TYPEDEF_REF: return "typedef_ref";
    default:             return "<unknown-type>";
    }
}

/* ---- Pretty printer ---- */

static void indent_print(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

/* type_print is defined in types.c */

void node_print(Node *n, int indent) {
    if (!n) { indent_print(indent); printf("<null>\n"); return; }
    indent_print(indent);
    printf("(%s", node_kind_name(n->kind));
    if (n->type) { printf(" :"); type_print(n->type); }
    printf("\n");

    switch (n->kind) {
    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->unit.count; i++)
            node_print(n->unit.decls[i], indent + 1);
        break;
    case ND_INT_LIT:
        indent_print(indent + 1);
        printf("%lld\n", n->ival);
        break;
    case ND_FLOAT_LIT:
        indent_print(indent + 1);
        printf("%g\n", n->fval);
        break;
    case ND_CHAR_LIT:
        indent_print(indent + 1);
        printf("'%c'\n", (char)n->ival);
        break;
    case ND_STR_LIT:
        indent_print(indent + 1);
        printf("\"%s\"\n", n->sval);
        break;
    case ND_VAR:
    case ND_IDENT:
        indent_print(indent + 1);
        printf("%s\n", n->ident.name);
        break;
    case ND_FUNC_DEF:
        indent_print(indent + 1);
        printf("name: %s\n", n->func.name);
        for (int i = 0; i < n->func.param_count; i++)
            node_print(n->func.params[i], indent + 1);
        node_print(n->func.body, indent + 1);
        break;
    case ND_VAR_DECL:
        indent_print(indent + 1);
        printf("name: %s\n", n->decl.name);
        if (n->decl.init) node_print(n->decl.init, indent + 1);
        break;
    case ND_COMPOUND:
        for (int i = 0; i < n->compound.count; i++)
            node_print(n->compound.stmts[i], indent + 1);
        break;
    case ND_IF:
        node_print(n->if_.cond, indent + 1);
        node_print(n->if_.then, indent + 1);
        if (n->if_.else_) node_print(n->if_.else_, indent + 1);
        break;
    case ND_WHILE:
    case ND_DO_WHILE:
        node_print(n->while_.cond, indent + 1);
        node_print(n->while_.body, indent + 1);
        break;
    case ND_FOR:
        if (n->for_.init) node_print(n->for_.init, indent + 1);
        if (n->for_.cond) node_print(n->for_.cond, indent + 1);
        if (n->for_.step) node_print(n->for_.step, indent + 1);
        node_print(n->for_.body, indent + 1);
        break;
    case ND_RETURN:
        if (n->return_.value) node_print(n->return_.value, indent + 1);
        break;
    case ND_CALL:
        node_print(n->call.callee, indent + 1);
        for (int i = 0; i < n->call.arg_count; i++)
            node_print(n->call.args[i], indent + 1);
        break;
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_AND: case ND_OR:  case ND_XOR: case ND_SHL: case ND_SHR:
    case ND_LOGIC_AND: case ND_LOGIC_OR:
    case ND_EQ: case ND_NE: case ND_LT: case ND_GT: case ND_LE: case ND_GE:
    case ND_ASSIGN: case ND_COMMA:
        node_print(n->binary.left, indent + 1);
        node_print(n->binary.right, indent + 1);
        break;
    case ND_NEG: case ND_NOT: case ND_BITNOT:
    case ND_ADDR: case ND_DEREF:
    case ND_PRE_INC: case ND_PRE_DEC: case ND_POST_INC: case ND_POST_DEC:
    case ND_SIZEOF_EXPR: case ND_EXPR_STMT:
        node_print(n->unary.operand, indent + 1);
        break;
    case ND_MEMBER:
    case ND_ARROW:
        node_print(n->member_access.obj, indent + 1);
        indent_print(indent + 1);
        printf(".%s\n", n->member_access.member);
        break;
    case ND_INDEX:
        node_print(n->index.arr, indent + 1);
        node_print(n->index.idx, indent + 1);
        break;
    case ND_CAST:
        indent_print(indent + 1);
        type_print(n->cast.to);
        printf("\n");
        node_print(n->cast.expr, indent + 1);
        break;
    case ND_COND:
        node_print(n->ternary.cond, indent + 1);
        node_print(n->ternary.then, indent + 1);
        node_print(n->ternary.else_, indent + 1);
        break;
    default:
        break;
    }

    indent_print(indent);
    printf(")\n");
}
