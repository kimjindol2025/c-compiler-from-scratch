# CCC — C Compiler Design Document

**Version**: 1.0.0
**Last Updated**: 2026-03-16

---

## 개요

CCC는 C 언어로 구현된 C 컴파일러입니다. 이 문서는 컴파일러의 아키텍처와 구현 세부사항을 설명합니다.

---

## 컴파일 파이프라인

```
Source Code (.c)
    ↓
Preprocessor     (#include, #define, conditional compilation)
    ↓
Lexer            (토큰화)
    ↓
Parser           (AST 생성)
    ↓
Semantic         (타입 검사, name resolution)
Analyzer
    ↓
IR Generator     (3-주소 중간 코드)
    ↓
IR Optimizer     (선택사항: -O1)
    ↓
Code Generator   (x86-64 어셈블리)
    ↓
ELF Writer       (바이너리 생성)
    ↓
Executable       (또는 System Linker)
```

---

## 모듈 설명

### 1. Preprocessor (`preprocessor.c`)

**역할**: 전처리 지시문 처리

**기능**:
- `#include` 지시문 (로컬, 시스템)
- `#define` 매크로 (객체, 함수)
- `#ifdef` / `#ifndef` / `#endif` 조건부 컴파일
- `#if` / `#elif` / `#else` 식 기반 조건부

**데이터 구조**:
```c
typedef struct {
  char *name;
  char *replacement;
  int is_function;
  int arg_count;
} Macro;
```

---

### 2. Lexer (`lexer.c`)

**역할**: 소스 코드를 토큰으로 분해

**토큰 종류**:
- 예약어 (int, if, while, ...)
- 식별자 (변수명, 함수명)
- 리터럴 (정수, 실수, 문자열)
- 연산자 (+, -, *, /, ...)
- 구분자 ({, }, (, ), ...)

**데이터 구조**:
```c
typedef struct {
  int kind;           // TOKEN_INT, TOKEN_IDENT, ...
  char *text;         // 원본 텍스트
  int line, col;      // 위치 정보
  union {
    int i;            // 정수 값
    double d;         // 부동소수 값
    char *s;          // 문자열 값
  } val;
} Token;
```

---

### 3. Parser (`parser.c`)

**역할**: 토큰을 Abstract Syntax Tree(AST)로 변환

**파싱 방식**: 재귀 하강 파서 (Recursive Descent)

**주요 함수**:
- `parse_program()` - 최상위 정의들 파싱
- `parse_declaration()` - 선언 파싱
- `parse_function()` - 함수 정의 파싱
- `parse_statement()` - 문장 파싱
- `parse_expression()` - 식 파싱

**AST 노드 예시**:
```c
typedef struct Node {
  int kind;  // NODE_FUNC_DEF, NODE_IF, NODE_BINOP, ...
  union {
    struct {
      Type *ret_type;
      char *name;
      Node *body;
    } func_def;

    struct {
      Node *cond;
      Node *then_body;
      Node *else_body;
    } if_stmt;
  } u;
} Node;
```

---

### 4. Semantic Analyzer (`sema.c`)

**역할**: 타입 검사 및 의미 검증

**기능**:
- 타입 호환성 검사
- 함수 호출 검증
- 미정의 변수/함수 감지
- 심볼 테이블 관리

**데이터 구조**:
```c
typedef struct {
  char *name;
  Type *type;
  int is_global;
  int is_func;
  void *value;  // 초기값
} Symbol;
```

---

### 5. IR Generator (`ir.c`)

**역할**: AST를 3-주소 중간 코드로 변환

**3-주소 명령어 예시**:
```
t1 = a + b
t2 = t1 * 2
if t2 > 10 goto L1
```

**데이터 구조**:
```c
typedef struct {
  int op;           // IR_ADD, IR_SUB, ...
  Value *dst;       // 대상
  Value *src1, *src2; // 피연산자
  char *label;      // 조건 분기용
} IRInst;
```

---

### 6. Code Generator (`codegen.c`)

**역할**: x86-64 어셈블리 생성

**주요 기능**:
- 함수 프롤로그/에필로그 생성
- 레지스터 할당
- 호출 규약 (System V AMD64 ABI)
- 메모리 할당 (스택)

**생성 예시**:
```asm
push %rbp
mov %rsp, %rbp
sub $16, %rsp     # 16바이트 로컬 변수 공간
...
leave
ret
```

---

### 7. ELF Writer (`elf_writer.c`)

**역할**: 어셈블리를 실행 가능한 ELF 바이너리로 작성

**구성**:
- ELF 헤더
- 프로그램 헤더 테이블
- 섹션 (`.text`, `.data`, `.bss`, etc.)
- 심볼 테이블
- 재배치 정보

---

## 타입 시스템

### 지원 타입

```c
// 기본 타입
int, char, long, short, float, double

// 파생 타입
int *              // 포인터
int [10]           // 배열
struct Point { int x; int y; }  // 구조체
```

### 타입 호환성

```c
int → long          // 암시적 변환 가능
char → int          // 암시적 변환 가능
int → float         // 암시적 변환 가능
int* → void*        // 암시적 변환 가능
```

---

## 호출 규약 (ABI)

System V AMD64 ABI를 따릅니다.

### 정수 인자 전달

```
첫 6개 인자: rdi, rsi, rdx, rcx, r8, r9
나머지:      스택
```

### 반환값

```
정수 (8바이트 이하):    rax
부동소수:              xmm0
큰 값 (구조체):        메모리 (rdi 포인터)
```

---

## 메모리 관리

### 할당자 (arena.c)

메모리 효율성을 위해 Arena allocator 사용:
- 여러 작은 할당을 큰 블록에서 처리
- 해제는 전체 arena만 지원

```c
Arena *arena_new(size_t cap);
void *arena_alloc(Arena *a, size_t size);
void arena_free(Arena *a);
```

---

## 최적화 (IR 레벨)

### -O1 활성화 시

1. **상수 폴딩** (Constant Folding)
   ```
   t1 = 5 + 3      →  t1 = 8
   ```

2. **Dead Code Elimination** (DCE)
   ```
   t1 = a + b
   t2 = c + d      →  t2 = c + d
   (t1 미사용)
   ```

---

## 디버깅 및 로깅

### 컴파일러 플래그

```bash
-v              # 파이프라인 단계 출력
-ir             # IR 덤프
-S              # 어셈블리만 생성
```

### 에러 메시지 형식

```
ccc: main.c:10:5: error: 'x' undeclared
    int y = x + 1;
        ^
```

---

## 성능 특성

| 작업 | 복잡도 | 비고 |
|-----|--------|------|
| 렉서 | O(n) | n = 소스 길이 |
| 파서 | O(n) | 재귀 깊이 = 중첩 깊이 |
| 의미 분석 | O(n) | 심볼 테이블 해시 O(1) |
| 코드 생성 | O(n) | 선형 IR 처리 |

---

## 테스트 전략

### 단위 테스트

- `test_lexer.c`: 토큰화 검증
- `test_parser.c`: AST 생성 검증

### 통합 테스트

```bash
make -C scripts check
```

간단한 C 파일 컴파일 후 실행 결과 검증

---

## 향후 개선사항

1. **성능 최적화**
   - 루프 최적화
   - 인라인 함수
   - 전역 값 번호 지정 (GVN)

2. **기능 확장**
   - 복잡한 타입 (추상 선언자)
   - 더 많은 표준 라이브러리 함수
   - 디버깅 정보 (DWARF)

3. **다중 백엔드**
   - ARM / RISC-V 지원
   - LLVM IR 출력

---

**설계 문서 작성**: 2026-03-16
**현재 버전**: 1.0.0
