#ifndef dotk_chunk_h
#define dotk_chunk_h
#include "common.h"
#include "value.h"

typedef enum
{
    OP_CONSTANT,
    OP_CONSTANT_LONG,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_DUP,
    OP_CALL,
    OP_DEF_GLOBAL,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_JUMP_IF_FALSE,
    OP_CLOSE_UPVALUE,
    OP_JUMP,
    OP_LOOP,
    OP_EQUAL,
    OP_LESS,
    OP_BIN_SHIFT_LEFT,
    OP_GREATER,
    OP_BIN_SHIFT_RIGHT,
    OP_BIN_OR,
    OP_BIN_AND,
    OP_BIN_XOR,
    OP_ADD,
    OP_SUB,
    OP_MULT,
    OP_POW,
    OP_MOD,
    OP_DIV,
    OP_INT_DIV,
    OP_NOT,
    OP_NEGATE,
    OP_PRINT,
    OP_CLOSURE,
    OP_RETURN,
    OP_CLASS,
    OP_METHOD,
    OP_INVOKE,
    OP_INHERIT,
    OP_SET_PROPERTY,
    OP_GET_PROPERTY,
    OP_GET_SUPER,
    OP_SUPER_INVOKE,
    OP_BUILD_LIST,
    OP_BUILD_DEFAULT_LIST,
    OP_INDEX_SUBSCR,
    OP_STORE_SUBSCR,
    OP_WIDE,
    OP_STATIC_VAR,
    OP_IMPORT,
    OP_BUILD_SLICE,
    OP_BUILD_MAP,
    OP_TRY,
    OP_CATCH
} OpCode;

typedef struct
{
    int offset;
    int line;
    int col;
    char *file;
    const char *lineStart;
} Position;

typedef struct
{
    int size;
    int capacity;
    uint8_t *code;
    ValueArray constants;
    int posCount;
    int posCapacity;
    Position *positions;
} Chunk;

void initChunk(Chunk *chunk);
void writeChunk(Chunk *chunk, uint8_t byte, int line, int col, char *file, const char *lineStart);
int addConst(Chunk *chunk, Value value);
// int writeConst(Chunk *chunk, Value value, int line);
Position getPos(Chunk *chunk, int inst);
void freeChunk(Chunk *chunk);

#endif