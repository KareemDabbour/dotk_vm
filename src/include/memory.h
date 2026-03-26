#ifndef dotk_memory_h
#define dotk_memory_h

#include "common.h"
#include "object.h"

#define ALLOCATE(type, count) \
    (type *)reallocateDebug(NULL, 0, sizeof(type) * (count), __FILE__, __LINE__, #type, "alloc")

#define GROW_CAPACITY(cap) ((cap) < 8 ? 8 : (cap) * 2)

#define GROW_ARRAY(type, pointer, oldCount, newCount) \
    (type *)reallocateDebug(pointer, sizeof(type) * (oldCount), sizeof(type) * (newCount), __FILE__, __LINE__, #type, "grow")

#define FREE_ARRAY(type, pointer, size) \
    (type *)reallocateDebug(pointer, sizeof(type) * (size), 0, __FILE__, __LINE__, #type, "free array")

#define FREE(type, pointer) \
    reallocateDebug(pointer, sizeof(type), 0, __FILE__, __LINE__, #type, "free")

void *reallocate(void *pointer, size_t oldSize, size_t newSize);
void *reallocateDebug(void *pointer, size_t oldSize, size_t newSize, const char *file, int line, const char *type, const char *op);
void markValue(Value value);
void markObj(Obj *object);
void collectGarbage();
void freeObjects();

#endif