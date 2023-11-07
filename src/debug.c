#include "include/debug.h"
#include "include/object.h"
#include "include/value.h"
#include <stdio.h>

bool nextWideOp = false;

void disassembleChunk(Chunk *chunk, const char *name)
{
    printf("== %s ==\n", name);
    if (chunk == NULL)
        return;
    for (int offset = 0; offset < chunk->size;)
    {
        offset = disassembleInst(chunk, offset);
    }
}
static int simpleInst(const char *name, int offset)
{
    printf("%s\n", name);
    return offset + 1;
}

static int byteInst(const char *name, Chunk *chunk, int offset)
{
    if (nextWideOp)
    {
        nextWideOp = false;
        uint16_t slot = chunk->code[offset + 1] << 8 | chunk->code[offset + 2];
        printf("%-16s %4d\n", name, slot);
        return offset + 3;
    }
    uint8_t slot = chunk->code[offset + 1];
    printf("%-16s %4d\n", name, slot);
    return offset + 2;
}

static int jumpInst(const char *name, int sign, Chunk *chunk, int offset)
{
    uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
    jump |= chunk->code[offset + 2];
    printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

static int constInst(const char *name, Chunk *chunk, int offset)
{
    if (nextWideOp)
    {
        nextWideOp = false;
        uint16_t constant = chunk->code[offset + 1];
        constant = constant << 8;
        constant |= chunk->code[offset + 2];
        printf("%-16s %4d '", name, constant);
        printValue(chunk->constants.values[constant], 1);
        printf("'\n");
        return offset + 3;
    }
    uint8_t constant = chunk->code[offset + 1];
    printf("%-16s %4d '", name, constant);
    printValue(chunk->constants.values[constant], 1);
    printf("'\n");
    return offset + 2;
}

static int wideInst(const char *name, Chunk *chunk, int offset)
{
    nextWideOp = true;
    printf("%-16s [ '%4d' | '%4d' ]\n", name, chunk->code[offset + 2], chunk->code[offset + 3]);
    return offset + 1;
}

static int longConstInst(const char *name, Chunk *chunk, int offset)
{
    uint32_t constant = chunk->code[offset + 1] |
                        (chunk->code[offset + 2] << 8) |
                        (chunk->code[offset + 3] << 16);
    printf("%-16s %4d '", name, constant);
    printValue(chunk->constants.values[constant], 1);
    printf("'\n");
    return offset + 4;
}

static int invokeInst(const char *name, Chunk *chunk, int offset)
{
    uint16_t constant;
    uint16_t argc;
    if (nextWideOp)
    {
        nextWideOp = false;
        constant = chunk->code[offset + 1] << 8 | chunk->code[offset + 2];
        argc = chunk->code[offset + 3];
        offset += 4;
    }
    else
    {
        constant = chunk->code[offset + 1];
        argc = chunk->code[offset + 2];
        offset += 3;
    }
    printf("%-16s (%d args) %4d '", name, argc, constant);
    printValue(chunk->constants.values[constant], 1);
    printf("'\n");
    return offset;
}

int disassembleInst(Chunk *chunk, int offset)
{
    printf("%04d ", offset);
    Position pos = getPos(chunk, offset);
    if (offset > 0 && pos.line == getPos(chunk, offset - 1).line)
        printf("   | ");
    else
        printf("%4d ", pos.line);

    uint8_t inst = chunk->code[offset];
    switch (inst)
    {
    case OP_ADD:
        return simpleInst("OP_ADD", offset);
    case OP_SUB:
        return simpleInst("OP_SUB", offset);
    case OP_MULT:
        return simpleInst("OP_MULTIPLY", offset);
    case OP_POW:
        return simpleInst("OP_POWER", offset);
    case OP_MOD:
        return simpleInst("OP_MOD", offset);
    case OP_DIV:
        return simpleInst("OP_DIVIDE", offset);
    case OP_INT_DIV:
        return simpleInst("OP_INT_DIVIDE", offset);
    case OP_NEGATE:
        return simpleInst("OP_NEGATE", offset);
    case OP_RETURN:
        return simpleInst("OP_RETURN", offset);
    case OP_NIL:
        return simpleInst("OP_NIL", offset);
    case OP_TRUE:
        return simpleInst("OP_TRUE", offset);
    case OP_FALSE:
        return simpleInst("OP_FALSE", offset);
    case OP_NOT:
        return simpleInst("OP_NOT", offset);
    case OP_BUILD_SLICE:
        return simpleInst("OP_BUILD_SLICE", offset);
    case OP_EQUAL:
        return simpleInst("OP_EQUAL", offset);
    case OP_GREATER:
        return simpleInst("OP_GREATER", offset);
    case OP_LESS:
        return simpleInst("OP_LESS", offset);
    case OP_PRINT:
        return simpleInst("OP_PRINT", offset);
    case OP_POP:
        return simpleInst("OP_POP", offset);
    case OP_DUP:
        return simpleInst("OP_DUP", offset);
    case OP_CLOSE_UPVALUE:
        return simpleInst("OP_CLOSE_UPVALUE", offset);
    case OP_INHERIT:
        return simpleInst("OP_INHERIT", offset);
    case OP_STORE_SUBSCR:
        return simpleInst("OP_STORE_LIST", offset);
    case OP_INDEX_SUBSCR:
        return simpleInst("OP_INDEX_LIST", offset);
    case OP_CLOSURE:
    {
        uint16_t constant;
        offset++;
        if (nextWideOp)
        {
            nextWideOp = false;
            // constant = chunk->code[offset + 1];
            // constant = constant << 8;
            // constant |= chunk->code[offset + 2];
            constant = (chunk->code[offset++] << 8) | (chunk->code[offset++]);
            //
        }
        else
        {
            constant = chunk->code[offset++];
        }
        printf("%-16s %4d ", "OP_CLOSURE", constant);
        printValue(chunk->constants.values[constant], 1);
        printf("\n");

        ObjFunction *function = AS_FUN(chunk->constants.values[constant]);
        for (int j = 0; j < function->upValueCount; j++)
        {
            int isLocal = chunk->code[offset++];
            int index = chunk->code[offset++];
            printf("%04d    |                   %s %d\n", offset - 2, isLocal ? "local" : "upvalue", index);
        }

        return offset;
    }
    case OP_WIDE:
        return wideInst("OP_WIDE", chunk, offset);
    case OP_CONSTANT_LONG:
        return longConstInst("OP_CONST_LONG", chunk, offset);
    case OP_CONSTANT:
        return constInst("OP_CONST", chunk, offset);
    case OP_IMPORT:
        return simpleInst("OP_IMPORT", offset);
    case OP_GET_SUPER:
        return constInst("OP_GET_SUPER", chunk, offset);
    case OP_DEF_GLOBAL:
        return constInst("OP_DEF_GLOBAL", chunk, offset);
    case OP_GET_GLOBAL:
        return constInst("OP_GET_GLOBAL", chunk, offset);
    case OP_SET_GLOBAL:
        return constInst("OP_SET_GLOBAL", chunk, offset);
    case OP_SET_PROPERTY:
        return constInst("OP_SET_PROP", chunk, offset);
    case OP_GET_PROPERTY:
        return constInst("OP_GET_PROP", chunk, offset);
    case OP_CLASS:
        return constInst("OP_CLASS", chunk, offset);
    case OP_METHOD:
        return constInst("OP_METHOD", chunk, offset);
    case OP_STATIC_VAR:
        return constInst("OP_STATIC_VAR", chunk, offset);
    case OP_SET_LOCAL:
        return byteInst("OP_SET_LOCAL", chunk, offset);
    case OP_GET_LOCAL:
        return byteInst("OP_GET_LOCAL", chunk, offset);
    case OP_GET_UPVALUE:
        return byteInst("OP_GET_UPVALUE", chunk, offset);
    case OP_SET_UPVALUE:
        return byteInst("OP_SET_UPVALUE", chunk, offset);
    case OP_BUILD_LIST:
        return byteInst("OP_BUILD_LIST", chunk, offset);
    case OP_BUILD_DEFAULT_LIST:
        return byteInst("OP_BUILD_DEFAULT_LIST", chunk, offset);
    case OP_CALL:
        return byteInst("OP_CALL", chunk, offset);
    case OP_INVOKE:
        return invokeInst("OP_INVOKE", chunk, offset);
    case OP_SUPER_INVOKE:
        return invokeInst("OP_SUPER_INVOKE", chunk, offset);
    case OP_JUMP_IF_FALSE:
        return jumpInst("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_JUMP:
        return jumpInst("OP_JUMP", 1, chunk, offset);
    case OP_LOOP:
        return jumpInst("OP_LOOP", -1, chunk, offset);
    default:
        printf("Unknown OpCode %d\n", inst);
        return offset + 1;
    }
}