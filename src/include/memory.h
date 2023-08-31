#ifndef dotk_memory_h
#define dotk_memory_h

#include "common.h"
#include "object.h"

#define ALLOCATE(type, count) \
    (type *)reallocate(NULL, 0, sizeof(type) * (count))

#define GROW_CAPACITY(cap) ((cap) < 8 ? 8 : (cap)*2)

#define GROW_ARRAY(type, pointer, oldCount, newCount) \
    (type *)reallocate(pointer, sizeof(type) * (oldCount), sizeof(type) * (newCount))

#define FREE_ARRAY(type, pointer, size) \
    (type *)reallocate(pointer, sizeof(type) * (size), 0)

#define FREE(type, pointer) reallocate(pointer, sizeof(type), 0)

void *reallocate(void *pointer, size_t oldSize, size_t newSize);
void markValue(Value value);
void markObj(Obj *object);
void collectGarbage();
void freeObjects();

#endif