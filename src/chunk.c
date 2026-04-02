#include "include/chunk.h"
#include "include/memory.h"
#include "include/vm.h"
#include <stdlib.h>

static uint32_t getOrAddSource(Chunk *chunk, int line, char *file, const char *lineStart)
{
    if (chunk->sourceCount > 0)
    {
        PositionSource *last = &chunk->sources[chunk->sourceCount - 1];
        if (last->line == line && last->file == file && last->lineStart == lineStart)
            return (uint32_t)(chunk->sourceCount - 1);
    }

    if (chunk->sourceCapacity < chunk->sourceCount + 1)
    {
        int oldCapacity = chunk->sourceCapacity;
        chunk->sourceCapacity = GROW_CAPACITY(oldCapacity);
        chunk->sources = GROW_ARRAY(PositionSource, chunk->sources, oldCapacity, chunk->sourceCapacity);
    }

    PositionSource *source = &chunk->sources[chunk->sourceCount++];
    source->line = line;
    source->file = file;
    source->lineStart = lineStart;
    return (uint32_t)(chunk->sourceCount - 1);
}

void initChunk(Chunk *chunk)
{
    chunk->capacity = 0;
    chunk->size = 0;
    chunk->code = NULL;
    chunk->hasDebugPositions = DOTK_ENABLE_DEBUG_POSITIONS;
    chunk->sourceCount = 0;
    chunk->sourceCapacity = 0;
    chunk->sources = NULL;
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

    if (!chunk->hasDebugPositions)
        return;

    uint32_t sourceIndex = getOrAddSource(chunk, line, file, lineStart);

    if (chunk->posCount > 0)
    {
        PositionEntry *last = &chunk->positions[chunk->posCount - 1];
        if (last->sourceIndex == sourceIndex && (int)last->col == col)
            return;
    }

    if (chunk->posCapacity < chunk->posCount + 1)
    {
        int oldCapacity = chunk->posCapacity;
        chunk->posCapacity = GROW_CAPACITY(oldCapacity);
        chunk->positions = GROW_ARRAY(PositionEntry, chunk->positions, oldCapacity, chunk->posCapacity);
    }

    PositionEntry *position = &chunk->positions[chunk->posCount++];
    position->offset = chunk->size - 1;
    position->sourceIndex = sourceIndex;
    position->col = (uint16_t)col;
}

Position getPos(Chunk *chunk, int inst)
{
    if (chunk->posCount == 0 || !chunk->hasDebugPositions)
    {
        Position fallback = {0};
        fallback.offset = inst;
        fallback.line = 0;
        fallback.col = 0;
        fallback.file = "<unknown>";
        fallback.lineStart = "";
        return fallback;
    }

    int start = 0;
    int end = chunk->posCount - 1;
    for (;;)
    {
        if (start > end)
        {
            /* inst is out of range — return the last known position */
            PositionEntry *entry = &chunk->positions[chunk->posCount - 1];
            PositionSource *source = &chunk->sources[entry->sourceIndex];
            Position position = {0};
            position.offset = entry->offset;
            position.line = source->line;
            position.col = (int)entry->col;
            position.file = source->file;
            position.lineStart = source->lineStart;
            return position;
        }
        int mid = (start + end) / 2;
        PositionEntry *entry = &chunk->positions[mid];
        if (inst < entry->offset)
            end = mid - 1;
        else if (mid == chunk->posCount - 1 || inst < chunk->positions[mid + 1].offset)
        {
            PositionSource *source = &chunk->sources[entry->sourceIndex];
            Position position = {0};
            position.offset = entry->offset;
            position.line = source->line;
            position.col = (int)entry->col;
            position.file = source->file;
            position.lineStart = source->lineStart;
            return position;
        }
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
    FREE_ARRAY(PositionSource, chunk->sources, chunk->sourceCapacity);
    FREE_ARRAY(PositionEntry, chunk->positions, chunk->posCapacity);
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}