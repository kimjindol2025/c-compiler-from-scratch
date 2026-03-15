# c-compiler-from-scratch Project Charter

**Project Name**: c-compiler-from-scratch
**Category**: compiler
**Status**: 🟡 In Progress
**Repository**: https://gogs.dclub.kr/kim/c-compiler-from-scratch.git

---

## 프로젝트 개요

처음부터 만드는 C 컴파일러.
**목표: 작동하는 C 컴파일러 구현 (부분 호환)**

---

## 폴더 구조

```
c-compiler-from-scratch/
├─ src/                     # 컴파일러 소스
│  ├─ lexer.c               # 렉서
│  ├─ parser.c              # 파서
│  ├─ codegen.c             # 코드 생성
│  └─ main.c                # 진입점
│
├─ tests/                   # 테스트
│  └─ unit/
│
├─ examples/                # 예제 C 프로그램
│  ├─ hello.c
│  └─ fibonacci.c
│
├─ docs/                    # 문서
│  └─ DESIGN.md
│
├─ CLAUDE.md                # 이 파일
└─ MEMORY.md                # 진행 상황
```

---

## 작업 규칙

### 1. 컴파일 파이프라인
- Lexer: 토큰화
- Parser: AST 생성
- Codegen: 기계어/어셈블리 생성

### 2. 지원 기능
- 기본 데이터 타입 (int, char, float)
- 함수 선언 & 호출
- 변수 & 상수
- 제어 흐름 (if, for, while)

### 3. 테스트
- 각 단계별 유닛 테스트
- 통합 테스트

---

## 메모리 시스템

### MEMORY.md 기록
- 현재 구현 단계
- 완료된 기능
- 다음 할 일
- 발견된 버그

---

## 라이선스

MIT

---

## 문의

- 이슈: GOGS Issues
