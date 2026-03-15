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

/* 타입 안전 캐스트 */
#define AS_STRING(v)   ((ObjString*)  (v).obj)
#define AS_ARRAY(v)    ((ObjArray*)   (v).obj)
#define AS_FUNC(v)     ((ObjFunc*)    (v).obj)
#define AS_CLOSURE(v)  ((ObjClosure*) (v).obj)
#define AS_UPVAL(v)    ((ObjUpval*)   (v).obj)
#define AS_CLASS(v)    ((ObjClass*)   (v).obj)
#define AS_INSTANCE(v) ((ObjInstance*)(v).obj)
