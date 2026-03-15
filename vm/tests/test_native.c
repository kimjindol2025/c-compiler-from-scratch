/*
 * test_native.c — OBJ_NATIVE & vm_define_native 검증
 *
 * V가 못 하는 것을 실제로 실행해서 확인한다:
 *   1. 런타임에 C 함수를 Value로 등록
 *   2. VM 코드에서 네이티브 함수를 일반 함수처럼 호출
 *   3. 네이티브가 클로저에 캡처됨
 *   4. ObjExternal — 외부 핸들을 GC 아래로
 *   5. ObjMap — 동적 키-값
 */
#include "../include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* ── 가짜 FreeLang 함수들 ────────────────────────────
 * 실제로는 FreeLang 런타임이 이걸 제공한다.
 * 여기선 동작 검증을 위해 C로 구현.
 */
static Value fl_add(VM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 2) return val_int(0);
    int64_t a = argv[0].kind == VAL_INT ? argv[0].i : (int64_t)argv[0].f;
    int64_t b = argv[1].kind == VAL_INT ? argv[1].i : (int64_t)argv[1].f;
    return val_int(a + b);
}

static Value fl_greet(VM *vm, int argc, Value *argv) {
    if (argc < 1 || !val_is_obj_kind(argv[0], OBJ_STRING))
        return val_obj((Obj*)vm_alloc_string(vm, "Hello!", 6));
    char buf[128];
    int  n = snprintf(buf, sizeof(buf), "Hello, %s!", AS_STRING(argv[0])->chars);
    return val_obj((Obj*)vm_alloc_string(vm, buf, n));
}

static int finalizer_called = 0;
static void fake_fl_finalizer(void *h) {
    (void)h;
    finalizer_called++;
}

/* ── T1: 네이티브 함수 등록 및 직접 호출 ───────────── */
static void test_native_call(VM *vm) {
    printf("T1: native call (fl_add(10, 32) = 42)\n");

    vm_define_native(vm, "fl_add", fl_add, 2);

    /* main: fl_add(10, 32) */
    ObjFunc *fn = vm_alloc_func(vm, "main");
    fn->arity = 0; fn->max_locals = 0;

    /* 전역에서 fl_add 로드 */
    int idx = -1;
    for (int i = 0; i < vm->global_count; i++)
        if (strcmp(vm->global_names[i], "fl_add") == 0) { idx = i; break; }
    assert(idx >= 0);

    int c10 = add_constant(fn, vm, val_int(10));
    int c32 = add_constant(fn, vm, val_int(32));
    emit_byte(fn, OP_LOAD_GLOBAL); emit_u16(fn, (uint16_t)idx);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c10);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c32);
    emit_byte(fn, OP_CALL); emit_byte(fn, 2); emit_u16(fn, 0);
    emit_byte(fn, OP_RETURN);

    ObjClosure *cl = vm_alloc_closure(vm, fn);
    vm->stack_top = 0; vm->frame_count = 0;
    VMResult r = vm_run(vm, cl);

    CHECK(r == VM_OK, "runs OK");
    CHECK(vm->value_stack[0].i == 42, "fl_add(10,32) == 42");
}

/* ── T2: 네이티브가 문자열 반환 ─────────────────────── */
static void test_native_string(VM *vm) {
    printf("T2: native returns string (greet)\n");

    vm_define_native(vm, "fl_greet", fl_greet, 1);

    int gidx = -1;
    for (int i = 0; i < vm->global_count; i++)
        if (strcmp(vm->global_names[i], "fl_greet") == 0) { gidx = i; break; }

    ObjFunc *fn = vm_alloc_func(vm, "main2");
    fn->arity = 0; fn->max_locals = 0;

    ObjString *s = vm_alloc_string(vm, "World", 5);
    int cs = add_constant(fn, vm, val_obj((Obj*)s));

    emit_byte(fn, OP_LOAD_GLOBAL); emit_u16(fn, (uint16_t)gidx);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, cs);
    emit_byte(fn, OP_CALL); emit_byte(fn, 1); emit_u16(fn, 0);
    emit_byte(fn, OP_RETURN);

    ObjClosure *cl = vm_alloc_closure(vm, fn);
    vm->stack_top = 0; vm->frame_count = 0;
    vm_run(vm, cl);

    Value result = vm->value_stack[0];
    CHECK(val_is_obj_kind(result, OBJ_STRING), "result is string");
    CHECK(strcmp(AS_STRING(result)->chars, "Hello, World!") == 0,
          "greet('World') == 'Hello, World!'");
}

/* ── T3: ObjMap ─────────────────────────────────────── */
static void test_map(VM *vm) {
    printf("T3: ObjMap dynamic key-value\n");

    ObjMap *m = vm_alloc_map(vm);
    vm_map_set(vm, m, val_obj((Obj*)vm_alloc_string(vm, "name", 4)),
                       val_obj((Obj*)vm_alloc_string(vm, "Alice", 5)));
    vm_map_set(vm, m, val_int(42), val_float(3.14));

    Value v1 = vm_map_get(m, val_obj((Obj*)vm_alloc_string(vm, "name", 4)));
    Value v2 = vm_map_get(m, val_int(42));
    Value v3 = vm_map_get(m, val_int(99));

    CHECK(val_is_obj_kind(v1, OBJ_STRING) &&
          strcmp(AS_STRING(v1)->chars, "Alice") == 0, "map['name'] == 'Alice'");
    CHECK(v2.kind == VAL_FLOAT && v2.f == 3.14, "map[42] == 3.14");
    CHECK(v3.kind == VAL_NIL, "map[99] == nil");
}

/* ── T4: ObjExternal — finalizer 호출 ─────────────────
 * FreeLang 핸들을 GC 아래로: GC가 회수할 때 FL 쪽에 알림.
 */
static void test_external(VM *vm) {
    printf("T4: ObjExternal finalizer\n");

    int dummy_handle = 0xDEAD;
    ObjExternal *ext = vm_alloc_external(vm, &dummy_handle,
                                          fake_fl_finalizer, "FLHandle");
    (void)ext;

    /* GC 강제 실행 (ext는 루트에 없으므로 수집됨) */
    vm->next_gc_threshold = 0;
    vm_gc(vm);

    CHECK(finalizer_called > 0, "finalizer called on GC");
}

/* ── T5: 내장 함수 (vm_register_builtins) ───────────── */
static void test_builtins(VM *vm) {
    printf("T5: builtin natives (print, len, type)\n");

    vm_register_builtins(vm);

    /* type("hello") == "string" */
    int tidx = -1;
    for (int i = 0; i < vm->global_count; i++)
        if (strcmp(vm->global_names[i], "type") == 0) { tidx = i; break; }
    CHECK(tidx >= 0, "type() registered");

    /* len([1,2,3]) == 3 */
    int lidx = -1;
    for (int i = 0; i < vm->global_count; i++)
        if (strcmp(vm->global_names[i], "len") == 0) { lidx = i; break; }
    CHECK(lidx >= 0, "len() registered");

    ObjFunc *fn = vm_alloc_func(vm, "test_builtins");
    fn->arity = 0; fn->max_locals = 0;

    /* len([10, 20, 30]) → 3 */
    int c10 = add_constant(fn, vm, val_int(10));
    int c20 = add_constant(fn, vm, val_int(20));
    int c30 = add_constant(fn, vm, val_int(30));
    emit_byte(fn, OP_LOAD_GLOBAL); emit_u16(fn, (uint16_t)lidx);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c10);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c20);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c30);
    emit_byte(fn, OP_MAKE_ARRAY);  emit_u16(fn, 3);
    emit_byte(fn, OP_CALL); emit_byte(fn, 1); emit_u16(fn, 0);
    emit_byte(fn, OP_RETURN);

    ObjClosure *cl = vm_alloc_closure(vm, fn);
    vm->stack_top = 0; vm->frame_count = 0;
    vm_run(vm, cl);
    CHECK(vm->value_stack[0].i == 3, "len([10,20,30]) == 3");
}

int main(void) {
    printf("=== Native / FFI Tests ===\n\n");

    VM *vm = vm_new();

    test_native_call(vm);   printf("\n");
    test_native_string(vm); printf("\n");
    test_map(vm);           printf("\n");
    test_external(vm);      printf("\n");
    test_builtins(vm);      printf("\n");

    vm_free(vm);

    printf("=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
