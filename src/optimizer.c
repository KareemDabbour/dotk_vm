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

static bool isFalseyConst(Value v)
{
    if (IS_NIL(v))
        return true;
    if (IS_BOOL(v))
        return !AS_BOOL(v);
    if (IS_NUM(v))
        return AS_NUM(v) == 0;
    if (IS_STR(v))
        return AS_STR(v)->len == 0;
    if (IS_LIST(v))
        return AS_LIST(v)->count == 0;
    if (IS_MAP(v))
        return AS_MAP(v)->map.count == 0;
    return false;
}

static bool decodeConstantAt(Chunk *chunk, int offset, bool widePending,
                             int *instLen, int *constIdx, Value *constVal)
{
    if (offset < 0 || offset >= chunk->size)
        return false;

    uint8_t inst = chunk->code[offset];
    int idx = -1;
    int len = 0;

    if (inst == OP_CONSTANT)
    {
        if (widePending)
        {
            if (offset + 2 >= chunk->size)
                return false;
            idx = ((int)chunk->code[offset + 1] << 8) | (int)chunk->code[offset + 2];
            len = 3;
        }
        else
        {
            if (offset + 1 >= chunk->size)
                return false;
            idx = (int)chunk->code[offset + 1];
            len = 2;
        }
    }
    else if (inst == OP_CONSTANT_LONG)
    {
        if (offset + 3 >= chunk->size)
            return false;
        idx = (int)chunk->code[offset + 1] | ((int)chunk->code[offset + 2] << 8) | ((int)chunk->code[offset + 3] << 16);
        len = 4;
    }
    else
    {
        return false;
    }

    if (idx < 0 || idx >= chunk->constants.size)
        return false;

    if (instLen != NULL)
        *instLen = len;
    if (constIdx != NULL)
        *constIdx = idx;
    if (constVal != NULL)
        *constVal = chunk->constants.values[idx];

    return true;
}

static bool writeConstantAt(Chunk *chunk, int offset, int idx, int spanLen)
{
    if (offset < 0 || offset >= chunk->size)
        return false;

    if (idx < 0)
        return false;

    if (spanLen < 2)
        return false;

    if (idx <= UINT8_MAX)
    {
        if (offset + 1 >= chunk->size)
            return false;
        chunk->code[offset] = OP_CONSTANT;
        chunk->code[offset + 1] = (uint8_t)idx;
        if (spanLen > 2)
            fillNop(chunk->code, offset + 2, offset + spanLen);
        return true;
    }

    if (idx <= 0xFFFFFF)
    {
        // Do not expand into the next instruction when replacing a short span.
        if (spanLen < 4)
            return false;
        if (offset + 3 >= chunk->size)
            return false;
        chunk->code[offset] = OP_CONSTANT_LONG;
        chunk->code[offset + 1] = (uint8_t)(idx & 0xff);
        chunk->code[offset + 2] = (uint8_t)((idx >> 8) & 0xff);
        chunk->code[offset + 3] = (uint8_t)((idx >> 16) & 0xff);
        if (spanLen > 4)
            fillNop(chunk->code, offset + 4, offset + spanLen);
        return true;
    }

    return false;
}

static bool instructionSupportsWide(uint8_t inst)
{
    switch (inst)
    {
    case OP_CALL:
    case OP_TAIL_CALL:
    case OP_DEF_GLOBAL:
    case OP_DEF_CONST_GLOBAL:
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_GLOBAL:
    case OP_SET_GLOBAL:
    case OP_GET_UPVALUE:
    case OP_SET_UPVALUE:
    case OP_BUILD_LIST:
    case OP_BUILD_TUPLE:
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
    case OP_TAIL_CALL:
    case OP_DEF_GLOBAL:
    case OP_DEF_CONST_GLOBAL:
    case OP_GET_LOCAL:
    case OP_SET_LOCAL:
    case OP_GET_GLOBAL:
    case OP_SET_GLOBAL:
    case OP_GET_UPVALUE:
    case OP_SET_UPVALUE:
    case OP_BUILD_LIST:
    case OP_BUILD_TUPLE:
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

    case OP_CALL_UNPACK:
    {
        if (offset + 3 >= chunk->size)
            return 1;
        uint8_t partCount = chunk->code[offset + 1];
        return 4 + partCount;
    }

    case OP_IMPORT_NAME:
        return 3;

    case OP_UNPACK:
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

    case OP_DEFAULT_LOCAL:
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

static bool hasIncomingEdgeToOffset(Chunk *chunk, int targetOffset, int ignoreSourceOffset);

static bool globalNameEqualsAt(const Chunk *chunk, int idxA, int idxB)
{
    if (chunk == NULL)
        return false;
    if (idxA < 0 || idxB < 0 || idxA >= chunk->constants.size || idxB >= chunk->constants.size)
        return false;

    Value a = chunk->constants.values[idxA];
    Value b = chunk->constants.values[idxB];
    if (!IS_STR(a) || !IS_STR(b))
        return idxA == idxB;

    ObjString *sa = AS_STR(a);
    ObjString *sb = AS_STR(b);
    if (sa->len != sb->len)
        return false;
    return memcmp(sa->chars, sb->chars, (size_t)sa->len) == 0;
}

typedef struct
{
    int attempted;
    int inlined;
    int rejected;
} InlineStats;

static InlineStats gInlineStats = {0, 0, 0};

typedef enum
{
    INLINE_RET_NONE = 0,
    INLINE_RET_CONST = 1,
    INLINE_RET_ARG0 = 2,
} InlineReturnKind;

static InlineReturnKind gInlineRetKind = INLINE_RET_NONE;

static bool inlineStatsEnabled(void)
{
    static int cached = -1;
    if (cached == -1)
    {
        const char *env = getenv("DOTK_INLINE_STATS");
        cached = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
    }
    return cached == 1;
}

static int skipUntargetedNops(Chunk *chunk, int offset)
{
    int cur = offset;
    while (cur < chunk->size && chunk->code[cur] == OP_NOP)
    {
        if (hasIncomingEdgeToOffset(chunk, cur, -1))
            break;
        cur++;
    }
    return cur;
}

static bool foldNumericBinary(Chunk *chunk, int offset, bool widePending)
{
    uint8_t *code = chunk->code;

    int leftLen = 0;
    Value va;
    if (!decodeConstantAt(chunk, offset, widePending, &leftLen, NULL, &va))
        return false;

    int rightOff = skipUntargetedNops(chunk, offset + leftLen);
    if (rightOff >= chunk->size)
        return false;

    int rightLen = 0;
    Value vb;
    if (!decodeConstantAt(chunk, rightOff, false, &rightLen, NULL, &vb))
        return false;

    int opOff = skipUntargetedNops(chunk, rightOff + rightLen);
    if (opOff >= chunk->size)
        return false;

    uint8_t op = code[opOff];
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
    case OP_BIN_AND:
    case OP_BIN_OR:
    case OP_BIN_XOR:
    case OP_BIN_SHIFT_LEFT:
    case OP_BIN_SHIFT_RIGHT:
        break;
    default:
        return false;
    }

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
    case OP_BIN_AND:
        out = NUM_VAL(((long)a) & ((long)b));
        break;
    case OP_BIN_OR:
        out = NUM_VAL(((long)a) | ((long)b));
        break;
    case OP_BIN_XOR:
        out = NUM_VAL(((long)a) ^ ((long)b));
        break;
    case OP_BIN_SHIFT_LEFT:
    {
        long shift = (long)b;
        if (shift < 0 || shift >= (long)(sizeof(long) * 8))
            return false;
        out = NUM_VAL(((long)a) << shift);
        break;
    }
    case OP_BIN_SHIFT_RIGHT:
    {
        long shift = (long)b;
        if (shift < 0 || shift >= (long)(sizeof(long) * 8))
            return false;
        out = NUM_VAL(((long)a) >> shift);
        break;
    }
    default:
        return false;
    }

    int outIdx = addConst(chunk, out);
    if (outIdx < 0)
        return false;

    int spanLen = (opOff - offset) + 1;
    return writeConstantAt(chunk, offset, outIdx, spanLen);
}

static bool foldUnary(Chunk *chunk, int offset, bool widePending)
{
    uint8_t *code = chunk->code;
    if (offset >= chunk->size)
        return false;

    if (offset + 1 >= chunk->size)
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

    int constLen = 0;
    int idx = -1;
    Value v;
    if (!decodeConstantAt(chunk, offset, widePending, &constLen, &idx, &v))
        return false;

    int opOff = offset + constLen;
    if (opOff >= chunk->size)
        return false;

    if (code[opOff] == OP_NEGATE)
    {
        if (!isFiniteNumber(v))
            return false;
        int outIdx = addConst(chunk, NUM_VAL(-AS_NUM(v)));
        if (outIdx < 0)
            return false;

        return writeConstantAt(chunk, offset, outIdx, constLen + 1);
    }

    if (code[opOff] == OP_NOT)
    {
        bool falsey = isFalseyConst(v);
        code[offset] = falsey ? OP_TRUE : OP_FALSE;
        if (constLen + 1 > 1)
            fillNop(code, offset + 1, offset + constLen + 1);
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
    if (offset >= chunk->size)
        return false;

    int jumpOff = -1;
    bool falsey = false;
    bool matched = false;

    uint8_t lit = code[offset];
    if (lit == OP_TRUE || lit == OP_FALSE || lit == OP_NIL)
    {
        if (offset + 3 >= chunk->size || code[offset + 1] != OP_JUMP_IF_FALSE)
            return false;
        falsey = (lit != OP_TRUE);
        jumpOff = offset + 1;
        matched = true;
    }
    else
    {
        int constLen = 0;
        Value v;
        if (!decodeConstantAt(chunk, offset, false, &constLen, NULL, &v))
            return false;
        jumpOff = offset + constLen;
        if (jumpOff + 2 >= chunk->size || code[jumpOff] != OP_JUMP_IF_FALSE)
            return false;
        falsey = isFalseyConst(v);
        matched = true;
    }

    if (!matched)
        return false;

    // Skip if this jump point has alternate predecessors.
    // This prevents unsafe rewrites on short-circuit expression shapes.
    if (hasIncomingEdgeToOffset(chunk, jumpOff, -1))
        return false;

    if (!falsey)
    {
        code[jumpOff] = OP_NOP;
        code[jumpOff + 1] = OP_NOP;
        code[jumpOff + 2] = OP_NOP;
    }
    else
    {
        code[jumpOff] = OP_JUMP;
    }

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

static bool hasIncomingEdgeToOffset(Chunk *chunk, int targetOffset, int ignoreSourceOffset)
{
    if (chunk == NULL || chunk->code == NULL || targetOffset < 0 || targetOffset >= chunk->size)
        return false;

    bool widePending = false;
    for (int off = 0; off < chunk->size;)
    {
        uint8_t inst = chunk->code[off];

        if (off != ignoreSourceOffset)
        {
            if (inst == OP_JUMP || inst == OP_JUMP_IF_FALSE || inst == OP_LOOP)
            {
                int t = decodeJumpTarget(chunk, off, inst);
                if (t == targetOffset)
                    return true;
            }
            else if (inst == OP_DEFAULT_LOCAL)
            {
                if (off + 3 < chunk->size)
                {
                    int jump = ((int)chunk->code[off + 2] << 8) | (int)chunk->code[off + 3];
                    int t = off + 4 + jump;
                    if (t == targetOffset)
                        return true;
                }
            }
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

    return false;
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

static bool isReturnInst(uint8_t inst)
{
    return inst == OP_RETURN || inst == OP_RETURN_NIL || inst == OP_RETURN_THIS;
}

static bool analyzeInlineReturn(ObjFunction *callee, Value *out)
{
    if (callee == NULL || out == NULL)
        return false;

    Chunk *chunk = &callee->chunk;
    if (chunk->size <= 0 || chunk->code == NULL)
        return false;

    // Fast path: identity function for one-arg calls.
    // Expected minimal shape: OP_GET_LOCAL 1; OP_RETURN
    if (chunk->size >= 3 && chunk->code[0] == OP_GET_LOCAL && chunk->code[1] == 1 && chunk->code[2] == OP_RETURN)
    {
        gInlineRetKind = INLINE_RET_ARG0;
        return true;
    }

    bool sawValue = false;
    bool sawReturn = false;
    Value returnValue = NIL_VAL;

    bool widePending = false;
    for (int off = 0; off < chunk->size;)
    {
        uint8_t inst = chunk->code[off];

        if (inst == OP_NOP)
        {
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
            continue;
        }

        if (inst == OP_WIDE)
        {
            widePending = true;
            off += 1;
            continue;
        }

        if (!sawValue)
        {
            int constLen = 0;
            Value v;
            if (decodeConstantAt(chunk, off, widePending, &constLen, NULL, &v))
            {
                returnValue = v;
                sawValue = true;
                widePending = false;
                off += constLen;
                continue;
            }

            if (inst == OP_TRUE)
            {
                returnValue = BOOL_VAL(true);
                sawValue = true;
                off += 1;
                continue;
            }
            if (inst == OP_FALSE)
            {
                returnValue = BOOL_VAL(false);
                sawValue = true;
                off += 1;
                continue;
            }
            if (inst == OP_NIL)
            {
                returnValue = NIL_VAL;
                sawValue = true;
                off += 1;
                continue;
            }
        }

        if (isReturnInst(inst))
        {
            if (inst == OP_RETURN_NIL)
                returnValue = NIL_VAL;
            sawReturn = true;
            off += 1;
            continue;
        }

        // v1 inlining only supports constant-producing bodies.
        return false;
    }

    if (!sawReturn)
        return false;

    *out = returnValue;
    gInlineRetKind = INLINE_RET_CONST;
    return true;
}

static bool isSideEffectFreeDroppableArgProducer(Chunk *chunk, int startOff, int endOff)
{
    if (chunk == NULL || startOff < 0 || endOff <= startOff || endOff > chunk->size)
        return false;

    uint8_t inst = chunk->code[startOff];

    if (inst == OP_WIDE)
    {
        if (startOff + 1 >= chunk->size)
            return false;

        uint8_t wideInst = chunk->code[startOff + 1];
        if (wideInst == OP_GET_LOCAL || wideInst == OP_GET_UPVALUE || wideInst == OP_CONSTANT)
            return true;

        return false;
    }

    if (inst == OP_TRUE || inst == OP_FALSE || inst == OP_NIL)
        return (startOff + 1) == endOff;

    if (inst == OP_CONSTANT)
    {
        if (startOff + 1 >= chunk->size)
            return false;
        return (startOff + 2) == endOff;
    }

    if (inst == OP_CONSTANT_LONG)
        return true;

    if (inst == OP_GET_LOCAL || inst == OP_GET_UPVALUE)
        return true;

    if (inst == OP_CONSTANT)
        return true;

    return false;
}

static bool runInliningPass(ObjFunction *function)
{
    Chunk *chunk = &function->chunk;
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

    if (!buildInstructionStarts(chunk, isStart, nextStart) || !isStart[0])
    {
        free(isStart);
        free(nextStart);
        return false;
    }

    int prevStart = -1;
    for (int off = 0; off < size;)
    {
        if (!isStart[off])
        {
            off++;
            continue;
        }

        uint8_t inst = chunk->code[off];
        if (inst == OP_CALL && off + 1 < size)
        {
            uint8_t argc = chunk->code[off + 1];
            int calleeStart = -1;
            int argStart = -1;

            int prevOpcodeStart = prevStart;
            int prevEffectiveStart = prevOpcodeStart;
            if (prevOpcodeStart > 0 && chunk->code[prevOpcodeStart - 1] == OP_WIDE &&
                isStart[prevOpcodeStart - 1] && nextStart[prevOpcodeStart - 1] == prevOpcodeStart)
            {
                prevEffectiveStart = prevOpcodeStart - 1;
            }

            if (argc == 0 && prevOpcodeStart >= 0 && nextStart[prevOpcodeStart] == off)
            {
                calleeStart = prevEffectiveStart;
            }
            else if (argc == 1 && prevOpcodeStart >= 0 && nextStart[prevOpcodeStart] == off)
            {
                int argEffectiveStart = prevEffectiveStart;

                int calleeOpcodeStart = -1;
                for (int p = argEffectiveStart - 1; p >= 0; p--)
                {
                    if (isStart[p])
                    {
                        calleeOpcodeStart = p;
                        break;
                    }
                }

                if (calleeOpcodeStart >= 0 && nextStart[calleeOpcodeStart] == argEffectiveStart)
                {
                    int calleeEffectiveStart = calleeOpcodeStart;
                    if (calleeOpcodeStart > 0 && chunk->code[calleeOpcodeStart - 1] == OP_WIDE &&
                        isStart[calleeOpcodeStart - 1] && nextStart[calleeOpcodeStart - 1] == calleeOpcodeStart)
                    {
                        calleeEffectiveStart = calleeOpcodeStart - 1;
                    }

                    calleeStart = calleeEffectiveStart;
                    argStart = argEffectiveStart;
                }
            }

            if (calleeStart >= 0)
            {
                gInlineStats.attempted++;

                bool unsafeEdge = false;
                for (int pos = calleeStart + 1; pos < off + 2; pos++)
                {
                    if (hasIncomingEdgeToOffset(chunk, pos, -1))
                    {
                        unsafeEdge = true;
                        break;
                    }
                }
                if (unsafeEdge)
                {
                    gInlineStats.rejected++;
                    prevStart = off;
                    off = nextStart[off];
                    continue;
                }

                ObjFunction *callee = NULL;
                uint8_t prevInst = chunk->code[calleeStart];

                if (prevInst == OP_CLOSURE)
                {
                    if (calleeStart + 1 < size)
                    {
                        int fnConst = -1;
                        if (calleeStart > 0 && chunk->code[calleeStart - 1] == OP_WIDE)
                        {
                            if (calleeStart + 2 >= size)
                                fnConst = -1;
                            else
                                fnConst = ((int)chunk->code[calleeStart + 1] << 8) |
                                          (int)chunk->code[calleeStart + 2];
                        }
                        else
                        {
                            fnConst = (int)chunk->code[calleeStart + 1];
                        }
                        if (fnConst >= 0 && fnConst < chunk->constants.size)
                        {
                            Value fnVal = chunk->constants.values[fnConst];
                            if (IS_FUN(fnVal))
                                callee = AS_FUN(fnVal);
                        }
                    }
                }
                else if (prevInst == OP_CONSTANT)
                {
                    if (calleeStart + 1 < size)
                    {
                        int fnConst = -1;
                        if (calleeStart > 0 && chunk->code[calleeStart - 1] == OP_WIDE)
                        {
                            if (calleeStart + 2 >= size)
                                fnConst = -1;
                            else
                                fnConst = ((int)chunk->code[calleeStart + 1] << 8) |
                                          (int)chunk->code[calleeStart + 2];
                        }
                        else
                        {
                            fnConst = (int)chunk->code[calleeStart + 1];
                        }
                        if (fnConst >= 0 && fnConst < chunk->constants.size)
                        {
                            Value fnVal = chunk->constants.values[fnConst];
                            if (IS_FUN(fnVal))
                                callee = AS_FUN(fnVal);
                        }
                    }
                }
                else if (prevInst == OP_CONSTANT_LONG)
                {
                    if (calleeStart + 3 < size)
                    {
                        int fnConst = (int)chunk->code[calleeStart + 1] |
                                      ((int)chunk->code[calleeStart + 2] << 8) |
                                      ((int)chunk->code[calleeStart + 3] << 16);
                        if (fnConst >= 0 && fnConst < chunk->constants.size)
                        {
                            Value fnVal = chunk->constants.values[fnConst];
                            if (IS_FUN(fnVal))
                                callee = AS_FUN(fnVal);
                        }
                    }
                }
                else if (prevInst == OP_GET_GLOBAL)
                {
                    if (calleeStart + 1 < size)
                    {
                        int globalIdx = -1;
                        if (calleeStart > 0 && chunk->code[calleeStart - 1] == OP_WIDE)
                        {
                            if (calleeStart + 2 >= size)
                                globalIdx = -1;
                            else
                                globalIdx = ((int)chunk->code[calleeStart + 1] << 8) |
                                            (int)chunk->code[calleeStart + 2];
                        }
                        else
                        {
                            globalIdx = (int)chunk->code[calleeStart + 1];
                        }

                        // Resolve latest global definition of this slot before call site.
                        int lastDefOff = -1;
                        ObjFunction *resolved = NULL;
                        bool invalidated = false;

                        bool scanWide = false;
                        for (int scan = 0; scan < off;)
                        {
                            bool consumedWide = false;
                            bool setsWide = false;
                            int scanLen = instructionLength(chunk, scan, scanWide, &consumedWide, &setsWide);
                            if (scanLen <= 0)
                                scanLen = 1;

                            uint8_t scanInst = chunk->code[scan];

                            if (!scanWide && (scanInst == OP_DEF_GLOBAL || scanInst == OP_DEF_CONST_GLOBAL || scanInst == OP_SET_GLOBAL))
                            {
                                if (scan + 1 < size)
                                {
                                    int scanIdx = (int)chunk->code[scan + 1];
                                    if (globalNameEqualsAt(chunk, scanIdx, globalIdx))
                                    {
                                        if (scanInst == OP_SET_GLOBAL)
                                        {
                                            invalidated = true;
                                            resolved = NULL;
                                            lastDefOff = -1;
                                        }
                                        else
                                        {
                                            invalidated = false;
                                            lastDefOff = scan;
                                            resolved = NULL;

                                            int producer = -1;
                                            // Find immediate producer instruction start before this def.
                                            bool backWide = false;
                                            for (int b = 0; b < scan;)
                                            {
                                                bool bConsumed = false;
                                                bool bSets = false;
                                                int bLen = instructionLength(chunk, b, backWide, &bConsumed, &bSets);
                                                if (bLen <= 0)
                                                    bLen = 1;
                                                int next = b + bLen;
                                                if (next == scan)
                                                {
                                                    producer = b;
                                                    break;
                                                }
                                                if (bConsumed)
                                                    backWide = false;
                                                if (bSets)
                                                    backWide = true;
                                                b = next;
                                            }

                                            if (producer >= 0)
                                            {
                                                uint8_t pInst = chunk->code[producer];
                                                if (pInst == OP_CLOSURE && producer + 1 < size)
                                                {
                                                    int fnConst = (int)chunk->code[producer + 1];
                                                    if (fnConst >= 0 && fnConst < chunk->constants.size)
                                                    {
                                                        Value fnVal = chunk->constants.values[fnConst];
                                                        if (IS_FUN(fnVal))
                                                            resolved = AS_FUN(fnVal);
                                                    }
                                                }
                                                else if (pInst == OP_CONSTANT && producer + 1 < size)
                                                {
                                                    int fnConst = (int)chunk->code[producer + 1];
                                                    if (fnConst >= 0 && fnConst < chunk->constants.size)
                                                    {
                                                        Value fnVal = chunk->constants.values[fnConst];
                                                        if (IS_FUN(fnVal))
                                                            resolved = AS_FUN(fnVal);
                                                    }
                                                }
                                                else if (pInst == OP_CONSTANT_LONG && producer + 3 < size)
                                                {
                                                    int fnConst = (int)chunk->code[producer + 1] |
                                                                  ((int)chunk->code[producer + 2] << 8) |
                                                                  ((int)chunk->code[producer + 3] << 16);
                                                    if (fnConst >= 0 && fnConst < chunk->constants.size)
                                                    {
                                                        Value fnVal = chunk->constants.values[fnConst];
                                                        if (IS_FUN(fnVal))
                                                            resolved = AS_FUN(fnVal);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (consumedWide)
                                scanWide = false;
                            if (setsWide)
                                scanWide = true;
                            scan += scanLen;
                        }

                        if (!invalidated && lastDefOff >= 0 && resolved != NULL)
                            callee = resolved;
                    }
                }
                else if (prevInst == OP_WIDE)
                {
                    if (calleeStart + 3 < size)
                    {
                        uint8_t wideInst = chunk->code[calleeStart + 1];
                        int wideIdx = ((int)chunk->code[calleeStart + 2] << 8) |
                                      (int)chunk->code[calleeStart + 3];

                        if (wideInst == OP_CLOSURE || wideInst == OP_CONSTANT)
                        {
                            if (wideIdx >= 0 && wideIdx < chunk->constants.size)
                            {
                                Value fnVal = chunk->constants.values[wideIdx];
                                if (IS_FUN(fnVal))
                                    callee = AS_FUN(fnVal);
                            }
                        }
                        else if (wideInst == OP_GET_GLOBAL)
                        {
                            int globalIdx = wideIdx;

                            int lastDefOff = -1;
                            ObjFunction *resolved = NULL;
                            bool invalidated = false;

                            bool scanWide = false;
                            for (int scan = 0; scan < off;)
                            {
                                bool consumedWide = false;
                                bool setsWide = false;
                                int scanLen = instructionLength(chunk, scan, scanWide, &consumedWide, &setsWide);
                                if (scanLen <= 0)
                                    scanLen = 1;

                                uint8_t scanInst = chunk->code[scan];

                                if (!scanWide && (scanInst == OP_DEF_GLOBAL || scanInst == OP_DEF_CONST_GLOBAL || scanInst == OP_SET_GLOBAL))
                                {
                                    if (scan + 1 < size)
                                    {
                                        int scanIdx = (int)chunk->code[scan + 1];
                                        if (globalNameEqualsAt(chunk, scanIdx, globalIdx))
                                        {
                                            if (scanInst == OP_SET_GLOBAL)
                                            {
                                                invalidated = true;
                                                resolved = NULL;
                                                lastDefOff = -1;
                                            }
                                            else
                                            {
                                                invalidated = false;
                                                lastDefOff = scan;
                                                resolved = NULL;

                                                int producer = -1;
                                                bool backWide = false;
                                                for (int b = 0; b < scan;)
                                                {
                                                    bool bConsumed = false;
                                                    bool bSets = false;
                                                    int bLen = instructionLength(chunk, b, backWide, &bConsumed, &bSets);
                                                    if (bLen <= 0)
                                                        bLen = 1;
                                                    int next = b + bLen;
                                                    if (next == scan)
                                                    {
                                                        producer = b;
                                                        break;
                                                    }
                                                    if (bConsumed)
                                                        backWide = false;
                                                    if (bSets)
                                                        backWide = true;
                                                    b = next;
                                                }

                                                if (producer >= 0)
                                                {
                                                    uint8_t pInst = chunk->code[producer];
                                                    if (pInst == OP_CLOSURE && producer + 1 < size)
                                                    {
                                                        int fnConst = (int)chunk->code[producer + 1];
                                                        if (fnConst >= 0 && fnConst < chunk->constants.size)
                                                        {
                                                            Value fnVal = chunk->constants.values[fnConst];
                                                            if (IS_FUN(fnVal))
                                                                resolved = AS_FUN(fnVal);
                                                        }
                                                    }
                                                    else if (pInst == OP_CONSTANT && producer + 1 < size)
                                                    {
                                                        int fnConst = (int)chunk->code[producer + 1];
                                                        if (fnConst >= 0 && fnConst < chunk->constants.size)
                                                        {
                                                            Value fnVal = chunk->constants.values[fnConst];
                                                            if (IS_FUN(fnVal))
                                                                resolved = AS_FUN(fnVal);
                                                        }
                                                    }
                                                    else if (pInst == OP_CONSTANT_LONG && producer + 3 < size)
                                                    {
                                                        int fnConst = (int)chunk->code[producer + 1] |
                                                                      ((int)chunk->code[producer + 2] << 8) |
                                                                      ((int)chunk->code[producer + 3] << 16);
                                                        if (fnConst >= 0 && fnConst < chunk->constants.size)
                                                        {
                                                            Value fnVal = chunk->constants.values[fnConst];
                                                            if (IS_FUN(fnVal))
                                                                resolved = AS_FUN(fnVal);
                                                        }
                                                    }
                                                    else if (pInst == OP_WIDE && producer + 3 < size)
                                                    {
                                                        uint8_t pWideInst = chunk->code[producer + 1];
                                                        int fnConst = ((int)chunk->code[producer + 2] << 8) |
                                                                      (int)chunk->code[producer + 3];
                                                        if ((pWideInst == OP_CLOSURE || pWideInst == OP_CONSTANT) &&
                                                            fnConst >= 0 && fnConst < chunk->constants.size)
                                                        {
                                                            Value fnVal = chunk->constants.values[fnConst];
                                                            if (IS_FUN(fnVal))
                                                                resolved = AS_FUN(fnVal);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                if (consumedWide)
                                    scanWide = false;
                                if (setsWide)
                                    scanWide = true;
                                scan += scanLen;
                            }

                            if (!invalidated && lastDefOff >= 0 && resolved != NULL)
                                callee = resolved;
                        }
                    }
                }

                if (callee == NULL)
                {
                    gInlineStats.rejected++;
                    prevStart = off;
                    off = nextStart[off];
                    continue;
                }

                if (callee->arity != argc || callee->minArity != argc ||
                    callee->isVariadic || callee->isGenerator || callee->isAsync ||
                    callee->upValueCount != 0)
                {
                    gInlineStats.rejected++;
                    prevStart = off;
                    off = nextStart[off];
                    continue;
                }

                Value inlinedRet = NIL_VAL;
                gInlineRetKind = INLINE_RET_NONE;
                if (!analyzeInlineReturn(callee, &inlinedRet))
                {
                    gInlineStats.rejected++;
                    prevStart = off;
                    off = nextStart[off];
                    continue;
                }

                if (gInlineRetKind == INLINE_RET_CONST)
                {
                    if (argc == 1)
                    {
                        if (argStart < 0 || !isSideEffectFreeDroppableArgProducer(chunk, argStart, off))
                        {
                            gInlineStats.rejected++;
                            prevStart = off;
                            off = nextStart[off];
                            continue;
                        }
                    }
                    else if (argc != 0)
                    {
                        gInlineStats.rejected++;
                        prevStart = off;
                        off = nextStart[off];
                        continue;
                    }

                    int outIdx = addConst(chunk, inlinedRet);
                    if (outIdx < 0)
                    {
                        gInlineStats.rejected++;
                        prevStart = off;
                        off = nextStart[off];
                        continue;
                    }

                    int spanLen = (off + 2) - calleeStart;
                    if (!writeConstantAt(chunk, calleeStart, outIdx, spanLen))
                    {
                        gInlineStats.rejected++;
                        prevStart = off;
                        off = nextStart[off];
                        continue;
                    }
                }
                else if (gInlineRetKind == INLINE_RET_ARG0 && argc == 1 && argStart >= 0)
                {
                    fillNop(chunk->code, calleeStart, nextStart[calleeStart]);
                    fillNop(chunk->code, off, off + 2);
                }
                else
                {
                    gInlineStats.rejected++;
                    prevStart = off;
                    off = nextStart[off];
                    continue;
                }

                gInlineStats.inlined++;
                free(isStart);
                free(nextStart);
                return true;
            }
        }

        prevStart = off;
        off = nextStart[off];
    }

    free(isStart);
    free(nextStart);
    return false;
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

        if (inst == OP_DEFAULT_LOCAL)
        {
            int jump = ((int)chunk->code[off + 2] << 8) | (int)chunk->code[off + 3];
            int target = off + 4 + jump;
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

static bool runJumpCanonicalization(Chunk *chunk)
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

        if (target == nextStart[off])
        {
            fillNop(chunk->code, off, off + 3);
            changed = true;
            continue;
        }
    }

    free(isStart);
    free(nextStart);
    return changed;
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

        if (target == nextStart[off])
        {
            fillNop(chunk->code, off, off + 3);
            changed = true;
            continue;
        }

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
        if (foldNumericBinary(chunk, offset, widePending))
            changed = true;
        if (foldUnary(chunk, offset, widePending))
            changed = true;
        if (!widePending && eliminateLiteralPop(chunk, offset))
            changed = true;
        if (!widePending && simplifyConstantBranches(chunk, offset))
            changed = true;

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
        if (inst != OP_JUMP && inst != OP_JUMP_IF_FALSE && inst != OP_LOOP && inst != OP_DEFAULT_LOCAL)
            continue;

        if (inst == OP_DEFAULT_LOCAL)
        {
            int oldJump = ((int)oldCode[off + 2] << 8) | (int)oldCode[off + 3];
            int oldTarget = off + 4 + oldJump;
            int newOff = oldToNew[off];
            int newTarget = mapOldOffsetToNew(oldTarget, oldSize, newSize, oldToNew, nextKept);
            int delta = newTarget - (newOff + 4);
            if (delta < 0)
                delta = 0;
            if (delta > UINT16_MAX)
                delta = UINT16_MAX;
            newCode[newOff + 2] = (uint8_t)((delta >> 8) & 0xff);
            newCode[newOff + 3] = (uint8_t)(delta & 0xff);
            continue;
        }

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
    int maxPasses = level >= 2 ? 8 : 4;

    for (int i = 0; i < maxPasses; i++)
    {
        bool passChanged = runPeepholePass(function);
        if (level >= 2)
            passChanged = runInliningPass(function) || passChanged;
        if (level >= 2)
            passChanged = runJumpCanonicalization(&function->chunk) || passChanged;
        if (level >= 2)
            passChanged = runJumpThreading(&function->chunk) || passChanged;
        if (level >= 2)
            passChanged = eliminateUnreachable(&function->chunk) || passChanged;
        changed = changed || passChanged;
        if (!passChanged)
            break;
    }

    if (level >= 1)
        changed = compactNops(&function->chunk) || changed;

    if (level >= 1)
        changed = runPeepholePass(function) || changed;

    if (level >= 2)
        changed = compactNops(&function->chunk) || changed;

    if (inlineStatsEnabled())
    {
        const char *fname = (function->name != NULL) ? function->name->chars : "<script>";
        fprintf(stderr, "[inline] fn=%s attempted=%d inlined=%d rejected=%d\n",
                fname,
                gInlineStats.attempted,
                gInlineStats.inlined,
                gInlineStats.rejected);
    }

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

        if (inst == OP_DEFAULT_LOCAL)
        {
            int jump = ((int)chunk->code[off + 2] << 8) | (int)chunk->code[off + 3];
            int target = off + 4 + jump;
            if (target < 0 || target > size || (target < size && !isStart[target]))
            {
                free(isStart);
                free(nextStart);
                if (errBuf != NULL && errCap > 0)
                    snprintf(errBuf, errCap, "invalid default_local jump target at offset %d -> %d", off, target);
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
