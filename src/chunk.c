#include "include/chunk.h"
#include "include/memory.h"
#include "include/vm.h"
#include <stdlib.h>

void initChunk(Chunk *chunk)
{
    chunk->capacity = 0;
    chunk->size = 0;
    chunk->code = NULL;
    chunk->positions = NULL;
    chunk->posCapacity = 0;
    chunk->posCount = 0;
    initValueArray(&chunk->constants);
}

void writeChunk(Chunk *chunk, uint8_t byte, int line, int col, char *file, const char *lineStart)
{
    if (chunk->capacity < chunk->size + 1)
    {
        int oldCapacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(oldCapacity);
        chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);
    }
    chunk->code[chunk->size] = byte;
    chunk->size++;

    if (chunk->posCount > 0 && chunk->positions[chunk->posCount - 1].line == line && chunk->positions[chunk->posCount - 1].col == col)
        return;

    if (chunk->posCapacity < chunk->posCount + 1)
    {
        int oldCapacity = chunk->posCapacity;
        chunk->posCapacity = GROW_CAPACITY(oldCapacity);
        chunk->positions = GROW_ARRAY(Position, chunk->positions, oldCapacity, chunk->posCapacity);
    }

    Position *position = &chunk->positions[chunk->posCount++];
    position->offset = chunk->size - 1;
    position->line = line;
    position->lineStart = lineStart;
    position->col = col;
    position->file = file;
}

Position getPos(Chunk *chunk, int inst)
{
    int start = 0;
    int end = chunk->posCount - 1;
    for (;;)
    {
        int mid = (start + end) / 2;
        Position *line = &chunk->positions[mid];
        if (inst < line->offset)
            end = mid - 1;
        else if (mid == chunk->posCount - 1 || inst < chunk->positions[mid + 1].offset)
            return *line;
        else
            start = mid + 1;
    }
}

int addConst(Chunk *chunk, Value value)
{
    push(value);
    writeValueArray(&chunk->constants, value);
    pop();
    return chunk->constants.size - 1;
}

void freeChunk(Chunk *chunk)
{
    FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    FREE_ARRAY(Position, chunk->positions, chunk->posCapacity);
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}