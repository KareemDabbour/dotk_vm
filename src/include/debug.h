#ifndef dotk_debug_h
#define dotk_debug_h

#include "chunk.h"

void disassembleChunk(Chunk* chunk, const char* name);
int disassembleInst(Chunk* chunk, int offset);

#endif