/*
 * vm.h — VM 전체 상태 및 공개 API
 *
 * 이 파일 하나를 읽으면 "VM이 어떻게 생겼는가"를 전부 알 수 있다.
 * 각 구조체가 어떤 질문에 답하는지 주석으로 표기했다.
 */
#pragma once
#include "vm_value.h"
#include "vm_bytecode.h"
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Q7: inline cache / dispatch 최적화는 어떻게 되는가?
 *
 * Monomorphic Inline Cache (MIC):
 *   - 첫 CALL_METHOD/GET_FIELD 실행 시: 선형 탐색 → cache에 저장
 *   - 이후 실행: shape_id 비교 (1 cmpeq) → O(1) dispatch
 *   - shape_id 불일치 시: miss_count 증가, 재탐색, 재캐시
 *   - miss가 너무 많으면 (megamorphic): cache 포기, 항상 탐색
 *
 * Polymorphic IC (PIC):
 *   - 최대 4가지 shape을 기억
 *   - 선형 탐색이지만 4개로 제한 → 실질적으로 O(1)
 * ================================================================ */
typedef struct InlineCache {
    uint32_t  shape_id;        /* 마지막으로 히트한 클래스 shape */
    int       field_index;     /* 필드 오프셋 인덱스 */
    Value     cached_method;   /* 캐시된 메서드 클로저 */
    uint32_t  hit_count;
    uint32_t  miss_count;
} InlineCache;

#define IC_MEGAMORPHIC_THRESHOLD 16

typedef struct {
    uint32_t shape_ids[4];
    int      field_indices[4];
    Value    methods[4];
    int      count;
} PolyIC;

/* ================================================================
 * Q1: 스택 프레임은 정확히 어떻게 생기는가?
 *
 * 스택에는 두 가지 병렬 구조가 있다:
 *
 *   value_stack[]: 모든 피연산자 + 로컬 변수가 섞여 있음
 *   frame_stack[]: 각 호출의 "문맥 정보"만 담음
 *
 * 호출 전:
 *   value_stack: [...] [arg0] [arg1] [arg2]  ← 3개 인수
 *
 * CALL nargs=3 후:
 *   frame_stack에 새 CallFrame 추가
 *   CallFrame.slots = &value_stack[현재_top - 3]
 *   CallFrame.ip    = new_func->code
 *
 * 프레임 내부에서:
 *   slots[0] = arg0  (OP_LOAD_LOCAL 0)
 *   slots[1] = arg1  (OP_LOAD_LOCAL 1)
 *   slots[2] = arg2  (OP_LOAD_LOCAL 2)
 *   slots[3..] = 로컬 변수들 (컴파일러가 max_locals 계산)
 *
 * RETURN 후:
 *   frame_stack.pop()
 *   value_stack을 frame 시작 이전으로 복원
 *   반환값을 stack top에 push
 * ================================================================ */
#define FRAME_MAX   256
#define STACK_MAX   (FRAME_MAX * 256)

typedef struct {
    ObjClosure *closure;     /* 실행 중인 클로저 (ObjFunc에 접근) */
    uint8_t    *ip;          /* Instruction Pointer */
    Value      *slots;       /* value_stack 내의 이 프레임 슬롯 시작 */
    int         slot_base;   /* 절대 인덱스 (GC가 루트 스캔 시 사용) */
} CallFrame;

/* ================================================================
 * Q5: 예외/에러 전파는 스택을 어떻게 언와인드하는가?
 *
 * TRY_BEGIN 실행 시: 현재 상태를 스냅샷으로 저장
 *   - 얼마나 깊이 프레임이 쌓여 있는지 (frame_depth)
 *   - value_stack이 어디까지 차 있는지 (stack_depth)
 *   - catch/finally 핸들러의 주소 (catch_ip, finally_ip)
 *
 * THROW 실행 시:
 *   1. try_stack을 위에서 아래로 탐색
 *   2. 첫 번째 TryFrame 발견:
 *      a. frame_depth까지 CallFrame 팝 (각 팝 시 upvalue close)
 *      b. stack_depth까지 value_stack 복원
 *      c. 예외 값을 stack top에 push
 *      d. ip → catch_ip (또는 finally_ip)
 *   3. TryFrame이 없으면: 프로세스 종료
 * ================================================================ */
#define TRY_MAX 64

typedef struct {
    int         frame_depth;   /* THROW 시 복원할 CallFrame 수 */
    int         stack_depth;   /* THROW 시 복원할 value_stack 위치 */
    uint8_t    *catch_ip;      /* catch 블록 시작 주소 (NULL이면 없음) */
    uint8_t    *finally_ip;    /* finally 블록 시작 주소 */
    ObjClosure *closure;       /* catch_ip가 속한 클로저 */
} TryFrame;

/* ================================================================
 * Q3: GC / 메모리 소유권 모델은 어떻게 작동하는가?
 *
 * Mark-and-Sweep (두 단계):
 *
 * MARK:
 *   루트에서 도달 가능한 모든 객체에 marked=true 설정
 *   루트 = value_stack + frame closures + globals + open_upvalues
 *   회색(gray) 워크리스트: 마킹은 했지만 자식을 아직 탐색 안 한 것들
 *   → gray_stack에서 꺼내 자식 마킹 반복 (= tri-color marking)
 *
 * SWEEP:
 *   all_objects 연결리스트 순회
 *   marked=false인 것은 free() → 리스트에서 제거
 *   살아있는 것은 marked=false로 리셋 (다음 GC 준비)
 *
 * GC 발동 시점: bytes_allocated > next_gc_threshold (기본 1MB)
 * GC 후 threshold: bytes_allocated * 2 (adaptive)
 * ================================================================ */

typedef struct VM {
    /* ── 실행 상태 ─────────────────────────────────────── */
    Value      value_stack[STACK_MAX];
    int        stack_top;              /* 다음 push 위치 (= 현재 크기) */

    CallFrame  frame_stack[FRAME_MAX];
    int        frame_count;

    TryFrame   try_stack[TRY_MAX];
    int        try_top;

    /* ── 전역 변수 ─────────────────────────────────────── */
    /*
     * Q2: 호출 규약은 누가 책임지는가?
     *
     * VM이 전부 책임진다. 네이티브 코드와 달리:
     * - 레지스터 저장: 없음 (소프트웨어 인터프리터라 레지스터가 없음)
     * - 인수 전달: value_stack에 이미 올려져 있음
     * - 반환값: rax 대신 stack top에 push
     * - 스택 정리: RETURN이 알아서 frame 이전으로 복원
     *
     * 이것이 바이트코드 VM의 핵심 장점:
     * 플랫폼마다 다른 호출 규약을 신경 쓸 필요 없음.
     */
    char      **global_names;
    Value      *global_values;
    int         global_count;
    int         global_cap;

    /* ── Q6: 클로저 캡처 — open upvalue 리스트 ─────────── */
    /*
     * 스택에 살아있는 upvalue들의 연결리스트.
     * 스택 위치 순서대로 정렬 (location 포인터 기준).
     * CLOSE_UPVAL 시: 해당 슬롯을 가리키는 upvalue를 찾아
     * closed 상태로 전환 (location을 자신의 closed로 변경).
     */
    ObjUpval   *open_upvalues;

    /* ── GC 상태 ────────────────────────────────────────── */
    Obj        *all_objects;           /* 모든 힙 객체 연결리스트 */
    size_t      bytes_allocated;
    size_t      next_gc_threshold;

    Obj       **gray_stack;            /* 트라이컬러 마킹 워크리스트 */
    int         gray_count;
    int         gray_cap;

    /* ── 문자열 인터닝 ──────────────────────────────────── */
    /*
     * 동일 내용 문자열은 하나만 존재 (포인터 비교로 동등 판정 가능).
     * 간단한 오픈 어드레싱 해시맵.
     */
    ObjString **string_table;
    int         string_count;
    int         string_cap;

    /* ── 내장 클래스 ────────────────────────────────────── */
    ObjClass   *class_string;
    ObjClass   *class_array;

    /* ── 인라인 캐시 풀 ─────────────────────────────────── */
    /*
     * Q7: 각 CALL/GET_FIELD 명령어마다 IC 슬롯 하나.
     * ObjFunc->ic_slots가 이 배열의 특정 범위를 가리킴.
     * VM 전역 풀로 관리하면 GC가 추적하기 쉬움.
     */
    InlineCache *ic_pool;
    int          ic_count;
    int          ic_cap;

} VM;

/* ================================================================
 * Q4: 바이트코드 VM vs 네이티브 JIT의 차이는?
 *
 * 이 VM = 바이트코드 인터프리터 (switch/case 디스패치)
 *
 * JIT로 전환하려면:
 *   vm_exec()의 switch 내부 각 case를:
 *   → x86-64 기계어로 동적 컴파일 (vm_jit.c)
 *   → 컴파일된 코드를 mmap(PROT_EXEC)에 저장
 *   → 호출 시 함수 포인터로 직접 실행
 *
 * Q8: IR은 어디까지가 추상이고 어디서부터 물리적 레지스터인가?
 *
 * 이 VM의 바이트코드 = "가상 레지스터 없는" 스택 IR
 *   - OP_ADD: 추상적 "두 값을 더한다" — 물리 레지스터 없음
 *
 * JIT 컴파일 시 경계:
 *   [바이트코드] → [선형 스캔 레지스터 할당] → [x86-64 명령어]
 *   이 경계 (레지스터 할당 직전)이 "추상 → 물리" 전환점.
 * ================================================================ */

typedef enum {
    VM_OK,
    VM_RUNTIME_ERROR,
    VM_STACK_OVERFLOW,
} VMResult;

/* ── 공개 API ───────────────────────────────────────────── */
VM      *vm_new(void);
void     vm_free(VM *vm);
VMResult vm_run(VM *vm, ObjClosure *entry);
void     vm_gc(VM *vm);

/* GC 인식 할당자 (GC가 추적) */
ObjString  *vm_alloc_string(VM *vm, const char *chars, size_t len);
ObjArray   *vm_alloc_array(VM *vm, int initial_cap);
ObjFunc    *vm_alloc_func(VM *vm, const char *name);
ObjClosure *vm_alloc_closure(VM *vm, ObjFunc *func);
ObjUpval   *vm_alloc_upval(VM *vm, Value *slot);
ObjClass   *vm_alloc_class(VM *vm, const char *name);
ObjInstance *vm_alloc_instance(VM *vm, ObjClass *klass);

/* 인라인 스택 조작 (vm_exec에서 빈번하게 호출 → inline) */
static inline void vm_push(VM *vm, Value v) {
    vm->value_stack[vm->stack_top++] = v;
}
static inline Value vm_pop(VM *vm) {
    return vm->value_stack[--vm->stack_top];
}
static inline Value vm_peek(VM *vm, int dist) {
    return vm->value_stack[vm->stack_top - 1 - dist];
}

/* GC 마킹 API (vm_gc.c ↔ vm_exec.c 연결) */
void vm_mark_value(VM *vm, Value v);
void vm_mark_object(VM *vm, Obj *obj);
void vm_blacken_object(VM *vm, Obj *obj);

/* ── 바이트코드 방출 API (vm_bytecode.c) ──────────────────── */
void emit_byte(ObjFunc *fn, uint8_t byte);
void emit_u16 (ObjFunc *fn, uint16_t v);
void emit_i32 (ObjFunc *fn, int32_t  v);
int  emit_jump(ObjFunc *fn, OpCode op);
void patch_jump(ObjFunc *fn, int patch_pos);
int  add_constant(ObjFunc *fn, VM *vm, Value val);

/* 디스어셈블러 */
void disassemble_func(const ObjFunc *fn, FILE *out);
int  disassemble_instruction(const ObjFunc *fn, int offset, FILE *out);

/* 클로저 upvalue 캡처/닫기 (vm_closure.c) */
ObjUpval *vm_capture_upval(VM *vm, Value *slot);
void      vm_close_upvals(VM *vm, Value *slot);
