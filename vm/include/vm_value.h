/*
 * vm_value.h — Value representation and Object model
 *
 * CONCEPT 1: 값 표현 (Value Representation)
 * -----------------------------------------
 * 모든 런타임 값을 하나의 Value 타입으로 표현한다.
 * 방법 1: Tagged Union (이 구현) — 이해하기 쉬움, 약간 느림
 * 방법 2: NaN Boxing (V8 방식) — 64비트에 모든 타입 압축
 * 방법 3: Pointer Tagging — 하위 비트를 타입 태그로 사용
 *
 * CONCEPT 2: 힙 객체 (Heap Objects)
 * -----------------------------------
 * 가비지 컬렉터가 관리하는 객체들은 Obj 헤더를 가진다.
 * 모든 Obj는 연결 리스트로 GC가 추적한다.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ================================================================
 * Heap Object Header (모든 GC 관리 객체의 공통 헤더)
 * ================================================================ */
typedef enum {
    OBJ_STRING,    /* 문자열 */
    OBJ_ARRAY,     /* 동적 배열 */
    OBJ_FUNC,      /* 함수 (바이트코드 + 메타데이터) */
    OBJ_CLOSURE,   /* 클로저 = 함수 + 캡처된 upvalue들 */
    OBJ_UPVAL,     /* 클로저 캡처 변수 (열린/닫힌 두 상태) */
    OBJ_CLASS,     /* 클래스 디스패치 대상 (인라인 캐시용) */
    OBJ_INSTANCE,  /* 클래스 인스턴스 */

    /* ── V가 못 하는 것들 ──────────────────────────────
     * V는 컴파일 타임에 모든 것이 고정된다.
     * 우리 VM은 런타임에 외부 세계를 Value로 흡수한다. */

    OBJ_NATIVE,    /* C/FreeLang 네이티브 함수 — GC 관리 callable */
    OBJ_FIBER,     /* 일시정지 가능한 실행 컨텍스트 — async의 기반 */
    OBJ_MAP,       /* 해시맵 — {key: value} 동적 객체 */
    OBJ_EXTERNAL,  /* 외부 런타임 핸들 (FreeLang/JS 객체를 GC 아래로) */
} ObjKind;

/*
 * GC 헤더: 모든 힙 객체의 첫 필드
 * - marked: GC 마킹 (tri-color의 gray/black는 별도 worklist로 처리)
 * - kind: 객체 타입 (다운캐스트 없이 switch로 처리 가능)
 * - next: GC가 추적하는 전역 객체 링크드 리스트
 */
typedef struct Obj {
    ObjKind      kind;
    bool         marked;   /* GC 마킹 플래그 */
    struct Obj  *next;     /* GC 연결 리스트 (vm->all_objects) */
} Obj;

/* ================================================================
 * Value (모든 런타임 값의 단일 표현)
 * ================================================================ */
typedef enum {
    VAL_NIL,     /* 없음 */
    VAL_BOOL,    /* true / false */
    VAL_INT,     /* 64비트 정수 */
    VAL_FLOAT,   /* 64비트 부동소수 */
    VAL_OBJ,     /* 힙 객체 포인터 */
} ValKind;

typedef struct {
    ValKind kind;
    union {
        bool    b;
        int64_t i;
        double  f;
        Obj    *obj;
    };
} Value;

/* 편의 생성자 */
static inline Value val_nil(void)        { return (Value){ VAL_NIL };              }
static inline Value val_bool(bool b)     { return (Value){ VAL_BOOL,  .b = b };    }
static inline Value val_int(int64_t i)   { return (Value){ VAL_INT,   .i = i };    }
static inline Value val_float(double f)  { return (Value){ VAL_FLOAT, .f = f };    }
static inline Value val_obj(Obj *o)      { return (Value){ VAL_OBJ,   .obj = o };  }

/* 타입 검사 */
static inline bool val_is_nil(Value v)   { return v.kind == VAL_NIL;   }
static inline bool val_is_bool(Value v)  { return v.kind == VAL_BOOL;  }
static inline bool val_is_int(Value v)   { return v.kind == VAL_INT;   }
static inline bool val_is_float(Value v) { return v.kind == VAL_FLOAT; }
static inline bool val_is_obj(Value v)   { return v.kind == VAL_OBJ;   }
static inline bool val_is_obj_kind(Value v, ObjKind k) {
    return v.kind == VAL_OBJ && v.obj && v.obj->kind == k;
}

/* Truthy 판정 (nil과 false만 falsy) */
static inline bool val_truthy(Value v) {
    if (v.kind == VAL_NIL)  return false;
    if (v.kind == VAL_BOOL) return v.b;
    return true;
}

/* 값 동등 비교 */
static inline bool val_equal(Value a, Value b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case VAL_NIL:   return true;
    case VAL_BOOL:  return a.b == b.b;
    case VAL_INT:   return a.i == b.i;
    case VAL_FLOAT: return a.f == b.f;
    case VAL_OBJ:   return a.obj == b.obj;
    }
    return false;
}

void val_print(Value v, FILE *out);

/* ================================================================
 * 구체적 객체 타입들
 * ================================================================ */

/* ObjString: 불변 문자열 */
typedef struct {
    Obj     base;
    size_t  len;
    uint32_t hash;   /* 캐시된 해시 (인라인 캐시의 키로 사용) */
    char    chars[]; /* 유연 배열 멤버 (C99): 문자열 데이터 인라인 저장 */
} ObjString;

/* ObjArray: 동적 배열 */
typedef struct {
    Obj     base;
    Value  *data;
    int     len;
    int     cap;
} ObjArray;

/* ObjFunc: 컴파일된 함수 (바이트코드 + 메타데이터)
 *
 * 이 객체는 "함수의 코드"를 담는다.
 * 클로저와 구분: ObjFunc는 코드만, ObjClosure는 코드 + 캡처 환경
 */
struct ObjUpval;  /* 전방 선언 */

typedef struct {
    Obj      base;
    char    *name;         /* 함수 이름 (디버깅용) */
    uint8_t *code;         /* 바이트코드 버퍼 */
    int      code_len;
    int      code_cap;
    Value   *constants;    /* 상수 풀 (리터럴 값들) */
    int      const_len;
    int      const_cap;
    int      arity;        /* 파라미터 수 */
    int      max_locals;   /* 로컬 변수 최대 수 (스택 슬롯 예약용) */
    int      upval_count;  /* 이 함수가 캡처하는 upvalue 수 */
    /* 인라인 캐시 슬롯들 (CALL 명령어마다 하나씩 */
    struct InlineCache *ic_slots; /* vm_dispatch.h 참조 */
    int      n_ic_slots;
    /* 소스 위치 (예외 스택 트레이스용) */
    int     *line_numbers; /* code[i]의 소스 줄 번호 */

    /* ── Specializing Adaptive Interpreter (SAI) ─────────────────
     * 각 바이트코드 위치의 특수화 카운트다운.
     * 최초 NULL → 지연 할당 (code_cap 크기).
     * counter[pos] 가 0에 도달하면:
     *   → 관찰된 타입에 맞는 특수화 opcode로 code[pos] 교체.
     * guard 실패 시: 다시 generic opcode로 복원.
     */
    uint8_t *spec_counters;
} ObjFunc;

/* ObjClosure: 함수 + 캡처된 환경
 *
 * CONCEPT 6: 클로저 캡처
 * -----------------------
 * 클로저 = 함수 코드(ObjFunc) + 자유 변수들(upvalues[])
 * upvalue가 스택에 살아있는 동안은 스택 슬롯을 직접 가리킨다. (open)
 * 해당 스코프가 끝나면 값을 복사해서 자체 저장소에 보관한다. (closed)
 */
typedef struct ObjClosure {
    Obj           base;
    ObjFunc      *func;
    struct ObjUpval **upvalues; /* func->upval_count 크기의 포인터 배열 */
} ObjClosure;

/* ObjUpval: 클로저 캡처 변수
 *
 * Open 상태: location이 스택의 실제 슬롯을 가리킴
 * Closed 상태: location이 자신의 closed 필드를 가리킴 (스택 독립)
 */
typedef struct ObjUpval {
    Obj           base;
    Value        *location;    /* 현재 값의 위치 (open: 스택, closed: &closed) */
    Value         closed;      /* 클로즈 후 복사된 값 */
    struct ObjUpval *next_open; /* VM의 open_upvalues 리스트 */
} ObjUpval;

/* ObjClass: 클래스 (인라인 캐시 디스패치 대상) */
typedef struct {
    Obj      base;
    char    *name;
    uint32_t shape_id;  /* 이 클래스의 고유 shape ID (인라인 캐시 키) */
    /* 메서드 테이블 (간단한 선형 탐색; 실제는 해시맵) */
    char   **method_names;
    Value   *methods;
    int      method_count;
} ObjClass;

/* ObjInstance: 클래스 인스턴스 */
typedef struct {
    Obj       base;
    ObjClass *klass;
    uint32_t  shape_id;  /* klass->shape_id와 같거나 subshape */
    char    **field_names;
    Value    *fields;
    int       field_count;
} ObjInstance;

/* ================================================================
 * OBJ_NATIVE — 네이티브 함수 (C 함수 포인터를 GC Value로)
 *
 * V가 못 하는 것 #1:
 *   V의 FFI는 컴파일 타임에 C 함수를 바인딩한다.
 *   우리 VM은 런타임에 어떤 함수든 Value로 만들어
 *   클로저가 캡처하고, 배열에 담고, 다른 함수에 넘길 수 있다.
 *
 * 사용 예:
 *   vm_define_native(vm, "fetch", fl_fetch_impl);
 *   → globals["fetch"] = ObjNative(fl_fetch_impl)
 *   → 우리 언어에서: let result = fetch("https://...")
 * ================================================================ */
struct VM; /* 전방 선언 */
typedef Value (*NativeFn)(struct VM *vm, int argc, Value *argv);

typedef struct {
    Obj       base;
    const char *name;    /* 디버깅/에러 메시지용 */
    NativeFn   fn;       /* 실제 C 함수 포인터 */
    int        arity;    /* -1 = 가변 인수 */
} ObjNative;

/* ================================================================
 * OBJ_MAP — 동적 해시맵 {key: value}
 *
 * V가 못 하는 것 #2:
 *   V는 map 타입이 있지만 동적 키를 런타임에 추가하는 건
 *   제한적이다. 우리 VM은 어떤 Value도 키가 될 수 있다.
 *   FreeLang의 JS 객체를 그대로 흡수하는 기반이다.
 * ================================================================ */
typedef struct {
    Obj      base;
    Value   *keys;
    Value   *vals;
    int      count;
    int      cap;
} ObjMap;

/* ================================================================
 * OBJ_FIBER — 일시정지 가능한 실행 컨텍스트
 *
 * V가 못 하는 것 #3:
 *   V에는 코루틴/파이버가 없다.
 *   우리 VM의 frame_stack은 스냅샷을 찍어서 Fiber에 저장하고
 *   나중에 resume할 수 있다. FreeLang의 Promise 기반 비동기를
 *   동기 스타일로 쓸 수 있는 기반이다.
 *
 * 상태 머신:
 *   FIBER_SUSPENDED → resume() → FIBER_RUNNING
 *   FIBER_RUNNING   → yield()  → FIBER_SUSPENDED
 *   FIBER_RUNNING   → return   → FIBER_DONE
 * ================================================================ */
typedef enum {
    FIBER_SUSPENDED,
    FIBER_RUNNING,
    FIBER_DONE,
    FIBER_ERROR,
} FiberState;

#define FIBER_STACK_MAX  1024
#define FIBER_FRAME_MAX  64

typedef struct ObjFiber {
    Obj        base;
    FiberState state;

    /* 일시정지된 실행 상태 스냅샷 */
    Value      stack[FIBER_STACK_MAX];
    int        stack_top;
    /* CallFrame은 vm.h에 정의되므로 여기선 raw bytes로 저장 */
    uint8_t    frames[FIBER_FRAME_MAX * 64]; /* sizeof(CallFrame) ≤ 64 */
    int        frame_count;

    Value      result;   /* FIBER_DONE 시 최종 반환값 */
    Value      error;    /* FIBER_ERROR 시 예외 값 */
} ObjFiber;

/* ================================================================
 * OBJ_EXTERNAL — 외부 런타임 핸들
 *
 * FreeLang/JS 객체를 우리 GC 아래로 가져온다.
 * GC가 이 객체를 회수할 때 finalizer를 호출해서
 * 외부 런타임에도 해제를 알린다.
 * ================================================================ */
typedef void (*ExternalFinalizer)(void *handle);

typedef struct {
    Obj                base;
    void              *handle;      /* FreeLang/JS 객체 포인터 */
    ExternalFinalizer  finalizer;   /* GC 회수 시 호출 */
    const char        *type_name;   /* "Promise", "Buffer", "Stream" 등 */
} ObjExternal;

/* 타입 안전 캐스트 */
#define AS_STRING(v)   ((ObjString*)  (v).obj)
#define AS_ARRAY(v)    ((ObjArray*)   (v).obj)
#define AS_FUNC(v)     ((ObjFunc*)    (v).obj)
#define AS_CLOSURE(v)  ((ObjClosure*) (v).obj)
#define AS_UPVAL(v)    ((ObjUpval*)   (v).obj)
#define AS_CLASS(v)    ((ObjClass*)   (v).obj)
#define AS_INSTANCE(v) ((ObjInstance*)(v).obj)
#define AS_NATIVE(v)   ((ObjNative*)  (v).obj)
#define AS_MAP(v)      ((ObjMap*)     (v).obj)
#define AS_FIBER(v)    ((ObjFiber*)   (v).obj)
#define AS_EXTERNAL(v) ((ObjExternal*)(v).obj)
