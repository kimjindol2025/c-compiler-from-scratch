/*
 * tests/test_parser.c — Comprehensive parser tests
 *
 * Covers:
 *   - Empty translation unit
 *   - Simple function with arithmetic
 *   - Variable declarations (with/without initializer)
 *   - All statement types: if/else, while, do-while, for, switch/case/default,
 *     break, continue, return, goto/label, compound, null, expr-stmt
 *   - All expression precedence levels
 *   - Pointer operations (&, *, ->, ++/--)
 *   - Struct / union / enum definitions and access
 *   - Function calls with multiple arguments
 *   - Cast expressions
 *   - Sizeof / _Alignof
 *   - Ternary operator
 *   - Compound-assignment operators
 *   - Comma expression
 *   - Typedef
 *   - Multiple declarators in one declaration
 *   - Nested functions / recursion
 *   - String literals (adjacent concatenation)
 *   - Array declarators and index
 *   - Function pointer declarators
 *   - Initializer lists
 */

#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL at %s:%d: %s\n", \
           __FILE__, __LINE__, #expr); } \
} while (0)

/* Helpers ---------------------------------------------------------- */

typedef struct { Parser *p; Lexer *l; Node *root; } PR;

static PR parse_keep(const char *src)
{
    PR r;
    r.l    = lexer_new(src, "<test>");
    r.p    = parser_new(r.l, "<test>");
    r.root = parser_parse(r.p);
    return r;
}

static void pr_free(PR *r)
{
    parser_free(r->p);
    lexer_free(r->l);
}

/* Return the first top-level declaration node */
static Node *first_decl(PR *r)
{
    if (!r->root || r->root->unit.count < 1) return NULL;
    return r->root->unit.decls[0];
}

/* Return the first statement from a function body */
static Node *first_body_stmt(Node *fn)
{
    if (!fn || fn->kind != ND_FUNC_DEF) return NULL;
    Node *body = fn->func.body;
    if (!body || body->kind != ND_COMPOUND) return NULL;
    if (body->compound.count < 1) return NULL;
    return body->compound.stmts[0];
}

/* ====================================================================
 * Tests
 * ==================================================================== */

/* --- 1. Empty translation unit ------------------------------------ */
static void test_empty(void)
{
    printf("--- test_empty\n");
    PR r = parse_keep("");
    CHECK(r.root != NULL);
    CHECK(r.root->kind == ND_TRANSLATION_UNIT);
    CHECK(r.root->unit.count == 0);
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 2. Simple function with arithmetic --------------------------- */
static void test_simple_func(void)
{
    printf("--- test_simple_func\n");
    PR r = parse_keep("int foo(void) { return 42; }");
    CHECK(r.root->unit.count >= 1);
    Node *fn = first_decl(&r);
    CHECK(fn && fn->kind == ND_FUNC_DEF);
    CHECK(fn->func.name && strcmp(fn->func.name, "foo") == 0);
    CHECK(fn->func.ret_type != NULL);
    CHECK(fn->func.body != NULL);
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 3. Variable declaration with initializer --------------------- */
static void test_var_decl(void)
{
    printf("--- test_var_decl\n");
    PR r = parse_keep("int x = 10;");
    CHECK(r.p->error_count == 0);
    Node *d = first_decl(&r);
    CHECK(d && d->kind == ND_VAR_DECL);
    CHECK(d->decl.name && strcmp(d->decl.name, "x") == 0);
    CHECK(d->decl.init != NULL);
    CHECK(d->decl.init->kind == ND_INT_LIT);
    CHECK(d->decl.init->ival == 10);
    pr_free(&r);
}

/* --- 4. Multiple variable declarations ---------------------------- */
static void test_multi_decl(void)
{
    printf("--- test_multi_decl\n");
    PR r = parse_keep("int a = 1, b = 2, c;");
    CHECK(r.p->error_count == 0);
    /* Should emit at least 3 ND_VAR_DECL nodes */
    CHECK(r.root->unit.count >= 3);
    pr_free(&r);
}

/* --- 5. Arithmetic expression precedence -------------------------- */
static void test_arith_prec(void)
{
    printf("--- test_arith_prec\n");
    /* 1 + 2 * 3 should parse as 1 + (2 * 3) */
    PR r = parse_keep("int f(void) { return 1 + 2 * 3; }");
    CHECK(r.p->error_count == 0);
    Node *fn = first_decl(&r);
    CHECK(fn != NULL);
    /* body first stmt = return */
    Node *ret = first_body_stmt(fn);
    CHECK(ret && ret->kind == ND_RETURN);
    if (ret && ret->kind == ND_RETURN) {
        Node *expr = ret->return_.value;
        CHECK(expr && expr->kind == ND_ADD);
        if (expr && expr->kind == ND_ADD) {
            CHECK(expr->binary.left->kind == ND_INT_LIT);
            CHECK(expr->binary.left->ival == 1);
            Node *rhs = expr->binary.right;
            CHECK(rhs && rhs->kind == ND_MUL);
            if (rhs && rhs->kind == ND_MUL) {
                CHECK(rhs->binary.left->ival == 2);
                CHECK(rhs->binary.right->ival == 3);
            }
        }
    }
    pr_free(&r);
}

/* --- 6. Comparison and logical operators -------------------------- */
static void test_logical(void)
{
    printf("--- test_logical\n");
    PR r = parse_keep(
        "int f(int a, int b) {\n"
        "    return (a > 0 && b < 10) || a == b;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 7. Assignment operators -------------------------------------- */
static void test_assign_ops(void)
{
    printf("--- test_assign_ops\n");
    PR r = parse_keep(
        "int f(void) {\n"
        "    int x = 5;\n"
        "    x += 1; x -= 1; x *= 2; x /= 2;\n"
        "    x %= 3; x &= 0xFF; x |= 0x01; x ^= 0x10;\n"
        "    x <<= 1; x >>= 1;\n"
        "    return x;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 8. Unary operators ------------------------------------------- */
static void test_unary(void)
{
    printf("--- test_unary\n");
    PR r = parse_keep(
        "int f(int *p, int x) {\n"
        "    int a = -x;\n"
        "    int b = !x;\n"
        "    int c = ~x;\n"
        "    int d = *p;\n"
        "    int *q = &x;\n"
        "    ++x; --x; x++; x--;\n"
        "    return a + b + c + d;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 9. If / else (dangling else) --------------------------------- */
static void test_if_else(void)
{
    printf("--- test_if_else\n");
    /* Dangling else: else binds to inner if */
    PR r = parse_keep(
        "int f(int x) {\n"
        "    if (x > 0)\n"
        "        if (x > 100) return 2;\n"
        "        else return 1;\n"
        "    return 0;\n"
        "}");
    CHECK(r.p->error_count == 0);
    Node *fn = first_decl(&r);
    CHECK(fn != NULL);
    /* First stmt should be an if */
    Node *outer_if = first_body_stmt(fn);
    CHECK(outer_if && outer_if->kind == ND_IF);
    if (outer_if && outer_if->kind == ND_IF) {
        /* outer else_ should be NULL (dangling else bound to inner) */
        CHECK(outer_if->if_.else_ == NULL);
        /* inner if should have an else_ */
        Node *inner_if = outer_if->if_.then;
        CHECK(inner_if && inner_if->kind == ND_IF);
        if (inner_if && inner_if->kind == ND_IF)
            CHECK(inner_if->if_.else_ != NULL);
    }
    pr_free(&r);
}

/* --- 10. While loop ----------------------------------------------- */
static void test_while(void)
{
    printf("--- test_while\n");
    PR r = parse_keep(
        "int sum(int n) {\n"
        "    int s = 0;\n"
        "    while (n > 0) { s = s + n; n = n - 1; }\n"
        "    return s;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 11. Do-while loop -------------------------------------------- */
static void test_do_while(void)
{
    printf("--- test_do_while\n");
    PR r = parse_keep(
        "int f(int n) {\n"
        "    int x = 0;\n"
        "    do { x++; n--; } while (n > 0);\n"
        "    return x;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 12. For loop ------------------------------------------------- */
static void test_for(void)
{
    printf("--- test_for\n");
    PR r = parse_keep(
        "int f(void) {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < 10; i++) { s += i; }\n"
        "    return s;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 13. Switch / case / default / break -------------------------- */
static void test_switch(void)
{
    printf("--- test_switch\n");
    PR r = parse_keep(
        "int day_type(int d) {\n"
        "    switch (d) {\n"
        "    case 0: return 0;\n"
        "    case 6: return 0;\n"
        "    default: return 1;\n"
        "    }\n"
        "}");
    CHECK(r.p->error_count == 0);
    Node *fn = first_decl(&r);
    Node *sw = first_body_stmt(fn);
    CHECK(sw && sw->kind == ND_SWITCH);
    pr_free(&r);
}

/* --- 14. Break and continue --------------------------------------- */
static void test_break_continue(void)
{
    printf("--- test_break_continue\n");
    PR r = parse_keep(
        "int f(int n) {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < n; i++) {\n"
        "        if (i == 5) break;\n"
        "        if (i % 2 == 0) continue;\n"
        "        s += i;\n"
        "    }\n"
        "    return s;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 15. Goto and label ------------------------------------------- */
static void test_goto(void)
{
    printf("--- test_goto\n");
    PR r = parse_keep(
        "int f(int n) {\n"
        "    if (n < 0) goto error;\n"
        "    return n;\n"
        "error:\n"
        "    return -1;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 16. Pointer operations --------------------------------------- */
static void test_pointer(void)
{
    printf("--- test_pointer\n");
    PR r = parse_keep(
        "int deref(int *p)  { return *p; }\n"
        "int *addr(int *p)  { return p; }\n"
        "void inc(int *p)   { (*p)++; }\n"
        "int arr_sum(int *a, int n) {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < n; i++) s += a[i];\n"
        "    return s;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 17. Struct definition and member access ---------------------- */
static void test_struct(void)
{
    printf("--- test_struct\n");
    PR r = parse_keep(
        "struct Point { int x; int y; };\n"
        "int distance_sq(struct Point p) {\n"
        "    return p.x * p.x + p.y * p.y;\n"
        "}\n"
        "void translate(struct Point *p, int dx, int dy) {\n"
        "    p->x += dx;\n"
        "    p->y += dy;\n"
        "}");
    CHECK(r.p->error_count == 0);
    CHECK(r.root->unit.count >= 3);
    pr_free(&r);
}

/* --- 18. Union ---------------------------------------------------- */
static void test_union(void)
{
    printf("--- test_union\n");
    PR r = parse_keep(
        "union Data { int i; float f; char bytes[4]; };\n"
        "int get_int(union Data d) { return d.i; }");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 19. Enum ----------------------------------------------------- */
static void test_enum(void)
{
    printf("--- test_enum\n");
    PR r = parse_keep(
        "enum Color { RED = 0, GREEN = 1, BLUE = 2 };\n"
        "int is_primary(int c) {\n"
        "    return c == RED || c == GREEN || c == BLUE;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 20. Typedef -------------------------------------------------- */
static void test_typedef(void)
{
    printf("--- test_typedef\n");
    PR r = parse_keep(
        "typedef int Integer;\n"
        "typedef unsigned long size_t_alias;\n"
        "Integer add(Integer a, Integer b) { return a + b; }");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 21. Typedef struct ------------------------------------------- */
static void test_typedef_struct(void)
{
    printf("--- test_typedef_struct\n");
    PR r = parse_keep(
        "typedef struct {\n"
        "    int x, y;\n"
        "} Vec2;\n"
        "float dot(Vec2 a, Vec2 b) {\n"
        "    return (float)(a.x * b.x + a.y * b.y);\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 22. Function call with multiple arguments -------------------- */
static void test_call(void)
{
    printf("--- test_call\n");
    PR r = parse_keep(
        "int add(int a, int b, int c) { return a + b + c; }\n"
        "int main(void) { return add(1, 2, 3); }\n");
    CHECK(r.p->error_count == 0);
    CHECK(r.root->unit.count >= 2);
    /* Inspect the call node inside main */
    Node *main_fn = r.root->unit.decls[1];
    CHECK(main_fn && main_fn->kind == ND_FUNC_DEF);
    pr_free(&r);
}

/* --- 23. Variadic function declaration ---------------------------- */
static void test_variadic(void)
{
    printf("--- test_variadic\n");
    PR r = parse_keep(
        "int printf_stub(const char *fmt, ...);\n"
        "int f(void) { return printf_stub(\"%d\", 42); }");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 24. Cast expression ------------------------------------------ */
static void test_cast(void)
{
    printf("--- test_cast\n");
    PR r = parse_keep(
        "double to_double(int x) { return (double)x; }\n"
        "int to_int(double x) { return (int)x; }\n"
        "void *to_voidp(int *p) { return (void *)p; }");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 25. Sizeof and _Alignof ------------------------------------- */
static void test_sizeof(void)
{
    printf("--- test_sizeof\n");
    PR r = parse_keep(
        "int f(void) {\n"
        "    int a = sizeof(int);\n"
        "    int b = sizeof(long long);\n"
        "    int c = _Alignof(double);\n"
        "    int x = 0;\n"
        "    int d = sizeof x;\n"
        "    return a + b + c + d;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 26. Ternary operator ----------------------------------------- */
static void test_ternary(void)
{
    printf("--- test_ternary\n");
    PR r = parse_keep(
        "int abs_val(int x) { return x >= 0 ? x : -x; }\n"
        /* Nested ternary: right-associative */
        "int clamp(int x, int lo, int hi) {\n"
        "    return x < lo ? lo : x > hi ? hi : x;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 27. Comma expression ----------------------------------------- */
static void test_comma(void)
{
    printf("--- test_comma\n");
    PR r = parse_keep(
        "int f(int *p) {\n"
        "    int a, b;\n"
        "    for (a = 0, b = 10; a < b; a++, b--) {}\n"
        "    return a;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 28. Array declarator and indexing ---------------------------- */
static void test_array(void)
{
    printf("--- test_array\n");
    PR r = parse_keep(
        "int arr[10];\n"
        "int mat[3][4];\n"
        "int sum_arr(int a[], int n) {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < n; i++) s += a[i];\n"
        "    return s;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 29. Initializer list ----------------------------------------- */
static void test_init_list(void)
{
    printf("--- test_init_list\n");
    PR r = parse_keep(
        "int arr[5] = {1, 2, 3, 4, 5};\n"
        "struct P { int x; int y; };\n"
        "struct P p = {10, 20};\n");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 30. Nested initializer list ---------------------------------- */
static void test_nested_init(void)
{
    printf("--- test_nested_init\n");
    PR r = parse_keep(
        "int mat[2][3] = {{1, 2, 3}, {4, 5, 6}};\n");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 31. Pointer to function declarator --------------------------- */
static void test_func_ptr(void)
{
    printf("--- test_func_ptr\n");
    PR r = parse_keep(
        "typedef int (*Comparator)(const void *, const void *);\n"
        "int apply(int (*fn)(int, int), int a, int b) { return fn(a, b); }");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 32. Storage class: static / extern --------------------------- */
static void test_storage_class(void)
{
    printf("--- test_storage_class\n");
    PR r = parse_keep(
        "static int counter = 0;\n"
        "extern int global_x;\n"
        "static int next(void) { return ++counter; }");
    CHECK(r.p->error_count == 0);
    Node *decl = first_decl(&r);
    CHECK(decl && decl->kind == ND_VAR_DECL);
    CHECK(decl->decl.is_static);
    pr_free(&r);
}

/* --- 33. Inline function ------------------------------------------ */
static void test_inline(void)
{
    printf("--- test_inline\n");
    PR r = parse_keep(
        "static inline int square(int x) { return x * x; }");
    CHECK(r.p->error_count == 0);
    Node *fn = first_decl(&r);
    CHECK(fn && fn->kind == ND_FUNC_DEF);
    CHECK(fn->func.is_static);
    CHECK(fn->func.is_inline);
    pr_free(&r);
}

/* --- 34. Const / volatile qualifiers ----------------------------- */
static void test_qualifiers(void)
{
    printf("--- test_qualifiers\n");
    PR r = parse_keep(
        "const int MAX = 100;\n"
        "volatile int tick;\n"
        "int f(const int *p) { return *p; }");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 35. Recursive function -------------------------------------- */
static void test_recursive(void)
{
    printf("--- test_recursive\n");
    PR r = parse_keep(
        "int fib(int n) {\n"
        "    if (n <= 1) return n;\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 36. String literal ------------------------------------------ */
static void test_string(void)
{
    printf("--- test_string\n");
    PR r = parse_keep(
        "const char *greeting = \"hello\";\n"
        "int f(void) {\n"
        "    const char *s = \"foo\" \"bar\";\n"   /* adjacent concat */
        "    return 0;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 37. Complex nested expression ------------------------------- */
static void test_complex_expr(void)
{
    printf("--- test_complex_expr\n");
    PR r = parse_keep(
        "int f(int a, int b, int c) {\n"
        "    return (a + b) * (c - 1) / (a != 0 ? a : 1) % 7\n"
        "           & 0xFF | (c << 2) ^ (b >> 1);\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 38. Null statement ------------------------------------------ */
static void test_null_stmt(void)
{
    printf("--- test_null_stmt\n");
    PR r = parse_keep(
        "int f(int n) {\n"
        "    while (n-- > 0) ;   /* null body */\n"
        "    return n;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 39. Post/pre increment/decrement on pointer ------------------ */
static void test_ptr_arith(void)
{
    printf("--- test_ptr_arith\n");
    PR r = parse_keep(
        "int sum_until_zero(int *p) {\n"
        "    int s = 0;\n"
        "    while (*p) s += *p++;\n"
        "    return s;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 40. Struct with multiple complex members -------------------- */
static void test_struct_complex(void)
{
    printf("--- test_struct_complex\n");
    PR r = parse_keep(
        "struct Node {\n"
        "    int data;\n"
        "    struct Node *next;\n"
        "    struct Node *prev;\n"
        "};\n"
        "void insert(struct Node *head, struct Node *n) {\n"
        "    n->next = head->next;\n"
        "    n->prev = head;\n"
        "    head->next = n;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 41. Nested struct ------------------------------------------- */
static void test_nested_struct(void)
{
    printf("--- test_nested_struct\n");
    PR r = parse_keep(
        "struct Color { unsigned char r, g, b, a; };\n"
        "struct Sprite {\n"
        "    int x, y;\n"
        "    struct Color tint;\n"
        "    unsigned int w, h;\n"
        "};\n"
        "unsigned char get_alpha(struct Sprite *s) {\n"
        "    return s->tint.a;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 42. Deeply nested blocks ------------------------------------ */
static void test_nested_blocks(void)
{
    printf("--- test_nested_blocks\n");
    PR r = parse_keep(
        "int f(int n) {\n"
        "    int a = 1;\n"
        "    {\n"
        "        int b = 2;\n"
        "        {\n"
        "            int c = 3;\n"
        "            a = a + b + c;\n"
        "        }\n"
        "    }\n"
        "    return a;\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 43. Multiple functions in one TU ----------------------------- */
static void test_multi_func(void)
{
    printf("--- test_multi_func\n");
    PR r = parse_keep(
        "int min(int a, int b) { return a < b ? a : b; }\n"
        "int max(int a, int b) { return a > b ? a : b; }\n"
        "int clamp(int x, int lo, int hi) {\n"
        "    return min(max(x, lo), hi);\n"
        "}");
    CHECK(r.p->error_count == 0);
    CHECK(r.root->unit.count == 3);
    pr_free(&r);
}

/* --- 44. Bitwise operators ---------------------------------------- */
static void test_bitwise(void)
{
    printf("--- test_bitwise\n");
    PR r = parse_keep(
        "int f(unsigned int x, unsigned int y) {\n"
        "    unsigned int a = x & y;\n"
        "    unsigned int b = x | y;\n"
        "    unsigned int c = x ^ y;\n"
        "    unsigned int d = ~x;\n"
        "    unsigned int e = x << 3;\n"
        "    unsigned int f2 = x >> 2;\n"
        "    return (int)(a + b + c + d + e + f2);\n"
        "}");
    CHECK(r.p->error_count == 0);
    pr_free(&r);
}

/* --- 45. Pre-declared extern and forward declaration -------------- */
static void test_forward_decl(void)
{
    printf("--- test_forward_decl\n");
    PR r = parse_keep(
        "int foo(int x);\n"          /* forward decl */
        "int bar(int x) { return foo(x + 1); }\n"
        "int foo(int x) { return x * 2; }\n");
    CHECK(r.p->error_count == 0);
    CHECK(r.root->unit.count == 3);
    pr_free(&r);
}

/* --- 46. node_print smoke-test ------------------------------------ */
static void test_node_print(void)
{
    printf("--- test_node_print\n");
    Lexer  *l = lexer_new("int f(int x) { return x * 2; }", "<test>");
    Parser *p = parser_new(l, "<test>");
    Node   *root = parser_parse(p);
    /* Just call node_print and make sure it doesn't crash */
    node_print(root, 0);
    CHECK(p->error_count == 0);
    parser_free(p);
    lexer_free(l);
}

/* ====================================================================
 * Main
 * ==================================================================== */
int main(void)
{
    printf("=== Parser Tests ===\n\n");

    test_empty();
    test_simple_func();
    test_var_decl();
    test_multi_decl();
    test_arith_prec();
    test_logical();
    test_assign_ops();
    test_unary();
    test_if_else();
    test_while();
    test_do_while();
    test_for();
    test_switch();
    test_break_continue();
    test_goto();
    test_pointer();
    test_struct();
    test_union();
    test_enum();
    test_typedef();
    test_typedef_struct();
    test_call();
    test_variadic();
    test_cast();
    test_sizeof();
    test_ternary();
    test_comma();
    test_array();
    test_init_list();
    test_nested_init();
    test_func_ptr();
    test_storage_class();
    test_inline();
    test_qualifiers();
    test_recursive();
    test_string();
    test_complex_expr();
    test_null_stmt();
    test_ptr_arith();
    test_struct_complex();
    test_nested_struct();
    test_nested_blocks();
    test_multi_func();
    test_bitwise();
    test_forward_decl();
    test_node_print();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
