/*
 * vm_gc.c — Mark-and-Sweep 가비지 컬렉터
 *
 * Q3: GC / 메모리 소유권 모델은 어떻게 작동하는가?
 *
 * 알고리즘: Tri-color mark-and-sweep
 *
 *  흰색 (white): 아직 방문 안 함 → SWEEP 단계에서 회수 대상
 *  회색 (gray):  발견은 했지만 자식을 아직 탐색 안 함
 *                → gray_stack에서 꺼내 blacken
 *  검정 (black): 자신 + 모든 자식 마킹 완료
 *                → marked=true이고 gray_stack에 없는 것
 *
 * MARK 단계:
 *   1. 루트를 회색으로 (vm_mark_object → gray_stack push)
 *   2. gray_stack이 빌 때까지: pop → 자식 마킹 (blacken)
 *
 * SWEEP 단계:
 *   all_objects 순회 → marked=false면 free, 아니면 marked=false 리셋
 */
#include "../include/vm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── 마킹 ─────────────────────────────────────────────── */

void vm_mark_object(VM *vm, Obj *obj) {
    if (!obj || obj->marked) return;
    obj->marked = true;

    /* gray_stack에 추가 */
    if (vm->gray_count >= vm->gray_cap) {
        vm->gray_cap = vm->gray_cap ? vm->gray_cap * 2 : 32;
        vm->gray_stack = realloc(vm->gray_stack, vm->gray_cap * sizeof(Obj*));
    }
    vm->gray_stack[vm->gray_count++] = obj;
}

void vm_mark_value(VM *vm, Value v) {
    if (v.kind == VAL_OBJ) vm_mark_object(vm, v.obj);
}

/* 객체 자신의 자식들을 마킹 (gray → black 전환) */
void vm_blacken_object(VM *vm, Obj *obj) {
    switch (obj->kind) {
    case OBJ_STRING:
        /* 자식 없음 */
        break;
    case OBJ_ARRAY: {
        ObjArray *arr = (ObjArray*)obj;
        for (int i = 0; i < arr->len; i++)
            vm_mark_value(vm, arr->data[i]);
        break;
    }
    case OBJ_FUNC: {
        ObjFunc *fn = (ObjFunc*)obj;
        /* 상수 풀의 모든 값 마킹 */
        for (int i = 0; i < fn->const_len; i++)
            vm_mark_value(vm, fn->constants[i]);
        break;
    }
    case OBJ_CLOSURE: {
        ObjClosure *cl = (ObjClosure*)obj;
        vm_mark_object(vm, (Obj*)cl->func);
        for (int i = 0; i < cl->func->upval_count; i++)
            vm_mark_object(vm, (Obj*)cl->upvalues[i]);
        break;
    }
    case OBJ_UPVAL: {
        ObjUpval *uv = (ObjUpval*)obj;
        /* closed 상태일 때 closed 값을 마킹 */
        vm_mark_value(vm, uv->closed);
        break;
    }
    case OBJ_CLASS: {
        ObjClass *klass = (ObjClass*)obj;
        for (int i = 0; i < klass->method_count; i++)
            vm_mark_value(vm, klass->methods[i]);
        break;
    }
    case OBJ_INSTANCE: {
        ObjInstance *inst = (ObjInstance*)obj;
        vm_mark_object(vm, (Obj*)inst->klass);
        for (int i = 0; i < inst->field_count; i++)
            vm_mark_value(vm, inst->fields[i]);
        break;
    }
    /* ── 새 타입들 (V가 못 하는 것의 GC 측) ────────── */
    case OBJ_NATIVE:
        /* 함수 포인터 — GC 추적 불필요, 자식 없음 */
        break;
    case OBJ_MAP: {
        ObjMap *map = (ObjMap*)obj;
        for (int i = 0; i < map->count; i++) {
            vm_mark_value(vm, map->keys[i]);
            vm_mark_value(vm, map->vals[i]);
        }
        break;
    }
    case OBJ_FIBER: {
        ObjFiber *fb = (ObjFiber*)obj;
        for (int i = 0; i < fb->stack_top; i++)
            vm_mark_value(vm, fb->stack[i]);
        vm_mark_value(vm, fb->result);
        vm_mark_value(vm, fb->error);
        break;
    }
    case OBJ_EXTERNAL:
        /* 외부 핸들 — finalizer가 책임짐, 우리 GC는 추적 불필요 */
        break;
    }
}

static void mark_roots(VM *vm) {
    /* 1. value_stack 전체 */
    for (int i = 0; i < vm->stack_top; i++)
        vm_mark_value(vm, vm->value_stack[i]);

    /* 2. 각 CallFrame의 클로저 */
    for (int i = 0; i < vm->frame_count; i++)
        vm_mark_object(vm, (Obj*)vm->frame_stack[i].closure);

    /* 3. open upvalue들 */
    for (ObjUpval *uv = vm->open_upvalues; uv; uv = uv->next_open)
        vm_mark_object(vm, (Obj*)uv);

    /* 4. 전역 변수들 */
    for (int i = 0; i < vm->global_count; i++)
        vm_mark_value(vm, vm->global_values[i]);

    /* 5. 내장 클래스 */
    vm_mark_object(vm, (Obj*)vm->class_string);
    vm_mark_object(vm, (Obj*)vm->class_array);

    /* 6. 인라인 캐시의 cached_method들 */
    for (int i = 0; i < vm->ic_count; i++)
        vm_mark_value(vm, vm->ic_pool[i].cached_method);
}

static void trace_references(VM *vm) {
    while (vm->gray_count > 0) {
        Obj *obj = vm->gray_stack[--vm->gray_count];
        vm_blacken_object(vm, obj);
    }
}

static void sweep(VM *vm) {
    Obj **link = &vm->all_objects;
    while (*link) {
        Obj *obj = *link;
        if (obj->marked) {
            /* 살아있음 → 다음 GC를 위해 리셋 */
            obj->marked = false;
            link = &obj->next;
        } else {
            /* 회수 */
            *link = obj->next;
            /* 종류별 내부 메모리 해제 */
            switch (obj->kind) {
            case OBJ_STRING:
                vm->bytes_allocated -= sizeof(ObjString) + ((ObjString*)obj)->len + 1;
                free(obj);
                break;
            case OBJ_ARRAY: {
                ObjArray *arr = (ObjArray*)obj;
                vm->bytes_allocated -= sizeof(ObjArray) + arr->cap * sizeof(Value);
                free(arr->data);
                free(arr);
                break;
            }
            case OBJ_FUNC: {
                ObjFunc *fn = (ObjFunc*)obj;
                vm->bytes_allocated -= sizeof(ObjFunc);
                free(fn->name);
                free(fn->code);
                free(fn->constants);
                free(fn->line_numbers);
                free(fn->ic_slots);
                free(fn);
                break;
            }
            case OBJ_CLOSURE: {
                ObjClosure *cl = (ObjClosure*)obj;
                vm->bytes_allocated -= sizeof(ObjClosure)
                    + cl->func->upval_count * sizeof(ObjUpval*);
                free(cl->upvalues);
                free(cl);
                break;
            }
            case OBJ_UPVAL:
                vm->bytes_allocated -= sizeof(ObjUpval);
                free(obj);
                break;
            case OBJ_CLASS: {
                ObjClass *klass = (ObjClass*)obj;
                for (int i = 0; i < klass->method_count; i++)
                    free(klass->method_names[i]);
                free(klass->method_names);
                free(klass->methods);
                free(klass->name);
                vm->bytes_allocated -= sizeof(ObjClass);
                free(klass);
                break;
            }
            case OBJ_INSTANCE: {
                ObjInstance *inst = (ObjInstance*)obj;
                for (int i = 0; i < inst->field_count; i++)
                    free(inst->field_names[i]);
                free(inst->field_names);
                free(inst->fields);
                vm->bytes_allocated -= sizeof(ObjInstance);
                free(inst);
                break;
            }
            case OBJ_NATIVE:
                vm->bytes_allocated -= sizeof(ObjNative);
                free(obj);
                break;
            case OBJ_MAP: {
                ObjMap *map = (ObjMap*)obj;
                free(map->keys);
                free(map->vals);
                vm->bytes_allocated -= sizeof(ObjMap);
                free(map);
                break;
            }
            case OBJ_FIBER:
                vm->bytes_allocated -= sizeof(ObjFiber);
                free(obj);
                break;
            case OBJ_EXTERNAL: {
                ObjExternal *ext = (ObjExternal*)obj;
                if (ext->finalizer) ext->finalizer(ext->handle);
                vm->bytes_allocated -= sizeof(ObjExternal);
                free(obj);
                break;
            }
            }
        }
    }
}

void vm_gc(VM *vm) {
    mark_roots(vm);
    trace_references(vm);
    sweep(vm);

    /* adaptive threshold: 살아있는 객체 크기의 2배 */
    vm->next_gc_threshold = vm->bytes_allocated * 2;
    if (vm->next_gc_threshold < 1024 * 1024)
        vm->next_gc_threshold = 1024 * 1024;
}

/* ── 할당 헬퍼 ────────────────────────────────────────── */

static void *gc_alloc(VM *vm, size_t size) {
    vm->bytes_allocated += size;
    if (vm->bytes_allocated > vm->next_gc_threshold)
        vm_gc(vm);
    void *p = calloc(1, size);
    return p;
}

static void obj_link(VM *vm, Obj *obj, ObjKind kind) {
    obj->kind   = kind;
    obj->marked = false;
    obj->next   = vm->all_objects;
    vm->all_objects = obj;
}

ObjString *vm_alloc_string(VM *vm, const char *chars, size_t len) {
    /* 인터닝: 동일 내용이 이미 있으면 재사용 */
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++)
        hash = (hash ^ (uint8_t)chars[i]) * 16777619u;

    if (vm->string_table) {
        int cap = vm->string_cap;
        for (int i = (int)(hash % cap); ; i = (i + 1) % cap) {
            ObjString *s = vm->string_table[i];
            if (!s) break;
            if (s->hash == hash && s->len == len &&
                memcmp(s->chars, chars, len) == 0)
                return s;
        }
    }

    ObjString *s = gc_alloc(vm, sizeof(ObjString) + len + 1);
    s->len  = len;
    s->hash = hash;
    memcpy(s->chars, chars, len);
    s->chars[len] = '\0';
    obj_link(vm, (Obj*)s, OBJ_STRING);

    /* 인터닝 테이블에 추가 */
    if (vm->string_count + 1 > vm->string_cap * 3 / 4) {
        int nc = vm->string_cap ? vm->string_cap * 2 : 16;
        ObjString **nt = calloc(nc, sizeof(ObjString*));
        for (int i = 0; i < vm->string_cap; i++) {
            if (!vm->string_table[i]) continue;
            int j = (int)(vm->string_table[i]->hash % nc);
            while (nt[j]) j = (j + 1) % nc;
            nt[j] = vm->string_table[i];
        }
        free(vm->string_table);
        vm->string_table = nt;
        vm->string_cap   = nc;
    }
    {
        int i = (int)(hash % vm->string_cap);
        while (vm->string_table[i]) i = (i + 1) % vm->string_cap;
        vm->string_table[i] = s;
        vm->string_count++;
    }

    return s;
}

ObjArray *vm_alloc_array(VM *vm, int initial_cap) {
    ObjArray *arr = gc_alloc(vm, sizeof(ObjArray));
    obj_link(vm, (Obj*)arr, OBJ_ARRAY);
    if (initial_cap > 0) {
        arr->data = calloc(initial_cap, sizeof(Value));
        arr->cap  = initial_cap;
        vm->bytes_allocated += initial_cap * sizeof(Value);
    }
    return arr;
}

ObjFunc *vm_alloc_func(VM *vm, const char *name) {
    ObjFunc *fn = gc_alloc(vm, sizeof(ObjFunc));
    obj_link(vm, (Obj*)fn, OBJ_FUNC);
    fn->name = name ? strdup(name) : NULL;
    return fn;
}

ObjClosure *vm_alloc_closure(VM *vm, ObjFunc *func) {
    int n = func->upval_count;
    ObjClosure *cl = gc_alloc(vm, sizeof(ObjClosure));
    obj_link(vm, (Obj*)cl, OBJ_CLOSURE);
    cl->func     = func;
    cl->upvalues = calloc(n, sizeof(ObjUpval*));
    vm->bytes_allocated += n * sizeof(ObjUpval*);
    return cl;
}

ObjUpval *vm_alloc_upval(VM *vm, Value *slot) {
    ObjUpval *uv = gc_alloc(vm, sizeof(ObjUpval));
    obj_link(vm, (Obj*)uv, OBJ_UPVAL);
    uv->location = slot;
    uv->closed   = val_nil();
    return uv;
}

ObjClass *vm_alloc_class(VM *vm, const char *name) {
    static uint32_t next_shape = 1;
    ObjClass *klass = gc_alloc(vm, sizeof(ObjClass));
    obj_link(vm, (Obj*)klass, OBJ_CLASS);
    klass->name     = name ? strdup(name) : NULL;
    klass->shape_id = next_shape++;
    return klass;
}

ObjInstance *vm_alloc_instance(VM *vm, ObjClass *klass) {
    ObjInstance *inst = gc_alloc(vm, sizeof(ObjInstance));
    obj_link(vm, (Obj*)inst, OBJ_INSTANCE);
    inst->klass    = klass;
    inst->shape_id = klass->shape_id;
    return inst;
}
