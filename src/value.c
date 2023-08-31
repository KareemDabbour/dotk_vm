#include "include/value.h"
#include "include/memory.h"
#include "include/object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void initValueArray(ValueArray *array)
{
    array->capacity = 0;
    array->size = 0;
    array->values = NULL;
}

void writeValueArray(ValueArray *array, Value value)
{
    if (array->capacity < array->size + 1)
    {
        int oldCapacity = array->capacity;
        array->capacity = GROW_CAPACITY(oldCapacity);
        array->values = GROW_ARRAY(Value, array->values, oldCapacity, array->capacity);
    }
    array->values[array->size] = value;
    array->size++;
}

void freeValueArray(ValueArray *array)
{
    FREE_ARRAY(uint8_t, array->values, array->capacity);
    initValueArray(array);
}

void printValue(Value value)
{
    switch (value.type)
    {
    case VAL_BOOL:
        printf(AS_BOOL(value) ? "true" : "false");
        break;
    case VAL_NIL:
        printf("null");
        break;
    case VAL_NUMBER:
        printf("%g", AS_NUM(value));
        break;
    case VAL_OBJ:
        printObj(value);
        break;
    default:
        break;
    }
}

bool valuesEqual(Value a, Value b)
{
    if (a.type != b.type)
        return false;
    switch (a.type)
    {
    case VAL_BOOL:
        return AS_BOOL(a) == AS_BOOL(b);
    case VAL_NIL:
        return true;
    case VAL_NUMBER:
        return AS_NUM(a) == AS_NUM(b);
    case VAL_OBJ:
    {
        if (IS_LIST(a) && IS_LIST(b))
        {
            ObjList *aL = AS_LIST(a);
            ObjList *bL = AS_LIST(b);
            if (aL->count != bL->count)
                return false;
            for (int i = 0; i < aL->count; i++)
            {
                if (!valuesEqual(aL->items[i], bL->items[i]))
                    return false;
            }
            return true;
        }
        return AS_OBJ(a) == AS_OBJ(b);
    }
    default:
        return false;
    }
}