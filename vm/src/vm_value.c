/*
 * vm_value.c — Value 출력 및 객체 생명주기
 */
#include "../include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void val_print(Value v, FILE *out) {
    switch (v.kind) {
    case VAL_NIL:   fprintf(out, "nil"); break;
    case VAL_BOOL:  fprintf(out, "%s", v.b ? "true" : "false"); break;
    case VAL_INT:   fprintf(out, "%lld", (long long)v.i); break;
    case VAL_FLOAT: fprintf(out, "%g", v.f); break;
    case VAL_OBJ:
        switch (v.obj->kind) {
        case OBJ_STRING:   fprintf(out, "%s", ((ObjString*)v.obj)->chars); break;
        case OBJ_ARRAY:    fprintf(out, "[Array len=%d]", ((ObjArray*)v.obj)->len); break;
        case OBJ_FUNC:     fprintf(out, "<fn %s>", ((ObjFunc*)v.obj)->name ?: "?"); break;
        case OBJ_CLOSURE:  fprintf(out, "<closure %s>",
                                   ((ObjClosure*)v.obj)->func->name ?: "?"); break;
        case OBJ_UPVAL:    fprintf(out, "<upval>"); break;
        case OBJ_CLASS:    fprintf(out, "<class %s>", ((ObjClass*)v.obj)->name ?: "?"); break;
        case OBJ_INSTANCE: fprintf(out, "<instance of %s>",
                                   ((ObjInstance*)v.obj)->klass->name ?: "?"); break;
        case OBJ_NATIVE:   fprintf(out, "<native %s>", ((ObjNative*)v.obj)->name ?: "?"); break;
        case OBJ_MAP:      fprintf(out, "<map n=%d>",  ((ObjMap*)v.obj)->count); break;
        case OBJ_FIBER:    fprintf(out, "<fiber>"); break;
        case OBJ_EXTERNAL: fprintf(out, "<external %s>",
                                   ((ObjExternal*)v.obj)->type_name ?: "?"); break;
        }
        break;
    }
}
