/*
 * vm_exec.c — 바이트코드 인터프리터 메인 루프
 *
 * Q1: 스택 프레임은 정확히 어떻게 생기는가?
 * Q2: 호출 규약은 누가 책임지는가?
 * Q4: 바이트코드 VM vs 네이티브 JIT의 차이는?
 * Q5: 예외/에러 전파는 어떻게 언와인드하는가?
 * Q6: 클로저 캡처는 어떻게 유지되는가?
 *
 * 모든 질문의 답이 이 파일에 구체적으로 구현되어 있다.
 */
#include "../include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── 전방 선언 ──────────────────────────────────────────── */
ObjUpval *vm_capture_upval(VM *vm, Value *slot);
void      vm_close_upvals(VM *vm, Value *slot);

/* ── 편의 매크로 ─────────────────────────────────────────
 * PUSH/POP/PEEK: 인라인 함수보다 빠른 매크로 버전
 * READ_BYTE/U16/I32: IP를 전진하면서 피연산자 읽기
 * ────────────────────────────────────────────────────── */
#define PUSH(v)    vm_push(vm, v)
#define POP()      vm_pop(vm)
#define PEEK(d)    vm_peek(vm, d)

#define READ_BYTE()  (*ip++)
#define READ_U16()   (ip += 2, (uint16_t)(ip[-2] | (ip[-1] << 8)))
#define READ_I32()   (ip += 4, (int32_t)((uint32_t)(ip[-4] | (ip[-3]<<8) | (ip[-2]<<16) | (ip[-1]<<24))))

/* 현재 프레임의 슬롯 */
#define SLOT(i)   (frame->slots[i])

/* 런타임 에러: 에러 메시지를 출력하고 VM_RUNTIME_ERROR 반환 */
#define RUNTIME_ERR(fmt, ...) do {                                    \
    fprintf(stderr, "RuntimeError: " fmt "\n", ##__VA_ARGS__);       \
    goto error;                                                       \
} while (0)

/* VM 초기화 / 해제 ────────────────────────────────────── */

VM *vm_new(void) {
    VM *vm = calloc(1, sizeof(VM));
    vm->next_gc_threshold = 1024 * 1024; /* 1MB */
    return vm;
}

void vm_free(VM *vm) {
    /* 남은 객체 전부 해제 */
    Obj *obj = vm->all_objects;
    while (obj) {
        Obj *next = obj->next;
        free(obj);  /* 간소화: gc의 sweep과 동일한 처리가 필요하나 여기선 단순화 */
        obj = next;
    }
    free(vm->global_names);
    free(vm->global_values);
    free(vm->gray_stack);
    free(vm->string_table);
    free(vm->ic_pool);
    free(vm);
}

/* 전역 변수 ────────────────────────────────────────────── */

static int global_lookup(VM *vm, const char *name) {
    for (int i = 0; i < vm->global_count; i++)
        if (strcmp(vm->global_names[i], name) == 0) return i;
    return -1;
}

static int global_define(VM *vm, const char *name) {
    int idx = global_lookup(vm, name);
    if (idx >= 0) return idx;
    if (vm->global_count >= vm->global_cap) {
        vm->global_cap = vm->global_cap ? vm->global_cap * 2 : 8;
        vm->global_names  = realloc(vm->global_names,  vm->global_cap * sizeof(char*));
        vm->global_values = realloc(vm->global_values, vm->global_cap * sizeof(Value));
    }
    vm->global_names[vm->global_count]  = strdup(name);
    vm->global_values[vm->global_count] = val_nil();
    return vm->global_count++;
}

/* 인라인 캐시 ──────────────────────────────────────────── */

static InlineCache *ic_get(VM *vm, int slot_idx) {
    if (slot_idx < 0 || slot_idx >= vm->ic_count) return NULL;
    return &vm->ic_pool[slot_idx];
}

/* Q7: 인라인 캐시 기반 필드 검색
 *
 * 히트: shape_id 일치 → field_index 직접 사용 (O(1))
 * 미스: 선형 탐색 → 결과 캐시 (다음엔 O(1))
 * 메가모픽: miss_count >= threshold → 항상 탐색
 */
static Value ic_get_field(VM *vm, ObjInstance *inst, uint16_t name_idx,
                           int ic_slot_idx, const ObjFunc *fn) {
    InlineCache *ic = ic_get(vm, ic_slot_idx);

    /* 히트 경로 */
    if (ic && ic->miss_count < IC_MEGAMORPHIC_THRESHOLD
           && ic->shape_id == inst->shape_id
           && ic->field_index < inst->field_count) {
        ic->hit_count++;
        return inst->fields[ic->field_index];
    }

    /* 미스 경로: 이름으로 탐색 */
    (void)fn; (void)name_idx; /* TODO: 상수 풀에서 이름 조회 */
    /* 단순화: 인덱스로 직접 접근 */
    if (ic) {
        ic->miss_count++;
        /* 캐시 갱신 (megamorphic이 아닌 경우만) */
        if (ic->miss_count < IC_MEGAMORPHIC_THRESHOLD) {
            ic->shape_id    = inst->shape_id;
            ic->field_index = 0; /* TODO: 실제 필드 인덱스 */
        }
    }
    return val_nil();
}

/* 산술 헬퍼 ────────────────────────────────────────────── */

static Value arith_add(Value a, Value b) {
    if (a.kind == VAL_INT   && b.kind == VAL_INT)   return val_int(a.i + b.i);
    if (a.kind == VAL_FLOAT && b.kind == VAL_FLOAT) return val_float(a.f + b.f);
    if (a.kind == VAL_INT   && b.kind == VAL_FLOAT) return val_float((double)a.i + b.f);
    if (a.kind == VAL_FLOAT && b.kind == VAL_INT)   return val_float(a.f + (double)b.i);
    return val_nil(); /* 타입 오류 */
}
static Value arith_sub(Value a, Value b) {
    if (a.kind == VAL_INT   && b.kind == VAL_INT)   return val_int(a.i - b.i);
    if (a.kind == VAL_FLOAT && b.kind == VAL_FLOAT) return val_float(a.f - b.f);
    if (a.kind == VAL_INT   && b.kind == VAL_FLOAT) return val_float((double)a.i - b.f);
    if (a.kind == VAL_FLOAT && b.kind == VAL_INT)   return val_float(a.f - (double)b.i);
    return val_nil();
}
static Value arith_mul(Value a, Value b) {
    if (a.kind == VAL_INT   && b.kind == VAL_INT)   return val_int(a.i * b.i);
    if (a.kind == VAL_FLOAT && b.kind == VAL_FLOAT) return val_float(a.f * b.f);
    if (a.kind == VAL_INT   && b.kind == VAL_FLOAT) return val_float((double)a.i * b.f);
    if (a.kind == VAL_FLOAT && b.kind == VAL_INT)   return val_float(a.f * (double)b.i);
    return val_nil();
}
static Value arith_div(Value a, Value b) {
    double fa = (a.kind == VAL_INT) ? (double)a.i : a.f;
    double fb = (b.kind == VAL_INT) ? (double)b.i : b.f;
    return val_float(fa / fb);
}

/* ── 메인 실행 루프 ──────────────────────────────────────
 *
 * Q4: 왜 switch/case인가?
 *   - 인터프리터 방식: 각 opcode를 C 코드로 에뮬레이션
 *   - GCC/Clang이 computed goto로 최적화 가능 (--enable-dispatch-table)
 *   - JIT 방식이면 이 switch 자체가 제거되고 기계어가 직접 실행됨
 * ────────────────────────────────────────────────────── */
VMResult vm_run(VM *vm, ObjClosure *entry) {
    if (vm->frame_count >= FRAME_MAX) return VM_STACK_OVERFLOW;

    /* 초기 프레임 설정
     * Q1: 첫 번째 스택 프레임 생성
     *   - slots = value_stack[0] (스택의 맨 처음)
     *   - ip    = entry->func->code (첫 번째 바이트코드)
     */
    CallFrame *frame = &vm->frame_stack[vm->frame_count++];
    frame->closure   = entry;
    frame->ip        = entry->func->code;
    frame->slots     = &vm->value_stack[vm->stack_top];
    frame->slot_base = vm->stack_top;

    /* 로컬 변수 공간 예약 */
    for (int i = 0; i < entry->func->max_locals; i++)
        PUSH(val_nil());

    /* ip를 로컬 변수로 캐시 (매번 frame->ip 접근보다 빠름) */
    register uint8_t *ip = frame->ip;

    /* ─ 디스패치 루프 ─────────────────────────────────── */
#define DISPATCH() goto dispatch
dispatch:;
    if (!ip || ip >= frame->closure->func->code + frame->closure->func->code_len)
        goto halt;

    uint8_t op = READ_BYTE();
    switch ((OpCode)op) {

    /* ── 상수 로드 ──────────────────────────────────── */
    case OP_LOAD_CONST: {
        uint16_t idx = READ_U16();
        PUSH(frame->closure->func->constants[idx]);
        DISPATCH();
    }
    case OP_LOAD_NIL:   PUSH(val_nil());        DISPATCH();
    case OP_LOAD_TRUE:  PUSH(val_bool(true));   DISPATCH();
    case OP_LOAD_FALSE: PUSH(val_bool(false));  DISPATCH();

    /* ── 로컬 변수 ──────────────────────────────────── */
    case OP_LOAD_LOCAL: {
        uint8_t idx = READ_BYTE();
        PUSH(SLOT(idx));
        DISPATCH();
    }
    case OP_STORE_LOCAL: {
        uint8_t idx = READ_BYTE();
        SLOT(idx) = PEEK(0); /* pop하지 않음: 대입식 자체가 값 */
        DISPATCH();
    }

    /* ── 전역 변수 ──────────────────────────────────── */
    case OP_LOAD_GLOBAL: {
        uint16_t idx = READ_U16();
        if (idx >= (uint16_t)vm->global_count) RUNTIME_ERR("global index out of range");
        PUSH(vm->global_values[idx]);
        DISPATCH();
    }
    case OP_STORE_GLOBAL: {
        uint16_t idx = READ_U16();
        if (idx >= (uint16_t)vm->global_count) RUNTIME_ERR("global index out of range");
        vm->global_values[idx] = PEEK(0);
        DISPATCH();
    }

    /* ── Upvalue (클로저 캡처) ──────────────────────────
     * Q6: location 포인터를 통해 읽기/쓰기
     * open: 스택 슬롯 직접 접근
     * closed: upval 내부 복사본 접근
     */
    case OP_LOAD_UPVAL: {
        uint8_t idx = READ_BYTE();
        PUSH(*frame->closure->upvalues[idx]->location);
        DISPATCH();
    }
    case OP_STORE_UPVAL: {
        uint8_t idx = READ_BYTE();
        *frame->closure->upvalues[idx]->location = PEEK(0);
        DISPATCH();
    }
    case OP_CLOSE_UPVAL:
        /* 현재 스택 top 슬롯을 close → closed 상태로 전환 */
        vm_close_upvals(vm, &vm->value_stack[vm->stack_top - 1]);
        POP();
        DISPATCH();

    /* ── 스택 조작 ──────────────────────────────────── */
    case OP_POP:  POP();                                DISPATCH();
    case OP_DUP:  PUSH(PEEK(0));                        DISPATCH();
    case OP_SWAP: {
        Value t = PEEK(0);
        vm->value_stack[vm->stack_top - 1] = PEEK(1);
        vm->value_stack[vm->stack_top - 2] = t;
        DISPATCH();
    }

    /* ── 산술 ───────────────────────────────────────── */
    case OP_ADD: { Value b=POP(), a=POP(); PUSH(arith_add(a,b)); DISPATCH(); }
    case OP_SUB: { Value b=POP(), a=POP(); PUSH(arith_sub(a,b)); DISPATCH(); }
    case OP_MUL: { Value b=POP(), a=POP(); PUSH(arith_mul(a,b)); DISPATCH(); }
    case OP_DIV: { Value b=POP(), a=POP(); PUSH(arith_div(a,b)); DISPATCH(); }
    case OP_MOD: {
        Value b=POP(), a=POP();
        if (a.kind==VAL_INT && b.kind==VAL_INT && b.i!=0)
            PUSH(val_int(a.i % b.i));
        else RUNTIME_ERR("mod requires integers");
        DISPATCH();
    }
    case OP_NEG: {
        Value a = POP();
        if (a.kind == VAL_INT)   PUSH(val_int(-a.i));
        else if (a.kind == VAL_FLOAT) PUSH(val_float(-a.f));
        else RUNTIME_ERR("neg requires number");
        DISPATCH();
    }
    case OP_POW: {
        Value b=POP(), a=POP();
        double fa = (a.kind==VAL_INT) ? (double)a.i : a.f;
        double fb = (b.kind==VAL_INT) ? (double)b.i : b.f;
        PUSH(val_float(pow(fa, fb)));
        DISPATCH();
    }

    /* ── 비교 ───────────────────────────────────────── */
#define CMP_OP(op) do { \
    Value b=POP(), a=POP(); \
    bool res; \
    if (a.kind==VAL_INT && b.kind==VAL_INT)     res = a.i op b.i; \
    else if (a.kind==VAL_FLOAT&&b.kind==VAL_FLOAT) res = a.f op b.f; \
    else if (a.kind==VAL_INT&&b.kind==VAL_FLOAT)   res = (double)a.i op b.f; \
    else if (a.kind==VAL_FLOAT&&b.kind==VAL_INT)   res = a.f op (double)b.i; \
    else res = false; \
    PUSH(val_bool(res)); \
} while(0)

    case OP_EQ: { Value b=POP(),a=POP(); PUSH(val_bool(val_equal(a,b))); DISPATCH(); }
    case OP_NE: { Value b=POP(),a=POP(); PUSH(val_bool(!val_equal(a,b))); DISPATCH(); }
    case OP_LT: CMP_OP(<);  DISPATCH();
    case OP_LE: CMP_OP(<=); DISPATCH();
    case OP_GT: CMP_OP(>);  DISPATCH();
    case OP_GE: CMP_OP(>=); DISPATCH();

    /* ── 논리 ───────────────────────────────────────── */
    case OP_NOT: { Value a=POP(); PUSH(val_bool(!val_truthy(a))); DISPATCH(); }
    case OP_AND: {
        /* short-circuit: falsy면 top 유지 (JUMP가 처리) */
        DISPATCH();
    }
    case OP_OR: { DISPATCH(); }

    /* ── 분기 ───────────────────────────────────────── */
    case OP_JUMP: {
        int32_t off = READ_I32();
        ip += off;
        DISPATCH();
    }
    case OP_JUMP_IF_FALSE: {
        int32_t off = READ_I32();
        if (!val_truthy(PEEK(0))) ip += off;
        DISPATCH();
    }
    case OP_JUMP_IF_TRUE: {
        int32_t off = READ_I32();
        if (val_truthy(PEEK(0))) ip += off;
        DISPATCH();
    }
    case OP_POP_JUMP_IF_FALSE: {
        int32_t off = READ_I32();
        if (!val_truthy(POP())) ip += off;
        DISPATCH();
    }
    case OP_POP_JUMP_IF_TRUE: {
        int32_t off = READ_I32();
        if (val_truthy(POP())) ip += off;
        DISPATCH();
    }

    /* ── 함수 호출 ──────────────────────────────────────
     * Q1: 새 스택 프레임 생성
     * Q2: VM이 호출 규약을 책임짐
     *
     * 호출 전 스택:      [...] [closure] [arg0] [arg1] ... [argN-1]
     * 호출 후 frame.slots: 위 배열에서 closure 바로 뒤 = [arg0] ...
     *   → slots[0]=arg0, slots[1]=arg1, ...이 자연스럽게 맞아떨어짐
     */
    case OP_CALL: {
        uint8_t  nargs   = READ_BYTE();
        uint16_t ic_slot = READ_U16();
        (void)ic_slot;

        Value callee = PEEK(nargs); /* 인수 아래에 있는 callable */

        /* ── OBJ_NATIVE 고속 경로 ────────────────────────────
         * V가 못 하는 것: 런타임에 어떤 함수든 Value로 호출.
         * 네이티브 함수는 새 CallFrame 없이 직접 실행.
         * → FreeLang의 fetch/println/json_parse가 여기서 실행됨.
         */
        if (val_is_obj_kind(callee, OBJ_NATIVE)) {
            ObjNative *nat = AS_NATIVE(callee);
            if (nat->arity >= 0 && nargs != (uint8_t)nat->arity)
                RUNTIME_ERR("native '%s': expected %d args got %d",
                            nat->name, nat->arity, nargs);
            Value *argv = &vm->value_stack[vm->stack_top - nargs];
            Value  result = nat->fn(vm, nargs, argv);
            /* 인수 + callee 팝 후 결과 push */
            vm->stack_top -= (nargs + 1);
            PUSH(result);
            DISPATCH();
        }

        if (!val_is_obj_kind(callee, OBJ_CLOSURE))
            RUNTIME_ERR("can only call closures or natives");

        ObjClosure *cl = AS_CLOSURE(callee);

        if (nargs != (uint8_t)cl->func->arity)
            RUNTIME_ERR("arity mismatch: expected %d got %d",
                        cl->func->arity, nargs);

        if (vm->frame_count >= FRAME_MAX) RUNTIME_ERR("stack overflow");

        /* 현재 IP 저장 */
        frame->ip = ip;

        /* 새 프레임 생성
         * Q1: slots = 스택에서 callee 다음 위치 (= 첫 번째 인수)
         */
        CallFrame *new_frame = &vm->frame_stack[vm->frame_count++];
        new_frame->closure   = cl;
        new_frame->ip        = cl->func->code;
        new_frame->slots     = &vm->value_stack[vm->stack_top - nargs];
        new_frame->slot_base = (int)(new_frame->slots - vm->value_stack);

        frame = new_frame;
        ip    = frame->ip;

        /* 로컬 변수 슬롯 예약 (인수 제외한 나머지) */
        int extra = cl->func->max_locals - nargs;
        for (int i = 0; i < extra; i++) PUSH(val_nil());

        DISPATCH();
    }

    /* ── 반환 ───────────────────────────────────────────
     * Q1: 프레임 팝 + 스택 복원
     * Q6: 반환 전에 이 프레임의 upvalue 닫기
     */
    case OP_RETURN: {
        Value ret = POP();

        /* Q6: 이 프레임의 모든 로컬을 가리키는 open upvalue close */
        vm_close_upvals(vm, frame->slots);

        vm->frame_count--;

        if (vm->frame_count == 0) {
            /* 최상위 함수 반환 → 스택 초기화 후 반환값 [0]에 저장 */
            vm->stack_top = 0;
            PUSH(ret);
            goto halt;
        }

        /* 스택을 callee 이전으로 복원
         * (callee 포인터 자리도 포함해서 팝)
         */
        vm->stack_top = (int)(frame->slots - vm->value_stack) - 1;
        PUSH(ret);

        /* 이전 프레임 복원 */
        frame = &vm->frame_stack[vm->frame_count - 1];
        ip    = frame->ip;
        DISPATCH();
    }

    case OP_RETURN_NIL: {
        vm_close_upvals(vm, frame->slots);
        vm->frame_count--;
        if (vm->frame_count == 0) {
            vm->stack_top = 0;
            PUSH(val_nil());
            goto halt;
        }
        vm->stack_top = (int)(frame->slots - vm->value_stack) - 1;
        PUSH(val_nil());
        frame = &vm->frame_stack[vm->frame_count - 1];
        ip    = frame->ip;
        DISPATCH();
    }

    /* ── 클로저 생성 ─────────────────────────────────────
     * Q6: MAKE_CLOSURE 시 upvalue 캡처
     *
     * 인코딩: [func_idx:u16] [n_upvals:u8]
     *          [is_local:u8 idx:u8] × n_upvals
     *
     * is_local=1: 현재 프레임의 슬롯을 캡처 (새 ObjUpval 또는 재사용)
     * is_local=0: 현재 클로저의 upvalue를 전달 (중첩 클로저)
     */
    case OP_MAKE_CLOSURE: {
        uint16_t func_idx = READ_U16();
        uint8_t  n_upvals = READ_BYTE();

        Value    fval = frame->closure->func->constants[func_idx];
        if (!val_is_obj_kind(fval, OBJ_FUNC))
            RUNTIME_ERR("MAKE_CLOSURE: constant is not ObjFunc");

        ObjFunc    *fn = AS_FUNC(fval);
        ObjClosure *cl = vm_alloc_closure(vm, fn);

        for (int i = 0; i < n_upvals; i++) {
            uint8_t is_local = READ_BYTE();
            uint8_t idx      = READ_BYTE();
            if (is_local) {
                /* 현재 프레임 슬롯 캡처 */
                cl->upvalues[i] = vm_capture_upval(vm, &frame->slots[idx]);
            } else {
                /* 부모 클로저의 upvalue 전달 */
                cl->upvalues[i] = frame->closure->upvalues[idx];
            }
        }

        PUSH(val_obj((Obj*)cl));
        DISPATCH();
    }

    /* ── 예외 처리 ──────────────────────────────────────
     * Q5: TryFrame 기반 스택 언와인딩
     */
    case OP_TRY_BEGIN: {
        int32_t catch_off   = READ_I32();
        int32_t finally_off = READ_I32();

        if (vm->try_top >= TRY_MAX) RUNTIME_ERR("try stack overflow");

        TryFrame *tf  = &vm->try_stack[vm->try_top++];
        tf->frame_depth = vm->frame_count;
        tf->stack_depth = vm->stack_top;
        tf->catch_ip    = catch_off  ? ip + catch_off  : NULL;
        tf->finally_ip  = finally_off ? ip + finally_off : NULL;
        tf->closure     = frame->closure;
        DISPATCH();
    }

    case OP_TRY_END:
        if (vm->try_top > 0) vm->try_top--;
        DISPATCH();

    case OP_THROW: {
        Value exc = POP();

        if (vm->try_top == 0) {
            fprintf(stderr, "Uncaught exception: ");
            val_print(exc, stderr);
            fprintf(stderr, "\n");
            goto error;
        }

        /* Q5: TryFrame을 찾아 스택 언와인딩
         * 중간 CallFrame들을 팝하면서 각각 upvalue를 닫음
         */
        TryFrame *tf = &vm->try_stack[--vm->try_top];

        /* 중간 프레임 언와인드 */
        while (vm->frame_count > tf->frame_depth) {
            CallFrame *f = &vm->frame_stack[vm->frame_count - 1];
            vm_close_upvals(vm, f->slots);
            vm->frame_count--;
        }

        /* 스택 복원 */
        vm->stack_top = tf->stack_depth;

        /* 예외 값 push */
        PUSH(exc);

        /* 핸들러로 점프 */
        frame = &vm->frame_stack[vm->frame_count - 1];
        ip    = tf->catch_ip ? tf->catch_ip
                             : tf->finally_ip;
        if (!ip) goto error;
        DISPATCH();
    }

    case OP_RETHROW: {
        Value exc = POP();
        /* 같은 위치에서 다시 THROW처럼 처리 */
        PUSH(exc);
        /* 간소화: OP_THROW와 동일 로직 재사용을 위해 점프 */
        goto do_throw;
    }

    /* ── 배열 ───────────────────────────────────────── */
    case OP_MAKE_ARRAY: {
        uint16_t n = READ_U16();
        ObjArray *arr = vm_alloc_array(vm, n);
        arr->len = n;
        arr->data = malloc(n * sizeof(Value));
        /* 스택에서 n개 꺼내 배열에 담기 (순서 역전) */
        for (int i = n - 1; i >= 0; i--)
            arr->data[i] = POP();
        PUSH(val_obj((Obj*)arr));
        DISPATCH();
    }
    case OP_ARRAY_GET: {
        Value idx_v = POP();
        Value arr_v = POP();
        if (!val_is_obj_kind(arr_v, OBJ_ARRAY)) RUNTIME_ERR("not an array");
        ObjArray *arr = AS_ARRAY(arr_v);
        int64_t idx = idx_v.kind == VAL_INT ? idx_v.i : (int64_t)idx_v.f;
        if (idx < 0 || idx >= arr->len) RUNTIME_ERR("array index out of bounds");
        PUSH(arr->data[idx]);
        DISPATCH();
    }
    case OP_ARRAY_SET: {
        Value val   = POP();
        Value idx_v = POP();
        Value arr_v = POP();
        if (!val_is_obj_kind(arr_v, OBJ_ARRAY)) RUNTIME_ERR("not an array");
        ObjArray *arr = AS_ARRAY(arr_v);
        int64_t idx = idx_v.kind == VAL_INT ? idx_v.i : (int64_t)idx_v.f;
        if (idx < 0 || idx >= arr->len) RUNTIME_ERR("array index out of bounds");
        arr->data[idx] = val;
        PUSH(val);
        DISPATCH();
    }
    case OP_ARRAY_LEN: {
        Value arr_v = POP();
        if (!val_is_obj_kind(arr_v, OBJ_ARRAY)) RUNTIME_ERR("not an array");
        PUSH(val_int(AS_ARRAY(arr_v)->len));
        DISPATCH();
    }

    /* ── 인스턴스 필드 ──────────────────────────────────
     * Q7: 인라인 캐시를 통한 O(1) 필드 접근
     */
    case OP_GET_FIELD: {
        uint16_t name_idx = READ_U16();
        uint16_t ic_slot  = READ_U16();
        Value obj_v = POP();
        if (!val_is_obj_kind(obj_v, OBJ_INSTANCE)) RUNTIME_ERR("not an instance");
        Value result = ic_get_field(vm, AS_INSTANCE(obj_v),
                                    name_idx, ic_slot, frame->closure->func);
        PUSH(result);
        DISPATCH();
    }
    case OP_SET_FIELD: {
        uint16_t name_idx = READ_U16();
        uint16_t ic_slot  = READ_U16();
        (void)name_idx; (void)ic_slot;
        Value val   = POP();
        Value obj_v = POP();
        if (!val_is_obj_kind(obj_v, OBJ_INSTANCE)) RUNTIME_ERR("not an instance");
        /* TODO: 이름으로 필드 인덱스 찾아서 저장 */
        (void)val;
        DISPATCH();
    }

    /* ── 메서드 호출 ─────────────────────────────────── */
    case OP_CALL_METHOD: {
        uint16_t name_idx = READ_U16();
        uint8_t  nargs    = READ_BYTE();
        uint16_t ic_slot  = READ_U16();
        (void)name_idx; (void)ic_slot;
        /* 간소화: 일반 CALL과 동일하게 처리 */
        Value callee = PEEK(nargs);
        if (!val_is_obj_kind(callee, OBJ_CLOSURE)) RUNTIME_ERR("not callable");
        /* CALL과 동일 로직 (실제 구현에서는 receiver 처리 추가) */
        ObjClosure *cl = AS_CLOSURE(callee);
        frame->ip = ip;
        CallFrame *nf = &vm->frame_stack[vm->frame_count++];
        nf->closure  = cl;
        nf->ip       = cl->func->code;
        nf->slots    = &vm->value_stack[vm->stack_top - nargs];
        nf->slot_base = (int)(nf->slots - vm->value_stack);
        frame = nf;
        ip    = frame->ip;
        int extra = cl->func->max_locals - nargs;
        for (int i = 0; i < extra; i++) PUSH(val_nil());
        DISPATCH();
    }

    /* ── 기타 ───────────────────────────────────────── */
    case OP_PRINT:
        val_print(POP(), stdout);
        printf("\n");
        DISPATCH();

    case OP_HALT:
        goto halt;

    default:
        RUNTIME_ERR("unknown opcode %d", op);
    }

do_throw:;
    /* RETHROW 처리 */
    {
        Value exc = POP();
        if (vm->try_top == 0) {
            fprintf(stderr, "Uncaught exception: ");
            val_print(exc, stderr);
            fprintf(stderr, "\n");
            goto error;
        }
        TryFrame *tf = &vm->try_stack[--vm->try_top];
        while (vm->frame_count > tf->frame_depth) {
            CallFrame *f = &vm->frame_stack[vm->frame_count - 1];
            vm_close_upvals(vm, f->slots);
            vm->frame_count--;
        }
        vm->stack_top = tf->stack_depth;
        PUSH(exc);
        frame = &vm->frame_stack[vm->frame_count - 1];
        ip    = tf->catch_ip ? tf->catch_ip : tf->finally_ip;
        if (!ip) goto error;
        goto dispatch;
    }

error:
    return VM_RUNTIME_ERROR;

halt:
    return VM_OK;
}
