/*
 * vm_closure.c — 클로저 캡처 (Upvalue open/close 메커니즘)
 *
 * Q6: 클로저 캡처는 런타임에서 어떻게 유지되는가?
 *
 * 핵심 아이디어 (Lua 5.x에서 차용):
 *
 * 변수가 스택에 살아있는 동안 (open upvalue):
 *   upval->location = &value_stack[slot]   // 스택 슬롯 직접 참조
 *   → 읽기/쓰기가 스택에 직접 반영됨
 *
 * 변수가 스코프를 벗어날 때 (close upvalue):
 *   upval->closed    = *upval->location    // 값 복사
 *   upval->location  = &upval->closed      // 자기 자신 가리킴
 *   → 스택이 없어져도 값은 upval 안에 보존됨
 *
 * 같은 슬롯을 캡처하는 여러 클로저는 같은 ObjUpval을 공유한다.
 * (open_upvalues 리스트는 스택 위치 기준 정렬 유지)
 */
#include "../include/vm.h"
#include <stdlib.h>

/*
 * vm_capture_upval: slot 위치의 값을 캡처하는 ObjUpval 획득.
 * 이미 같은 슬롯을 가리키는 open upvalue가 있으면 재사용 (공유).
 */
ObjUpval *vm_capture_upval(VM *vm, Value *slot) {
    /* open_upvalues는 location 내림차순(stack top 방향) 정렬 */
    ObjUpval *prev = NULL;
    ObjUpval *cur  = vm->open_upvalues;

    while (cur && cur->location > slot) {
        prev = cur;
        cur  = cur->next_open;
    }

    /* 동일 슬롯 발견 → 재사용 */
    if (cur && cur->location == slot) return cur;

    /* 새로 생성 */
    ObjUpval *uv = vm_alloc_upval(vm, slot);
    uv->next_open = cur;
    if (prev) prev->next_open = uv;
    else      vm->open_upvalues = uv;

    return uv;
}

/*
 * vm_close_upvals: slot 위치 이상의 모든 open upvalue를 close.
 * OP_CLOSE_UPVAL 및 RETURN 시 호출.
 *
 * 예) slot = &value_stack[base]이면 base 이상 슬롯의 upvalue 전부 close.
 */
void vm_close_upvals(VM *vm, Value *slot) {
    while (vm->open_upvalues && vm->open_upvalues->location >= slot) {
        ObjUpval *uv = vm->open_upvalues;

        /* close: 현재 값을 복사하고 location을 자신으로 전환 */
        uv->closed   = *uv->location;
        uv->location = &uv->closed;

        vm->open_upvalues = uv->next_open;
        uv->next_open = NULL;
    }
}
