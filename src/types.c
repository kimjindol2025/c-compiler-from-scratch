/*
 * types.c — Type utility functions for the C compiler
 *
 * Target: x86-64 Linux (System V AMD64 ABI)
 *
 * Sizes / alignments:
 *   _Bool        1 / 1     char         1 / 1
 *   short        2 / 2     int          4 / 4
 *   long         8 / 8     long long    8 / 8
 *   float        4 / 4     double       8 / 8
 *   long double 16 / 16    pointer      8 / 8
 *
 * NOTE: type_ptr_to(), type_array_of(), type_is_integer(),
 *       type_is_unsigned(), type_is_float(), type_is_arithmetic(),
 *       type_is_scalar(), type_is_pointer(), type_sizeof_val(),
 *       type_alignof_val(), type_print()
 *       are declared in ast.h and implemented here (they belong to
 *       this compilation unit as part of the type subsystem).
 */

#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------
 * Predefined type singletons
 * --------------------------------------------------------- */
Type *ty_void    = NULL;
Type *ty_bool    = NULL;
Type *ty_char    = NULL;
Type *ty_schar   = NULL;
Type *ty_uchar   = NULL;
Type *ty_short   = NULL;
Type *ty_ushort  = NULL;
Type *ty_int     = NULL;
Type *ty_uint    = NULL;
Type *ty_long    = NULL;
Type *ty_ulong   = NULL;
Type *ty_llong   = NULL;
Type *ty_ullong  = NULL;
Type *ty_float   = NULL;
Type *ty_double  = NULL;
Type *ty_ldouble = NULL;

/* ---------------------------------------------------------
 * Internal primitive allocator
 * --------------------------------------------------------- */
static Type *make_prim(TypeKind kind, int size, int align)
{
    Type *t = calloc(1, sizeof(Type));
    if (!t) { perror("calloc"); exit(1); }
    t->kind        = kind;
    t->size        = size;
    t->align       = align;
    t->is_complete = true;
    return t;
}

/* ---------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------- */

void types_init(void)
{
    if (ty_int) return; /* idempotent */

    ty_void    = make_prim(TY_VOID,    0,  1);
    ty_bool    = make_prim(TY_BOOL,    1,  1);
    ty_char    = make_prim(TY_CHAR,    1,  1);
    ty_schar   = make_prim(TY_SCHAR,   1,  1);
    ty_uchar   = make_prim(TY_UCHAR,   1,  1);
    ty_short   = make_prim(TY_SHORT,   2,  2);
    ty_ushort  = make_prim(TY_USHORT,  2,  2);
    ty_int     = make_prim(TY_INT,     4,  4);
    ty_uint    = make_prim(TY_UINT,    4,  4);
    ty_long    = make_prim(TY_LONG,    8,  8);
    ty_ulong   = make_prim(TY_ULONG,   8,  8);
    ty_llong   = make_prim(TY_LLONG,   8,  8);
    ty_ullong  = make_prim(TY_ULLONG,  8,  8);
    ty_float   = make_prim(TY_FLOAT,   4,  4);
    ty_double  = make_prim(TY_DOUBLE,  8,  8);
    /* 80-bit extended precision, padded to 16 bytes on x86-64 */
    ty_ldouble = make_prim(TY_LDOUBLE, 16, 16);
}

void types_fini(void)
{
    free(ty_void);    ty_void    = NULL;
    free(ty_bool);    ty_bool    = NULL;
    free(ty_char);    ty_char    = NULL;
    free(ty_schar);   ty_schar   = NULL;
    free(ty_uchar);   ty_uchar   = NULL;
    free(ty_short);   ty_short   = NULL;
    free(ty_ushort);  ty_ushort  = NULL;
    free(ty_int);     ty_int     = NULL;
    free(ty_uint);    ty_uint    = NULL;
    free(ty_long);    ty_long    = NULL;
    free(ty_ulong);   ty_ulong   = NULL;
    free(ty_llong);   ty_llong   = NULL;
    free(ty_ullong);  ty_ullong  = NULL;
    free(ty_float);   ty_float   = NULL;
    free(ty_double);  ty_double  = NULL;
    free(ty_ldouble); ty_ldouble = NULL;
}

/* ---------------------------------------------------------
 * Type constructors declared in ast.h
 * --------------------------------------------------------- */

/* Allocate a bare type node of the given kind. */
Type *type_new(TypeKind kind)
{
    Type *t = calloc(1, sizeof(Type));
    if (!t) { perror("calloc"); exit(1); }
    t->kind = kind;
    return t;
}

/* Build a pointer-to-base type. */
Type *type_ptr_to(Type *base)
{
    Type *t   = type_new(TY_PTR);
    t->base   = base;
    t->size   = 8;
    t->align  = 8;
    t->is_complete = true;
    return t;
}

/* Build an array type.  len == -1 means incomplete/unspecified. */
Type *type_array_of(Type *base, int len)
{
    Type *t       = type_new(TY_ARRAY);
    t->base       = base;
    t->array_len  = len;
    t->align      = base ? base->align : 1;
    if (len >= 0 && base && base->size > 0) {
        t->size        = base->size * len;
        t->is_complete = true;
    } else {
        t->size        = 0;
        t->is_complete = false;
    }
    return t;
}

/*
 * Build a function type.
 * params is a caller-provided Param linked list; variadic adds "...".
 */
Type *type_func_returning(Type *ret, Type **params, int np, bool variadic)
{
    Type *t         = type_new(TY_FUNC);
    t->base         = ret;   /* convention: base = return type */
    t->return_ty    = ret;
    t->params       = params;
    t->param_count  = np;
    t->is_variadic  = variadic;
    t->size         = 1;     /* functions don't have a sizeof in the usual sense */
    t->align        = 1;
    t->is_complete  = true;
    return t;
}

/* ---------------------------------------------------------
 * Type constructors for aggregate types (declared in types.h)
 * --------------------------------------------------------- */

Type *type_struct_new(const char *tag)
{
    Type *t       = type_new(TY_STRUCT);
    t->tag        = (char *)tag; /* borrowed */
    t->is_complete = false;
    return t;
}

Type *type_union_new(const char *tag)
{
    Type *t       = type_new(TY_UNION);
    t->tag        = (char *)tag;
    t->is_complete = false;
    return t;
}

Type *type_enum_new(const char *tag)
{
    Type *t       = type_new(TY_ENUM);
    t->tag        = (char *)tag;
    t->size       = 4;   /* underlying type is int */
    t->align      = 4;
    t->is_complete = true;
    return t;
}

/* ---------------------------------------------------------
 * Type predicates declared in ast.h
 * --------------------------------------------------------- */

bool type_is_integer(Type *ty)
{
    switch (ty->kind) {
    case TY_BOOL:
    case TY_CHAR:  case TY_SCHAR:  case TY_UCHAR:
    case TY_SHORT: case TY_USHORT:
    case TY_INT:   case TY_UINT:
    case TY_LONG:  case TY_ULONG:
    case TY_LLONG: case TY_ULLONG:
    case TY_ENUM:
        return true;
    default:
        return false;
    }
}

bool type_is_unsigned(Type *ty)
{
    switch (ty->kind) {
    case TY_BOOL:
    case TY_UCHAR:
    case TY_USHORT:
    case TY_UINT:
    case TY_ULONG:
    case TY_ULLONG:
        return true;
    default:
        return false;
    }
}

/* Signed integer types (not bool, not unsigned) */
int type_is_signed(Type *t)
{
    switch (t->kind) {
    case TY_CHAR:  /* char is signed on x86-64 Linux */
    case TY_SCHAR:
    case TY_SHORT:
    case TY_INT:
    case TY_LONG:
    case TY_LLONG:
        return 1;
    default:
        return 0;
    }
}

bool type_is_float(Type *ty)
{
    return ty->kind == TY_FLOAT ||
           ty->kind == TY_DOUBLE ||
           ty->kind == TY_LDOUBLE;
}

bool type_is_arithmetic(Type *ty)
{
    return type_is_integer(ty) || type_is_float(ty);
}

bool type_is_scalar(Type *ty)
{
    return type_is_arithmetic(ty) ||
           ty->kind == TY_PTR ||
           ty->kind == TY_ARRAY;
}

bool type_is_pointer(Type *ty)
{
    return ty->kind == TY_PTR;
}

int type_is_pointer_like(Type *t)
{
    return t->kind == TY_PTR || t->kind == TY_ARRAY;
}

/* ---------------------------------------------------------
 * Layout  (ast.h public interface)
 * --------------------------------------------------------- */

int type_sizeof_val(Type *ty)
{
    if (!ty) return -1;
    return ty->size;
}

int type_alignof_val(Type *ty)
{
    if (!ty || ty->align <= 0) return -1;
    return ty->align;
}

/* Align value up to the nearest multiple of align. */
static int align_up(int v, int a)
{
    if (a <= 1) return v;
    return (v + a - 1) & ~(a - 1);
}

void type_layout_struct(Type *t)
{
    if (!t) return;

    if (t->kind == TY_STRUCT) {
        int offset    = 0;
        int max_align = 1;
        for (Member *m = t->members; m; m = m->next) {
            if (!m->ty) continue;
            int ma = m->ty->align > 0 ? m->ty->align : 1;
            if (ma > max_align) max_align = ma;
            offset    = align_up(offset, ma);
            m->offset = offset;
            if (m->ty->size > 0) offset += m->ty->size;
        }
        t->align       = max_align;
        t->size        = align_up(offset, max_align);
        t->is_complete = true;

    } else if (t->kind == TY_UNION) {
        int max_size  = 0;
        int max_align = 1;
        for (Member *m = t->members; m; m = m->next) {
            if (!m->ty) continue;
            m->offset = 0;
            int ma = m->ty->align > 0 ? m->ty->align : 1;
            int ms = m->ty->size  > 0 ? m->ty->size  : 0;
            if (ma > max_align) max_align = ma;
            if (ms > max_size)  max_size  = ms;
        }
        t->align       = max_align;
        t->size        = align_up(max_size, max_align);
        t->is_complete = true;
    }
}

/* ---------------------------------------------------------
 * Type copy (ast.h)
 * --------------------------------------------------------- */
Type *type_copy(Arena *a, Type *ty)
{
    (void)a; /* We use malloc; arena variant can be added later */
    if (!ty) return NULL;
    Type *t = malloc(sizeof(Type));
    if (!t) { perror("malloc"); exit(1); }
    *t = *ty;
    return t;
}

/* ---------------------------------------------------------
 * Type compatibility  (C11 §6.2.7)
 * --------------------------------------------------------- */

int type_is_compatible(Type *a, Type *b)
{
    if (!a || !b)  return 0;
    if (a == b)    return 1;
    if (a->kind != b->kind) {
        /* enum is compatible with its underlying type (int) */
        if (a->kind == TY_ENUM && b->kind == TY_INT) return 1;
        if (b->kind == TY_ENUM && a->kind == TY_INT) return 1;
        return 0;
    }
    switch (a->kind) {
    case TY_PTR:
        return type_is_compatible(a->base, b->base);
    case TY_ARRAY:
        if (a->array_len != -1 && b->array_len != -1 &&
            a->array_len != b->array_len) return 0;
        return type_is_compatible(a->base, b->base);
    case TY_FUNC: {
        if (!type_is_compatible(a->return_ty, b->return_ty)) return 0;
        if (a->param_count != b->param_count) return 0;
        for (int i = 0; i < a->param_count; i++) {
            if (!type_is_compatible(a->params[i], b->params[i])) return 0;
        }
        return 1;
    }
    case TY_STRUCT:
    case TY_UNION:
        if (a->tag && b->tag) return strcmp(a->tag, b->tag) == 0;
        return a == b;
    default:
        return a->kind == b->kind;
    }
}

/* ---------------------------------------------------------
 * Integer conversion rank  (C11 §6.3.1.1)
 * --------------------------------------------------------- */

int type_integer_rank(Type *t)
{
    switch (t->kind) {
    case TY_BOOL:                return 0;
    case TY_CHAR:  case TY_SCHAR: case TY_UCHAR:  return 1;
    case TY_SHORT: case TY_USHORT:                return 2;
    case TY_INT:   case TY_UINT:  case TY_ENUM:  return 3;
    case TY_LONG:  case TY_ULONG:                 return 4;
    case TY_LLONG: case TY_ULLONG:                return 5;
    default:                      return -1;
    }
}

/* ---------------------------------------------------------
 * Integer promotions  (C11 §6.3.1.1)
 * --------------------------------------------------------- */

Type *type_integer_promote(Type *t)
{
    if (!type_is_integer(t)) return t;
    if (type_integer_rank(t) < type_integer_rank(ty_int))
        return ty_int;
    return t;
}

/* ---------------------------------------------------------
 * Usual arithmetic conversions  (C11 §6.3.1.8)
 * --------------------------------------------------------- */

Type *type_usual_arith_conv(Type *a, Type *b)
{
    /* long double beats everything */
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE) return ty_ldouble;
    /* double */
    if (a->kind == TY_DOUBLE  || b->kind == TY_DOUBLE)  return ty_double;
    /* float */
    if (a->kind == TY_FLOAT   || b->kind == TY_FLOAT)   return ty_float;

    /* Both integer — promote first */
    a = type_integer_promote(a);
    b = type_integer_promote(b);

    if (a->kind == b->kind) return a;

    int ra = type_integer_rank(a);
    int rb = type_integer_rank(b);

    /* Same signedness → higher rank wins */
    if (type_is_unsigned(a) == type_is_unsigned(b))
        return (ra >= rb) ? a : b;

    /* Mixed signedness (C11 §6.3.1.8 bullet points 4-6) */
    Type *u = type_is_unsigned(a) ? a : b;
    Type *s = type_is_unsigned(a) ? b : a;
    int   ru = type_integer_rank(u);
    int   rs = type_integer_rank(s);

    /* If unsigned rank >= signed rank → result is unsigned */
    if (ru >= rs) return u;

    /* If signed type can represent all values of unsigned type → result signed */
    if (s->size > u->size) return s;

    /* Otherwise → unsigned version of the signed type */
    if (s == ty_int)   return ty_uint;
    if (s == ty_long)  return ty_ulong;
    if (s == ty_llong) return ty_ullong;
    return s; /* unreachable in practice */
}

/* ---------------------------------------------------------
 * Decay  (C11 §6.3.2.1)
 * --------------------------------------------------------- */

Type *type_decay(Type *t)
{
    if (!t) return t;
    if (t->kind == TY_ARRAY) return type_ptr_to(t->base);
    if (t->kind == TY_FUNC)  return type_ptr_to(t);
    return t;
}

/* ---------------------------------------------------------
 * Diagnostic helpers
 * --------------------------------------------------------- */

void type_to_str(Type *t, char *buf, int n)
{
    if (!t) { snprintf(buf, n, "<null>"); return; }
    switch (t->kind) {
    case TY_VOID:    snprintf(buf, n, "void");               break;
    case TY_BOOL:    snprintf(buf, n, "_Bool");              break;
    case TY_CHAR:    snprintf(buf, n, "char");               break;
    case TY_SCHAR:   snprintf(buf, n, "signed char");        break;
    case TY_UCHAR:   snprintf(buf, n, "unsigned char");      break;
    case TY_SHORT:   snprintf(buf, n, "short");              break;
    case TY_USHORT:  snprintf(buf, n, "unsigned short");     break;
    case TY_INT:     snprintf(buf, n, "int");                break;
    case TY_UINT:    snprintf(buf, n, "unsigned int");       break;
    case TY_LONG:    snprintf(buf, n, "long");               break;
    case TY_ULONG:   snprintf(buf, n, "unsigned long");      break;
    case TY_LLONG:   snprintf(buf, n, "long long");          break;
    case TY_ULLONG:  snprintf(buf, n, "unsigned long long"); break;
    case TY_FLOAT:   snprintf(buf, n, "float");              break;
    case TY_DOUBLE:  snprintf(buf, n, "double");             break;
    case TY_LDOUBLE: snprintf(buf, n, "long double");        break;
    case TY_PTR: {
        char base[128];
        type_to_str(t->base, base, sizeof(base));
        snprintf(buf, n, "%s *", base);
        break;
    }
    case TY_ARRAY: {
        char base[128];
        type_to_str(t->base, base, sizeof(base));
        if (t->array_len >= 0)
            snprintf(buf, n, "%s[%d]", base, t->array_len);
        else
            snprintf(buf, n, "%s[]", base);
        break;
    }
    case TY_STRUCT:
        snprintf(buf, n, "struct %s", t->tag ? t->tag : "<anon>");
        break;
    case TY_UNION:
        snprintf(buf, n, "union %s",  t->tag ? t->tag : "<anon>");
        break;
    case TY_FUNC: {
        char ret[64];
        type_to_str(t->return_ty ? t->return_ty : t->base, ret, sizeof(ret));
        snprintf(buf, n, "%s (...)", ret);
        break;
    }
    case TY_ENUM:
        snprintf(buf, n, "enum %s", t->tag ? t->tag : "<anon>");
        break;
    default:
        snprintf(buf, n, "<type-kind-%d>", (int)t->kind);
        break;
    }
}

/* Declared in ast.h */
void type_print(Type *t)
{
    char buf[256];
    type_to_str(t, buf, sizeof(buf));
    printf("%s", buf);
}
