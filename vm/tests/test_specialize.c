/*
 * test_specialize.c — Specializing Adaptive Interpreter 검증
 *
 * "코드가 실행될수록 코드 자체가 빠르게 변한다"
 *
 * 테스트 구조:
 *   1. fib(n) 바이트코드를 직접 작성
 *   2. 첫 번째 실행: 모든 opcode가 generic (OP_ADD, OP_LE, OP_SUB 등)
 *   3. SPEC_THRESHOLD번 실행 후: fast opcode로 교체됨
 *   4. 이후 실행: spec_hits가 증가 (fast path 실행 확인)
 *   5. fib(28)을 여러 번 실행해서 warm path 성능 측정
 *
 * 정적 컴파일러와의 차이:
 *   V/C로 컴파일된 fib는 컴파일 후 코드가 변하지 않는다.
 *   우리 VM은 실행하면서 스스로 최적화된다.
 */
#include "../include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

/* ── 타이머 ─────────────────────────────────────────────── */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ── fib(n) 바이트코드 생성 ─────────────────────────────── */
/*
 * FreeLang 코드로 쓰면:
 *   fn fib(n) {
 *     if n <= 1 { return n }
 *     return fib(n-1) + fib(n-2)
 *   }
 *
 * 바이트코드:
 *   LOAD_LOCAL 0         ; n
 *   LOAD_CONST 0         ; 1
 *   LE                   ; n <= 1
 *   POP_JUMP_IF_FALSE →L1
 *   LOAD_LOCAL 0         ; n (base case)
 *   RETURN
 * L1:
 *   LOAD_GLOBAL fib_idx  ; fib 함수
 *   LOAD_LOCAL 0         ; n
 *   LOAD_CONST 0         ; 1
 *   SUB                  ; n-1
 *   CALL 1 0             ; fib(n-1)
 *   LOAD_GLOBAL fib_idx  ; fib 함수
 *   LOAD_LOCAL 0         ; n
 *   LOAD_CONST 1         ; 2
 *   SUB                  ; n-2
 *   CALL 1 0             ; fib(n-2)
 *   ADD                  ; fib(n-1) + fib(n-2)
 *   RETURN
 */
static ObjClosure *build_fib(VM *vm, int fib_global_idx) {
    ObjFunc *fn = vm_alloc_func(vm, "fib");
    fn->arity      = 1;
    fn->max_locals = 1;

    /* 상수 풀: [0] = int(1), [1] = int(2) */
    int c1 = add_constant(fn, vm, val_int(1));
    int c2 = add_constant(fn, vm, val_int(2));

    /* LOAD_LOCAL 0 */
    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 0);
    /* LOAD_CONST 1 */
    emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, c1);
    /* LE */
    emit_byte(fn, OP_LE);
    /* POP_JUMP_IF_FALSE → L1 */
    int jmp_pos = emit_jump(fn, OP_POP_JUMP_IF_FALSE);
    /* base case: LOAD_LOCAL 0, RETURN */
    emit_byte(fn, OP_LOAD_LOCAL); emit_byte(fn, 0);
    emit_byte(fn, OP_RETURN);
    /* L1: */
    patch_jump(fn, jmp_pos);

    /* fib(n-1) */
    emit_byte(fn, OP_LOAD_GLOBAL); emit_u16(fn, fib_global_idx);
    emit_byte(fn, OP_LOAD_LOCAL);  emit_byte(fn, 0);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c1);
    emit_byte(fn, OP_SUB);
    emit_byte(fn, OP_CALL); emit_byte(fn, 1); emit_u16(fn, 0);

    /* fib(n-2) */
    emit_byte(fn, OP_LOAD_GLOBAL); emit_u16(fn, fib_global_idx);
    emit_byte(fn, OP_LOAD_LOCAL);  emit_byte(fn, 0);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, c2);
    emit_byte(fn, OP_SUB);
    emit_byte(fn, OP_CALL); emit_byte(fn, 1); emit_u16(fn, 0);

    /* ADD, RETURN */
    emit_byte(fn, OP_ADD);
    emit_byte(fn, OP_RETURN);

    return vm_alloc_closure(vm, fn);
}

/* fib(n)을 호출하는 main 클로저 */
static ObjClosure *build_main(VM *vm, int fib_global_idx, int n) {
    ObjFunc *fn = vm_alloc_func(vm, "main");
    fn->arity      = 0;
    fn->max_locals = 0;

    int cn = add_constant(fn, vm, val_int(n));
    emit_byte(fn, OP_LOAD_GLOBAL); emit_u16(fn, fib_global_idx);
    emit_byte(fn, OP_LOAD_CONST);  emit_u16(fn, cn);
    emit_byte(fn, OP_CALL); emit_byte(fn, 1); emit_u16(fn, 0);
    emit_byte(fn, OP_RETURN);

    return vm_alloc_closure(vm, fn);
}

/* ── 바이트코드 내 특수화 opcode 개수 세기 ─────────────── */
static int count_specialized(ObjFunc *fn) {
    int count = 0;
    for (int i = 0; i < fn->code_len; i++) {
        uint8_t op = fn->code[i];
        if (op == OP_ADD_INT || op == OP_ADD_FLOAT ||
            op == OP_SUB_INT || op == OP_MUL_INT   ||
            op == OP_LT_INT  || op == OP_LE_INT    ||
            op == OP_GT_INT  || op == OP_GE_INT    ||
            op == OP_EQ_INT)
            count++;
    }
    return count;
}

/* ── 메인 테스트 ────────────────────────────────────────── */
int main(void) {
    printf("=== Specializing Adaptive Interpreter (SAI) Test ===\n\n");

    /* ── 설정 ──────────────────────────────────────────── */
    VM *vm = vm_new();

    /* fib 전역 변수 슬롯 예약 (이름만) */
    int fib_idx = -1;
    for (int i = 0; i < vm->global_count; i++)
        if (strcmp(vm->global_names[i], "fib") == 0) { fib_idx = i; break; }
    if (fib_idx < 0) {
        /* 수동 등록 */
        vm->global_cap = 8;
        vm->global_names  = realloc(vm->global_names,  vm->global_cap * sizeof(char*));
        vm->global_values = realloc(vm->global_values, vm->global_cap * sizeof(Value));
        fib_idx = vm->global_count++;
        vm->global_names[fib_idx]  = strdup("fib");
        vm->global_values[fib_idx] = val_nil();
    }

    /* fib 클로저 빌드 후 전역에 저장 */
    ObjClosure *fib_cl = build_fib(vm, fib_idx);
    vm->global_values[fib_idx] = val_obj((Obj*)fib_cl);

    ObjFunc *fib_fn = fib_cl->func;

    /* ── T1: Cold 실행 (특수화 전) ─────────────────────── */
    printf("T1: Cold 실행 — generic opcodes\n");
    {
        int before = count_specialized(fib_fn);
        ObjClosure *m = build_main(vm, fib_idx, 10);
        vm->stack_top = 0; vm->frame_count = 0;
        VMResult r = vm_run(vm, m);
        int64_t result = vm->value_stack[0].i;
        printf("  fib(10) = %lld  (expected 55)  %s\n",
               (long long)result, result == 55 ? "PASS" : "FAIL");
        printf("  specialized opcodes before: %d\n", before);
        printf("  spec_hits after cold run: %llu\n",
               (unsigned long long)vm->stats.spec_hits);
    }

    /* ── T2: Warm-up 실행 — 특수화 유도 ───────────────── */
    printf("\nT2: Warm-up — 특수화 유도 (%d번 실행)\n", SPEC_THRESHOLD + 2);
    {
        /* SPEC_THRESHOLD번 이상 실행 → 특수화 발동 */
        for (int i = 0; i < SPEC_THRESHOLD + 2; i++) {
            ObjClosure *m = build_main(vm, fib_idx, 8);
            vm->stack_top = 0; vm->frame_count = 0;
            vm_run(vm, m);
        }
        int after = count_specialized(fib_fn);
        printf("  specialized opcodes after warmup: %d\n", after);
        printf("  specializations total: %llu\n",
               (unsigned long long)vm->stats.specializations);
        printf("  spec_hits so far: %llu\n",
               (unsigned long long)vm->stats.spec_hits);
        if (after > 0) printf("  -> PASS: 바이트코드가 스스로 변했다!\n");
        else           printf("  -> FAIL: 특수화가 발동되지 않았다\n");
    }

    /* ── T3: Hot 실행 성능 비교 ─────────────────────────── */
    printf("\nT3: 성능 비교 — fib(25)\n");
    {
        int reps = 5;

        /* (a) 첫 VM (이미 특수화됨) */
        double t0 = now_ms();
        int64_t result = 0;
        for (int i = 0; i < reps; i++) {
            ObjClosure *m = build_main(vm, fib_idx, 25);
            vm->stack_top = 0; vm->frame_count = 0;
            vm_run(vm, m);
            result = vm->value_stack[0].i;
        }
        double t_warm = (now_ms() - t0) / reps;

        /* (b) 새 VM (cold, 특수화 없음) */
        VM *cold_vm = vm_new();
        {
            cold_vm->global_cap = 8;
            cold_vm->global_names  = realloc(cold_vm->global_names,  8 * sizeof(char*));
            cold_vm->global_values = realloc(cold_vm->global_values, 8 * sizeof(Value));
            int cidx = cold_vm->global_count++;
            cold_vm->global_names[cidx]  = strdup("fib");
            cold_vm->global_values[cidx] = val_nil();
            ObjClosure *cf = build_fib(cold_vm, cidx);
            cold_vm->global_values[cidx] = val_obj((Obj*)cf);
        }
        int cold_fib_idx = 0;

        double t1 = now_ms();
        for (int i = 0; i < reps; i++) {
            ObjClosure *m = build_main(cold_vm, cold_fib_idx, 25);
            cold_vm->stack_top = 0; cold_vm->frame_count = 0;
            vm_run(cold_vm, m);
        }
        double t_cold = (now_ms() - t1) / reps;
        vm_free(cold_vm);

        printf("  fib(25) = %lld\n", (long long)result);
        printf("  cold VM avg: %.2f ms\n", t_cold);
        printf("  warm VM avg: %.2f ms\n", t_warm);
        if (t_cold > 0 && t_warm > 0) {
            double ratio = t_cold / t_warm;
            printf("  speedup: %.2fx %s\n", ratio,
                   ratio > 1.0 ? "(warm faster!)" : "(similar)");
        }
    }

    /* ── T4: Guard 실패 — despecialization ─────────────── */
    printf("\nT4: Guard 실패 (despecialization)\n");
    {
        uint64_t before_despec = vm->stats.despecializations;

        /* float 인수로 호출 → guard 실패 → generic으로 복원 */
        ObjFunc *fn = vm_alloc_func(vm, "mixed_add");
        fn->arity = 0; fn->max_locals = 0;
        int cf = add_constant(fn, vm, val_float(3.14));
        int ci = add_constant(fn, vm, val_int(2));
        /* OP_ADD_INT가 이미 있는 위치에서 float을 넣어야 guard 실패.
         * 여기선 새 함수에 float+int ADD를 16번 실행해서 ADD_FLOAT 유도,
         * 그다음 int+int로 실행해서 despecialize 유도. */
        /* 간단화: ADD를 직접 OP_ADD_INT로 패치한 뒤 float을 넣음 */
        emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, cf);  /* float */
        emit_byte(fn, OP_LOAD_CONST); emit_u16(fn, ci);  /* int */
        int add_pos = fn->code_len;
        emit_byte(fn, OP_ADD_INT);  /* 수동으로 이미 특수화된 상태 가정 */
        emit_byte(fn, OP_RETURN);

        /* spec_counters는 NULL이므로 despecialize가 guard 실패 시 복원 */
        ObjClosure *cl = vm_alloc_closure(vm, fn);
        vm->stack_top = 0; vm->frame_count = 0;
        vm_run(vm, cl);  /* float+int → guard 실패 → OP_ADD로 복원 */

        uint64_t after_despec = vm->stats.despecializations;
        printf("  despecializations: %llu → %llu\n",
               (unsigned long long)before_despec,
               (unsigned long long)after_despec);

        /* 복원된 후 code[add_pos]가 OP_ADD인지 확인 */
        if (fn->code[add_pos] == OP_ADD)
            printf("  code[%d] = OP_ADD (복원됨)  PASS\n", add_pos);
        else
            printf("  code[%d] = %d (복원 안 됨)  FAIL\n", add_pos, fn->code[add_pos]);
    }

    /* ── 최종 통계 ──────────────────────────────────────── */
    printf("\n");
    vm_print_stats(vm);

    vm_free(vm);
    printf("\n=== 완료 ===\n");
    return 0;
}
