#ifndef dotk_map_h
#define dotk_map_h

#include "common.h"
#include "value.h"
#define MAP_MAX_LOAD 0.75

typedef struct
{
    Value key;
    Value value;
    uint32_t keyHash;
    bool isUsed;
    bool isTombstone;
} MapEntry;

typedef struct
{
    int count;
    int capacity;
    MapEntry *entries;
} Map;

void initMap(Map *map);
void freeMap(Map *map);

#endif