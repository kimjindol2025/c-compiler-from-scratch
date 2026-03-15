/*
 * vm_bytecode.h — 바이트코드 명령어 집합
 *
 * CONCEPT 4: 바이트코드 VM vs 네이티브 JIT
 * -----------------------------------------
 * 바이트코드 VM:
 *   - 명령어 = 1바이트 opcode + 가변 피연산자
 *   - 실행 = switch/case 루프 (인터프리터)
 *   - 이식성 최고, 속도 최저
 *
 * 네이티브 JIT:
 *   - 바이트코드를 런타임에 기계어로 변환
 *   - 실행 = 직접 CPU 명령어 실행
 *   - 이식성 낮음, 속도 최고
 *
 * 스택 기반 vs 레지스터 기반:
 *   - 스택: JVM, CPython — 명령어 수 많음, 구현 단순
 *   - 레지스터: Lua 5, Dalvik — 명령어 수 적음, 레지스터 할당 필요
 */
#pragma once
#include <stdint.h>

typedef enum {
    /* ── 상수 로드 ────────────────────────────────── */
    OP_LOAD_CONST,    /* [idx:u16] → 상수 풀에서 로드 후 push */
    OP_LOAD_NIL,      /* → push nil */
    OP_LOAD_TRUE,     /* → push true */
    OP_LOAD_FALSE,    /* → push false */

    /* ── 로컬 변수 ────────────────────────────────── */
    /*
     * CONCEPT 1: 스택 프레임 로컬 변수
     * 로컬 변수는 스택 슬롯으로 표현된다.
     * 프레임 베이스(slots[0])로부터 idx 오프셋.
     */
    OP_LOAD_LOCAL,    /* [idx:u8]  → slots[idx] push */
    OP_STORE_LOCAL,   /* [idx:u8]  ← pop → slots[idx] */

    /* ── 전역 변수 ────────────────────────────────── */
    OP_LOAD_GLOBAL,   /* [name_idx:u16] → 전역 테이블에서 로드 */
    OP_STORE_GLOBAL,  /* [name_idx:u16] ← pop → 전역 테이블에 저장 */

    /* ── Upvalue (클로저 캡처) ───────────────────── */
    /*
     * CONCEPT 6: 클로저 캡처
     * upvalue는 현재 클로저의 ObjClosure->upvalues[idx]를 통해 접근.
     * 스택에 살아있으면 location이 스택을 직접 가리킴 (open).
     * 스코프가 끝나면 값을 복사하고 location을 self로 전환 (closed).
     */
    OP_LOAD_UPVAL,    /* [idx:u8]  → *upvalues[idx]->location push */
    OP_STORE_UPVAL,   /* [idx:u8]  ← pop → *upvalues[idx]->location */
    OP_CLOSE_UPVAL,   /* 스택 맨 위 슬롯을 close (스코프 종료 시) */

    /* ── 스택 조작 ────────────────────────────────── */
    OP_POP,           /* top 버림 */
    OP_DUP,           /* top 복제 */
    OP_SWAP,          /* top-1 ↔ top 교환 */

    /* ── 산술 ─────────────────────────────────────── */
    OP_ADD,  OP_SUB,  OP_MUL,  OP_DIV,  OP_MOD,
    OP_NEG,           /* 단항 부정 */
    OP_POW,           /* 거듭제곱 */

    /* ── 비교 ─────────────────────────────────────── */
    OP_EQ, OP_NE,
    OP_LT, OP_LE,
    OP_GT, OP_GE,

    /* ── 논리 ─────────────────────────────────────── */
    OP_NOT,           /* ! */
    OP_AND,           /* 단락 평가용 (JIF_FALSE + POP) */
    OP_OR,            /* 단락 평가용 */

    /* ── 분기 ─────────────────────────────────────── */
    /*
     * 오프셋은 현재 IP(다음 명령어 시작)로부터의 상대값.
     * 32비트 signed offset → 최대 ±2GB 점프 (충분히 큼).
     */
    OP_JUMP,          /* [off:i32] → 무조건 점프 */
    OP_JUMP_IF_FALSE, /* [off:i32] → falsy면 점프, 값은 스택에 유지 */
    OP_JUMP_IF_TRUE,  /* [off:i32] → truthy면 점프 */
    OP_POP_JUMP_IF_FALSE, /* [off:i32] → pop 후 falsy면 점프 */
    OP_POP_JUMP_IF_TRUE,  /* [off:i32] → pop 후 truthy면 점프 */

    /* ── 함수 호출 / 반환 ────────────────────────── */
    /*
     * CONCEPT 1: 스택 프레임 생성
     * CALL 명령어:
     *   1. 스택에서 인수 nargs개를 꺼냄
     *   2. 새 Frame을 frame_stack에 push
     *   3. Frame.slots = 현재 스택 위치 (로컬 변수 공간 예약)
     *   4. IP를 새 함수의 code[0]으로 설정
     *
     * CONCEPT 2: 호출 규약 — VM이 책임진다
     * - 레지스터 저장/복원: 필요 없음 (인터프리터는 소프트웨어 레지스터)
     * - 인수 전달: 스택에 이미 올라가 있음
     * - 반환값: rax 대신 스택 top에 push
     */
    OP_CALL,          /* [nargs:u8] [ic_slot:u16] → 호출 */
    OP_CALL_METHOD,   /* [name_idx:u16] [nargs:u8] [ic_slot:u16] → 메서드 호출 */
    OP_RETURN,        /* → 현재 프레임 팝, 반환값을 caller 스택에 push */
    OP_RETURN_NIL,    /* → 반환값 없이 반환 */

    /* ── 클로저 생성 ──────────────────────────────── */
    OP_MAKE_CLOSURE,  /* [func_idx:u16] [n_upvals:u8] [upval_desc...] */
                      /* upval_desc: [is_local:u8][idx:u8] */

    /* ── 예외 처리 ────────────────────────────────── */
    /*
     * CONCEPT 5: 예외와 스택 언와인딩
     * TRY_BEGIN: 현재 프레임/스택 깊이를 TryFrame에 기록
     * THROW: TryFrame 스택을 역방향으로 찾아 핸들러로 점프
     *        그 과정에서 중간 프레임들을 팝 (언와인딩)
     *        각 프레임 팝 시 open upvalue들을 close
     */
    OP_TRY_BEGIN,     /* [catch_off:i32] [finally_off:i32] */
    OP_TRY_END,       /* TryFrame 팝 */
    OP_THROW,         /* top을 예외로 던짐 */
    OP_RETHROW,       /* catch에서 예외 재던짐 */

    /* ── 객체/배열 ────────────────────────────────── */
    OP_MAKE_ARRAY,    /* [n:u16] → 스택 위 n개로 배열 생성 */
    OP_ARRAY_GET,     /* arr[idx] */
    OP_ARRAY_SET,     /* arr[idx] = val */
    OP_ARRAY_LEN,     /* len(arr) */

    /* ── 인스턴스 필드 ────────────────────────────── */
    OP_GET_FIELD,     /* [name_idx:u16] [ic_slot:u16] */
    OP_SET_FIELD,     /* [name_idx:u16] [ic_slot:u16] */

    /* ── 기타 ─────────────────────────────────────── */
    OP_PRINT,         /* 디버그 출력 */
    OP_HALT,          /* VM 종료 */

    /* ── Specializing Adaptive Interpreter (SAI) ─────────────
     *
     * 이 opcodes는 컴파일러가 절대 생성하지 않는다.
     * VM 자신이 실행 중 타입 프로파일을 관찰한 뒤,
     * 바이트코드 배열을 in-place로 덮어써서 교체한다.
     *
     * 동작 원리:
     *   1. 범용 OP_ADD가 실행될 때마다 spec_counter 감소
     *   2. counter == 0 이 되면 관찰된 타입 확인
     *   3. 항상 int+int이었다면 → code[pos] = OP_ADD_INT로 교체
     *   4. 이후 OP_ADD_INT: 타입 체크 없음 (guard만 있음)
     *   5. guard 실패 (타입이 바뀜) → OP_ADD로 복원 (despecialize)
     *
     * 이것이 "정적 컴파일러가 절대 할 수 없는 것"이다:
     *   코드가 실행될수록 코드 자체가 빠르게 변한다.
     */
    OP_ADD_INT,    /* int + int  (guard 포함, 타입 체크 없음) */
    OP_ADD_FLOAT,  /* float + float */
    OP_SUB_INT,    /* int - int */
    OP_MUL_INT,    /* int * int */
    OP_LT_INT,     /* int < int */
    OP_LE_INT,     /* int <= int */
    OP_GT_INT,     /* int > int */
    OP_GE_INT,     /* int >= int */
    OP_EQ_INT,     /* int == int */

    OP_COUNT          /* 총 opcode 수 */
} OpCode;

/* 명령어 이름 (디버깅/디스어셈블러용) */
extern const char *opcode_names[OP_COUNT];

/* ────────────────────────────────────────────────────────────
 * 바이트코드 인코딩 헬퍼 함수 선언
 * (구현: vm_bytecode.c / 타입: vm_value.h + vm.h 참조)
 * ──────────────────────────────────────────────────────────── */
