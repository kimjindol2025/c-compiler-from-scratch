/*
 * vm_native.c — 네이티브 함수 바인딩 & FreeLang FFI 레이어
 *
 * 이게 V가 못 하는 것의 핵심 구현이다.
 *
 * V의 한계:
 *   V는 C 함수를 컴파일 타임에 바인딩한다.
 *   extern fn fetch(url string) string  ← C에만 붙을 수 있음
 *
 * 우리의 해법:
 *   어떤 함수든 런타임에 vm_define_native()로 등록한다.
 *   등록된 함수는 ObjNative(Value)가 되어 VM 안에서
 *   클로저처럼 전달/캡처/호출된다.
 *
 *   // C 코드에서:
 *   vm_define_native(vm, "fetch",    fl_fetch,    1);
 *   vm_define_native(vm, "println",  fl_println, -1);  // 가변 인수
 *   vm_define_native(vm, "json_parse", fl_json,  1);
 *
 *   // 우리 언어에서 (파서가 @fl을 보면 이쪽으로 라우팅):
 *   @fl let result = fetch("https://api.com/users")
 *   @fl println("Hello", name)
 */
#include "../include/vm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── ObjNative 할당 ───────────────────────────────── */

ObjNative *vm_alloc_native(VM *vm, const char *name, NativeFn fn, int arity) {
    ObjNative *nat = calloc(1, sizeof(ObjNative));
    nat->base.kind   = OBJ_NATIVE;
    nat->base.marked = false;
    nat->base.next   = vm->all_objects;
    vm->all_objects  = (Obj*)nat;
    vm->bytes_allocated += sizeof(ObjNative);

    nat->name  = name;
    nat->fn    = fn;
    nat->arity = arity;
    return nat;
}

/* ── ObjMap 할당 ──────────────────────────────────── */

ObjMap *vm_alloc_map(VM *vm) {
    ObjMap *map = calloc(1, sizeof(ObjMap));
    map->base.kind   = OBJ_MAP;
    map->base.marked = false;
    map->base.next   = vm->all_objects;
    vm->all_objects  = (Obj*)map;
    vm->bytes_allocated += sizeof(ObjMap);
    return map;
}

void vm_map_set(VM *vm, ObjMap *map, Value key, Value val) {
    /* 선형 탐색 (작은 맵에서 충분히 빠름; 실제론 해시로 교체) */
    for (int i = 0; i < map->count; i++) {
        if (val_equal(map->keys[i], key)) {
            map->vals[i] = val;
            return;
        }
    }
    if (map->count >= map->cap) {
        int nc = map->cap ? map->cap * 2 : 8;
        map->keys = realloc(map->keys, nc * sizeof(Value));
        map->vals = realloc(map->vals, nc * sizeof(Value));
        map->cap  = nc;
        vm->bytes_allocated += (nc - map->cap/2) * 2 * sizeof(Value);
    }
    map->keys[map->count] = key;
    map->vals[map->count] = val;
    map->count++;
}

Value vm_map_get(ObjMap *map, Value key) {
    for (int i = 0; i < map->count; i++)
        if (val_equal(map->keys[i], key)) return map->vals[i];
    return val_nil();
}

/* ── ObjExternal 할당 ─────────────────────────────── */

ObjExternal *vm_alloc_external(VM *vm, void *handle,
                                ExternalFinalizer fin, const char *type_name) {
    ObjExternal *ext = calloc(1, sizeof(ObjExternal));
    ext->base.kind   = OBJ_EXTERNAL;
    ext->base.marked = false;
    ext->base.next   = vm->all_objects;
    vm->all_objects  = (Obj*)ext;
    vm->bytes_allocated += sizeof(ObjExternal);

    ext->handle    = handle;
    ext->finalizer = fin;
    ext->type_name = type_name;
    return ext;
}

/* ── 핵심 API: vm_define_native ───────────────────────
 *
 * @fl 어노테이션 처리기가 이 함수를 호출한다.
 * FreeLang 런타임 초기화 시 한 번만 실행하면
 * 이후 우리 언어에서 네이티브 함수처럼 쓸 수 있다.
 */
void vm_define_native(VM *vm, const char *name, NativeFn fn, int arity) {
    /* GC 안전: 문자열과 native 객체 모두 임시 스택에 올려두기 */
    ObjString *name_str = vm_alloc_string(vm, name, strlen(name));
    vm_push(vm, val_obj((Obj*)name_str));
    ObjNative *nat = vm_alloc_native(vm, name, fn, arity);
    vm_push(vm, val_obj((Obj*)nat));

    /* 전역 변수로 등록 */
    if (vm->global_count >= vm->global_cap) {
        int nc = vm->global_cap ? vm->global_cap * 2 : 16;
        vm->global_names  = realloc(vm->global_names,  nc * sizeof(char*));
        vm->global_values = realloc(vm->global_values, nc * sizeof(Value));
        vm->global_cap = nc;
    }
    /* 이미 같은 이름이 있으면 덮어쓰기 */
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name) == 0) {
            vm->global_values[i] = val_obj((Obj*)nat);
            vm_pop(vm); vm_pop(vm);
            return;
        }
    }
    vm->global_names[vm->global_count]  = strdup(name);
    vm->global_values[vm->global_count] = val_obj((Obj*)nat);
    vm->global_count++;
    vm_pop(vm); vm_pop(vm);
}

/* ================================================================
 * 기본 내장 네이티브 함수들
 * FreeLang 없이도 기본 동작하는 것들.
 * FreeLang 버전은 나중에 같은 이름으로 덮어쓴다.
 * ================================================================ */

static Value native_print(VM *vm, int argc, Value *argv) {
    (void)vm;
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        val_print(argv[i], stdout);
    }
    printf("\n");
    return val_nil();
}

static Value native_str(VM *vm, int argc, Value *argv) {
    if (argc < 1) return val_obj((Obj*)vm_alloc_string(vm, "", 0));
    char buf[64];
    int  len = 0;
    Value v = argv[0];
    if (v.kind == VAL_INT)   len = snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
    if (v.kind == VAL_FLOAT) len = snprintf(buf, sizeof(buf), "%g", v.f);
    if (v.kind == VAL_BOOL)  len = snprintf(buf, sizeof(buf), "%s", v.b ? "true" : "false");
    if (v.kind == VAL_NIL)   len = snprintf(buf, sizeof(buf), "nil");
    if (v.kind == VAL_OBJ && v.obj->kind == OBJ_STRING)
        return v;
    return val_obj((Obj*)vm_alloc_string(vm, buf, len));
}

static Value native_len(VM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 1) return val_int(0);
    Value v = argv[0];
    if (val_is_obj_kind(v, OBJ_STRING)) return val_int((int64_t)AS_STRING(v)->len);
    if (val_is_obj_kind(v, OBJ_ARRAY))  return val_int((int64_t)AS_ARRAY(v)->len);
    if (val_is_obj_kind(v, OBJ_MAP))    return val_int((int64_t)AS_MAP(v)->count);
    return val_int(0);
}

static Value native_type(VM *vm, int argc, Value *argv) {
    if (argc < 1) return val_obj((Obj*)vm_alloc_string(vm, "nil", 3));
    const char *s = "nil";
    Value v = argv[0];
    switch (v.kind) {
    case VAL_NIL:   s = "nil";    break;
    case VAL_BOOL:  s = "bool";   break;
    case VAL_INT:   s = "int";    break;
    case VAL_FLOAT: s = "float";  break;
    case VAL_OBJ:
        switch (v.obj->kind) {
        case OBJ_STRING:   s = "string";   break;
        case OBJ_ARRAY:    s = "array";    break;
        case OBJ_MAP:      s = "map";      break;
        case OBJ_CLOSURE:  s = "function"; break;
        case OBJ_NATIVE:   s = "native";   break;
        case OBJ_FIBER:    s = "fiber";    break;
        case OBJ_EXTERNAL: s = ((ObjExternal*)v.obj)->type_name; break;
        default:           s = "object";   break;
        }
    }
    return val_obj((Obj*)vm_alloc_string(vm, s, strlen(s)));
}

static Value native_assert(VM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 1 || !val_truthy(argv[0])) {
        const char *msg = "assertion failed";
        if (argc >= 2 && val_is_obj_kind(argv[1], OBJ_STRING))
            msg = AS_STRING(argv[1])->chars;
        fprintf(stderr, "AssertionError: %s\n", msg);
        /* THROW처럼 처리하려면 vm이 필요하지만 여기선 단순화 */
        return val_bool(false);
    }
    return val_bool(true);
}

/* ── vm_register_builtins: VM 초기화 시 한 번 호출 ── */

void vm_register_builtins(VM *vm) {
    vm_define_native(vm, "print",  native_print,  -1); /* 가변 인수 */
    vm_define_native(vm, "str",    native_str,     1);
    vm_define_native(vm, "len",    native_len,     1);
    vm_define_native(vm, "type",   native_type,    1);
    vm_define_native(vm, "assert", native_assert, -1);
}
