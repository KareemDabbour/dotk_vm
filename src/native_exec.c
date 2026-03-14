#include "include/native_exec.h"

#include <stdarg.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include "include/chunk.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum
{
    NIR_PUSH_CONST,
    NIR_PUSH_STR,
    NIR_PUSH_NIL,
    NIR_PUSH_TRUE,
    NIR_PUSH_FALSE,
    NIR_ADD,
    NIR_SUB,
    NIR_MUL,
    NIR_POW,
    NIR_MOD,
    NIR_DIV,
    NIR_INT_DIV,
    NIR_EQ,
    NIR_LT,
    NIR_GT,
    NIR_NOT,
    NIR_BIN_AND,
    NIR_BIN_OR,
    NIR_BIN_XOR,
    NIR_BIN_SHL,
    NIR_BIN_SHR,
    NIR_BUILD_LIST,
    NIR_BUILD_SLICE,
    NIR_BUILD_MAP,
    NIR_INDEX_SUBSCR,
    NIR_STORE_SUBSCR,
    NIR_GET_LOCAL,
    NIR_SET_LOCAL,
    NIR_GET_GLOBAL,
    NIR_SET_GLOBAL,
    NIR_DEF_GLOBAL,
    NIR_DEF_CONST_GLOBAL,
    NIR_EXPORT,
    NIR_IMPORT,
    NIR_MAKE_CLOSURE,
    NIR_CALL,
    NIR_CLASS,
    NIR_METHOD,
    NIR_STATIC_VAR,
    NIR_GET_PROPERTY,
    NIR_SET_PROPERTY,
    NIR_INVOKE,
    NIR_INHERIT,
    NIR_GET_SUPER,
    NIR_SUPER_INVOKE,
    NIR_PUSH_SUPER_THIS,
    NIR_TRY,
    NIR_CATCH,
    NIR_JUMP,
    NIR_JUMP_IF_FALSE,
    NIR_NEG,
    NIR_POP,
    NIR_DUP,
    NIR_PRINT,
    NIR_RET,
    NIR_RET_THIS,
    NIR_RET_NIL,
    NIR_BUILD_DEFAULT_LIST,
} NativeIrOp;

typedef struct
{
    NativeIrOp op;
    double number;
    int arg;
    int arg2;
    int sourceIp;
} NativeIrInst;

typedef struct
{
    NativeIrInst *items;
    int count;
    int capacity;
    int maxDepth;
} NativeIrVec;

typedef struct
{
    uint8_t *bytes;
    size_t len;
    size_t cap;
} CodeBuf;

static int gIrSourceIpForPush = 0;

static bool irPush(NativeIrVec *ir, NativeIrInst inst)
{
    if (ir->count + 1 > ir->capacity)
    {
        int next = ir->capacity < 16 ? 16 : ir->capacity * 2;
        NativeIrInst *grown = (NativeIrInst *)realloc(ir->items, (size_t)next * sizeof(NativeIrInst));
        if (grown == NULL)
            return false;
        ir->items = grown;
        ir->capacity = next;
    }
    inst.sourceIp = gIrSourceIpForPush;
    ir->items[ir->count++] = inst;
    return true;
}

static bool codeEnsure(CodeBuf *cb, size_t needed)
{
    if (cb->len + needed <= cb->cap)
        return true;
    size_t next = cb->cap == 0 ? 256 : cb->cap * 2;
    while (next < cb->len + needed)
        next *= 2;
    uint8_t *grown = (uint8_t *)realloc(cb->bytes, next);
    if (grown == NULL)
        return false;
    cb->bytes = grown;
    cb->cap = next;
    return true;
}

static bool emit1(CodeBuf *cb, uint8_t b)
{
    if (!codeEnsure(cb, 1))
        return false;
    cb->bytes[cb->len++] = b;
    return true;
}

static bool emit4(CodeBuf *cb, uint32_t v)
{
    if (!codeEnsure(cb, 4))
        return false;
    cb->bytes[cb->len++] = (uint8_t)(v & 0xff);
    cb->bytes[cb->len++] = (uint8_t)((v >> 8) & 0xff);
    cb->bytes[cb->len++] = (uint8_t)((v >> 16) & 0xff);
    cb->bytes[cb->len++] = (uint8_t)((v >> 24) & 0xff);
    return true;
}

static bool emit8(CodeBuf *cb, uint64_t v)
{
    if (!codeEnsure(cb, 8))
        return false;
    for (int i = 0; i < 8; i++)
        cb->bytes[cb->len++] = (uint8_t)((v >> (i * 8)) & 0xff);
    return true;
}

static bool emitMovRaxImm64(CodeBuf *cb, uint64_t imm)
{
    return emit1(cb, 0x48) && emit1(cb, 0xB8) && emit8(cb, imm);
}

static bool emitMovQXmm1Rax(CodeBuf *cb)
{
    return emit1(cb, 0x66) && emit1(cb, 0x48) && emit1(cb, 0x0F) && emit1(cb, 0x6E) && emit1(cb, 0xC8);
}

static bool emitBinaryXmm(CodeBuf *cb, uint8_t op)
{
    return emit1(cb, 0xF2) && emit1(cb, 0x0F) && emit1(cb, op) && emit1(cb, 0xC1);
}

static bool emitXorPd(CodeBuf *cb)
{
    return emit1(cb, 0x66) && emit1(cb, 0x0F) && emit1(cb, 0x57) && emit1(cb, 0xC1);
}

static bool emitLoadXmm0FromSlot(CodeBuf *cb, int slotOffset)
{
    if (slotOffset <= 127)
        return emit1(cb, 0xF2) && emit1(cb, 0x0F) && emit1(cb, 0x10) && emit1(cb, 0x45) && emit1(cb, (uint8_t)(-slotOffset));
    return emit1(cb, 0xF2) && emit1(cb, 0x0F) && emit1(cb, 0x10) && emit1(cb, 0x85) && emit4(cb, (uint32_t)(-(int32_t)slotOffset));
}

static bool emitLoadXmm1FromSlot(CodeBuf *cb, int slotOffset)
{
    if (slotOffset <= 127)
        return emit1(cb, 0xF2) && emit1(cb, 0x0F) && emit1(cb, 0x10) && emit1(cb, 0x4D) && emit1(cb, (uint8_t)(-slotOffset));
    return emit1(cb, 0xF2) && emit1(cb, 0x0F) && emit1(cb, 0x10) && emit1(cb, 0x8D) && emit4(cb, (uint32_t)(-(int32_t)slotOffset));
}

static bool emitStoreRaxToSlot(CodeBuf *cb, int slotOffset)
{
    if (slotOffset <= 127)
        return emit1(cb, 0x48) && emit1(cb, 0x89) && emit1(cb, 0x45) && emit1(cb, (uint8_t)(-slotOffset));
    return emit1(cb, 0x48) && emit1(cb, 0x89) && emit1(cb, 0x85) && emit4(cb, (uint32_t)(-(int32_t)slotOffset));
}

static bool emitStoreXmm0ToSlot(CodeBuf *cb, int slotOffset)
{
    if (slotOffset <= 127)
        return emit1(cb, 0xF2) && emit1(cb, 0x0F) && emit1(cb, 0x11) && emit1(cb, 0x45) && emit1(cb, (uint8_t)(-slotOffset));
    return emit1(cb, 0xF2) && emit1(cb, 0x0F) && emit1(cb, 0x11) && emit1(cb, 0x85) && emit4(cb, (uint32_t)(-(int32_t)slotOffset));
}

static int normalizeJumpTarget(const Chunk *chunk, int target)
{
    if (chunk == NULL)
        return target;
    if (target < 0)
        return target;
    if (target >= chunk->size)
        return target;

    while (target < chunk->size)
    {
        uint8_t op = chunk->code[target];
        if (op == OP_WIDE || op == OP_NOP)
        {
            target++;
            continue;
        }
        break;
    }
    return target;
}

static int resolveJumpTargetToEmittedIp(const NativeIrVec *ir, int codeSize, int target)
{
    if (ir == NULL)
        return target;
    if (target < 0)
        return target;
    if (codeSize < 0)
        return target;
    if (target > codeSize)
        target = codeSize;

    for (int t = target; t < codeSize; t++)
    {
        for (int i = 0; i < ir->count; i++)
        {
            if (ir->items[i].sourceIp == t)
                return t;
        }
    }
    return codeSize;
}

static bool parseBytecodeToNativeIr(ObjFunction *function, NativeIrVec *ir)
{
    Chunk *chunk = &function->chunk;
    int ip = 0;
    bool wideForNext = false;
    int depth = 0;
    bool sawReturn = false;
    bool sawControlFlow = false;

#define REQUIRE_DEPTH_CF(minDepth) \
    do                             \
    {                              \
        if (depth < (minDepth))    \
        {                          \
            if (!sawControlFlow)   \
                return false;      \
            depth = (minDepth);    \
        }                          \
    } while (0)

    while (ip < chunk->size)
    {
        int opStart = ip;
        gIrSourceIpForPush = opStart;
        uint8_t op = chunk->code[ip++];
        switch (op)
        {
        case OP_WIDE:
            wideForNext = true;
            break;
        case OP_NOP:
            wideForNext = false;
            break;
        case OP_CONSTANT:
        {
            int idx;
            if (wideForNext)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (chunk->code[ip] << 8) | chunk->code[ip + 1];
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            wideForNext = false;
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            Value c = chunk->constants.values[idx];
            NativeIrInst pushInst;
            if (IS_NUM(c))
                pushInst = (NativeIrInst){.op = NIR_PUSH_CONST, .number = AS_NUM(c)};
            else if (IS_STR(c))
                pushInst = (NativeIrInst){.op = NIR_PUSH_STR, .number = 0, .arg = idx};
            else if (IS_BOOL(c))
                pushInst = (NativeIrInst){.op = AS_BOOL(c) ? NIR_PUSH_TRUE : NIR_PUSH_FALSE, .number = AS_BOOL(c) ? 1 : 0};
            else if (IS_NIL(c))
                pushInst = (NativeIrInst){.op = NIR_PUSH_NIL, .number = 0};
            else
                return false;
            if (!irPush(ir, pushInst))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_CONSTANT_LONG:
        {
            if (ip + 2 >= chunk->size)
                return false;
            int idx = (chunk->code[ip] & 0xff) | ((chunk->code[ip + 1] << 8) & 0xff00) | ((chunk->code[ip + 2] << 16) & 0xff0000);
            ip += 3;
            wideForNext = false;
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            Value c = chunk->constants.values[idx];
            NativeIrInst pushInst;
            if (IS_NUM(c))
                pushInst = (NativeIrInst){.op = NIR_PUSH_CONST, .number = AS_NUM(c)};
            else if (IS_STR(c))
                pushInst = (NativeIrInst){.op = NIR_PUSH_STR, .number = 0, .arg = idx};
            else if (IS_BOOL(c))
                pushInst = (NativeIrInst){.op = AS_BOOL(c) ? NIR_PUSH_TRUE : NIR_PUSH_FALSE, .number = AS_BOOL(c) ? 1 : 0};
            else if (IS_NIL(c))
                pushInst = (NativeIrInst){.op = NIR_PUSH_NIL, .number = 0};
            else
                return false;
            if (!irPush(ir, pushInst))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_NIL:
            wideForNext = false;
            if (!irPush(ir, (NativeIrInst){.op = NIR_PUSH_NIL, .number = 0}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        case OP_TRUE:
            wideForNext = false;
            if (!irPush(ir, (NativeIrInst){.op = NIR_PUSH_TRUE, .number = 1}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        case OP_FALSE:
            wideForNext = false;
            if (!irPush(ir, (NativeIrInst){.op = NIR_PUSH_FALSE, .number = 0}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        case OP_ADD:
        case OP_SUB:
        case OP_MULT:
        case OP_POW:
        case OP_MOD:
        case OP_DIV:
        case OP_INT_DIV:
        case OP_EQUAL:
        case OP_LESS:
        case OP_GREATER:
        case OP_BIN_AND:
        case OP_BIN_OR:
        case OP_BIN_XOR:
        case OP_BIN_SHIFT_LEFT:
        case OP_BIN_SHIFT_RIGHT:
            wideForNext = false;
            if (depth < 2)
            {
                if (!sawControlFlow)
                    return false;
                depth = 2;
            }
            if (!irPush(ir, (NativeIrInst){.op = op == OP_ADD               ? NIR_ADD
                                                 : op == OP_SUB             ? NIR_SUB
                                                 : op == OP_MULT            ? NIR_MUL
                                                 : op == OP_POW             ? NIR_POW
                                                 : op == OP_MOD             ? NIR_MOD
                                                 : op == OP_DIV             ? NIR_DIV
                                                 : op == OP_INT_DIV         ? NIR_INT_DIV
                                                 : op == OP_EQUAL           ? NIR_EQ
                                                 : op == OP_LESS            ? NIR_LT
                                                 : op == OP_GREATER         ? NIR_GT
                                                 : op == OP_BIN_AND         ? NIR_BIN_AND
                                                 : op == OP_BIN_OR          ? NIR_BIN_OR
                                                 : op == OP_BIN_XOR         ? NIR_BIN_XOR
                                                 : op == OP_BIN_SHIFT_LEFT  ? NIR_BIN_SHL
                                                 : op == OP_BIN_SHIFT_RIGHT ? NIR_BIN_SHR
                                                                            : NIR_DIV,
                                           .number = 0}))
                return false;
            depth--;
            if (depth < 0)
                depth = 0;
            break;
        case OP_GET_LOCAL:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t slot;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                slot = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                slot = chunk->code[ip++];
            }
            if (!irPush(ir, (NativeIrInst){.op = NIR_GET_LOCAL, .number = 0, .arg = (int)slot}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_SET_LOCAL:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t slot;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                slot = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                slot = chunk->code[ip++];
            }
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_SET_LOCAL, .number = 0, .arg = (int)slot}))
                return false;
            break;
        }
        case OP_DEF_GLOBAL:
        case OP_DEF_CONST_GLOBAL:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = op == OP_DEF_CONST_GLOBAL ? NIR_DEF_CONST_GLOBAL : NIR_DEF_GLOBAL, .number = 0, .arg = (int)idx}))
                return false;
            depth--;
            break;
        }
        case OP_GET_GLOBAL:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (!irPush(ir, (NativeIrInst){.op = NIR_GET_GLOBAL, .number = 0, .arg = (int)idx}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_SET_GLOBAL:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_SET_GLOBAL, .number = 0, .arg = (int)idx}))
                return false;
            break;
        }
        case OP_EXPORT:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            if (!irPush(ir, (NativeIrInst){.op = NIR_EXPORT, .number = 0, .arg = (int)idx}))
                return false;
            break;
        }
        case OP_IMPORT:
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_IMPORT, .number = 0, .arg = 0}))
                return false;
            break;
        case OP_CLOSURE:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            Value cv = chunk->constants.values[idx];
            if (!IS_FUN(cv))
                return false;
            ObjFunction *inner = AS_FUN(cv);
            if (!irPush(ir, (NativeIrInst){.op = NIR_MAKE_CLOSURE, .number = 0, .arg = (int)idx}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            for (int u = 0; u < inner->upValueCount; u++)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                ip += 2;
            }
            break;
        }
        case OP_CALL:
        {
            wideForNext = false;
            if (ip >= chunk->size)
                return false;
            int argc = chunk->code[ip++];
            REQUIRE_DEPTH_CF(argc + 1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_CALL, .number = 0, .arg = argc}))
                return false;
            depth -= argc;
            break;
        }
        case OP_CLASS:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            if (!irPush(ir, (NativeIrInst){.op = NIR_CLASS, .number = 0, .arg = (int)idx}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_METHOD:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            REQUIRE_DEPTH_CF(2);
            if (!irPush(ir, (NativeIrInst){.op = NIR_METHOD, .number = 0, .arg = (int)idx}))
                return false;
            depth--;
            break;
        }
        case OP_STATIC_VAR:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            REQUIRE_DEPTH_CF(2);
            if (!irPush(ir, (NativeIrInst){.op = NIR_STATIC_VAR, .number = 0, .arg = (int)idx}))
                return false;
            depth--;
            break;
        }
        case OP_GET_PROPERTY:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_GET_PROPERTY, .number = 0, .arg = (int)idx}))
                return false;
            break;
        }
        case OP_SET_PROPERTY:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            REQUIRE_DEPTH_CF(2);
            if (!irPush(ir, (NativeIrInst){.op = NIR_SET_PROPERTY, .number = 0, .arg = (int)idx}))
                return false;
            depth--;
            break;
        }
        case OP_INVOKE:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (ip >= chunk->size)
                return false;
            int argc = chunk->code[ip++];
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            REQUIRE_DEPTH_CF(argc + 1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_INVOKE, .number = 0, .arg = (int)idx, .arg2 = argc}))
                return false;
            depth -= argc;
            break;
        }
        case OP_INVOKE_KW:
        case OP_SUPER_INVOKE_KW:
            return false;
        case OP_INHERIT:
            wideForNext = false;
            REQUIRE_DEPTH_CF(2);
            if (!irPush(ir, (NativeIrInst){.op = NIR_INHERIT, .number = 0, .arg = 0}))
                return false;
            depth--;
            break;
        case OP_GET_SUPER:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            REQUIRE_DEPTH_CF(2);
            if (!irPush(ir, (NativeIrInst){.op = NIR_GET_SUPER, .number = 0, .arg = (int)idx}))
                return false;
            depth--;
            break;
        }
        case OP_SUPER_INVOKE:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t idx;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                idx = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                idx = chunk->code[ip++];
            }
            if (ip >= chunk->size)
                return false;
            int argc = chunk->code[ip++];
            if (idx < 0 || idx >= chunk->constants.size)
                return false;
            if (!IS_STR(chunk->constants.values[idx]))
                return false;
            REQUIRE_DEPTH_CF(argc + 2);
            if (!irPush(ir, (NativeIrInst){.op = NIR_SUPER_INVOKE, .number = 0, .arg = (int)idx, .arg2 = argc}))
                return false;
            depth -= (argc + 1);
            break;
        }
        case OP_GET_UPVALUE:
        {
            wideForNext = false;
            if (ip >= chunk->size)
                return false;
            uint8_t slot = chunk->code[ip++];
            if (slot != 0)
                return false;
            if (ip >= chunk->size)
                return false;
            uint8_t nextOp = chunk->code[ip];
            if (!(nextOp == OP_SUPER_INVOKE || nextOp == OP_GET_SUPER || nextOp == OP_SUPER_INVOKE_KW))
                return false;
            if (!irPush(ir, (NativeIrInst){.op = NIR_PUSH_SUPER_THIS, .number = 0, .arg = 0}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_CLOSE_UPVALUE:
            wideForNext = false;
            if (depth < 1)
                depth = 1;
            if (!irPush(ir, (NativeIrInst){.op = NIR_POP, .number = 0}))
                return false;
            depth--;
            if (depth < 0)
                depth = 0;
            break;
        case OP_TRY:
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_TRY, .number = 0, .arg = 0}))
                return false;
            break;
        case OP_CATCH:
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_CATCH, .number = 0, .arg = 0}))
                return false;
            break;
        case OP_JUMP:
        {
            wideForNext = false;
            if (ip + 1 >= chunk->size)
                return false;
            sawControlFlow = true;
            uint16_t offset = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
            ip += 2;
            int target = normalizeJumpTarget(chunk, ip + offset);
            if (!irPush(ir, (NativeIrInst){.op = NIR_JUMP, .number = 0, .arg = target}))
                return false;
            break;
        }
        case OP_JUMP_IF_FALSE:
        {
            wideForNext = false;
            if (ip + 1 >= chunk->size)
                return false;
            sawControlFlow = true;
            uint16_t offset = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
            ip += 2;
            int target = normalizeJumpTarget(chunk, ip + offset);
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_JUMP_IF_FALSE, .number = 0, .arg = target}))
                return false;
            break;
        }
        case OP_LOOP:
        {
            wideForNext = false;
            if (ip + 1 >= chunk->size)
                return false;
            sawControlFlow = true;
            uint16_t offset = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
            ip += 2;
            int target = normalizeJumpTarget(chunk, ip - offset);
            if (!irPush(ir, (NativeIrInst){.op = NIR_JUMP, .number = 0, .arg = target}))
                return false;
            break;
        }
        case OP_NOT:
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_NOT, .number = 0}))
                return false;
            break;
        case OP_BUILD_LIST:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t itemCount;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                itemCount = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                itemCount = chunk->code[ip++];
            }
            REQUIRE_DEPTH_CF((int)itemCount);
            if (!irPush(ir, (NativeIrInst){.op = NIR_BUILD_LIST, .number = 0, .arg = (int)itemCount}))
                return false;
            depth = depth - itemCount + 1;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_BUILD_DEFAULT_LIST:
        {
            wideForNext = false;
            if (ip >= chunk->size)
                return false;
            int hasDefault = chunk->code[ip++] ? 1 : 0;
            if (hasDefault)
            {
                REQUIRE_DEPTH_CF(2);
                depth -= 1;
            }
            else
            {
                REQUIRE_DEPTH_CF(1);
            }
            if (!irPush(ir, (NativeIrInst){.op = NIR_BUILD_DEFAULT_LIST, .number = 0, .arg = hasDefault}))
                return false;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_BUILD_SLICE:
            wideForNext = false;
            REQUIRE_DEPTH_CF(3);
            if (!irPush(ir, (NativeIrInst){.op = NIR_BUILD_SLICE, .number = 0, .arg = 0}))
                return false;
            depth -= 2;
            break;
        case OP_BUILD_MAP:
        {
            bool wasWide = wideForNext;
            wideForNext = false;
            uint16_t itemCount;
            if (wasWide)
            {
                if (ip + 1 >= chunk->size)
                    return false;
                itemCount = (uint16_t)((chunk->code[ip] << 8) | chunk->code[ip + 1]);
                ip += 2;
            }
            else
            {
                if (ip >= chunk->size)
                    return false;
                itemCount = chunk->code[ip++];
            }
            REQUIRE_DEPTH_CF((int)(itemCount * 2));
            if (!irPush(ir, (NativeIrInst){.op = NIR_BUILD_MAP, .number = 0, .arg = (int)itemCount}))
                return false;
            depth = depth - (itemCount * 2) + 1;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        }
        case OP_INDEX_SUBSCR:
            wideForNext = false;
            REQUIRE_DEPTH_CF(2);
            if (!irPush(ir, (NativeIrInst){.op = NIR_INDEX_SUBSCR, .number = 0, .arg = 0}))
                return false;
            depth -= 1;
            break;
        case OP_STORE_SUBSCR:
            wideForNext = false;
            REQUIRE_DEPTH_CF(3);
            if (!irPush(ir, (NativeIrInst){.op = NIR_STORE_SUBSCR, .number = 0, .arg = 0}))
                return false;
            depth -= 2;
            break;
        case OP_NEGATE:
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_NEG, .number = 0}))
                return false;
            break;
        case OP_POP:
            wideForNext = false;
            if (depth < 1)
            {
                depth = 1;
            }
            if (!irPush(ir, (NativeIrInst){.op = NIR_POP, .number = 0}))
                return false;
            depth--;
            if (depth < 0)
                depth = 0;
            break;
        case OP_DUP:
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_DUP, .number = 0}))
                return false;
            depth++;
            if (depth > ir->maxDepth)
                ir->maxDepth = depth;
            break;
        case OP_PRINT:
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_PRINT, .number = 0}))
                return false;
            depth--;
            break;
        case OP_RETURN:
        {
            wideForNext = false;
            REQUIRE_DEPTH_CF(1);
            if (!irPush(ir, (NativeIrInst){.op = NIR_RET, .number = 0}))
                return false;
            sawReturn = true;
            bool hasForwardTarget = false;
            for (int j = 0; j < ir->count; j++)
            {
                NativeIrInst prev = ir->items[j];
                if ((prev.op == NIR_JUMP || prev.op == NIR_JUMP_IF_FALSE) && prev.arg > opStart)
                {
                    hasForwardTarget = true;
                    break;
                }
            }
            if (!hasForwardTarget)
                return sawReturn;
            depth = 0;
            break;
        }
        case OP_RETURN_THIS:
        {
            wideForNext = false;
            if (!irPush(ir, (NativeIrInst){.op = NIR_RET_THIS, .number = 0}))
                return false;
            sawReturn = true;
            bool hasForwardTarget = false;
            for (int j = 0; j < ir->count; j++)
            {
                NativeIrInst prev = ir->items[j];
                if ((prev.op == NIR_JUMP || prev.op == NIR_JUMP_IF_FALSE) && prev.arg > opStart)
                {
                    hasForwardTarget = true;
                    break;
                }
            }
            if (!hasForwardTarget)
                return sawReturn;
            depth = 0;
            break;
        }
        case OP_RETURN_NIL:
        {
            wideForNext = false;
            if (depth != 0)
            {
                if (!sawControlFlow)
                    return false;
                depth = 0;
            }
            if (!irPush(ir, (NativeIrInst){.op = NIR_RET_NIL, .number = 0}))
                return false;
            sawReturn = true;
            bool hasForwardTarget = false;
            for (int j = 0; j < ir->count; j++)
            {
                NativeIrInst prev = ir->items[j];
                if ((prev.op == NIR_JUMP || prev.op == NIR_JUMP_IF_FALSE) && prev.arg > opStart)
                {
                    hasForwardTarget = true;
                    break;
                }
            }
            if (!hasForwardTarget)
                return sawReturn;
            depth = 0;
            break;
        }
        default:
            return false;
        }
    }

#undef REQUIRE_DEPTH_CF
    return sawReturn;
}

static const char *irOpName(NativeIrOp op)
{
    switch (op)
    {
    case NIR_PUSH_CONST:
        return "push_const";
    case NIR_PUSH_STR:
        return "push_str";
    case NIR_PUSH_NIL:
        return "push_nil";
    case NIR_PUSH_TRUE:
        return "push_true";
    case NIR_PUSH_FALSE:
        return "push_false";
    case NIR_ADD:
        return "add";
    case NIR_SUB:
        return "sub";
    case NIR_MUL:
        return "mul";
    case NIR_POW:
        return "pow";
    case NIR_MOD:
        return "mod";
    case NIR_DIV:
        return "div";
    case NIR_INT_DIV:
        return "int_div";
    case NIR_EQ:
        return "eq";
    case NIR_LT:
        return "lt";
    case NIR_GT:
        return "gt";
    case NIR_NOT:
        return "not";
    case NIR_BIN_AND:
        return "bin_and";
    case NIR_BIN_OR:
        return "bin_or";
    case NIR_BIN_XOR:
        return "bin_xor";
    case NIR_BIN_SHL:
        return "bin_shl";
    case NIR_BIN_SHR:
        return "bin_shr";
    case NIR_BUILD_LIST:
        return "build_list";
    case NIR_BUILD_DEFAULT_LIST:
        return "build_default_list";
    case NIR_BUILD_SLICE:
        return "build_slice";
    case NIR_BUILD_MAP:
        return "build_map";
    case NIR_INDEX_SUBSCR:
        return "index_subscr";
    case NIR_STORE_SUBSCR:
        return "store_subscr";
    case NIR_GET_LOCAL:
        return "get_local";
    case NIR_SET_LOCAL:
        return "set_local";
    case NIR_GET_GLOBAL:
        return "get_global";
    case NIR_SET_GLOBAL:
        return "set_global";
    case NIR_DEF_GLOBAL:
        return "def_global";
    case NIR_DEF_CONST_GLOBAL:
        return "def_const_global";
    case NIR_EXPORT:
        return "export";
    case NIR_IMPORT:
        return "import";
    case NIR_MAKE_CLOSURE:
        return "make_closure";
    case NIR_CALL:
        return "call";
    case NIR_CLASS:
        return "class";
    case NIR_METHOD:
        return "method";
    case NIR_STATIC_VAR:
        return "static_var";
    case NIR_GET_PROPERTY:
        return "get_property";
    case NIR_SET_PROPERTY:
        return "set_property";
    case NIR_INVOKE:
        return "invoke";
    case NIR_INHERIT:
        return "inherit";
    case NIR_GET_SUPER:
        return "get_super";
    case NIR_SUPER_INVOKE:
        return "super_invoke";
    case NIR_PUSH_SUPER_THIS:
        return "push_super_this";
    case NIR_TRY:
        return "try";
    case NIR_CATCH:
        return "catch";
    case NIR_JUMP:
        return "jump";
    case NIR_JUMP_IF_FALSE:
        return "jump_if_false";
    case NIR_NEG:
        return "neg";
    case NIR_POP:
        return "pop";
    case NIR_DUP:
        return "dup";
    case NIR_PRINT:
        return "print";
    case NIR_RET:
        return "ret";
    case NIR_RET_THIS:
        return "ret_this";
    case NIR_RET_NIL:
        return "ret_nil";
    }
    return "unknown";
}

bool nativeDumpFunctionIr(ObjFunction *function, FILE *out)
{
    if (out == NULL)
        out = stderr;
    if (function == NULL)
    {
        fprintf(out, "[native-ir] <null function>\n");
        return false;
    }

    const char *fnName = function->name != NULL ? function->name->chars : "<script>";
    fprintf(out, "[native-ir] function=%s\n", fnName);

    NativeIrVec ir = {0};
    bool ok = parseBytecodeToNativeIr(function, &ir);
    if (!ok)
    {
        fprintf(out, "[native-ir] unsupported bytecode for native lowering; VM fallback will be used\n");
        free(ir.items);
        return false;
    }

    fprintf(out, "[native-ir] supported=true instructions=%d max_stack=%d\n", ir.count, ir.maxDepth);
    for (int i = 0; i < ir.count; i++)
    {
        NativeIrInst inst = ir.items[i];
        if (inst.op == NIR_PUSH_CONST)
            fprintf(out, "  %03d  %-10s %.17g\n", i, irOpName(inst.op), inst.number);
        else if (inst.op == NIR_PUSH_STR)
            fprintf(out, "  %03d  %-10s const[%d]\n", i, irOpName(inst.op), inst.arg);
        else if (inst.op == NIR_GET_LOCAL || inst.op == NIR_SET_LOCAL ||
                 inst.op == NIR_GET_GLOBAL || inst.op == NIR_SET_GLOBAL || inst.op == NIR_DEF_GLOBAL || inst.op == NIR_DEF_CONST_GLOBAL ||
                 inst.op == NIR_MAKE_CLOSURE || inst.op == NIR_CALL || inst.op == NIR_BUILD_LIST || inst.op == NIR_BUILD_MAP ||
                 inst.op == NIR_CLASS || inst.op == NIR_METHOD || inst.op == NIR_STATIC_VAR ||
                 inst.op == NIR_GET_PROPERTY || inst.op == NIR_SET_PROPERTY || inst.op == NIR_GET_SUPER || inst.op == NIR_PUSH_SUPER_THIS ||
                 inst.op == NIR_JUMP || inst.op == NIR_JUMP_IF_FALSE)
            fprintf(out, "  %03d  %-10s %d\n", i, irOpName(inst.op), inst.arg);
        else if (inst.op == NIR_INVOKE || inst.op == NIR_SUPER_INVOKE)
            fprintf(out, "  %03d  %-10s name=%d argc=%d\n", i, irOpName(inst.op), inst.arg, inst.arg2);
        else
            fprintf(out, "  %03d  %s\n", i, irOpName(inst.op));
    }

    free(ir.items);
    return true;
}

static bool compileNativeMachineCode(const NativeIrVec *ir, CodeBuf *cb)
{
    int stackBytes = ir->maxDepth * 8;
    if (stackBytes > 0)
    {
        int rem = stackBytes % 16;
        if (rem != 0)
            stackBytes += (16 - rem);
    }

    if (!emit1(cb, 0x55) || !emit1(cb, 0x48) || !emit1(cb, 0x89) || !emit1(cb, 0xE5))
        return false;

    if (stackBytes > 0)
    {
        if (stackBytes <= 127)
        {
            if (!emit1(cb, 0x48) || !emit1(cb, 0x83) || !emit1(cb, 0xEC) || !emit1(cb, (uint8_t)stackBytes))
                return false;
        }
        else
        {
            if (!emit1(cb, 0x48) || !emit1(cb, 0x81) || !emit1(cb, 0xEC) || !emit4(cb, (uint32_t)stackBytes))
                return false;
        }
    }

    int depth = 0;
    for (int i = 0; i < ir->count; i++)
    {
        NativeIrInst inst = ir->items[i];
        int slotA;
        int slotB;
        uint64_t bits;

        switch (inst.op)
        {
        case NIR_PUSH_CONST:
            slotA = (depth + 1) * 8;
            memcpy(&bits, &inst.number, sizeof(bits));
            if (!emitMovRaxImm64(cb, bits) || !emitStoreRaxToSlot(cb, slotA))
                return false;
            depth++;
            break;
        case NIR_PUSH_STR:
        case NIR_PUSH_NIL:
        case NIR_PUSH_TRUE:
        case NIR_PUSH_FALSE:
        case NIR_POW:
        case NIR_MOD:
        case NIR_INT_DIV:
        case NIR_EQ:
        case NIR_LT:
        case NIR_GT:
        case NIR_NOT:
        case NIR_BIN_AND:
        case NIR_BIN_OR:
        case NIR_BIN_XOR:
        case NIR_BIN_SHL:
        case NIR_BIN_SHR:
        case NIR_BUILD_LIST:
        case NIR_BUILD_SLICE:
        case NIR_BUILD_MAP:
        case NIR_INDEX_SUBSCR:
        case NIR_STORE_SUBSCR:
        case NIR_GET_LOCAL:
        case NIR_SET_LOCAL:
        case NIR_GET_GLOBAL:
        case NIR_SET_GLOBAL:
        case NIR_DEF_GLOBAL:
        case NIR_DEF_CONST_GLOBAL:
        case NIR_EXPORT:
        case NIR_IMPORT:
        case NIR_CLASS:
        case NIR_METHOD:
        case NIR_STATIC_VAR:
        case NIR_GET_PROPERTY:
        case NIR_SET_PROPERTY:
        case NIR_INVOKE:
        case NIR_INHERIT:
        case NIR_GET_SUPER:
        case NIR_SUPER_INVOKE:
        case NIR_PUSH_SUPER_THIS:
        case NIR_JUMP:
        case NIR_JUMP_IF_FALSE:
        case NIR_BUILD_DEFAULT_LIST:
            return false;
        case NIR_ADD:
        case NIR_SUB:
        case NIR_MUL:
        case NIR_DIV:
            if (depth < 2)
                return false;
            slotA = (depth - 1) * 8;
            slotB = (depth) * 8;
            if (!emitLoadXmm0FromSlot(cb, slotA) || !emitLoadXmm1FromSlot(cb, slotB))
                return false;
            if (inst.op == NIR_ADD)
            {
                if (!emitBinaryXmm(cb, 0x58))
                    return false;
            }
            else if (inst.op == NIR_SUB)
            {
                if (!emitBinaryXmm(cb, 0x5C))
                    return false;
            }
            else if (inst.op == NIR_MUL)
            {
                if (!emitBinaryXmm(cb, 0x59))
                    return false;
            }
            else
            {
                if (!emitBinaryXmm(cb, 0x5E))
                    return false;
            }
            if (!emitStoreXmm0ToSlot(cb, slotA))
                return false;
            depth--;
            break;
        case NIR_NEG:
            if (depth < 1)
                return false;
            slotA = depth * 8;
            if (!emitLoadXmm0FromSlot(cb, slotA))
                return false;
            if (!emitMovRaxImm64(cb, 0x8000000000000000ULL) || !emitMovQXmm1Rax(cb) || !emitXorPd(cb))
                return false;
            if (!emitStoreXmm0ToSlot(cb, slotA))
                return false;
            break;
        case NIR_POP:
            if (depth < 1)
                return false;
            depth--;
            break;
        case NIR_DUP:
            if (depth < 1)
                return false;
            slotA = depth * 8;
            slotB = (depth + 1) * 8;
            if (!emitLoadXmm0FromSlot(cb, slotA) || !emitStoreXmm0ToSlot(cb, slotB))
                return false;
            depth++;
            break;
        case NIR_RET:
            if (depth < 1)
                return false;
            slotA = depth * 8;
            if (!emitLoadXmm0FromSlot(cb, slotA) || !emit1(cb, 0xC9) || !emit1(cb, 0xC3))
                return false;
            break;
        case NIR_PRINT:
        case NIR_RET_THIS:
        case NIR_RET_NIL:
            return false;
        }
    }
    return true;
}

static bool writeAll(FILE *f, const char *text)
{
    return fputs(text, f) >= 0;
}

static int resolveGlobalSlot(ObjFunction *function, int constIdx, int *slotByConst, int *nextSlot)
{
    if (function == NULL || slotByConst == NULL || nextSlot == NULL)
        return -1;
    if (constIdx < 0 || constIdx >= function->chunk.constants.size)
        return -1;
    if (slotByConst[constIdx] >= 0)
        return slotByConst[constIdx];

    Value key = function->chunk.constants.values[constIdx];
    if (!IS_STR(key))
        return -1;
    ObjString *name = AS_STR(key);

    for (int i = 0; i < function->chunk.constants.size; i++)
    {
        if (slotByConst[i] < 0)
            continue;
        Value probe = function->chunk.constants.values[i];
        if (!IS_STR(probe))
            continue;
        ObjString *probeName = AS_STR(probe);
        if (probeName->len == name->len && memcmp(probeName->chars, name->chars, (size_t)name->len) == 0)
        {
            slotByConst[constIdx] = slotByConst[i];
            return slotByConst[constIdx];
        }
    }

    int slot = *nextSlot;
    (*nextSlot)++;
    slotByConst[constIdx] = slot;
    return slot;
}

static int resolveGlobalSlotByName(ObjString *name, ObjString ***names, int *count, int *cap)
{
    if (name == NULL || names == NULL || count == NULL || cap == NULL)
        return -1;

    for (int i = 0; i < *count; i++)
    {
        ObjString *existing = (*names)[i];
        if (existing->len == name->len && memcmp(existing->chars, name->chars, (size_t)name->len) == 0)
            return i;
    }

    if (*count + 1 > *cap)
    {
        int next = *cap < 16 ? 16 : (*cap * 2);
        ObjString **grown = (ObjString **)realloc(*names, sizeof(ObjString *) * (size_t)next);
        if (grown == NULL)
            return -1;
        *names = grown;
        *cap = next;
    }

    (*names)[*count] = name;
    (*count)++;
    return (*count) - 1;
}

static bool isPseudoCallableGlobalName(ObjString *name)
{
    if (name == NULL)
        return false;
    return (name->len == 3 && memcmp(name->chars, "len", 3) == 0) ||
           (name->len == 7 && memcmp(name->chars, "randInt", 7) == 0) ||
           (name->len == 5 && memcmp(name->chars, "clear", 5) == 0) ||
           (name->len == 5 && memcmp(name->chars, "sleep", 5) == 0) ||
           (name->len == 3 && memcmp(name->chars, "int", 3) == 0) ||
           (name->len == 4 && memcmp(name->chars, "type", 4) == 0) ||
           (name->len == 3 && memcmp(name->chars, "cos", 3) == 0) ||
           (name->len == 3 && memcmp(name->chars, "sin", 3) == 0) ||
           (name->len == 3 && memcmp(name->chars, "chr", 3) == 0) ||
           (name->len == 11 && memcmp(name->chars, "print_error", 11) == 0);
}

static bool isPseudoArgvGlobalName(ObjString *name)
{
    return name != NULL && name->len == 5 && memcmp(name->chars, "ARG_V", 5) == 0;
}

static bool isPseudoArgcGlobalName(ObjString *name)
{
    return name != NULL && name->len == 5 && memcmp(name->chars, "ARG_C", 5) == 0;
}

static bool writeEscapedCStringLiteral(FILE *f, const char *src, int len)
{
    if (fputc('"', f) == EOF)
        return false;
    for (int i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)src[i];
        switch (c)
        {
        case '\\':
            if (fputs("\\\\", f) < 0)
                return false;
            break;
        case '"':
            if (fputs("\\\"", f) < 0)
                return false;
            break;
        case '\n':
            if (fputs("\\n", f) < 0)
                return false;
            break;
        case '\r':
            if (fputs("\\r", f) < 0)
                return false;
            break;
        case '\t':
            if (fputs("\\t", f) < 0)
                return false;
            break;
        default:
            if (c < 32 || c > 126)
            {
                if (fprintf(f, "\\x%02X", (unsigned)c) < 0)
                    return false;
            }
            else if (fputc((int)c, f) == EOF)
                return false;
            break;
        }
    }
    return fputc('"', f) != EOF;
}

static bool writeAotCProgram(FILE *f, const NativeIrVec *ir, ObjFunction *function)
{
    if (!writeAll(f,
                  "#include <stdio.h>\n"
                  "#include <math.h>\n"
                  "#include <stdlib.h>\n"
                  "#include <string.h>\n"
                  "#include <time.h>\n"
                  "#include <unistd.h>\n"
                  "#include <dlfcn.h>\n"
                  "#include <limits.h>\n"
                  "\n"
                  "typedef enum { VT_NUM=0, VT_STR=1, VT_BOOL=2, VT_NIL=3, VT_FUN=4, VT_LIST=5, VT_MAP=6, VT_SLICE=7, VT_CLASS=8, VT_INST=9, VT_BMETH=10 } VTag;\n"
                  "typedef struct { VTag t; double n; const char *s; int b; void *p; } V;\n"
                  "typedef struct { int count; int cap; V *items; } List;\n"
                  "typedef struct { V key; V value; } MapEntry;\n"
                  "typedef struct { int count; int cap; MapEntry *entries; } Map;\n"
                  "typedef struct { int start; int end; int step; } Slice;\n"
                  "typedef struct Class Class;\n"
                  "typedef struct Instance Instance;\n"
                  "typedef struct BoundMethod BoundMethod;\n"
                  "struct Class { const char *name; Map methods; Map staticVars; int initializerFn; Class *superclass; };\n"
                  "struct Instance { Class *klass; Map fields; };\n"
                  "struct BoundMethod { V receiver; int fnId; };\n"
                  "\n"
                  "typedef int (*AotHostFn)(int argc, const V *argv, V *out, const char **lastErr);\n"
                  "typedef struct { const char *name; V value; } AotConstSym;\n"
                  "typedef struct { const char *name; AotHostFn fn; } AotFnSym;\n"
                  "typedef struct {\n"
                  "  int version;\n"
                  "  void (*registerFunction)(const char *name, AotHostFn fn);\n"
                  "  void (*registerNumberConstant)(const char *name, double value);\n"
                  "  void (*registerStringConstant)(const char *name, const char *value);\n"
                  "  void *(*listNew)(void);\n"
                  "  int (*listPush)(void *list, V value);\n"
                  "  void *(*mapNew)(void);\n"
                  "  int (*mapSet)(void *map, V key, V value);\n"
                  "} DotkAotApi;\n"
                  "typedef int (*DotkAotInitFn)(const DotkAotApi *api);\n"
                  "\n"
                  "static int call_fn(int fnId, V *args, int argc, const V *thisRecv, V *out, V *g, unsigned char *gdef, unsigned char *gconst, const char **lastErr);\n"
                  "static int call_value(V callee, V *args, int argc, V *out, V *g, unsigned char *gdef, unsigned char *gconst, const char **lastErr);\n"
                  "static int invoke_value(V receiver, V name, V *args, int argc, V *out, V *g, unsigned char *gdef, unsigned char *gconst, const char **lastErr);\n"
                  "static int resolve_dynamic_global(const char *name, V *out, const char **lastErr);\n"
                  "\n"
                  "static List *list_new(void){ List *l=(List*)calloc(1,sizeof(List)); return l; }\n"
                  "static int list_push(List *l, V v){ if(!l) return 0; if(l->count+1>l->cap){ int nc=l->cap<8?8:l->cap*2; V *nv=(V*)realloc(l->items,sizeof(V)*(size_t)nc); if(!nv) return 0; l->items=nv; l->cap=nc;} l->items[l->count++]=v; return 1; }\n"
                  "static int list_norm_index(List *l, int idx){ if(!l) return -1; if(idx<0) idx=l->count+idx; if(idx<0||idx>=l->count) return -1; return idx; }\n"
                  "static Map *map_new(void){ Map *m=(Map*)calloc(1,sizeof(Map)); return m; }\n"
                  "static Slice *slice_new(int start,int end,int step){ Slice *s=(Slice*)malloc(sizeof(Slice)); if(!s) return NULL; s->start=start; s->end=end; s->step=(step==0?1:step); return s; }\n"
                  "static int list_slice(List *l, Slice *sl, V *out){ if(!l||!sl||!out) return 0; int n=l->count; int st=sl->step==0?1:sl->step; int a=sl->start; int b=sl->end; if(a<0) a=n+a; if(b<0) b=n+b; if(st>0){ if(a<0) a=0; if(a>n) a=n; if(b<0) b=0; if(b>n) b=n; } else { if(a<0) a=-1; if(a>=n) a=n-1; if(b<-1) b=-1; if(b>=n) b=n-1; } List *nl=list_new(); if(!nl) return 0; if(st>0){ for(int i=a;i<b;i+=st){ if(i>=0&&i<n){ if(!list_push(nl,l->items[i])) return 0; } } } else { for(int i=a;i>b;i+=st){ if(i>=0&&i<n){ if(!list_push(nl,l->items[i])) return 0; } } } *out=(V){VT_LIST,0,NULL,0,nl}; return 1; }\n"
                  "static int list_join(List *l, const char *sep, V *out){ if(!l||!out) return 0; if(!sep) sep=\"\"; size_t sepn=strlen(sep); size_t total=1; for(int i=0;i<l->count;i++){ char buf[128]; const char *s=NULL; if(l->items[i].t==VT_STR) s=l->items[i].s?l->items[i].s:\"\"; else if(l->items[i].t==VT_NUM){ snprintf(buf,sizeof(buf),\"%.15g\",l->items[i].n); s=buf; } else if(l->items[i].t==VT_BOOL) s=l->items[i].b?\"true\":\"false\"; else if(l->items[i].t==VT_NIL) s=\"null\"; else s=\"<obj>\"; total+=strlen(s); if(i+1<l->count) total+=sepn; } char *res=(char*)malloc(total); if(!res) return 0; res[0]='\\0'; for(int i=0;i<l->count;i++){ char buf[128]; const char *s=NULL; if(l->items[i].t==VT_STR) s=l->items[i].s?l->items[i].s:\"\"; else if(l->items[i].t==VT_NUM){ snprintf(buf,sizeof(buf),\"%.15g\",l->items[i].n); s=buf; } else if(l->items[i].t==VT_BOOL) s=l->items[i].b?\"true\":\"false\"; else if(l->items[i].t==VT_NIL) s=\"null\"; else s=\"<obj>\"; strcat(res,s); if(i+1<l->count) strcat(res,sep); } *out=(V){VT_STR,0,res,0,NULL}; return 1; }\n"
                  "\n"
                  "static int v_falsey(V v){\n"
                  "  if (v.t == VT_NIL) return 1;\n"
                  "  if (v.t == VT_BOOL) return !v.b;\n"
                  "  if (v.t == VT_NUM) return v.n == 0.0;\n"
                  "  if (v.t == VT_STR) return v.s == NULL || v.s[0] == '\\0';\n"
                  "  if (v.t == VT_LIST){ List *l=(List*)v.p; return l==NULL || l->count==0; }\n"
                  "  if (v.t == VT_MAP){ Map *m=(Map*)v.p; return m==NULL || m->count==0; }\n"
                  "  return 0;\n"
                  "}\n"
                  "\n"
                  "static int v_to_num(V v, double *out){\n"
                  "  if (v.t != VT_NUM) return 0;\n"
                  "  *out = v.n;\n"
                  "  return 1;\n"
                  "}\n"
                  "\n"
                  "static char *v_to_heap_cstr(V v){\n"
                  "  char buf[128];\n"
                  "  const char *src = NULL;\n"
                  "  if (v.t == VT_STR) src = v.s ? v.s : \"\";\n"
                  "  else if (v.t == VT_NUM){ snprintf(buf, sizeof(buf), \"%.15g\", v.n); src = buf; }\n"
                  "  else if (v.t == VT_BOOL) src = v.b ? \"true\" : \"false\";\n"
                  "  else if (v.t == VT_LIST) src = \"[list]\";\n"
                  "  else if (v.t == VT_MAP) src = \"{map}\";\n"
                  "  else if (v.t == VT_CLASS) src = \"<class>\";\n"
                  "  else if (v.t == VT_INST) src = \"<instance>\";\n"
                  "  else src = \"null\";\n"
                  "  size_t n = strlen(src);\n"
                  "  char *o = (char*)malloc(n + 1);\n"
                  "  if (!o) return NULL;\n"
                  "  memcpy(o, src, n + 1);\n"
                  "  return o;\n"
                  "}\n"
                  "\n"
                  "static V v_add(V a, V b, int *ok){\n"
                  "  if (a.t == VT_NUM && b.t == VT_NUM) return (V){VT_NUM, a.n + b.n, NULL, 0};\n"
                  "  char *sa = v_to_heap_cstr(a);\n"
                  "  char *sb = v_to_heap_cstr(b);\n"
                  "  if (!sa || !sb){ free(sa); free(sb); *ok = 0; return (V){VT_NIL,0,NULL,0}; }\n"
                  "  size_t la = strlen(sa), lb = strlen(sb);\n"
                  "  char *out = (char*)malloc(la + lb + 1);\n"
                  "  if (!out){ free(sa); free(sb); *ok = 0; return (V){VT_NIL,0,NULL,0}; }\n"
                  "  memcpy(out, sa, la);\n"
                  "  memcpy(out + la, sb, lb + 1);\n"
                  "  free(sa); free(sb);\n"
                  "  return (V){VT_STR,0,out,0};\n"
                  "}\n"
                  "\n"
                  "static int v_eq(V a, V b){\n"
                  "  if (a.t != b.t) return 0;\n"
                  "  if (a.t == VT_NUM) return a.n == b.n;\n"
                  "  if (a.t == VT_BOOL) return a.b == b.b;\n"
                  "  if (a.t == VT_NIL) return 1;\n"
                  "  if (a.t == VT_FUN) return a.b == b.b;\n"
                  "  if (a.t == VT_LIST || a.t == VT_MAP || a.t == VT_SLICE || a.t == VT_CLASS || a.t == VT_INST || a.t == VT_BMETH) return a.p == b.p;\n"
                  "  return strcmp(a.s ? a.s : \"\", b.s ? b.s : \"\") == 0;\n"
                  "}\n"
                  "\n"
                  "static int map_find(Map *m, V key){ if(!m) return -1; for(int i=0;i<m->count;i++){ if(v_eq(m->entries[i].key,key)) return i; } return -1; }\n"
                  "static int map_set(Map *m, V key, V val){ if(!m) return 0; int idx=map_find(m,key); if(idx>=0){ m->entries[idx].value=val; return 1;} if(m->count+1>m->cap){ int nc=m->cap<8?8:m->cap*2; MapEntry *ne=(MapEntry*)realloc(m->entries,sizeof(MapEntry)*(size_t)nc); if(!ne) return 0; m->entries=ne; m->cap=nc;} m->entries[m->count].key=key; m->entries[m->count].value=val; m->count++; return 1; }\n"
                  "static int map_get(Map *m, V key, V *out){ if(!m||!out) return 0; int idx=map_find(m,key); if(idx<0) return 0; *out=m->entries[idx].value; return 1; }\n"
                  "static int value_len(V v, V *out){ if(!out) return 0; if(v.t==VT_LIST){ List *l=(List*)v.p; *out=(V){VT_NUM,(double)(l?l->count:0),NULL,0,NULL}; return 1; } if(v.t==VT_MAP){ Map *m=(Map*)v.p; *out=(V){VT_NUM,(double)(m?m->count:0),NULL,0,NULL}; return 1; } if(v.t==VT_STR){ *out=(V){VT_NUM,(double)strlen(v.s?v.s:\"\"),NULL,0,NULL}; return 1; } return 0; }\n"
                  "#define AOT_MAX_SYMBOLS 8192\n"
                  "#define AOT_MAX_MODULES 256\n"
                  "static AotConstSym g_aot_consts[AOT_MAX_SYMBOLS];\n"
                  "static int g_aot_const_count=0;\n"
                  "static AotFnSym g_aot_funcs[AOT_MAX_SYMBOLS];\n"
                  "static int g_aot_func_count=0;\n"
                  "static char *g_loaded_modules[AOT_MAX_MODULES];\n"
                  "static int g_loaded_module_count=0;\n"
                  "static void *g_module_handles[AOT_MAX_MODULES];\n"
                  "static int g_module_handle_count=0;\n"
                  "static DotkAotApi g_host_api;\n"
                  "static int g_host_api_ready=0;\n"
                  "static int aot_register_constant(const char *name, V value){ if(!name||!name[0]) return 0; for(int i=0;i<g_aot_const_count;i++){ if(strcmp(g_aot_consts[i].name,name)==0){ g_aot_consts[i].value=value; return 1; } } if(g_aot_const_count>=AOT_MAX_SYMBOLS) return 0; g_aot_consts[g_aot_const_count].name=name; g_aot_consts[g_aot_const_count].value=value; g_aot_const_count++; return 1; }\n"
                  "static int aot_register_function(const char *name, AotHostFn fn){ if(!name||!name[0]||!fn) return 0; for(int i=0;i<g_aot_func_count;i++){ if(strcmp(g_aot_funcs[i].name,name)==0){ g_aot_funcs[i].fn=fn; return 1; } } if(g_aot_func_count>=AOT_MAX_SYMBOLS) return 0; g_aot_funcs[g_aot_func_count].name=name; g_aot_funcs[g_aot_func_count].fn=fn; g_aot_func_count++; return 1; }\n"
                  "static void aot_register_function_api(const char *name, AotHostFn fn){ (void)aot_register_function(name,fn); }\n"
                  "static void aot_register_number_constant_api(const char *name, double value){ (void)aot_register_constant(name,(V){VT_NUM,value,NULL,0,NULL}); }\n"
                  "static void aot_register_string_constant_api(const char *name, const char *value){ (void)aot_register_constant(name,(V){VT_STR,0,value?value:\"\",0,NULL}); }\n"
                  "static void *aot_list_new_api(void){ return (void*)list_new(); }\n"
                  "static int aot_list_push_api(void *list, V value){ return list_push((List*)list,value); }\n"
                  "static void *aot_map_new_api(void){ return (void*)map_new(); }\n"
                  "static int aot_map_set_api(void *map, V key, V value){ return map_set((Map*)map,key,value); }\n"
                  "static int aot_lookup_constant(const char *name, V *out){ if(!name||!out) return 0; for(int i=0;i<g_aot_const_count;i++){ if(strcmp(g_aot_consts[i].name,name)==0){ *out=g_aot_consts[i].value; return 1; } } return 0; }\n"
                  "static int aot_lookup_function(const char *name, AotHostFn *out){ if(!name||!out) return 0; for(int i=0;i<g_aot_func_count;i++){ if(strcmp(g_aot_funcs[i].name,name)==0){ *out=g_aot_funcs[i].fn; return 1; } } return 0; }\n"
                  "static int aot_module_loaded(const char *base){ if(!base) return 0; for(int i=0;i<g_loaded_module_count;i++){ if(g_loaded_modules[i] && strcmp(g_loaded_modules[i],base)==0) return 1; } return 0; }\n"
                  "static int aot_mark_module_loaded(const char *base){ if(!base||!base[0]) return 0; if(aot_module_loaded(base)) return 1; if(g_loaded_module_count>=AOT_MAX_MODULES) return 0; size_t n=strlen(base); char *cp=(char*)malloc(n+1); if(!cp) return 0; memcpy(cp,base,n+1); g_loaded_modules[g_loaded_module_count++]=cp; return 1; }\n"
                  "static int resolve_dynamic_global(const char *name, V *out, const char **lastErr){ if(!name||!out){ if(lastErr)*lastErr=\"bad dynamic global lookup\"; return 0; } if(strcmp(name,\"len\")==0||strcmp(name,\"randInt\")==0||strcmp(name,\"clear\")==0||strcmp(name,\"sleep\")==0||strcmp(name,\"int\")==0||strcmp(name,\"type\")==0||strcmp(name,\"cos\")==0||strcmp(name,\"sin\")==0||strcmp(name,\"print_error\")==0){ *out=(V){VT_STR,0,name,0,NULL}; return 1; } if(aot_lookup_constant(name,out)) return 1; AotHostFn fn=NULL; if(aot_lookup_function(name,&fn)){ (void)fn; *out=(V){VT_STR,0,name,0,NULL}; return 1; } if(strcmp(name,\"ARG_V\")==0){ List *l=list_new(); if(!l){ if(lastErr)*lastErr=\"oom ARG_V\"; return 0; } *out=(V){VT_LIST,0,NULL,0,l}; return 1; } if(strcmp(name,\"ARG_C\")==0){ *out=(V){VT_NUM,0,NULL,0,NULL}; return 1; } *out=(V){VT_NIL,0,NULL,0,NULL}; return 1; }\n"
                  "static int import_value(V file, V *out, const char **lastErr){ if(!out){ if(lastErr)*lastErr=\"bad import out\"; return 1;} if(file.t!=VT_STR||!file.s){ if(lastErr)*lastErr=\"import path must be string\"; return 1;} const char *raw=file.s; const char *base=raw; const char *slash=strrchr(raw,'/'); if(slash&&slash[1]) base=slash+1; char mod[256]; size_t blen=strlen(base); if(blen>=sizeof(mod)) blen=sizeof(mod)-1; memcpy(mod,base,blen); mod[blen]='\\0'; char *dot=strrchr(mod,'.'); if(dot) *dot='\\0'; if(mod[0]=='\\0'){ if(lastErr)*lastErr=\"empty module name\"; return 1; } int constBefore=g_aot_const_count, fnBefore=g_aot_func_count; if(!g_host_api_ready){ memset(&g_host_api,0,sizeof(g_host_api)); g_host_api.version=1; g_host_api.registerFunction=aot_register_function_api; g_host_api.registerNumberConstant=aot_register_number_constant_api; g_host_api.registerStringConstant=aot_register_string_constant_api; g_host_api.listNew=aot_list_new_api; g_host_api.listPush=aot_list_push_api; g_host_api.mapNew=aot_map_new_api; g_host_api.mapSet=aot_map_set_api; g_host_api_ready=1; } if(!aot_module_loaded(mod)){ char p1[PATH_MAX], p2[PATH_MAX], p3[PATH_MAX]; snprintf(p1,sizeof(p1),\"./modules/%s_module.so\",mod); snprintf(p2,sizeof(p2),\"modules/%s_module.so\",mod); snprintf(p3,sizeof(p3),\"./%s_module.so\",mod); const char *cands[3]={p1,p2,p3}; void *h=NULL; for(int i=0;i<3;i++){ h=dlopen(cands[i],RTLD_NOW|RTLD_GLOBAL); if(h) break; } if(h){ if(g_module_handle_count<AOT_MAX_MODULES) g_module_handles[g_module_handle_count++]=h; DotkAotInitFn initFn=(DotkAotInitFn)dlsym(h,\"dotk_aot_init\"); if(!initFn){ if(lastErr)*lastErr=\"missing dotk_aot_init\"; return 1; } if(initFn(&g_host_api)==0){ if(lastErr&&!*lastErr) *lastErr=\"module init failed\"; return 1; } if(!aot_mark_module_loaded(mod)){ if(lastErr)*lastErr=\"failed to mark module loaded\"; return 1; } } } Map *m=map_new(); if(!m){ if(lastErr)*lastErr=\"oom import map\"; return 1; } for(int i=constBefore;i<g_aot_const_count;i++){ V k=(V){VT_STR,0,g_aot_consts[i].name,0,NULL}; if(!map_set(m,k,g_aot_consts[i].value)){ if(lastErr)*lastErr=\"oom import map const\"; return 1; } } for(int i=fnBefore;i<g_aot_func_count;i++){ V k=(V){VT_STR,0,g_aot_funcs[i].name,0,NULL}; V v=(V){VT_STR,0,g_aot_funcs[i].name,0,NULL}; if(!map_set(m,k,v)){ if(lastErr)*lastErr=\"oom import map fn\"; return 1; } } *out=(V){VT_MAP,0,NULL,0,m}; return 0; }\n"
                  "static Class *class_new(const char *name){ Class *k=(Class*)calloc(1,sizeof(Class)); if(!k) return NULL; k->name=name?name:\"<class>\"; k->initializerFn=-1; return k; }\n"
                  "static Instance *instance_new(Class *klass){ Instance *in=(Instance*)calloc(1,sizeof(Instance)); if(!in) return NULL; in->klass=klass; return in; }\n"
                  "static BoundMethod *bm_new(V recv, int fnId){ BoundMethod *bm=(BoundMethod*)malloc(sizeof(BoundMethod)); if(!bm) return NULL; bm->receiver=recv; bm->fnId=fnId; return bm; }\n"
                  "static int class_set_method(Class *klass, V name, V method){ if(!klass||name.t!=VT_STR) return 0; const char *nm=name.s?name.s:\"\"; if(!map_set(&klass->methods,name,method)) return 0; if(strcmp(nm,\"__eq__\")==0){ V a=(V){VT_STR,0,\"_eq_\",0,NULL}; if(!map_set(&klass->methods,a,method)) return 0; } else if(strcmp(nm,\"__lt__\")==0){ V a=(V){VT_STR,0,\"_lt_\",0,NULL}; if(!map_set(&klass->methods,a,method)) return 0; } else if(strcmp(nm,\"__gt__\")==0){ V a=(V){VT_STR,0,\"_gt_\",0,NULL}; if(!map_set(&klass->methods,a,method)) return 0; } else if(strcmp(nm,\"__getitem__\")==0){ V a=(V){VT_STR,0,\"_get_\",0,NULL}; if(!map_set(&klass->methods,a,method)) return 0; } else if(strcmp(nm,\"__setitem__\")==0){ V a=(V){VT_STR,0,\"_set_\",0,NULL}; if(!map_set(&klass->methods,a,method)) return 0; } if(method.t==VT_FUN && (strcmp(nm,\"init\")==0||strcmp(nm,\"_init_\")==0)) klass->initializerFn=method.b; return 1; }\n"
                  "static int class_inherit(Class *sub, Class *sup){ if(!sub||!sup) return 0; sub->superclass=sup; sub->initializerFn=sup->initializerFn; for(int i=0;i<sup->methods.count;i++){ if(!map_set(&sub->methods,sup->methods.entries[i].key,sup->methods.entries[i].value)) return 0; } for(int i=0;i<sup->staticVars.count;i++){ if(!map_set(&sub->staticVars,sup->staticVars.entries[i].key,sup->staticVars.entries[i].value)) return 0; } return 1; }\n"
                  "static int super_from_this(V recv, V *out){ if(!out) return 0; if(recv.t==VT_INST){ Instance *in=(Instance*)recv.p; if(!in||!in->klass||!in->klass->superclass) return 0; *out=(V){VT_CLASS,0,NULL,0,in->klass->superclass}; return 1; } if(recv.t==VT_CLASS){ Class *k=(Class*)recv.p; if(!k||!k->superclass) return 0; *out=(V){VT_CLASS,0,NULL,0,k->superclass}; return 1; } return 0; }\n"
                  "static int get_property(V obj, V name, V *out){ if(!out||name.t!=VT_STR) return 0; if(obj.t==VT_INST){ Instance *in=(Instance*)obj.p; if(in&&map_get(&in->fields,name,out)) return 1; V mv; if(in&&in->klass&&map_get(&in->klass->methods,name,&mv)){ if(mv.t==VT_FUN){ BoundMethod *bm=bm_new(obj,mv.b); if(!bm) return 0; *out=(V){VT_BMETH,0,NULL,0,bm}; } else *out=mv; return 1; } return 0; } if(obj.t==VT_CLASS){ Class *k=(Class*)obj.p; if(k&&map_get(&k->staticVars,name,out)) return 1; V mv; if(k&&map_get(&k->methods,name,&mv)){ if(mv.t==VT_FUN){ BoundMethod *bm=bm_new(obj,mv.b); if(!bm) return 0; *out=(V){VT_BMETH,0,NULL,0,bm}; } else *out=mv; return 1; } return 0; } return 0; }\n"
                  "static int get_super_bound(V receiver, Class *superK, V name, V *out){ if(!superK||!out||name.t!=VT_STR) return 0; V mv; if(!map_get(&superK->methods,name,&mv)) return 0; if(mv.t==VT_FUN){ BoundMethod *bm=bm_new(receiver,mv.b); if(!bm) return 0; *out=(V){VT_BMETH,0,NULL,0,bm}; return 1; } *out=mv; return 1; }\n"
                  "static int set_property(V obj, V name, V value){ if(name.t!=VT_STR) return 0; if(obj.t==VT_INST){ Instance *in=(Instance*)obj.p; return in?map_set(&in->fields,name,value):0; } if(obj.t==VT_CLASS){ Class *k=(Class*)obj.p; return k?map_set(&k->staticVars,name,value):0; } return 0; }\n"
                  "static int invoke_super(V receiver, Class *superK, V name, V *args, int argc, V *out, V *g, unsigned char *gdef, unsigned char *gconst, const char **lastErr){ if(!superK||name.t!=VT_STR){ if(lastErr)*lastErr=\"bad super invoke\"; return 1;} V callable; if(!map_get(&superK->staticVars,name,&callable) && !map_get(&superK->methods,name,&callable)){ if(lastErr)*lastErr=\"undefined super method\"; return 1;} if(callable.t==VT_FUN) return call_fn(callable.b,args,argc,&receiver,out,g,gdef,gconst,lastErr); return call_value(callable,args,argc,out,g,gdef,gconst,lastErr); }\n"
                  "static int call_value(V callee, V *args, int argc, V *out, V *g, unsigned char *gdef, unsigned char *gconst, const char **lastErr){\n"
                  "  if(callee.t==VT_FUN) return call_fn(callee.b,args,argc,NULL,out,g,gdef,gconst,lastErr);\n"
                  "  if(callee.t==VT_BMETH){ BoundMethod *bm=(BoundMethod*)callee.p; if(!bm){ if(lastErr)*lastErr=\"bad bound method\"; return 1;} return call_fn(bm->fnId,args,argc,&bm->receiver,out,g,gdef,gconst,lastErr); }\n"
                  "  if(callee.t==VT_CLASS){ Class *k=(Class*)callee.p; if(!k){ if(lastErr)*lastErr=\"bad class\"; return 1;} Instance *in=instance_new(k); if(!in){ if(lastErr)*lastErr=\"oom instance\"; return 1;} V inst=(V){VT_INST,0,NULL,0,in}; if(k->initializerFn>=0){ V ignore=(V){VT_NIL,0,NULL,0}; int r=call_fn(k->initializerFn,args,argc,&inst,&ignore,g,gdef,gconst,lastErr); if(r!=0) return r; *out=inst; return 0; } if(argc!=0){ if(lastErr)*lastErr=\"init arity mismatch\"; return 1; } *out=inst; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"len\")==0){ if(argc!=1){ if(lastErr)*lastErr=\"len arity mismatch\"; return 1;} if(!value_len(args[0],out)){ if(lastErr)*lastErr=\"bad len arg\"; return 1;} return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"randInt\")==0){ if(argc!=2||args[0].t!=VT_NUM||args[1].t!=VT_NUM){ if(lastErr)*lastErr=\"randInt arity/type mismatch\"; return 1;} static int seeded=0; if(!seeded){ seeded=1; srand((unsigned int)time(NULL)); } int lo=(int)args[0].n; int hi=(int)args[1].n; if(hi<lo){ int t=lo; lo=hi; hi=t; } int span=hi-lo+1; int rv=(span<=0)?lo:(lo+(rand()%span)); *out=(V){VT_NUM,(double)rv,NULL,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"clear\")==0){ if(argc!=0){ if(lastErr)*lastErr=\"clear arity mismatch\"; return 1;} printf(\"\\033[2J\\033[H\"); fflush(stdout); *out=(V){VT_NIL,0,NULL,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"sleep\")==0){ if(argc!=1||args[0].t!=VT_NUM){ if(lastErr)*lastErr=\"sleep arity/type mismatch\"; return 1;} int ms=(int)(args[0].n*1000.0); if(ms<0) ms=0; usleep((useconds_t)ms); *out=(V){VT_NIL,0,NULL,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"int\")==0){ if(argc!=1){ if(lastErr)*lastErr=\"int arity mismatch\"; return 1;} if(args[0].t==VT_NUM){ *out=(V){VT_NUM,(double)((long)args[0].n),NULL,0,NULL}; return 0;} if(args[0].t==VT_BOOL){ *out=(V){VT_NUM,(double)(args[0].b?1:0),NULL,0,NULL}; return 0;} if(args[0].t==VT_STR){ *out=(V){VT_NUM,(double)strtol(args[0].s?args[0].s:\"0\",NULL,10),NULL,0,NULL}; return 0;} *out=(V){VT_NUM,0,NULL,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"type\")==0){ if(argc!=1){ if(lastErr)*lastErr=\"type arity mismatch\"; return 1;} const char *tn=args[0].t==VT_NUM?\"float\":args[0].t==VT_STR?\"string\":args[0].t==VT_BOOL?\"bool\":args[0].t==VT_NIL?\"null\":args[0].t==VT_LIST?\"list\":args[0].t==VT_MAP?\"map\":args[0].t==VT_CLASS?\"class\":args[0].t==VT_INST?\"instance\":\"function\"; *out=(V){VT_STR,0,tn,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"cos\")==0){ if(argc!=1||args[0].t!=VT_NUM){ if(lastErr)*lastErr=\"cos arity/type mismatch\"; return 1;} *out=(V){VT_NUM,cos(args[0].n),NULL,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"sin\")==0){ if(argc!=1||args[0].t!=VT_NUM){ if(lastErr)*lastErr=\"sin arity/type mismatch\"; return 1;} *out=(V){VT_NUM,sin(args[0].n),NULL,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR && strcmp(callee.s?callee.s:\"\",\"print_error\")==0){ if(argc!=1){ if(lastErr)*lastErr=\"print_error arity mismatch\"; return 1;} char *s=v_to_heap_cstr(args[0]); if(!s){ if(lastErr)*lastErr=\"oom print_error\"; return 1;} fprintf(stderr,\"%s\\n\",s); free(s); *out=(V){VT_NIL,0,NULL,0,NULL}; return 0; }\n"
                  "  if(callee.t==VT_STR){ AotHostFn modFn=NULL; if(aot_lookup_function(callee.s?callee.s:\"\",&modFn)){ return modFn(argc,args,out,lastErr); } }\n"
                  "  if(lastErr) *lastErr=\"call non-fn\";\n"
                  "  return 1;\n"
                  "}\n"
                  "static int str_format_one(const char *fmt, V arg, V *out){ if(!out) return 0; if(!fmt) fmt=\"\"; char *argS=v_to_heap_cstr(arg); if(!argS) return 0; const char *slot=strstr(fmt,\"{}\"); int slotLen=2; if(!slot){ slot=strstr(fmt,\"${}\"); slotLen=3; } if(!slot){ size_t lf=strlen(fmt), la=strlen(argS); char *res=(char*)malloc(lf+la+1); if(!res){ free(argS); return 0; } memcpy(res,fmt,lf); memcpy(res+lf,argS,la+1); free(argS); *out=(V){VT_STR,0,res,0,NULL}; return 1; } size_t pre=(size_t)(slot-fmt); size_t la=strlen(argS); size_t post=strlen(slot+slotLen); char *res=(char*)malloc(pre+la+post+1); if(!res){ free(argS); return 0; } memcpy(res,fmt,pre); memcpy(res+pre,argS,la); memcpy(res+pre+la,slot+slotLen,post+1); free(argS); *out=(V){VT_STR,0,res,0,NULL}; return 1; }\n"
                  "static int invoke_value(V receiver, V name, V *args, int argc, V *out, V *g, unsigned char *gdef, unsigned char *gconst, const char **lastErr){ if(receiver.t==VT_LIST && name.t==VT_STR){ const char *mn=name.s?name.s:\"\"; if(strcmp(mn,\"join\")==0){ if(argc!=1||args[0].t!=VT_STR){ if(lastErr) *lastErr=\"bad list.join args\"; return 1;} if(!list_join((List*)receiver.p,args[0].s?args[0].s:\"\",out)){ if(lastErr) *lastErr=\"list.join failed\"; return 1;} return 0; } if(strcmp(mn,\"append\")==0){ if(argc!=1){ if(lastErr) *lastErr=\"bad list.append args\"; return 1;} if(!list_push((List*)receiver.p,args[0])){ if(lastErr) *lastErr=\"list.append failed\"; return 1;} *out=(V){VT_NIL,0,NULL,0,NULL}; return 0; } if(strcmp(mn,\"pop\")==0){ List *l=(List*)receiver.p; if(argc!=0){ if(lastErr) *lastErr=\"bad list.pop args\"; return 1;} if(!l||l->count<=0){ if(lastErr) *lastErr=\"pop from empty list\"; return 1;} *out=l->items[l->count-1]; l->count--; return 0; } } if(receiver.t==VT_STR && name.t==VT_STR){ const char *mn=name.s?name.s:\"\"; if(strcmp(mn,\"join\")==0){ if(argc!=1||args[0].t!=VT_LIST){ if(lastErr) *lastErr=\"bad str.join args\"; return 1;} if(!list_join((List*)args[0].p,receiver.s?receiver.s:\"\",out)){ if(lastErr) *lastErr=\"str.join failed\"; return 1;} return 0; } if(strcmp(mn,\"f\")==0){ if(argc!=1){ if(lastErr) *lastErr=\"bad str.f args\"; return 1;} if(!str_format_one(receiver.s?receiver.s:\"\",args[0],out)){ if(lastErr) *lastErr=\"str.f failed\"; return 1;} return 0; } } V callee; if(!get_property(receiver,name,&callee)){ fprintf(stderr,\"[dotk-aot] undefined method '%s' on tag=%d\\n\", name.s?name.s:\"?\", (int)receiver.t); if(lastErr) *lastErr=\"undefined method\"; return 1;} return call_value(callee,args,argc,out,g,gdef,gconst,lastErr); }\n"
                  "\n"
                  "static int v_lt(V a, V b, int *ok){\n"
                  "  if (a.t == VT_NUM && b.t == VT_NUM) return a.n < b.n;\n"
                  "  if (a.t == VT_STR && b.t == VT_STR) return strcmp(a.s ? a.s : \"\", b.s ? b.s : \"\") < 0;\n"
                  "  *ok = 0;\n"
                  "  return 0;\n"
                  "}\n"
                  "\n"
                  "static int v_gt(V a, V b, int *ok){\n"
                  "  if (a.t == VT_NUM && b.t == VT_NUM) return a.n > b.n;\n"
                  "  if (a.t == VT_STR && b.t == VT_STR) return strcmp(a.s ? a.s : \"\", b.s ? b.s : \"\") > 0;\n"
                  "  *ok = 0;\n"
                  "  return 0;\n"
                  "}\n"
                  "\n"
                  "static void v_print(V v){\n"
                  "  if (v.t == VT_NUM) printf(\"%.15g\\n\", v.n);\n"
                  "  else if (v.t == VT_BOOL) printf(\"%s\\n\", v.b ? \"true\" : \"false\");\n"
                  "  else if (v.t == VT_NIL) printf(\"null\\n\");\n"
                  "  else if (v.t == VT_FUN) printf(\"<fn %d>\\n\", v.b);\n"
                  "  else if (v.t == VT_LIST) printf(\"[list size=%d]\\n\", v.p?((List*)v.p)->count:0);\n"
                  "  else if (v.t == VT_MAP) printf(\"{map size=%d}\\n\", v.p?((Map*)v.p)->count:0);\n"
                  "  else if (v.t == VT_SLICE) printf(\"<slice>\\n\");\n"
                  "  else if (v.t == VT_CLASS) printf(\"<class %s>\\n\", v.p?((Class*)v.p)->name:\"?\");\n"
                  "  else if (v.t == VT_INST) printf(\"<instance %s>\\n\", (v.p&&((Instance*)v.p)->klass)?((Instance*)v.p)->klass->name:\"?\");\n"
                  "  else if (v.t == VT_BMETH) printf(\"<bound-method>\\n\");\n"
                  "  else printf(\"%s\\n\", v.s ? v.s : \"\");\n"
                  "}\n"
                  "\n"))
        return false;

    int constCount = function != NULL ? function->chunk.constants.size : 0;
    int codeSize = function != NULL ? function->chunk.size : 0;
    int *globalSlotByConst = NULL;
    int nextGlobalSlot = 0;
    ObjString **globalNames = NULL;
    int globalNameCount = 0;
    int globalNameCap = 0;
    bool *labelNeeded = NULL;
    bool *fnNeededByConst = NULL;
    if (constCount > 0)
    {
        globalSlotByConst = (int *)malloc(sizeof(int) * (size_t)constCount);
        if (globalSlotByConst == NULL)
            return false;
        for (int i = 0; i < constCount; i++)
            globalSlotByConst[i] = -1;

        fnNeededByConst = (bool *)calloc((size_t)constCount, sizeof(bool));
        if (fnNeededByConst == NULL)
        {
            free(globalSlotByConst);
            return false;
        }
        for (int i = 0; i < constCount; i++)
        {
            if (IS_FUN(function->chunk.constants.values[i]))
                fnNeededByConst[i] = true;
        }
    }

    if (codeSize >= 0)
    {
        labelNeeded = (bool *)calloc((size_t)codeSize + 1u, sizeof(bool));
        if (labelNeeded == NULL)
        {
            free(globalSlotByConst);
            return false;
        }
        for (int i = 0; i < ir->count; i++)
        {
            NativeIrInst inst = ir->items[i];
            if (inst.op == NIR_JUMP || inst.op == NIR_JUMP_IF_FALSE)
            {
                int target = resolveJumpTargetToEmittedIp(ir, codeSize, inst.arg);
                if (target >= 0 && target <= codeSize)
                    labelNeeded[target] = true;
            }
        }
    }

    if (!writeAll(f, "static int call_fn(int fnId, V *args, int argc, const V *thisRecv, V *out, V *g, unsigned char *gdef, unsigned char *gconst, const char **lastErr){\n  switch(fnId){\n"))
    {
        free(fnNeededByConst);
        free(labelNeeded);
        free(globalSlotByConst);
        return false;
    }

    for (int fi = 0; fi < constCount; fi++)
    {
        if (!fnNeededByConst[fi])
            continue;
        Value fv = function->chunk.constants.values[fi];
        if (!IS_FUN(fv))
        {
            free(fnNeededByConst);
            free(labelNeeded);
            free(globalSlotByConst);
            return false;
        }
        ObjFunction *callee = AS_FUN(fv);
        NativeIrVec fir = {0};
        if (!parseBytecodeToNativeIr(callee, &fir))
        {
            free(fir.items);
            fprintf(f, "    case %d: { if(lastErr) *lastErr=\"unsupported nested function ir\"; return 1; }\n", fi);
            continue;
        }

        fprintf(f, "    case %d: {\n", fi);
        writeAll(f, "      V s[4096]; int sp=0; V l[8192];\n");
        writeAll(f, "      for(int i=0;i<8192;i++) l[i]=(V){VT_NIL,0,NULL,0};\n");
        writeAll(f, "      if(thisRecv) l[0]=*thisRecv;\n");
        fprintf(f, "      for(int i=0;i<%d;i++){ s[i]=(V){VT_NIL,0,NULL,0,NULL}; if(i<argc){ l[i+1]=args[i]; s[i]=args[i]; } } sp=%d;\n", callee->arity, callee->arity);

        bool *flabel = NULL;
        if (callee->chunk.size >= 0)
        {
            flabel = (bool *)calloc((size_t)callee->chunk.size + 1u, sizeof(bool));
            if (flabel == NULL)
            {
                free(fir.items);
                free(fnNeededByConst);
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            for (int j = 0; j < fir.count; j++)
            {
                if (fir.items[j].op == NIR_JUMP || fir.items[j].op == NIR_JUMP_IF_FALSE)
                {
                    int target = resolveJumpTargetToEmittedIp(&fir, callee->chunk.size, fir.items[j].arg);
                    if (target >= 0 && target <= callee->chunk.size)
                        flabel[target] = true;
                }
            }
        }

        for (int j = 0; j < fir.count; j++)
        {
            NativeIrInst inst = fir.items[j];
            if (flabel != NULL && inst.sourceIp >= 0 && inst.sourceIp < callee->chunk.size && flabel[inst.sourceIp])
                fprintf(f, "F%d_L%d:\n", fi, inst.sourceIp);

            switch (inst.op)
            {
            case NIR_PUSH_CONST:
                fprintf(f, "      s[sp++] = (V){VT_NUM, %.17a, NULL, 0};\n", inst.number);
                break;
            case NIR_PUSH_NIL:
                writeAll(f, "      s[sp++] = (V){VT_NIL,0,NULL,0};\n");
                break;
            case NIR_PUSH_TRUE:
                writeAll(f, "      s[sp++] = (V){VT_BOOL,0,NULL,1};\n");
                break;
            case NIR_PUSH_FALSE:
                writeAll(f, "      s[sp++] = (V){VT_BOOL,0,NULL,0};\n");
                break;
            case NIR_PUSH_STR:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    return false;
                }
                ObjString *sc = AS_STR(c);
                writeAll(f, "      s[sp++] = (V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, sc->chars, sc->len) || !writeAll(f, ",0};\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    return false;
                }
                break;
            }
            case NIR_BUILD_LIST:
                fprintf(f, "      { List *l=list_new(); if(!l){ if(lastErr) *lastErr=\"oom list\"; return 1;} int n=%d; int base=sp-n; for(int k=0;k<n;k++){ if(!list_push(l,s[base+k])){ if(lastErr) *lastErr=\"oom list\"; return 1; } } sp=base; s[sp++]=(V){VT_LIST,0,NULL,0,l}; }\n", inst.arg);
                break;
            case NIR_BUILD_DEFAULT_LIST:
                fprintf(f, "      { int hd=%d; V defv=(V){VT_NIL,0,NULL,0,NULL}; V sizev=hd?s[sp-2]:s[sp-1]; if(hd) defv=s[sp-1]; if(sizev.t!=VT_NUM){ if(lastErr)*lastErr=\"default list size must be number\"; return 1;} int n=(int)sizev.n; if(n<0){ if(lastErr)*lastErr=\"default list size negative\"; return 1;} List *l=list_new(); if(!l){ if(lastErr)*lastErr=\"oom list\"; return 1;} for(int k=0;k<n;k++){ if(!list_push(l,defv)){ if(lastErr)*lastErr=\"oom list\"; return 1;} } sp-=hd?2:1; s[sp++]=(V){VT_LIST,0,NULL,0,l}; }\n", inst.arg);
                break;
            case NIR_BUILD_SLICE:
                writeAll(f, "      { V stepv=s[sp-1]; V endv=s[sp-2]; V startv=s[sp-3]; if(stepv.t!=VT_NUM||endv.t!=VT_NUM||startv.t!=VT_NUM){ if(lastErr)*lastErr=\"bad slice args\"; return 1;} Slice *sl=slice_new((int)startv.n,(int)endv.n,(int)stepv.n); if(!sl){ if(lastErr)*lastErr=\"oom slice\"; return 1;} sp-=3; s[sp++]=(V){VT_SLICE,0,NULL,0,sl}; }\n");
                break;
            case NIR_BUILD_MAP:
                fprintf(f, "      { Map *m=map_new(); if(!m){ if(lastErr) *lastErr=\"oom map\"; return 1;} int n=%d; int base=sp-(n*2); for(int k=0;k<n;k++){ V key=s[base+k*2]; V val=s[base+k*2+1]; if(!map_set(m,key,val)){ if(lastErr) *lastErr=\"oom map\"; return 1; } } sp=base; s[sp++]=(V){VT_MAP,0,NULL,0,m}; }\n", inst.arg);
                break;
            case NIR_INDEX_SUBSCR:
                writeAll(f, "      { V idx=s[sp-1]; V obj=s[sp-2]; V outv=(V){VT_NIL,0,NULL,0,NULL}; if(obj.t==VT_LIST){ if(idx.t==VT_NUM){ int ii=list_norm_index((List*)obj.p,(int)idx.n); if(ii<0){ if(lastErr)*lastErr=\"list index oob\"; return 1;} outv=((List*)obj.p)->items[ii]; } else if(idx.t==VT_SLICE){ if(!list_slice((List*)obj.p,(Slice*)idx.p,&outv)){ if(lastErr)*lastErr=\"bad list slice\"; return 1;} } else { if(lastErr)*lastErr=\"list index type\"; return 1; } } else if(obj.t==VT_STR){ if(idx.t!=VT_NUM){ if(lastErr)*lastErr=\"str index type\"; return 1;} int ii=(int)idx.n; int len=(int)strlen(obj.s?obj.s:\"\"); if(ii<0) ii=len+ii; if(ii<0||ii>=len){ if(lastErr)*lastErr=\"str index oob\"; return 1;} char *tmp=(char*)malloc(2); if(!tmp){ if(lastErr)*lastErr=\"oom\"; return 1;} tmp[0]=obj.s[ii]; tmp[1]='\\0'; outv=(V){VT_STR,0,tmp,0,NULL}; } else if(obj.t==VT_MAP){ if(!map_get((Map*)obj.p,idx,&outv)){ if(lastErr)*lastErr=\"map key missing\"; return 1;} } else { if(lastErr)*lastErr=\"not subscriptable\"; return 1;} sp-=2; s[sp++]=outv; }\n");
                break;
            case NIR_STORE_SUBSCR:
                writeAll(f, "      { V val=s[sp-1]; V idx=s[sp-2]; V obj=s[sp-3]; if(obj.t==VT_LIST){ if(idx.t!=VT_NUM){ if(lastErr)*lastErr=\"list index type\"; return 1;} int ii=list_norm_index((List*)obj.p,(int)idx.n); if(ii<0){ if(lastErr)*lastErr=\"list index oob\"; return 1;} ((List*)obj.p)->items[ii]=val; } else if(obj.t==VT_MAP){ if(!map_set((Map*)obj.p,idx,val)){ if(lastErr)*lastErr=\"oom map\"; return 1;} } else { if(lastErr)*lastErr=\"not subscriptable\"; return 1;} sp-=3; s[sp++]=val; }\n");
                break;
            case NIR_GET_LOCAL:
                if (inst.arg == 0)
                    fprintf(f, "      s[sp++] = l[0];\n");
                else
                    fprintf(f, "      if(%d-1 < sp) s[sp++] = s[%d-1]; else s[sp++] = l[%d];\n", inst.arg, inst.arg, inst.arg);
                break;
            case NIR_SET_LOCAL:
                if (inst.arg == 0)
                    fprintf(f, "      l[0] = s[sp-1];\n");
                else
                    fprintf(f, "      if(%d-1 < 4096) s[%d-1] = s[sp-1]; l[%d] = s[sp-1];\n", inst.arg, inst.arg, inst.arg);
                break;
            case NIR_CLASS:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { Class *k=class_new(");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, "); if(!k){ if(lastErr) *lastErr=\"oom class\"; return 1;} s[sp++]=(V){VT_CLASS,0,NULL,0,k}; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_METHOD:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { V name=(V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V method=s[sp-1]; V klassv=s[sp-2]; if(klassv.t!=VT_CLASS){ if(lastErr)*lastErr=\"method target not class\"; return 1;} if(!class_set_method((Class*)klassv.p,name,method)){ if(lastErr)*lastErr=\"method define failed\"; return 1;} sp--; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_STATIC_VAR:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { V name=(V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V value=s[sp-1]; V klassv=s[sp-2]; if(klassv.t!=VT_CLASS){ if(lastErr)*lastErr=\"static target not class\"; return 1;} if(!map_set(&((Class*)klassv.p)->staticVars,name,value)){ if(lastErr)*lastErr=\"static set failed\"; return 1;} sp--; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_GET_PROPERTY:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { V name=(V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V prop; if(!get_property(s[sp-1],name,&prop)){ if(lastErr)*lastErr=\"undefined property\"; return 1;} s[sp-1]=prop; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_SET_PROPERTY:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { V name=(V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V value=s[sp-1]; V obj=s[sp-2]; if(!set_property(obj,name,value)){ if(lastErr)*lastErr=\"set property failed\"; return 1;} sp-=2; s[sp++]=value; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_INHERIT:
                writeAll(f, "      { V superv=s[sp-2]; V subv=s[sp-1]; if(superv.t!=VT_CLASS||subv.t!=VT_CLASS){ if(lastErr)*lastErr=\"inherit expects classes\"; return 1;} if(!class_inherit((Class*)subv.p,(Class*)superv.p)){ if(lastErr)*lastErr=\"inherit failed\"; return 1;} sp--; }\n");
                break;
            case NIR_GET_SUPER:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { V name=(V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V superv=s[sp-1]; V recv=s[sp-2]; if(superv.t!=VT_CLASS){ if(lastErr)*lastErr=\"bad super class\"; return 1;} V bv; if(!get_super_bound(recv,(Class*)superv.p,name,&bv)){ if(lastErr)*lastErr=\"undefined super method\"; return 1;} sp-=2; s[sp++]=bv; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_PUSH_SUPER_THIS:
                writeAll(f, "      { V sv; if(!super_from_this(l[0],&sv)){ if(lastErr)*lastErr=\"super not available\"; return 1;} s[sp++]=sv; }\n");
                break;
            case NIR_GET_GLOBAL:
            {
                if (inst.arg < 0 || inst.arg >= callee->chunk.constants.size)
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                Value gv = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(gv))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *gname = AS_STR(gv);
                int slot = resolveGlobalSlotByName(gname, &globalNames, &globalNameCount, &globalNameCap);
                if (fprintf(f, "      if(!gdef[%d]) { V gv=(V){VT_NIL,0,NULL,0,NULL}; if(!resolve_dynamic_global(", slot) < 0)
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                if (!writeEscapedCStringLiteral(f, gname->chars, gname->len) || fprintf(f, ",&gv,lastErr)) return 1; s[sp++] = gv; } else { s[sp++] = g[%d]; }\n", slot) < 0)
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_SET_GLOBAL:
            {
                if (inst.arg < 0 || inst.arg >= callee->chunk.constants.size)
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                Value gv = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(gv))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                int slot = resolveGlobalSlotByName(AS_STR(gv), &globalNames, &globalNameCount, &globalNameCap);
                fprintf(f, "      if(gconst[%d]) { if(lastErr) *lastErr=\"assign const global\"; return 1; } g[%d]=s[sp-1]; gdef[%d]=1;\n", slot, slot, slot);
                break;
            }
            case NIR_DEF_GLOBAL:
            {
                if (inst.arg < 0 || inst.arg >= callee->chunk.constants.size)
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                Value gv = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(gv))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                int slot = resolveGlobalSlotByName(AS_STR(gv), &globalNames, &globalNameCount, &globalNameCap);
                fprintf(f, "      g[%d]=s[sp-1]; gdef[%d]=1; sp--;\n", slot, slot);
                break;
            }
            case NIR_DEF_CONST_GLOBAL:
            {
                if (inst.arg < 0 || inst.arg >= callee->chunk.constants.size)
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                Value gv = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(gv))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                int slot = resolveGlobalSlotByName(AS_STR(gv), &globalNames, &globalNameCount, &globalNameCap);
                fprintf(f, "      if(gdef[%d]) { if(lastErr) *lastErr=\"const redeclare\"; return 1; } g[%d]=s[sp-1]; gdef[%d]=1; gconst[%d]=1; sp--;\n", slot, slot, slot, slot);
                break;
            }
            case NIR_EXPORT:
                break;
            case NIR_IMPORT:
                writeAll(f, "      { V mv=(V){VT_NIL,0,NULL,0,NULL}; if(import_value(s[sp-1],&mv,lastErr)!=0){ fprintf(stderr,\"[dotk-aot] import failed: %s\\n\", (lastErr&&*lastErr)?*lastErr:\"error\"); return 1; } s[sp-1]=mv; }\n");
                break;
            case NIR_ADD:
                writeAll(f, "      { int ok=1; s[sp-2]=v_add(s[sp-2],s[sp-1],&ok); if(!ok){ if(lastErr) *lastErr=\"bad add\"; return 1;} sp--; }\n");
                break;
            case NIR_SUB:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad sub\"; return 1;} s[sp-2]=(V){VT_NUM,a-b,NULL,0}; sp--; }\n");
                break;
            case NIR_MUL:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad mul\"; return 1;} s[sp-2]=(V){VT_NUM,a*b,NULL,0}; sp--; }\n");
                break;
            case NIR_DIV:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad div\"; return 1;} s[sp-2]=(V){VT_NUM,a/b,NULL,0}; sp--; }\n");
                break;
            case NIR_INT_DIV:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad int div\"; return 1;} s[sp-2]=(V){VT_NUM,(double)(((long)a)/((long)b)),NULL,0}; sp--; }\n");
                break;
            case NIR_MOD:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad mod\"; return 1;} s[sp-2]=(V){VT_NUM,fmod(a,b),NULL,0}; sp--; }\n");
                break;
            case NIR_POW:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad pow\"; return 1;} s[sp-2]=(V){VT_NUM,pow(a,b),NULL,0}; sp--; }\n");
                break;
            case NIR_NOT:
                writeAll(f, "      s[sp-1]=(V){VT_BOOL,0,NULL,v_falsey(s[sp-1])};\n");
                break;
            case NIR_NEG:
                writeAll(f, "      { double a; if(!v_to_num(s[sp-1],&a)){ if(lastErr) *lastErr=\"bad neg\"; return 1;} s[sp-1]=(V){VT_NUM,-a,NULL,0}; }\n");
                break;
            case NIR_EQ:
                writeAll(f, "      s[sp-2]=(V){VT_BOOL,0,NULL,v_eq(s[sp-2],s[sp-1])}; sp--;\n");
                break;
            case NIR_LT:
                writeAll(f, "      { int ok=1; int r=v_lt(s[sp-2],s[sp-1],&ok); s[sp-2]=(V){VT_BOOL,0,NULL, ok?r:0}; sp--; }\n");
                break;
            case NIR_GT:
                writeAll(f, "      { int ok=1; int r=v_gt(s[sp-2],s[sp-1],&ok); s[sp-2]=(V){VT_BOOL,0,NULL, ok?r:0}; sp--; }\n");
                break;
            case NIR_BIN_AND:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad bin and\"; return 1;} s[sp-2]=(V){VT_NUM,(double)(((long)a)&((long)b)),NULL,0}; sp--; }\n");
                break;
            case NIR_BIN_OR:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad bin or\"; return 1;} s[sp-2]=(V){VT_NUM,(double)(((long)a)|((long)b)),NULL,0}; sp--; }\n");
                break;
            case NIR_BIN_XOR:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad bin xor\"; return 1;} s[sp-2]=(V){VT_NUM,(double)(((long)a)^((long)b)),NULL,0}; sp--; }\n");
                break;
            case NIR_BIN_SHL:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad bin shl\"; return 1;} s[sp-2]=(V){VT_NUM,(double)(((long)a)<<((long)b)),NULL,0}; sp--; }\n");
                break;
            case NIR_BIN_SHR:
                writeAll(f, "      { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)){ if(lastErr) *lastErr=\"bad bin shr\"; return 1;} s[sp-2]=(V){VT_NUM,(double)(((long)a)>>((long)b)),NULL,0}; sp--; }\n");
                break;
            case NIR_TRY:
                writeAll(f, "      { V fv=s[sp-1]; V rv=(V){VT_NIL,0,NULL,0}; int er=call_value(fv,NULL,0,&rv,g,gdef,gconst,lastErr); s[sp-1]=(V){VT_BOOL,0,NULL, er?0:1}; }\n");
                break;
            case NIR_CATCH:
                writeAll(f, "      { V fv=s[sp-1]; V rv=(V){VT_NIL,0,NULL,0}; V carg=(V){VT_STR,0,(lastErr&&*lastErr)?*lastErr:\"error\",0,NULL}; if(call_value(fv,&carg,1,&rv,g,gdef,gconst,lastErr)!=0){ fprintf(stderr,\"[dotk-aot] throw call failed: %s\\n\", (lastErr&&*lastErr)?*lastErr:\"error\"); return 1; } s[sp-1]=rv; }\n");
                break;
            case NIR_PRINT:
                writeAll(f, "      v_print(s[sp-1]); sp--;\n");
                break;
            case NIR_POP:
                writeAll(f, "      sp--;\n");
                break;
            case NIR_DUP:
                writeAll(f, "      s[sp]=s[sp-1]; sp++;\n");
                break;
            case NIR_JUMP:
            {
                int target = resolveJumpTargetToEmittedIp(&fir, callee->chunk.size, inst.arg);
                fprintf(f, "      goto F%d_L%d;\n", fi, target);
                break;
            }
            case NIR_JUMP_IF_FALSE:
            {
                int target = resolveJumpTargetToEmittedIp(&fir, callee->chunk.size, inst.arg);
                fprintf(f, "      if(v_falsey(s[sp-1])) goto F%d_L%d;\n", fi, target);
                break;
            }
            case NIR_CALL:
                writeAll(f, "      { int ac=");
                fprintf(f, "%d", inst.arg);
                writeAll(f, "; V fv=s[sp-1-ac]; V rv=(V){VT_NIL,0,NULL,0}; if(call_value(fv,&s[sp-ac],ac,&rv,g,gdef,gconst,lastErr)!=0){ fprintf(stderr,\"[dotk-aot] call failed: %s\\n\", (lastErr&&*lastErr)?*lastErr:\"error\"); return 1; } sp-=ac+1; s[sp++]=rv; }\n");
                break;
            case NIR_INVOKE:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { int ac=");
                fprintf(f, "%d", inst.arg2);
                writeAll(f, "; V recv=s[sp-1-ac]; V rv=(V){VT_NIL,0,NULL,0}; V mname=(V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; if(invoke_value(recv,mname,&s[sp-ac],ac,&rv,g,gdef,gconst,lastErr)!=0){ fprintf(stderr,\"[dotk-aot] invoke failed: %s\\n\", (lastErr&&*lastErr)?*lastErr:\"error\"); return 1; } sp-=ac+1; s[sp++]=rv; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_SUPER_INVOKE:
            {
                Value c = callee->chunk.constants.values[inst.arg];
                if (!IS_STR(c))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                ObjString *name = AS_STR(c);
                writeAll(f, "      { int ac=");
                fprintf(f, "%d", inst.arg2);
                writeAll(f, "; V superv=s[sp-1]; if(superv.t!=VT_CLASS){ if(lastErr)*lastErr=\"bad super class\"; return 1;} V recv=s[sp-2-ac]; V rv=(V){VT_NIL,0,NULL,0}; V mname=(V){VT_STR,0,");
                if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; if(invoke_super(recv,(Class*)superv.p,mname,&s[sp-ac-1],ac,&rv,g,gdef,gconst,lastErr)!=0) return 1; sp-=ac+2; s[sp++]=rv; }\n"))
                {
                    free(flabel);
                    free(fir.items);
                    free(fnNeededByConst);
                    free(labelNeeded);
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                break;
            }
            case NIR_RET:
                writeAll(f, "      *out = s[sp-1]; return 0;\n");
                break;
            case NIR_RET_THIS:
                writeAll(f, "      *out = l[0]; return 0;\n");
                break;
            case NIR_RET_NIL:
                writeAll(f, "      *out = (V){VT_NIL,0,NULL,0}; return 0;\n");
                break;
            default:
                writeAll(f, "      if(lastErr) *lastErr=\"unsupported fn ir\"; return 1;\n");
                break;
            }
        }

        if (flabel != NULL && flabel[callee->chunk.size])
            fprintf(f, "F%d_L%d:\n", fi, callee->chunk.size);

        writeAll(f, "      *out = (V){VT_NIL,0,NULL,0}; return 0;\n    }\n");
        free(flabel);
        free(fir.items);
    }

    if (!writeAll(f, "    default: fprintf(stderr, \"[dotk-aot] unknown function id=%d\\n\", fnId); if(lastErr) *lastErr=\"unknown function\"; return 1;\n  }\n}\n\n"))
    {
        free(fnNeededByConst);
        free(labelNeeded);
        free(globalSlotByConst);
        return false;
    }

    if (!writeAll(f,
                  "int main(void){\n"
                  "  V s[4096];\n"
                  "  int sp = 0;\n"
                  "  V l[8192];\n"
                  "  V g[8192];\n"
                  "  unsigned char gdef[8192] = {0};\n"
                  "  unsigned char gconst[8192] = {0};\n"
                  "  const char *lastErr = \"native error\";\n"))
    {
        free(labelNeeded);
        free(globalSlotByConst);
        return false;
    }

    for (int i = 0; i < ir->count; i++)
    {
        NativeIrInst inst = ir->items[i];
        char line[256];
        int slot = -1;

        if (labelNeeded != NULL && inst.sourceIp >= 0 && inst.sourceIp < codeSize && labelNeeded[inst.sourceIp])
        {
            snprintf(line, sizeof(line), "L%d:\n", inst.sourceIp);
            if (!writeAll(f, line))
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
        }

        switch (inst.op)
        {
        case NIR_PUSH_CONST:
            snprintf(line, sizeof(line), "  s[sp++] = (V){VT_NUM, %.17a, NULL, 0};\n", inst.number);
            if (!writeAll(f, line))
                return false;
            break;
        case NIR_PUSH_STR:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            ObjString *sconst = AS_STR(c);
            if (!writeAll(f, "  s[sp++] = (V){VT_STR, 0, "))
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, sconst->chars, sconst->len))
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            if (!writeAll(f, ", 0};\n"))
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            break;
        }
        case NIR_PUSH_NIL:
            if (!writeAll(f, "  s[sp++] = (V){VT_NIL, 0, NULL, 0};\n"))
                return false;
            break;
        case NIR_PUSH_TRUE:
            if (!writeAll(f, "  s[sp++] = (V){VT_BOOL, 0, NULL, 1};\n"))
                return false;
            break;
        case NIR_PUSH_FALSE:
            if (!writeAll(f, "  s[sp++] = (V){VT_BOOL, 0, NULL, 0};\n"))
                return false;
            break;
        case NIR_BUILD_LIST:
            snprintf(line, sizeof(line), "  { List *l=list_new(); if(!l) return 1; int n=%d; int base=sp-n; for(int k=0;k<n;k++){ if(!list_push(l,s[base+k])) return 1; } sp=base; s[sp++]=(V){VT_LIST,0,NULL,0,l}; }\n", inst.arg);
            if (!writeAll(f, line))
                return false;
            break;
        case NIR_BUILD_DEFAULT_LIST:
            if (!writeAll(f, "  { int hd=") || fprintf(f, "%d", inst.arg) < 0 ||
                !writeAll(f, "; V defv=(V){VT_NIL,0,NULL,0,NULL}; V sizev=hd?s[sp-2]:s[sp-1]; if(hd) defv=s[sp-1]; if(sizev.t!=VT_NUM) return 1; int n=(int)sizev.n; if(n<0) return 1; List *l=list_new(); if(!l) return 1; for(int k=0;k<n;k++){ if(!list_push(l,defv)) return 1; } sp-=hd?2:1; s[sp++]=(V){VT_LIST,0,NULL,0,l}; }\n"))
                return false;
            break;
        case NIR_BUILD_SLICE:
            if (!writeAll(f, "  { V stepv=s[sp-1]; V endv=s[sp-2]; V startv=s[sp-3]; if(stepv.t!=VT_NUM||endv.t!=VT_NUM||startv.t!=VT_NUM) return 1; Slice *sl=slice_new((int)startv.n,(int)endv.n,(int)stepv.n); if(!sl) return 1; sp-=3; s[sp++]=(V){VT_SLICE,0,NULL,0,sl}; }\n"))
                return false;
            break;
        case NIR_BUILD_MAP:
            snprintf(line, sizeof(line), "  { Map *m=map_new(); if(!m) return 1; int n=%d; int base=sp-(n*2); for(int k=0;k<n;k++){ if(!map_set(m,s[base+k*2],s[base+k*2+1])) return 1; } sp=base; s[sp++]=(V){VT_MAP,0,NULL,0,m}; }\n", inst.arg);
            if (!writeAll(f, line))
                return false;
            break;
        case NIR_INDEX_SUBSCR:
            if (!writeAll(f, "  { V idx=s[sp-1]; V obj=s[sp-2]; V outv=(V){VT_NIL,0,NULL,0,NULL}; if(obj.t==VT_LIST){ if(idx.t==VT_NUM){ int ii=list_norm_index((List*)obj.p,(int)idx.n); if(ii<0) return 1; outv=((List*)obj.p)->items[ii]; } else if(idx.t==VT_SLICE){ if(!list_slice((List*)obj.p,(Slice*)idx.p,&outv)) return 1; } else return 1; } else if(obj.t==VT_STR){ if(idx.t!=VT_NUM) return 1; int ii=(int)idx.n; int len=(int)strlen(obj.s?obj.s:\"\"); if(ii<0) ii=len+ii; if(ii<0||ii>=len) return 1; char *tmp=(char*)malloc(2); if(!tmp) return 1; tmp[0]=obj.s[ii]; tmp[1]='\\0'; outv=(V){VT_STR,0,tmp,0,NULL}; } else if(obj.t==VT_MAP){ if(!map_get((Map*)obj.p,idx,&outv)) return 1; } else return 1; sp-=2; s[sp++]=outv; }\n"))
                return false;
            break;
        case NIR_STORE_SUBSCR:
            if (!writeAll(f, "  { V val=s[sp-1]; V idx=s[sp-2]; V obj=s[sp-3]; if(obj.t==VT_LIST){ if(idx.t!=VT_NUM) return 1; int ii=list_norm_index((List*)obj.p,(int)idx.n); if(ii<0) return 1; ((List*)obj.p)->items[ii]=val; } else if(obj.t==VT_MAP){ if(!map_set((Map*)obj.p,idx,val)) return 1; } else return 1; sp-=3; s[sp++]=val; }\n"))
                return false;
            break;
        case NIR_ADD:
            if (!writeAll(f, "  { int ok=1; s[sp-2] = v_add(s[sp-2], s[sp-1], &ok); if(!ok) return 1; sp--; }\n"))
                return false;
            break;
        case NIR_SUB:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,a-b,NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_MUL:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,a*b,NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_DIV:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,a/b,NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_INT_DIV:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,(double)(((long)a)/((long)b)),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_MOD:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,fmod(a,b),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_POW:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,pow(a,b),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_EQ:
            if (!writeAll(f, "  s[sp-2] = (V){VT_BOOL,0,NULL,v_eq(s[sp-2],s[sp-1])}; sp--;\n"))
                return false;
            break;
        case NIR_LT:
            if (!writeAll(f, "  { int ok=1; int r=v_lt(s[sp-2],s[sp-1],&ok); s[sp-2]=(V){VT_BOOL,0,NULL, ok?r:0}; sp--; }\n"))
                return false;
            break;
        case NIR_GT:
            if (!writeAll(f, "  { int ok=1; int r=v_gt(s[sp-2],s[sp-1],&ok); s[sp-2]=(V){VT_BOOL,0,NULL, ok?r:0}; sp--; }\n"))
                return false;
            break;
        case NIR_NOT:
            if (!writeAll(f, "  s[sp-1] = (V){VT_BOOL,0,NULL,v_falsey(s[sp-1])};\n"))
                return false;
            break;
        case NIR_BIN_AND:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,(double)(((long)a)&((long)b)),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_BIN_OR:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,(double)(((long)a)|((long)b)),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_BIN_XOR:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,(double)(((long)a)^((long)b)),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_BIN_SHL:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,(double)(((long)a)<<((long)b)),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_BIN_SHR:
            if (!writeAll(f, "  { double a,b; if(!v_to_num(s[sp-2],&a)||!v_to_num(s[sp-1],&b)) return 1; s[sp-2]=(V){VT_NUM,(double)(((long)a)>>((long)b)),NULL,0}; sp--; }\n"))
                return false;
            break;
        case NIR_GET_LOCAL:
            if (inst.arg == 0)
            {
                if (!writeAll(f, "  s[sp++] = l[0];\n"))
                    return false;
            }
            else
            {
                snprintf(line, sizeof(line), "  if(%d-1 < sp) s[sp++] = s[%d-1]; else s[sp++] = l[%d];\n", inst.arg, inst.arg, inst.arg);
                if (!writeAll(f, line))
                    return false;
            }
            break;
        case NIR_SET_LOCAL:
            if (inst.arg == 0)
                snprintf(line, sizeof(line), "  l[0] = s[sp-1];\n");
            else
                snprintf(line, sizeof(line), "  if(%d-1 < 4096) s[%d-1] = s[sp-1]; l[%d] = s[sp-1];\n", inst.arg, inst.arg, inst.arg);
            if (!writeAll(f, line))
                return false;
            break;
        case NIR_CLASS:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            if (!writeAll(f, "  { Class *k=class_new("))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, "); if(!k) return 1; s[sp++]=(V){VT_CLASS,0,NULL,0,k}; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_METHOD:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            if (!writeAll(f, "  { V name=(V){VT_STR,0,"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V method=s[sp-1]; V klassv=s[sp-2]; if(klassv.t!=VT_CLASS) return 1; if(!class_set_method((Class*)klassv.p,name,method)) return 1; sp--; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_STATIC_VAR:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            if (!writeAll(f, "  { V name=(V){VT_STR,0,"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V value=s[sp-1]; V klassv=s[sp-2]; if(klassv.t!=VT_CLASS) return 1; if(!map_set(&((Class*)klassv.p)->staticVars,name,value)) return 1; sp--; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_GET_PROPERTY:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            if (!writeAll(f, "  { V name=(V){VT_STR,0,"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V prop; if(!get_property(s[sp-1],name,&prop)) return 1; s[sp-1]=prop; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_SET_PROPERTY:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            if (!writeAll(f, "  { V name=(V){VT_STR,0,"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V value=s[sp-1]; V obj=s[sp-2]; if(!set_property(obj,name,value)) return 1; sp-=2; s[sp++]=value; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_INHERIT:
            if (!writeAll(f, "  { V superv=s[sp-2]; V subv=s[sp-1]; if(superv.t!=VT_CLASS||subv.t!=VT_CLASS) return 1; if(!class_inherit((Class*)subv.p,(Class*)superv.p)) return 1; sp--; }\n"))
                return false;
            break;
        case NIR_GET_SUPER:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            if (!writeAll(f, "  { V name=(V){VT_STR,0,"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; V superv=s[sp-1]; V recv=s[sp-2]; if(superv.t!=VT_CLASS) return 1; V bv; if(!get_super_bound(recv,(Class*)superv.p,name,&bv)) return 1; sp-=2; s[sp++]=bv; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_PUSH_SUPER_THIS:
            if (!writeAll(f, "  { V sv; if(!super_from_this(l[0],&sv)) return 1; s[sp++]=sv; }\n"))
                return false;
            break;
        case NIR_GET_GLOBAL:
            if (inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!IS_STR(function->chunk.constants.values[inst.arg]))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            slot = resolveGlobalSlotByName(AS_STR(function->chunk.constants.values[inst.arg]), &globalNames, &globalNameCount, &globalNameCap);
            if (slot < 0)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            {
                ObjString *gname = AS_STR(function->chunk.constants.values[inst.arg]);
                if (fprintf(f, "  if(!gdef[%d]) { V gv=(V){VT_NIL,0,NULL,0,NULL}; if(!resolve_dynamic_global(", slot) < 0)
                {
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
                if (!writeEscapedCStringLiteral(f, gname->chars, gname->len) || fprintf(f, ",&gv,&lastErr)) return 1; s[sp++] = gv; } else s[sp++] = g[%d];\n", slot) < 0)
                {
                    free(globalSlotByConst);
                    free(globalNames);
                    return false;
                }
            }
            break;
        case NIR_SET_GLOBAL:
            if (inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!IS_STR(function->chunk.constants.values[inst.arg]))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            slot = resolveGlobalSlotByName(AS_STR(function->chunk.constants.values[inst.arg]), &globalNames, &globalNameCount, &globalNameCap);
            if (slot < 0)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            snprintf(line, sizeof(line), "  if(gconst[%d]) return 1; g[%d] = s[sp-1]; gdef[%d] = 1;\n", slot, slot, slot);
            if (!writeAll(f, line))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        case NIR_EXPORT:
            break;
        case NIR_IMPORT:
            if (!writeAll(f, "  { V mv=(V){VT_NIL,0,NULL,0,NULL}; if(import_value(s[sp-1],&mv,&lastErr)!=0){ fprintf(stderr,\"[dotk-aot] import failed: %s\\n\", lastErr?lastErr:\"error\"); return 1; } s[sp-1]=mv; }\n"))
                return false;
            break;
        case NIR_DEF_GLOBAL:
            if (inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!IS_STR(function->chunk.constants.values[inst.arg]))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            slot = resolveGlobalSlotByName(AS_STR(function->chunk.constants.values[inst.arg]), &globalNames, &globalNameCount, &globalNameCap);
            if (slot < 0)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            snprintf(line, sizeof(line), "  g[%d] = s[sp-1]; gdef[%d] = 1; sp--;\n", slot, slot);
            if (!writeAll(f, line))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        case NIR_DEF_CONST_GLOBAL:
            if (inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!IS_STR(function->chunk.constants.values[inst.arg]))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            slot = resolveGlobalSlotByName(AS_STR(function->chunk.constants.values[inst.arg]), &globalNames, &globalNameCount, &globalNameCap);
            if (slot < 0)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            snprintf(line, sizeof(line), "  if(gdef[%d]) return 1; g[%d] = s[sp-1]; gdef[%d] = 1; gconst[%d] = 1; sp--;\n", slot, slot, slot, slot);
            if (!writeAll(f, line))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        case NIR_MAKE_CLOSURE:
            snprintf(line, sizeof(line), "  s[sp++] = (V){VT_FUN,0,NULL,%d};\n", inst.arg);
            if (!writeAll(f, line))
            {
                free(fnNeededByConst);
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            break;
        case NIR_CALL:
            snprintf(line, sizeof(line), "  { int ac=%d; V fv=s[sp-1-ac]; V rv=(V){VT_NIL,0,NULL,0}; if(call_value(fv,&s[sp-ac],ac,&rv,g,gdef,gconst,&lastErr)!=0){ fprintf(stderr,\"[dotk-aot] call failed: %%s\\n\", lastErr?lastErr:\"error\"); return 1; } sp-=ac+1; s[sp++]=rv; }\n", inst.arg);
            if (!writeAll(f, line))
            {
                free(fnNeededByConst);
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            break;
        case NIR_INVOKE:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            snprintf(line, sizeof(line), "  { int ac=%d; V recv=s[sp-1-ac]; V rv=(V){VT_NIL,0,NULL,0}; V mname=(V){VT_STR,0,", inst.arg2);
            if (!writeAll(f, line))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; if(invoke_value(recv,mname,&s[sp-ac],ac,&rv,g,gdef,gconst,&lastErr)!=0){ fprintf(stderr,\"[dotk-aot] invoke failed: %s\\n\", lastErr?lastErr:\"error\"); return 1; } sp-=ac+1; s[sp++]=rv; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_SUPER_INVOKE:
        {
            if (function == NULL || inst.arg < 0 || inst.arg >= function->chunk.constants.size)
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            Value c = function->chunk.constants.values[inst.arg];
            if (!IS_STR(c))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            ObjString *name = AS_STR(c);
            snprintf(line, sizeof(line), "  { int ac=%d; V superv=s[sp-1]; if(superv.t!=VT_CLASS) return 1; V recv=s[sp-2-ac]; V rv=(V){VT_NIL,0,NULL,0}; V mname=(V){VT_STR,0,", inst.arg2);
            if (!writeAll(f, line))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            if (!writeEscapedCStringLiteral(f, name->chars, name->len) || !writeAll(f, ",0,NULL}; if(invoke_super(recv,(Class*)superv.p,mname,&s[sp-ac-1],ac,&rv,g,gdef,gconst,&lastErr)!=0) return 1; sp-=ac+2; s[sp++]=rv; }\n"))
            {
                free(globalSlotByConst);
                free(globalNames);
                return false;
            }
            break;
        }
        case NIR_TRY:
            if (!writeAll(f, "  { V fv=s[sp-1]; V rv=(V){VT_NIL,0,NULL,0}; int er=call_value(fv,NULL,0,&rv,g,gdef,gconst,&lastErr); s[sp-1]=(V){VT_BOOL,0,NULL, er?0:1}; }\n"))
            {
                free(fnNeededByConst);
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            break;
        case NIR_CATCH:
            if (!writeAll(f, "  { V fv=s[sp-1]; V rv=(V){VT_NIL,0,NULL,0}; V carg=(V){VT_STR,0,lastErr?lastErr:\"error\",0}; if(call_value(fv,&carg,1,&rv,g,gdef,gconst,&lastErr)!=0){ fprintf(stderr,\"[dotk-aot] throw call failed: %s\\n\", lastErr?lastErr:\"error\"); return 1; } s[sp-1]=rv; }\n"))
            {
                free(fnNeededByConst);
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            break;
        case NIR_JUMP:
        {
            int target = resolveJumpTargetToEmittedIp(ir, codeSize, inst.arg);
            snprintf(line, sizeof(line), "  goto L%d;\n", target);
            if (!writeAll(f, line))
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            break;
        }
        case NIR_JUMP_IF_FALSE:
        {
            int target = resolveJumpTargetToEmittedIp(ir, codeSize, inst.arg);
            snprintf(line, sizeof(line), "  if (v_falsey(s[sp-1])) goto L%d;\n", target);
            if (!writeAll(f, line))
            {
                free(labelNeeded);
                free(globalSlotByConst);
                return false;
            }
            break;
        }
        case NIR_NEG:
            if (!writeAll(f, "  { double a; if(!v_to_num(s[sp-1],&a)) return 1; s[sp-1]=(V){VT_NUM,-a,NULL,0}; }\n"))
                return false;
            break;
        case NIR_POP:
            if (!writeAll(f, "  sp--;\n"))
                return false;
            break;
        case NIR_DUP:
            if (!writeAll(f, "  s[sp] = s[sp-1]; sp++;\n"))
                return false;
            break;
        case NIR_PRINT:
            if (!writeAll(f, "  v_print(s[sp-1]); sp--;\n"))
                return false;
            break;
        case NIR_RET:
            if (!writeAll(f, "  return 0;\n"))
            {
                free(fnNeededByConst);
                free(globalSlotByConst);
                return false;
            }
            break;
            free(fnNeededByConst);
        case NIR_RET_THIS:
            if (!writeAll(f, "  return 0;\n"))
            {
                free(globalSlotByConst);
                return false;
            }
            break;
        case NIR_RET_NIL:
            if (!writeAll(f, "  return 0;\n"))
            {
                free(globalSlotByConst);
                return false;
            }
            break;
        }
    }

    if (labelNeeded != NULL && codeSize >= 0 && labelNeeded[codeSize])
    {
        char endLabel[64];
        snprintf(endLabel, sizeof(endLabel), "L%d:\n", codeSize);
        if (!writeAll(f, endLabel))
        {
            free(labelNeeded);
            free(globalSlotByConst);
            return false;
        }
    }

    if (!writeAll(f, "  return 0;\n}\n"))
    {
        free(globalSlotByConst);
        return false;
    }
    free(globalSlotByConst);
    return true;
}

static void setErr(char *errBuf, size_t errBufCap, const char *fmt, ...)
{
    if (errBuf == NULL || errBufCap == 0)
        return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(errBuf, errBufCap, fmt, args);
    va_end(args);
}

bool nativeAotWriteExecutable(ObjFunction *function,
                              const char *outPath,
                              bool keepTempSource,
                              char *keptSourcePathOut,
                              size_t keptSourcePathOutCap,
                              char *errBuf,
                              size_t errBufCap)
{
    if (errBuf != NULL && errBufCap > 0)
        errBuf[0] = '\0';
    if (keptSourcePathOut != NULL && keptSourcePathOutCap > 0)
        keptSourcePathOut[0] = '\0';

    if (!nativeExecIsSupported())
    {
        setErr(errBuf, errBufCap, "native AOT is only supported on Linux x86_64 right now");
        return false;
    }
    if (function == NULL || outPath == NULL || outPath[0] == '\0')
    {
        setErr(errBuf, errBufCap, "invalid native AOT inputs");
        return false;
    }
    if (function->upValueCount != 0)
    {
        setErr(errBuf, errBufCap, "native AOT does not support closures yet");
        return false;
    }

    NativeIrVec ir = {0};
    if (!parseBytecodeToNativeIr(function, &ir))
    {
        free(ir.items);
        setErr(errBuf, errBufCap, "unsupported bytecode for native AOT lowering");
        return false;
    }

    char srcTemplate[] = "/tmp/dotk_aot_XXXXXX.c";
    int fd = mkstemps(srcTemplate, 2);
    if (fd < 0)
    {
        free(ir.items);
        setErr(errBuf, errBufCap, "failed to create temp source: %s", strerror(errno));
        return false;
    }

    FILE *temp = fdopen(fd, "w");
    if (temp == NULL)
    {
        close(fd);
        unlink(srcTemplate);
        free(ir.items);
        setErr(errBuf, errBufCap, "failed to open temp source stream: %s", strerror(errno));
        return false;
    }

    bool wrote = writeAotCProgram(temp, &ir, function);
    free(ir.items);
    if (!wrote || fflush(temp) != 0)
    {
        fclose(temp);
        unlink(srcTemplate);
        setErr(errBuf, errBufCap, "unsupported native IR for current AOT C emitter");
        return false;
    }
    fclose(temp);

    pid_t pid = fork();
    if (pid < 0)
    {
        unlink(srcTemplate);
        setErr(errBuf, errBufCap, "failed to fork for gcc: %s", strerror(errno));
        return false;
    }
    if (pid == 0)
    {
        execlp("gcc", "gcc", "-O3", "-s", srcTemplate, "-lm", "-ldl", "-o", outPath, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        unlink(srcTemplate);
        setErr(errBuf, errBufCap, "failed waiting for gcc: %s", strerror(errno));
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        unlink(srcTemplate);
        setErr(errBuf, errBufCap, "gcc failed to build native executable");
        return false;
    }

    if (keepTempSource)
    {
        if (keptSourcePathOut != NULL && keptSourcePathOutCap > 0)
            snprintf(keptSourcePathOut, keptSourcePathOutCap, "%s", srcTemplate);
    }
    else
    {
        unlink(srcTemplate);
    }

    return true;
}

bool nativeExecIsSupported(void)
{
    return true;
}

bool nativeTryExecuteFunction(ObjFunction *function, Value *resultOut)
{
    if (function == NULL || resultOut == NULL)
        return false;
    if (function->upValueCount != 0)
        return false;

    NativeIrVec ir = {0};
    CodeBuf cb = {0};
    bool ok = parseBytecodeToNativeIr(function, &ir);
    if (!ok)
    {
        free(ir.items);
        return false;
    }

    ok = compileNativeMachineCode(&ir, &cb);
    free(ir.items);
    if (!ok || cb.len == 0)
    {
        free(cb.bytes);
        return false;
    }

    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0)
    {
        free(cb.bytes);
        return false;
    }
    size_t allocSize = (cb.len + (size_t)pageSize - 1) & ~((size_t)pageSize - 1);
    void *mem = mmap(NULL, allocSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
    {
        free(cb.bytes);
        return false;
    }

    memcpy(mem, cb.bytes, cb.len);
    free(cb.bytes);
    if (mprotect(mem, allocSize, PROT_READ | PROT_EXEC) != 0)
    {
        munmap(mem, allocSize);
        return false;
    }

    typedef double (*NativeEntry)(void);
    NativeEntry fn = (NativeEntry)mem;
    double nativeResult = fn();
    *resultOut = NUM_VAL(nativeResult);
    munmap(mem, allocSize);
    return true;
}

#else

bool nativeExecIsSupported(void)
{
    return false;
}

bool nativeTryExecuteFunction(ObjFunction *function, Value *resultOut)
{
    (void)function;
    (void)resultOut;
    return false;
}

bool nativeDumpFunctionIr(ObjFunction *function, FILE *out)
{
    (void)function;
    if (out == NULL)
        out = stderr;
    fprintf(out, "[native-ir] unsupported platform for native backend\n");
    return false;
}

bool nativeAotWriteExecutable(ObjFunction *function,
                              const char *outPath,
                              bool keepTempSource,
                              char *keptSourcePathOut,
                              size_t keptSourcePathOutCap,
                              char *errBuf,
                              size_t errBufCap)
{
    (void)function;
    (void)outPath;
    (void)keepTempSource;
    (void)keptSourcePathOut;
    (void)keptSourcePathOutCap;
    if (errBuf != NULL && errBufCap > 0)
        snprintf(errBuf, errBufCap, "native AOT is unsupported on this platform");
    return false;
}

#endif