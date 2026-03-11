#pragma once
#include "ast.h"

/* =========================================================
 * types.h — Type utility functions and predefined type singletons
 *
 * Sizes follow x86-64 Linux (System V AMD64 ABI):
 *   _Bool       1   char    1   signed char   1   unsigned char  1
 *   short       2   int     4   long          8   long long      8
 *   float       4   double  8   long double  16   pointer        8
 *
 * All declarations here extend (never conflict with) ast.h.
 * ========================================================= */

/* ---------------------------------------------------------
 * Predefined type singletons  (initialised by types_init())
 *
 * These are the canonical, shared instances of each primitive type.
 * Do NOT write to them; use type_with_qualifiers() to add qualifiers.
 * --------------------------------------------------------- */
extern Type *ty_void;
extern Type *ty_bool;
extern Type *ty_char;     /* plain char  (signed on x86-64 Linux) */
extern Type *ty_schar;    /* signed char  */
extern Type *ty_uchar;    /* unsigned char */
extern Type *ty_short;
extern Type *ty_ushort;
extern Type *ty_int;
extern Type *ty_uint;
extern Type *ty_long;
extern Type *ty_ulong;
extern Type *ty_llong;    /* long long */
extern Type *ty_ullong;   /* unsigned long long */
extern Type *ty_float;
extern Type *ty_double;
extern Type *ty_ldouble;  /* long double */

/* ---------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------- */

/**
 * Initialise the predefined type singletons.
 * Must be called once before any other function in this module.
 * Safe to call multiple times (idempotent).
 */
void  types_init(void);

/**
 * Free all resources allocated by types_init().
 * After this call the extern pointers above are NULL.
 */
void  types_fini(void);

/* ---------------------------------------------------------
 * Type constructors
 * All returned types are heap-allocated; the caller owns them.
 * --------------------------------------------------------- */

/**
 * Allocate a fresh (incomplete) struct type.
 * Members and layout are filled in by type_layout_struct().
 */
Type *type_struct_new(const char *tag);

/**
 * Allocate a fresh (incomplete) union type.
 */
Type *type_union_new(const char *tag);

/**
 * Allocate a new enum type (underlying type = int).
 */
Type *type_enum_new(const char *tag);

/* ---------------------------------------------------------
 * Type predicates
 * (These complement the bool-returning predicates in ast.h)
 * --------------------------------------------------------- */

/** True for signed integer types (char, short, int, long, llong). */
int   type_is_signed(Type *t);

/** True for TY_PTR or TY_ARRAY. */
int   type_is_pointer_like(Type *t);

/**
 * C11 type compatibility (§6.2.7).
 * Two types are compatible if they designate the same type after
 * qualifiers are stripped.  Recursive for pointer/array/function types.
 */
int   type_is_compatible(Type *a, Type *b);

/* ---------------------------------------------------------
 * Implicit conversion helpers  (C11 §6.3)
 * --------------------------------------------------------- */

/**
 * Integer promotions (§6.3.1.1):
 * Any integer type whose conversion rank is less than that of int
 * is promoted to int (or unsigned int when the range exceeds int).
 * Returns ty_int, ty_uint, or t unchanged.
 */
Type *type_integer_promote(Type *t);

/**
 * Usual arithmetic conversions (§6.3.1.8):
 * Given the two operand types of a binary arithmetic operator,
 * return the type to which both operands should be converted.
 */
Type *type_usual_arith_conv(Type *a, Type *b);

/**
 * Array-to-pointer decay and function-to-pointer decay (§6.3.2.1):
 * If t is TY_ARRAY  → returns pointer to element type.
 * If t is TY_FUNC   → returns pointer to function.
 * Otherwise         → returns t unchanged.
 */
Type *type_decay(Type *t);

/* ---------------------------------------------------------
 * Integer conversion rank  (§6.3.1.1)
 * --------------------------------------------------------- */

/** Higher rank = wider type.  Returns -1 for non-integer types. */
int   type_integer_rank(Type *t);

/* ---------------------------------------------------------
 * Layout  (values are target-specific: x86-64 Linux)
 * --------------------------------------------------------- */

/**
 * Compute and assign byte offsets to every member of a struct or union.
 * Also sets t->size and t->align and marks t->is_complete = true.
 * Must be called after all Member nodes have been appended to t->members.
 */
void  type_layout_struct(Type *t);

/* ---------------------------------------------------------
 * Diagnostic helper
 * --------------------------------------------------------- */

/**
 * Write a human-readable description of t into buf (at most n bytes,
 * including the NUL terminator).
 */
void  type_to_str(Type *t, char *buf, int n);
