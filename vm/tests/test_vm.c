/*
 * test_vm.c — VM 동작 검증 테스트
 *
 * 각 테스트는 실제 바이트코드를 직접 방출해서 VM을 실행하고
 * 결과를 확인한다. 컴파일러 없이 VM 자체를 검증하기 위해.
 *
 * 테스트 목록:
 *   T1: 정수 산술 (1 + 2 * 3 = 7)
 *   T2: 조건 분기 (if truthy → 10, else → 20)
 *   T3: 루프 (1+2+...+10 = 55)
 *   T4: 함수 호출 & 반환 (add(3, 4) = 7)
 *   T5: 클로저 캡처 (counter 패턴)
 *   T6: 배열
 *   T7: 예외 throw/catch
 */
#include "../include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 테스트 성공/실패 카운터 */
static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass_count++; } \
    else      { printf("  FAIL: %s\n", msg); fail_count++; } \
} while(0)

/* ── 헬퍼: ObjFunc 생성 후 클로저로 래핑 ──────────── */
static ObjClosure *make_closure(VM *vm, const char *name, int arity, int max_locals) {
    ObjFunc *fn = vm_alloc_func(vm, name);
    fn->arity      = arity;
    fn->max_locals = max_locals;
    ObjClosure *cl = vm_alloc_closure(vm, fn);
    return cl;
}

/* ── T1: 정수 산술 ──────────────────────────────────
 * 바이트코드: LOAD_CONST 1, LOAD_CONST 2, LOAD_CONST 3,
 *              MUL, ADD, RETURN
 * 결과: 7
 */
static void test_arithmetic(VM *vm) {
    printf("T1: arithmetic (1 + 2*3 = 7)\n");

    ObjClosure *cl = make_closure(vm, "arith", 0, 0);
    ObjFunc    *fn = cl->func;

    int c1 = add_constant(fn, vm, val_int(1));
    int c2 = add_constant(fn, vm, val_int(2));
    int c3 = add_constant(fn, vm, val_int(3));

    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c1);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c2);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c3);
    emit_byte(fn, OP_MUL);
    emit_byte(fn, OP_ADD);
    emit_byte(fn, OP_RETURN);

    disassemble_func(fn, stdout);

    vm->stack_top  = 0;
    vm->frame_count = 0;
    VMResult r = vm_run(vm, cl);

    CHECK(r == VM_OK, "runs OK");
    CHECK(vm->stack_top == 1 && vm->value_stack[0].i == 7, "result == 7");
}

/* ── T2: 조건 분기 ──────────────────────────────────
 * if (true) { push 10 } else { push 20 } → return 10
 */
static void test_branch(VM *vm) {
    printf("T2: branch (if true → 10)\n");

    ObjClosure *cl = make_closure(vm, "branch", 0, 0);
    ObjFunc    *fn = cl->func;

    int c10 = add_constant(fn, vm, val_int(10));
    int c20 = add_constant(fn, vm, val_int(20));

    /* LOAD_TRUE */
    emit_byte(fn, OP_LOAD_TRUE);
    /* POP_JUMP_IF_FALSE → else */
    int jmp = emit_jump(fn, OP_POP_JUMP_IF_FALSE);
    /* then: push 10, jump over else */
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c10);
    int jmp2 = emit_jump(fn, OP_JUMP);
    /* else: push 20 */
    patch_jump(fn, jmp);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c20);
    patch_jump(fn, jmp2);
    emit_byte(fn, OP_RETURN);

    disassemble_func(fn, stdout);

    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, cl);

    CHECK(vm->value_stack[0].i == 10, "result == 10");
}

/* ── T3: 루프 (sum 1..10 = 55) ─────────────────────
 * 로컬[0] = sum = 0
 * 로컬[1] = i   = 1
 * while i <= 10: sum += i; i++
 * return sum
 */
static void test_loop(VM *vm) {
    printf("T3: loop (sum 1..10 = 55)\n");

    ObjClosure *cl = make_closure(vm, "loop", 0, 2);
    ObjFunc    *fn = cl->func;

    int c0  = add_constant(fn, vm, val_int(0));
    int c1  = add_constant(fn, vm, val_int(1));
    int c10 = add_constant(fn, vm, val_int(10));

    /* sum = 0 (slot 0) */
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c0);
    emit_byte(fn, OP_STORE_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_POP);

    /* i = 1 (slot 1) */
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c1);
    emit_byte(fn, OP_STORE_LOCAL); emit_byte(fn, 1);
    emit_byte(fn, OP_POP);

    /* loop_start: */
    int loop_start = fn->code_len;

    /* if i > 10: break */
    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 1);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c10);
    emit_byte(fn, OP_GT);
    int jmp_out = emit_jump(fn, OP_POP_JUMP_IF_TRUE);

    /* sum += i */
    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 1);
    emit_byte(fn, OP_ADD);
    emit_byte(fn, OP_STORE_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_POP);

    /* i++ */
    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 1);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c1);
    emit_byte(fn, OP_ADD);
    emit_byte(fn, OP_STORE_LOCAL); emit_byte(fn, 1);
    emit_byte(fn, OP_POP);

    /* jump back */
    emit_byte(fn, OP_JUMP);
    int32_t back = (int32_t)(loop_start - fn->code_len - 4);
    emit_i32(fn, back);

    patch_jump(fn, jmp_out);

    /* return sum */
    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_RETURN);

    disassemble_func(fn, stdout);

    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, cl);

    CHECK(vm->value_stack[0].i == 55, "sum == 55");
}

/* ── T4: 함수 호출 (add(3, 4) = 7) ─────────────────
 * add 함수: param[0] + param[1] return
 * main 함수: make closure add, push 3, push 4, call 2, return
 */
static void test_call(VM *vm) {
    printf("T4: function call (add(3,4) = 7)\n");

    /* add 함수 */
    ObjClosure *add_cl = make_closure(vm, "add", 2, 2);
    ObjFunc    *add_fn = add_cl->func;
    emit_byte(add_fn, OP_LOAD_LOCAL); emit_byte(add_fn, 0);
    emit_byte(add_fn, OP_LOAD_LOCAL); emit_byte(add_fn, 1);
    emit_byte(add_fn, OP_ADD);
    emit_byte(add_fn, OP_RETURN);

    /* main 함수 */
    ObjClosure *main_cl = make_closure(vm, "main", 0, 0);
    ObjFunc    *main_fn = main_cl->func;

    int cadd = add_constant(main_fn, vm, val_obj((Obj*)add_cl));
    int c3   = add_constant(main_fn, vm, val_int(3));
    int c4   = add_constant(main_fn, vm, val_int(4));

    /* push add closure */
    emit_byte(main_fn, OP_LOAD_CONST); emit_u16(main_fn, cadd);
    emit_byte(main_fn, OP_LOAD_CONST); emit_u16(main_fn, c3);
    emit_byte(main_fn, OP_LOAD_CONST); emit_u16(main_fn, c4);
    /* CALL nargs=2 ic_slot=0 */
    emit_byte(main_fn, OP_CALL); emit_byte(main_fn, 2); emit_u16(main_fn, 0);
    emit_byte(main_fn, OP_RETURN);

    disassemble_func(add_fn, stdout);
    disassemble_func(main_fn, stdout);

    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, main_cl);

    CHECK(vm->value_stack[0].i == 7, "add(3,4) == 7");
}

/* ── T5: 클로저 캡처 (counter 패턴) ─────────────────
 * make_counter() → closure that increments captured 'n'
 * counter() → 1
 * counter() → 2
 *
 * Q6를 직접 검증한다:
 *   - MAKE_CLOSURE [is_local=1 idx=0] → 슬롯 0(n)을 캡처
 *   - 내부 함수에서 LOAD_UPVAL 0 / STORE_UPVAL 0으로 접근
 */
static void test_closure(VM *vm) {
    printf("T5: closure capture (counter)\n");

    /* inner 함수: n을 읽어 +1하고 저장 후 반환
     * upvalue[0] = captured n
     */
    ObjFunc *inner_fn = vm_alloc_func(vm, "counter_inner");
    inner_fn->arity      = 0;
    inner_fn->max_locals = 0;
    inner_fn->upval_count = 1;

    int c1_i = add_constant(inner_fn, vm, val_int(1));
    emit_byte(inner_fn, OP_LOAD_UPVAL);  emit_byte(inner_fn, 0);
    emit_byte(inner_fn, OP_LOAD_CONST);  emit_u16(inner_fn, c1_i);
    emit_byte(inner_fn, OP_ADD);
    emit_byte(inner_fn, OP_STORE_UPVAL); emit_byte(inner_fn, 0);
    emit_byte(inner_fn, OP_RETURN);

    /* outer 함수: n=0 로컬, MAKE_CLOSURE inner [is_local=1 idx=0], return closure */
    ObjClosure *outer_cl = make_closure(vm, "make_counter", 0, 1);
    ObjFunc    *outer_fn = outer_cl->func;

    int c0 = add_constant(outer_fn, vm, val_int(0));
    int cfn = add_constant(outer_fn, vm, val_obj((Obj*)inner_fn));

    /* n = 0 → slot[0] */
    emit_byte(outer_fn, OP_LOAD_CONST);  emit_u16(outer_fn, c0);
    emit_byte(outer_fn, OP_STORE_LOCAL); emit_byte(outer_fn, 0);
    emit_byte(outer_fn, OP_POP);

    /* MAKE_CLOSURE inner_fn, 1 upval: [is_local=1, idx=0] */
    emit_byte(outer_fn, OP_MAKE_CLOSURE);
    emit_u16(outer_fn, cfn);
    emit_byte(outer_fn, 1);     /* n_upvals */
    emit_byte(outer_fn, 1);     /* is_local = 1 */
    emit_byte(outer_fn, 0);     /* idx = slot[0] = n */

    emit_byte(outer_fn, OP_RETURN);

    disassemble_func(inner_fn, stdout);
    disassemble_func(outer_fn, stdout);

    /* make_counter() → counter closure */
    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, outer_cl);
    Value counter_val = vm->value_stack[0];
    CHECK(val_is_obj_kind(counter_val, OBJ_CLOSURE), "make_counter returns closure");

    ObjClosure *counter_cl = AS_CLOSURE(counter_val);

    /* counter() 첫 번째 호출 → 1 */
    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, counter_cl);
    CHECK(vm->value_stack[0].i == 1, "counter() == 1");

    /* counter() 두 번째 호출 → 2 */
    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, counter_cl);
    CHECK(vm->value_stack[0].i == 2, "counter() == 2");
}

/* ── T6: 배열 ─────────────────────────────────────── */
static void test_array(VM *vm) {
    printf("T6: array [1, 2, 3][1] == 2\n");

    ObjClosure *cl = make_closure(vm, "array_test", 0, 0);
    ObjFunc    *fn = cl->func;

    int c1 = add_constant(fn, vm, val_int(1));
    int c2 = add_constant(fn, vm, val_int(2));
    int c3 = add_constant(fn, vm, val_int(3));
    int ci = add_constant(fn, vm, val_int(1)); /* index */

    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c1);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c2);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c3);
    emit_byte(fn, OP_MAKE_ARRAY); emit_u16(fn, 3);
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, ci);
    emit_byte(fn, OP_ARRAY_GET);
    emit_byte(fn, OP_RETURN);

    disassemble_func(fn, stdout);

    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, cl);

    CHECK(vm->value_stack[0].i == 2, "[1,2,3][1] == 2");
}

/* ── T7: 예외 throw/catch ──────────────────────────
 * Q5 검증: TryFrame 기반 언와인딩
 *
 * try { throw "error" } catch(e) { result = 42 }
 * return result
 */
static void test_exception(VM *vm) {
    printf("T7: exception throw/catch\n");

    ObjClosure *cl = make_closure(vm, "exc_test", 0, 1);
    ObjFunc    *fn = cl->func;

    /* result = 0 → slot[0] */
    int c0  = add_constant(fn, vm, val_int(0));
    int c42 = add_constant(fn, vm, val_int(42));

    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c0);
    emit_byte(fn, OP_STORE_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_POP);

    /* TRY_BEGIN: catch_off=?, finally_off=0 */
    emit_byte(fn, OP_TRY_BEGIN);
    int try_pos = fn->code_len;
    emit_i32(fn, 0); /* catch_off placeholder */
    emit_i32(fn, 0); /* finally_off = 0 */

    /* throw "error" */
    ObjString *err_str = vm_alloc_string(vm, "error", 5);
    int cerr = add_constant(fn, vm, val_obj((Obj*)err_str));
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, cerr);
    emit_byte(fn, OP_THROW);

    /* TRY_END */
    emit_byte(fn, OP_TRY_END);

    /* jump over catch */
    int jmp_over = emit_jump(fn, OP_JUMP);

    /* catch block 시작 → patch catch_off */
    int catch_start = fn->code_len;
    int32_t catch_off = (int32_t)(catch_start - try_pos - 8);
    uint32_t u = (uint32_t)catch_off;
    fn->code[try_pos + 0] = u & 0xFF;
    fn->code[try_pos + 1] = (u >> 8)  & 0xFF;
    fn->code[try_pos + 2] = (u >> 16) & 0xFF;
    fn->code[try_pos + 3] = (u >> 24) & 0xFF;

    /* catch: 예외 값은 stack top에 있음, POP 후 result=42 */
    emit_byte(fn, OP_POP); /* 예외 버리기 */
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c42);
    emit_byte(fn, OP_STORE_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_POP);

    patch_jump(fn, jmp_over);

    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_RETURN);

    disassemble_func(fn, stdout);

    vm->stack_top  = 0;
    vm->frame_count = 0;
    vm_run(vm, cl);

    CHECK(vm->value_stack[0].i == 42, "catch sets result == 42");
}

/* ── main ────────────────────────────────────────── */
int main(void) {
    printf("=== VM Tests ===\n\n");

    VM *vm = vm_new();

    test_arithmetic(vm);  printf("\n");
    test_branch(vm);      printf("\n");
    test_loop(vm);        printf("\n");
    test_call(vm);        printf("\n");
    test_closure(vm);     printf("\n");
    test_array(vm);       printf("\n");
    test_exception(vm);   printf("\n");

    vm_free(vm);

    printf("=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
