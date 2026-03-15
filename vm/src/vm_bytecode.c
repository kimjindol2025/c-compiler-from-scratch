/*
 * vm_bytecode.c — 바이트코드 방출 헬퍼 및 디스어셈블러
 */
#include "../include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *opcode_names[OP_COUNT] = {
    [OP_LOAD_CONST]       = "LOAD_CONST",
    [OP_LOAD_NIL]         = "LOAD_NIL",
    [OP_LOAD_TRUE]        = "LOAD_TRUE",
    [OP_LOAD_FALSE]       = "LOAD_FALSE",
    [OP_LOAD_LOCAL]       = "LOAD_LOCAL",
    [OP_STORE_LOCAL]      = "STORE_LOCAL",
    [OP_LOAD_GLOBAL]      = "LOAD_GLOBAL",
    [OP_STORE_GLOBAL]     = "STORE_GLOBAL",
    [OP_LOAD_UPVAL]       = "LOAD_UPVAL",
    [OP_STORE_UPVAL]      = "STORE_UPVAL",
    [OP_CLOSE_UPVAL]      = "CLOSE_UPVAL",
    [OP_POP]              = "POP",
    [OP_DUP]              = "DUP",
    [OP_SWAP]             = "SWAP",
    [OP_ADD]              = "ADD",
    [OP_SUB]              = "SUB",
    [OP_MUL]              = "MUL",
    [OP_DIV]              = "DIV",
    [OP_MOD]              = "MOD",
    [OP_NEG]              = "NEG",
    [OP_POW]              = "POW",
    [OP_EQ]               = "EQ",
    [OP_NE]               = "NE",
    [OP_LT]               = "LT",
    [OP_LE]               = "LE",
    [OP_GT]               = "GT",
    [OP_GE]               = "GE",
    [OP_NOT]              = "NOT",
    [OP_AND]              = "AND",
    [OP_OR]               = "OR",
    [OP_JUMP]             = "JUMP",
    [OP_JUMP_IF_FALSE]    = "JUMP_IF_FALSE",
    [OP_JUMP_IF_TRUE]     = "JUMP_IF_TRUE",
    [OP_POP_JUMP_IF_FALSE]= "POP_JUMP_IF_FALSE",
    [OP_POP_JUMP_IF_TRUE] = "POP_JUMP_IF_TRUE",
    [OP_CALL]             = "CALL",
    [OP_CALL_METHOD]      = "CALL_METHOD",
    [OP_RETURN]           = "RETURN",
    [OP_RETURN_NIL]       = "RETURN_NIL",
    [OP_MAKE_CLOSURE]     = "MAKE_CLOSURE",
    [OP_TRY_BEGIN]        = "TRY_BEGIN",
    [OP_TRY_END]          = "TRY_END",
    [OP_THROW]            = "THROW",
    [OP_RETHROW]          = "RETHROW",
    [OP_MAKE_ARRAY]       = "MAKE_ARRAY",
    [OP_ARRAY_GET]        = "ARRAY_GET",
    [OP_ARRAY_SET]        = "ARRAY_SET",
    [OP_ARRAY_LEN]        = "ARRAY_LEN",
    [OP_GET_FIELD]        = "GET_FIELD",
    [OP_SET_FIELD]        = "SET_FIELD",
    [OP_PRINT]            = "PRINT",
    [OP_HALT]             = "HALT",
};

/* ── 바이트코드 방출 ───────────────────────────────────── */

static void ensure_code(ObjFunc *fn, int needed) {
    if (fn->code_len + needed <= fn->code_cap) return;
    int nc = fn->code_cap ? fn->code_cap * 2 : 64;
    while (nc < fn->code_len + needed) nc *= 2;
    fn->code = realloc(fn->code, nc);
    fn->line_numbers = realloc(fn->line_numbers, nc * sizeof(int));
    fn->code_cap = nc;
}

void emit_byte(ObjFunc *fn, uint8_t byte) {
    ensure_code(fn, 1);
    fn->code[fn->code_len++] = byte;
}

void emit_u16(ObjFunc *fn, uint16_t v) {
    ensure_code(fn, 2);
    fn->code[fn->code_len++] = (uint8_t)(v & 0xFF);
    fn->code[fn->code_len++] = (uint8_t)(v >> 8);
}

void emit_i32(ObjFunc *fn, int32_t v) {
    ensure_code(fn, 4);
    uint32_t u = (uint32_t)v;
    fn->code[fn->code_len++] = u & 0xFF;
    fn->code[fn->code_len++] = (u >> 8)  & 0xFF;
    fn->code[fn->code_len++] = (u >> 16) & 0xFF;
    fn->code[fn->code_len++] = (u >> 24) & 0xFF;
}

/* jump placeholder: opcode + i32(0) → 반환값은 i32 시작 위치 */
int emit_jump(ObjFunc *fn, OpCode op) {
    emit_byte(fn, op);
    int pos = fn->code_len;
    emit_i32(fn, 0);
    return pos;
}

/* patch_pos: emit_jump가 반환한 위치 */
void patch_jump(ObjFunc *fn, int patch_pos) {
    int32_t offset = (int32_t)(fn->code_len - patch_pos - 4);
    uint32_t u = (uint32_t)offset;
    fn->code[patch_pos + 0] = u & 0xFF;
    fn->code[patch_pos + 1] = (u >> 8)  & 0xFF;
    fn->code[patch_pos + 2] = (u >> 16) & 0xFF;
    fn->code[patch_pos + 3] = (u >> 24) & 0xFF;
}

int add_constant(ObjFunc *fn, VM *vm, Value val) {
    if (fn->const_len >= fn->const_cap) {
        int nc = fn->const_cap ? fn->const_cap * 2 : 8;
        fn->constants = realloc(fn->constants, nc * sizeof(Value));
        fn->const_cap = nc;
    }
    /* GC 안전: 상수 추가 중 GC가 돌면 val이 수집될 수 있음.
     * vm_push로 임시 고정 후 추가. */
    vm_push(vm, val);
    int idx = fn->const_len;
    fn->constants[fn->const_len++] = val;
    vm_pop(vm);
    return idx;
}

/* ── 디스어셈블러 ─────────────────────────────────────── */

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static int32_t read_i32(const uint8_t *p) {
    uint32_t u = (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24));
    return (int32_t)u;
}

int disassemble_instruction(const ObjFunc *fn, int offset, FILE *out) {
    fprintf(out, "%04d  ", offset);
    if (offset >= fn->code_len) { fprintf(out, "EOF\n"); return offset + 1; }

    uint8_t op = fn->code[offset++];
    const char *name = (op < OP_COUNT) ? opcode_names[op] : "???";

    switch ((OpCode)op) {
    case OP_LOAD_CONST: {
        uint16_t idx = read_u16(fn->code + offset); offset += 2;
        fprintf(out, "%-20s #%d  ; ", name, idx);
        if (idx < fn->const_len) val_print(fn->constants[idx], out);
        fprintf(out, "\n");
        break;
    }
    case OP_LOAD_LOCAL:
    case OP_STORE_LOCAL:
    case OP_LOAD_UPVAL:
    case OP_STORE_UPVAL:
        fprintf(out, "%-20s %d\n", name, fn->code[offset++]);
        break;
    case OP_LOAD_GLOBAL:
    case OP_STORE_GLOBAL:
        fprintf(out, "%-20s #%d\n", name, read_u16(fn->code + offset));
        offset += 2;
        break;
    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_JUMP_IF_TRUE:
    case OP_POP_JUMP_IF_FALSE:
    case OP_POP_JUMP_IF_TRUE: {
        int32_t off = read_i32(fn->code + offset); offset += 4;
        fprintf(out, "%-20s %+d  ; → %04d\n", name, off, offset + off);
        break;
    }
    case OP_CALL: {
        uint8_t  nargs   = fn->code[offset++];
        uint16_t ic_slot = read_u16(fn->code + offset); offset += 2;
        fprintf(out, "%-20s nargs=%d ic=%d\n", name, nargs, ic_slot);
        break;
    }
    case OP_CALL_METHOD: {
        uint16_t name_idx = read_u16(fn->code + offset); offset += 2;
        uint8_t  nargs    = fn->code[offset++];
        uint16_t ic_slot  = read_u16(fn->code + offset); offset += 2;
        fprintf(out, "%-20s name=#%d nargs=%d ic=%d\n", name, name_idx, nargs, ic_slot);
        break;
    }
    case OP_MAKE_CLOSURE: {
        uint16_t func_idx = read_u16(fn->code + offset); offset += 2;
        uint8_t  n_upvals = fn->code[offset++];
        fprintf(out, "%-20s func=#%d upvals=%d\n", name, func_idx, n_upvals);
        for (int i = 0; i < n_upvals; i++) {
            uint8_t is_local = fn->code[offset++];
            uint8_t idx      = fn->code[offset++];
            fprintf(out, "                       [%d] %s #%d\n",
                    i, is_local ? "local" : "upval", idx);
        }
        break;
    }
    case OP_TRY_BEGIN: {
        int32_t catch_off   = read_i32(fn->code + offset); offset += 4;
        int32_t finally_off = read_i32(fn->code + offset); offset += 4;
        fprintf(out, "%-20s catch=→%04d finally=→%04d\n",
                name, offset + catch_off, offset + finally_off);
        break;
    }
    case OP_MAKE_ARRAY: {
        uint16_t n = read_u16(fn->code + offset); offset += 2;
        fprintf(out, "%-20s n=%d\n", name, n);
        break;
    }
    case OP_GET_FIELD:
    case OP_SET_FIELD: {
        uint16_t name_idx = read_u16(fn->code + offset); offset += 2;
        uint16_t ic_slot  = read_u16(fn->code + offset); offset += 2;
        fprintf(out, "%-20s name=#%d ic=%d\n", name, name_idx, ic_slot);
        break;
    }
    default:
        fprintf(out, "%s\n", name);
        break;
    }
    return offset;
}

void disassemble_func(const ObjFunc *fn, FILE *out) {
    fprintf(out, "=== fn:%s arity=%d locals=%d upvals=%d ===\n",
            fn->name ?: "<anon>", fn->arity, fn->max_locals, fn->upval_count);
    int offset = 0;
    while (offset < fn->code_len)
        offset = disassemble_instruction(fn, offset, out);
    fprintf(out, "\n");
}
