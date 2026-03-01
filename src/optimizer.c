#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/chunk.h"
#include "include/memory.h"
#include "include/optimizer.h"

static inline void fillNop(uint8_t *code, int start, int endExclusive)
{
    for (int i = start; i < endExclusive; i++)
        code[i] = OP_NOP;
}

static inline bool isFiniteNumber(Value v)
{
    return IS_NUM(v) && isfinite(AS_NUM(v));
}

static bool instructionSupportsWide(uint8_t inst)
{
    switch (inst)
    {
    case OP_CALL:
    case OP_DEF_GLOBAL:
    case OP_DEF_CONST_GLOBAL:
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_GLOBAL:
    case OP_SET_GLOBAL:
    case OP_GET_UPVALUE:
    case OP_SET_UPVALUE:
    case OP_BUILD_LIST:
    case OP_BUILD_MAP:
    case OP_BUILD_DEFAULT_LIST:
    case OP_CLASS:
    case OP_METHOD:
    case OP_SET_PROPERTY:
    case OP_GET_PROPERTY:
    case OP_GET_SUPER:
    case OP_STATIC_VAR:
    case OP_EXPORT:
    case OP_CONSTANT:
    case OP_INVOKE:
    case OP_SUPER_INVOKE:
    case OP_INVOKE_KW:
    case OP_SUPER_INVOKE_KW:
    case OP_CLOSURE:
        return true;
    default:
        return false;
    }
}

static int instructionLength(Chunk *chunk, int offset, bool widePending, bool *consumedWide, bool *setsWide)
{
    *consumedWide = false;
    *setsWide = false;

    if (offset >= chunk->size)
        return 1;

    uint8_t inst = chunk->code[offset];
    if (inst == OP_WIDE)
    {
        *setsWide = true;
        return 1;
    }

    if (widePending && inst == OP_NOP)
    {
        *consumedWide = true;
        return 1;
    }

    switch (inst)
    {
    case OP_CALL:
    case OP_DEF_GLOBAL:
    case OP_DEF_CONST_GLOBAL:
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_GLOBAL:
    case OP_SET_GLOBAL:
    case OP_GET_UPVALUE:
    case OP_SET_UPVALUE:
    case OP_BUILD_LIST:
    case OP_BUILD_MAP:
    case OP_BUILD_DEFAULT_LIST:
    case OP_CLASS:
    case OP_METHOD:
    case OP_SET_PROPERTY:
    case OP_GET_PROPERTY:
    case OP_GET_SUPER:
    case OP_STATIC_VAR:
    case OP_EXPORT:
    case OP_CONSTANT:
        *consumedWide = widePending;
        return widePending ? 3 : 2;

    case OP_JUMP_IF_FALSE:
    case OP_JUMP:
    case OP_LOOP:
    case OP_CALL_KW:
        return 3;

    case OP_INVOKE:
    case OP_SUPER_INVOKE:
        *consumedWide = widePending;
        return widePending ? 4 : 3;

    case OP_INVOKE_KW:
    case OP_SUPER_INVOKE_KW:
        *consumedWide = widePending;
        return widePending ? 5 : 4;

    case OP_CONSTANT_LONG:
        return 4;

    case OP_CLOSURE:
    {
        int constWidth = widePending ? 2 : 1;
        int cursor = offset + 1 + constWidth;
        if (cursor > chunk->size)
        {
            *consumedWide = widePending;
            return 1;
        }

        uint32_t constant = 0;
        if (widePending)
        {
            constant = ((uint32_t)chunk->code[offset + 1] << 8) | (uint32_t)chunk->code[offset + 2];
        }
        else
        {
            constant = (uint32_t)chunk->code[offset + 1];
        }

        if (constant < (uint32_t)chunk->constants.size)
        {
            Value fnVal = chunk->constants.values[constant];
            if (IS_FUN(fnVal))
            {
                ObjFunction *nested = AS_FUN(fnVal);
                cursor += nested->upValueCount * 2;
            }
        }

        *consumedWide = widePending;
        if (cursor > chunk->size)
            cursor = chunk->size;
        return cursor - offset;
    }

    default:
        return 1;
    }
}

static bool foldNumericBinary(Chunk *chunk, int offset)
{
    uint8_t *code = chunk->code;
    if (offset + 4 >= chunk->size)
        return false;
    if (code[offset] != OP_CONSTANT || code[offset + 2] != OP_CONSTANT)
        return false;

    uint8_t op = code[offset + 4];
    switch (op)
    {
    case OP_ADD:
    case OP_SUB:
    case OP_MULT:
    case OP_DIV:
    case OP_INT_DIV:
    case OP_MOD:
    case OP_POW:
    case OP_GREATER:
    case OP_LESS:
        break;
    default:
        return false;
    }

    uint8_t idxA = code[offset + 1];
    uint8_t idxB = code[offset + 3];
    if (idxA >= chunk->constants.size || idxB >= chunk->constants.size)
        return false;

    Value va = chunk->constants.values[idxA];
    Value vb = chunk->constants.values[idxB];
    if (!isFiniteNumber(va) || !isFiniteNumber(vb))
        return false;

    double a = AS_NUM(va);
    double b = AS_NUM(vb);

    Value out;
    switch (op)
    {
    case OP_ADD:
        out = NUM_VAL(a + b);
        break;
    case OP_SUB:
        out = NUM_VAL(a - b);
        break;
    case OP_MULT:
        out = NUM_VAL(a * b);
        break;
    case OP_DIV:
        out = NUM_VAL(a / b);
        break;
    case OP_INT_DIV:
        if ((long)b == 0)
            return false;
        out = NUM_VAL((long)a / (long)b);
        break;
    case OP_MOD:
        out = NUM_VAL(fmod(a, b));
        break;
    case OP_POW:
        out = NUM_VAL(pow(a, b));
        break;
    case OP_GREATER:
        out = BOOL_VAL(a > b);
        break;
    case OP_LESS:
        out = BOOL_VAL(a < b);
        break;
    default:
        return false;
    }

    int outIdx = addConst(chunk, out);
    if (outIdx < 0)
        return false;

    if (outIdx <= UINT8_MAX)
    {
        code[offset] = OP_CONSTANT;
        code[offset + 1] = (uint8_t)outIdx;
        fillNop(code, offset + 2, offset + 5);
        return true;
    }

    if (outIdx <= 0xFFFFFF)
    {
        code[offset] = OP_CONSTANT_LONG;
        code[offset + 1] = (uint8_t)(outIdx & 0xff);
        code[offset + 2] = (uint8_t)((outIdx >> 8) & 0xff);
        code[offset + 3] = (uint8_t)((outIdx >> 16) & 0xff);
        code[offset + 4] = OP_NOP;
        return true;
    }

    return false;
}

static bool foldUnary(Chunk *chunk, int offset)
{
    uint8_t *code = chunk->code;
    if (offset + 2 >= chunk->size)
        return false;

    if (code[offset] == OP_NIL && code[offset + 1] == OP_NOT)
    {
        code[offset] = OP_TRUE;
        code[offset + 1] = OP_NOP;
        return true;
    }
    if (code[offset] == OP_TRUE && code[offset + 1] == OP_NOT)
    {
        code[offset] = OP_FALSE;
        code[offset + 1] = OP_NOP;
        return true;
    }
    if (code[offset] == OP_FALSE && code[offset + 1] == OP_NOT)
    {
        code[offset] = OP_TRUE;
        code[offset + 1] = OP_NOP;
        return true;
    }

    if (code[offset] != OP_CONSTANT)
        return false;

    uint8_t idx = code[offset + 1];
    if (idx >= chunk->constants.size)
        return false;

    Value v = chunk->constants.values[idx];

    if (code[offset + 2] == OP_NEGATE)
    {
        if (!isFiniteNumber(v))
            return false;
        int outIdx = addConst(chunk, NUM_VAL(-AS_NUM(v)));
        if (outIdx < 0 || outIdx > UINT8_MAX)
            return false;

        code[offset] = OP_CONSTANT;
        code[offset + 1] = (uint8_t)outIdx;
        code[offset + 2] = OP_NOP;
        return true;
    }

    if (code[offset + 2] == OP_NOT)
    {
        bool falsey = false;
        if (IS_NIL(v))
            falsey = true;
        else if (IS_BOOL(v))
            falsey = !AS_BOOL(v);
        else if (IS_NUM(v))
            falsey = (AS_NUM(v) == 0);
        else if (IS_STR(v))
            falsey = (AS_STR(v)->len == 0);
        else
            return false;

        code[offset] = falsey ? OP_TRUE : OP_FALSE;
        code[offset + 1] = OP_NOP;
        code[offset + 2] = OP_NOP;
        return true;
    }

    return false;
}

static bool eliminateLiteralPop(Chunk *chunk, int offset)
{
    uint8_t *code = chunk->code;

    if (offset + 1 < chunk->size &&
        (code[offset] == OP_NIL || code[offset] == OP_TRUE || code[offset] == OP_FALSE) &&
        code[offset + 1] == OP_POP)
    {
        code[offset] = OP_NOP;
        code[offset + 1] = OP_NOP;
        return true;
    }

    if (offset + 2 < chunk->size && code[offset] == OP_CONSTANT && code[offset + 2] == OP_POP)
    {
        fillNop(code, offset, offset + 3);
        return true;
    }

    if (offset + 4 < chunk->size && code[offset] == OP_CONSTANT_LONG && code[offset + 4] == OP_POP)
    {
        fillNop(code, offset, offset + 5);
        return true;
    }

    return false;
}

static bool simplifyConstantBranches(Chunk *chunk, int offset)
{
    uint8_t *code = chunk->code;
    if (offset + 4 >= chunk->size)
        return false;

    uint8_t lit = code[offset];
    if (lit != OP_TRUE && lit != OP_FALSE && lit != OP_NIL)
        return false;

    if (code[offset + 1] != OP_JUMP_IF_FALSE)
        return false;

    if (lit == OP_TRUE)
    {
        code[offset + 1] = OP_NOP;
        code[offset + 2] = OP_NOP;
        code[offset + 3] = OP_NOP;
        return true;
    }

    code[offset + 1] = OP_JUMP;
    return true;
}

static int decodeJumpTarget(Chunk *chunk, int offset, uint8_t inst)
{
    if (offset + 2 >= chunk->size)
        return -1;

    int jump = ((int)chunk->code[offset + 1] << 8) | (int)chunk->code[offset + 2];
    if (inst == OP_LOOP)
        return offset + 3 - jump;
    return offset + 3 + jump;
}

static bool patchJumpTarget(Chunk *chunk, int offset, uint8_t inst, int target)
{
    if (offset + 2 >= chunk->size)
        return false;

    int delta = 0;
    if (inst == OP_LOOP)
    {
        delta = (offset + 3) - target;
        if (delta < 0 || delta > UINT16_MAX)
            return false;
    }
    else
    {
        delta = target - (offset + 3);
        if (delta < 0 || delta > UINT16_MAX)
            return false;
    }

    chunk->code[offset + 1] = (uint8_t)((delta >> 8) & 0xff);
    chunk->code[offset + 2] = (uint8_t)(delta & 0xff);
    return true;
}

static bool buildInstructionStarts(Chunk *chunk, bool *isStart, int *nextStart)
{
    if (chunk->size <= 0)
        return true;

    bool widePending = false;
    int offset = 0;
    while (offset < chunk->size)
    {
        isStart[offset] = true;

        bool consumedWide = false;
        bool setsWide = false;
        int len = instructionLength(chunk, offset, widePending, &consumedWide, &setsWide);
        if (len <= 0)
            return false;

        int next = offset + len;
        if (next > chunk->size)
            next = chunk->size;
        nextStart[offset] = next;

        if (consumedWide)
            widePending = false;
        if (setsWide)
            widePending = true;

        offset = next;
    }

    return true;
}

static bool eliminateUnreachable(Chunk *chunk)
{
    if (chunk->size <= 0 || chunk->code == NULL)
        return false;

    int size = chunk->size;
    bool *isStart = (bool *)calloc((size_t)size, sizeof(bool));
    bool *reachable = (bool *)calloc((size_t)size, sizeof(bool));
    int *nextStart = (int *)malloc((size_t)size * sizeof(int));
    int *queue = (int *)malloc((size_t)size * sizeof(int));
    if (isStart == NULL || reachable == NULL || nextStart == NULL || queue == NULL)
    {
        free(isStart);
        free(reachable);
        free(nextStart);
        free(queue);
        return false;
    }

    for (int i = 0; i < size; i++)
        nextStart[i] = -1;

    bool ok = buildInstructionStarts(chunk, isStart, nextStart);
    if (!ok || !isStart[0])
    {
        free(isStart);
        free(reachable);
        free(nextStart);
        free(queue);
        return false;
    }

    int qHead = 0;
    int qTail = 0;
    queue[qTail++] = 0;
    reachable[0] = true;

    while (qHead < qTail)
    {
        int off = queue[qHead++];
        int next = nextStart[off];
        if (next < 0)
            continue;

        uint8_t inst = chunk->code[off];
        bool terminates = (inst == OP_RETURN || inst == OP_RETURN_NIL || inst == OP_RETURN_THIS);

        if (inst == OP_JUMP || inst == OP_LOOP)
        {
            int target = decodeJumpTarget(chunk, off, inst);
            if (target >= 0 && target < size && isStart[target] && !reachable[target])
            {
                reachable[target] = true;
                queue[qTail++] = target;
            }
            continue;
        }

        if (inst == OP_JUMP_IF_FALSE)
        {
            int target = decodeJumpTarget(chunk, off, inst);
            if (target >= 0 && target < size && isStart[target] && !reachable[target])
            {
                reachable[target] = true;
                queue[qTail++] = target;
            }
        }

        if (!terminates && next < size && isStart[next] && !reachable[next])
        {
            reachable[next] = true;
            queue[qTail++] = next;
        }
    }

    bool changed = false;
    for (int off = 0; off < size; off++)
    {
        if (!isStart[off] || reachable[off])
            continue;

        int next = nextStart[off];
        if (next <= off)
            continue;

        fillNop(chunk->code, off, next);
        changed = true;
    }

    free(isStart);
    free(reachable);
    free(nextStart);
    free(queue);
    return changed;
}

static int resolveThreadedJumpTarget(Chunk *chunk, int initialTarget, const bool *isStart)
{
    if (initialTarget < 0 || initialTarget >= chunk->size || !isStart[initialTarget])
        return initialTarget;

    int cur = initialTarget;
    int hops = 0;
    int maxHops = chunk->size;

    while (hops++ < maxHops)
    {
        uint8_t inst = chunk->code[cur];
        if (inst != OP_JUMP)
            break;

        int next = decodeJumpTarget(chunk, cur, inst);
        if (next < 0 || next >= chunk->size || !isStart[next])
            break;
        if (next == cur)
            break;

        cur = next;
    }

    return cur;
}

static bool runJumpThreading(Chunk *chunk)
{
    if (chunk->size <= 0 || chunk->code == NULL)
        return false;

    int size = chunk->size;
    bool *isStart = (bool *)calloc((size_t)size, sizeof(bool));
    int *nextStart = (int *)malloc((size_t)size * sizeof(int));
    if (isStart == NULL || nextStart == NULL)
    {
        free(isStart);
        free(nextStart);
        return false;
    }

    for (int i = 0; i < size; i++)
        nextStart[i] = -1;

    if (!buildInstructionStarts(chunk, isStart, nextStart))
    {
        free(isStart);
        free(nextStart);
        return false;
    }

    bool changed = false;
    for (int off = 0; off < size; off++)
    {
        if (!isStart[off])
            continue;

        uint8_t inst = chunk->code[off];
        if (inst != OP_JUMP && inst != OP_JUMP_IF_FALSE)
            continue;

        int target = decodeJumpTarget(chunk, off, inst);
        if (target < 0 || target >= size || !isStart[target])
            continue;

        int threaded = resolveThreadedJumpTarget(chunk, target, isStart);
        if (threaded == target)
            continue;

        if (patchJumpTarget(chunk, off, inst, threaded))
            changed = true;
    }

    free(isStart);
    free(nextStart);
    return changed;
}

static bool runPeepholePass(ObjFunction *function)
{
    Chunk *chunk = &function->chunk;
    if (chunk->size <= 0 || chunk->code == NULL)
        return false;

    bool changed = false;
    bool widePending = false;

    for (int offset = 0; offset < chunk->size;)
    {
        if (!widePending)
        {
            if (foldNumericBinary(chunk, offset))
                changed = true;
            if (foldUnary(chunk, offset))
                changed = true;
            if (eliminateLiteralPop(chunk, offset))
                changed = true;
            if (simplifyConstantBranches(chunk, offset))
                changed = true;
        }

        bool consumedWide = false;
        bool setsWide = false;
        int len = instructionLength(chunk, offset, widePending, &consumedWide, &setsWide);
        if (len <= 0)
            len = 1;

        if (consumedWide)
            widePending = false;
        if (setsWide)
            widePending = true;

        offset += len;
    }

    return changed;
}

static int mapOldOffsetToNew(int oldOffset, int oldSize, int newSize, const int *oldToNew, const int *nextKept)
{
    if (oldOffset <= 0)
        return 0;
    if (oldOffset >= oldSize)
        return newSize;

    if (oldToNew[oldOffset] >= 0)
        return oldToNew[oldOffset];
    if (nextKept[oldOffset] >= 0)
        return nextKept[oldOffset];
    return newSize;
}

static bool compactNops(Chunk *chunk)
{
    if (chunk->size <= 0 || chunk->code == NULL)
        return false;

    int oldSize = chunk->size;
    uint8_t *oldCode = chunk->code;

    bool *keepByte = (bool *)malloc((size_t)oldSize * sizeof(bool));
    bool *isStart = (bool *)calloc((size_t)oldSize, sizeof(bool));
    int *nextStart = (int *)malloc((size_t)oldSize * sizeof(int));
    int *oldToNew = (int *)malloc((size_t)oldSize * sizeof(int));
    int *nextKept = (int *)malloc((size_t)(oldSize + 1) * sizeof(int));
    if (keepByte == NULL || isStart == NULL || nextStart == NULL || oldToNew == NULL || nextKept == NULL)
    {
        free(keepByte);
        free(isStart);
        free(nextStart);
        free(oldToNew);
        free(nextKept);
        return false;
    }

    for (int i = 0; i < oldSize; i++)
    {
        keepByte[i] = true;
        nextStart[i] = -1;
        oldToNew[i] = -1;
    }

    if (!buildInstructionStarts(chunk, isStart, nextStart))
    {
        free(keepByte);
        free(isStart);
        free(nextStart);
        free(oldToNew);
        free(nextKept);
        return false;
    }

    bool widePending = false;
    for (int off = 0; off < oldSize;)
    {
        if (isStart[off] && oldCode[off] == OP_NOP)
        {
            if (!widePending)
                keepByte[off] = false;
        }

        bool consumedWide = false;
        bool setsWide = false;
        int len = instructionLength(chunk, off, widePending, &consumedWide, &setsWide);
        if (len <= 0)
            len = 1;

        if (consumedWide)
            widePending = false;
        if (setsWide)
            widePending = true;

        off += len;
    }

    int newSize = 0;
    for (int i = 0; i < oldSize; i++)
    {
        if (keepByte[i])
            oldToNew[i] = newSize++;
    }

    if (newSize == oldSize)
    {
        free(keepByte);
        free(isStart);
        free(nextStart);
        free(oldToNew);
        free(nextKept);
        return false;
    }

    nextKept[oldSize] = -1;
    for (int i = oldSize - 1; i >= 0; i--)
    {
        if (oldToNew[i] >= 0)
            nextKept[i] = oldToNew[i];
        else
            nextKept[i] = nextKept[i + 1];
    }

    uint8_t *newCode = (uint8_t *)malloc((size_t)newSize * sizeof(uint8_t));
    if (newCode == NULL)
    {
        free(keepByte);
        free(isStart);
        free(nextStart);
        free(oldToNew);
        free(nextKept);
        return false;
    }

    for (int i = 0; i < oldSize; i++)
    {
        if (oldToNew[i] >= 0)
            newCode[oldToNew[i]] = oldCode[i];
    }

    for (int off = 0; off < oldSize; off++)
    {
        if (!isStart[off] || oldToNew[off] < 0)
            continue;

        uint8_t inst = oldCode[off];
        if (inst != OP_JUMP && inst != OP_JUMP_IF_FALSE && inst != OP_LOOP)
            continue;

        int oldTarget = decodeJumpTarget(chunk, off, inst);
        int newOff = oldToNew[off];
        int newTarget = mapOldOffsetToNew(oldTarget, oldSize, newSize, oldToNew, nextKept);

        int delta = 0;
        if (inst == OP_LOOP)
            delta = (newOff + 3) - newTarget;
        else
            delta = newTarget - (newOff + 3);

        if (delta < 0)
            delta = 0;
        if (delta > UINT16_MAX)
            delta = UINT16_MAX;

        newCode[newOff + 1] = (uint8_t)((delta >> 8) & 0xff);
        newCode[newOff + 2] = (uint8_t)(delta & 0xff);
    }

    if (chunk->hasDebugPositions && chunk->posCount > 0)
    {
        PositionEntry *newPos = (PositionEntry *)malloc((size_t)chunk->posCount * sizeof(PositionEntry));
        if (newPos != NULL)
        {
            int newPosCount = 0;
            for (int i = 0; i < chunk->posCount; i++)
            {
                int oldOff = chunk->positions[i].offset;
                int mapped = mapOldOffsetToNew(oldOff, oldSize, newSize, oldToNew, nextKept);
                if (mapped < 0 || mapped >= newSize)
                    continue;

                if (newPosCount > 0 && newPos[newPosCount - 1].offset == mapped)
                {
                    newPos[newPosCount - 1].sourceIndex = chunk->positions[i].sourceIndex;
                    newPos[newPosCount - 1].col = chunk->positions[i].col;
                    continue;
                }

                newPos[newPosCount] = chunk->positions[i];
                newPos[newPosCount].offset = mapped;
                newPosCount++;
            }

            FREE_ARRAY(PositionEntry, chunk->positions, chunk->posCapacity);
            chunk->positions = newPos;
            chunk->posCount = newPosCount;
            chunk->posCapacity = chunk->posCount;
        }
    }

    FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    chunk->code = newCode;
    chunk->size = newSize;
    chunk->capacity = newSize;

    free(keepByte);
    free(isStart);
    free(nextStart);
    free(oldToNew);
    free(nextKept);
    return true;
}

bool optimizeFunction(ObjFunction *function, int level)
{
    if (function == NULL || level <= 0)
        return false;

    bool changed = false;
    int maxPasses = level >= 2 ? 4 : 1;

    for (int i = 0; i < maxPasses; i++)
    {
        bool passChanged = runPeepholePass(function);
        if (level >= 2)
            passChanged = runJumpThreading(&function->chunk) || passChanged;
        if (level >= 2)
            passChanged = eliminateUnreachable(&function->chunk) || passChanged;
        changed = changed || passChanged;
        if (!passChanged)
            break;
    }

    if (level >= 2)
        changed = compactNops(&function->chunk) || changed;

    return changed;
}

bool optimizerValidateFunction(ObjFunction *function, char *errBuf, size_t errCap)
{
    if (errBuf != NULL && errCap > 0)
        errBuf[0] = '\0';

    if (function == NULL)
    {
        if (errBuf != NULL && errCap > 0)
            snprintf(errBuf, errCap, "null function");
        return false;
    }

    Chunk *chunk = &function->chunk;
    if (chunk->size == 0)
        return true;
    if (chunk->code == NULL)
    {
        if (errBuf != NULL && errCap > 0)
            snprintf(errBuf, errCap, "chunk has size but null code buffer");
        return false;
    }

    int size = chunk->size;
    bool *isStart = (bool *)calloc((size_t)size, sizeof(bool));
    int *nextStart = (int *)malloc((size_t)size * sizeof(int));
    if (isStart == NULL || nextStart == NULL)
    {
        free(isStart);
        free(nextStart);
        if (errBuf != NULL && errCap > 0)
            snprintf(errBuf, errCap, "allocation failure during validation");
        return false;
    }

    for (int i = 0; i < size; i++)
        nextStart[i] = -1;

    if (!buildInstructionStarts(chunk, isStart, nextStart) || !isStart[0])
    {
        free(isStart);
        free(nextStart);
        if (errBuf != NULL && errCap > 0)
            snprintf(errBuf, errCap, "unable to decode instruction boundaries");
        return false;
    }

    bool widePending = false;
    for (int off = 0; off < size; off++)
    {
        if (!isStart[off])
            continue;

        uint8_t inst = chunk->code[off];

        bool consumedWide = false;
        bool setsWide = false;
        int len = instructionLength(chunk, off, widePending, &consumedWide, &setsWide);
        if (len <= 0 || off + len > size)
        {
            free(isStart);
            free(nextStart);
            if (errBuf != NULL && errCap > 0)
                snprintf(errBuf, errCap, "invalid instruction length at offset %d (op=%u)", off, (unsigned)inst);
            return false;
        }

        if (widePending && !consumedWide && inst != OP_WIDE && inst != OP_NOP)
        {
            free(isStart);
            free(nextStart);
            if (errBuf != NULL && errCap > 0)
                snprintf(errBuf, errCap, "dangling OP_WIDE before unsupported opcode at offset %d", off);
            return false;
        }

        if (inst == OP_WIDE)
        {
            int next = nextStart[off];
            if (next < 0 || next >= size)
            {
                free(isStart);
                free(nextStart);
                if (errBuf != NULL && errCap > 0)
                    snprintf(errBuf, errCap, "OP_WIDE at offset %d has no following instruction", off);
                return false;
            }
            uint8_t nextInst = chunk->code[next];
            if (!(nextInst == OP_NOP || instructionSupportsWide(nextInst)))
            {
                free(isStart);
                free(nextStart);
                if (errBuf != NULL && errCap > 0)
                    snprintf(errBuf, errCap, "OP_WIDE at offset %d targets non-wide opcode %u", off, (unsigned)nextInst);
                return false;
            }
        }

        if (inst == OP_JUMP || inst == OP_JUMP_IF_FALSE || inst == OP_LOOP)
        {
            int target = decodeJumpTarget(chunk, off, inst);
            if (target < 0 || target > size || (target < size && !isStart[target]))
            {
                free(isStart);
                free(nextStart);
                if (errBuf != NULL && errCap > 0)
                    snprintf(errBuf, errCap, "invalid jump target at offset %d -> %d", off, target);
                return false;
            }
        }

        if (inst == OP_CONSTANT)
        {
            int idx = consumedWide ? (((int)chunk->code[off + 1] << 8) | (int)chunk->code[off + 2]) : (int)chunk->code[off + 1];
            if (idx < 0 || idx >= chunk->constants.size)
            {
                free(isStart);
                free(nextStart);
                if (errBuf != NULL && errCap > 0)
                    snprintf(errBuf, errCap, "constant index out of range at offset %d", off);
                return false;
            }
        }

        if (inst == OP_CONSTANT_LONG)
        {
            int idx = (int)chunk->code[off + 1] | ((int)chunk->code[off + 2] << 8) | ((int)chunk->code[off + 3] << 16);
            if (idx < 0 || idx >= chunk->constants.size)
            {
                free(isStart);
                free(nextStart);
                if (errBuf != NULL && errCap > 0)
                    snprintf(errBuf, errCap, "long constant index out of range at offset %d", off);
                return false;
            }
        }

        if (inst == OP_CLOSURE)
        {
            int idx = consumedWide ? (((int)chunk->code[off + 1] << 8) | (int)chunk->code[off + 2]) : (int)chunk->code[off + 1];
            if (idx < 0 || idx >= chunk->constants.size || !IS_FUN(chunk->constants.values[idx]))
            {
                free(isStart);
                free(nextStart);
                if (errBuf != NULL && errCap > 0)
                    snprintf(errBuf, errCap, "invalid closure constant at offset %d", off);
                return false;
            }
        }

        if (consumedWide)
            widePending = false;
        if (setsWide)
            widePending = true;
    }

    if (widePending)
    {
        free(isStart);
        free(nextStart);
        if (errBuf != NULL && errCap > 0)
            snprintf(errBuf, errCap, "dangling OP_WIDE at end of chunk");
        return false;
    }

    free(isStart);
    free(nextStart);
    return true;
}
