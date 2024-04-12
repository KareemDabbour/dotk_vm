#include "include/map.h"
#include "include/memory.h"
#include "include/object.h"
#include "include/value.h"
#include <stdlib.h>
#include <string.h>

#define MAP_MAX_LOAD 0.75

void initMap(Map *map)
{
    map->count = 0;
    map->capacity = 0;
    map->entries = NULL;
}

void freeMap(Map *map)
{
    FREE_ARRAY(MapEntry, map->entries, map->capacity);
    initMap(map);
}

void markMap(Map *map)
{
    for (int i = 0; i < map->capacity; i++)
    {
        MapEntry *entry = &map->entries[i];
        markValue(entry->value);
        markValue(entry->key);
    }
}