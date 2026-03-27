#include "include/value.h"
#include "include/memory.h"
#include "include/object.h"
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
    FREE_ARRAY(Value, array->values, array->capacity);
    initValueArray(array);
}

void printValue(Value value, int depth)
{
#ifdef NAN_BOXING
    if (IS_BOOL(value))
    {
        printf(AS_BOOL(value) ? "true" : "false");
        return;
    }
    if (IS_NIL(value))
    {
        printf("null");
        return;
    }
    if (IS_UNDEF(value))
    {
        printf("<undef>");
        return;
    }
    if (IS_NUM(value))
    {
        printf("%.15g", AS_NUM(value));
        return;
    }
    printObj(value, depth);
#else
    switch (value.type)
    {
    case VAL_BOOL:
        printf(AS_BOOL(value) ? "true" : "false");
        break;
    case VAL_NIL:
        printf("null");
        break;
    case VAL_UNDEF:
        printf("<undef>");
        break;
    case VAL_NUMBER:
        printf("%.15g", AS_NUM(value));
        break;
    case VAL_OBJ:
        printObj(value, depth);
        break;
    default:
        break;
    }
#endif
}

bool valuesEqual(Value a, Value b)
{
#ifdef NAN_BOXING
    if (IS_NUM(a) && IS_NUM(b))
        return AS_NUM(a) == AS_NUM(b);

    if (a == b)
        return true;

    if (!(IS_OBJ(a) && IS_OBJ(b)))
        return false;

    if (IS_STR(a) && IS_STR(b))
    {
        ObjString *aS = AS_STR(a);
        ObjString *bS = AS_STR(b);
        if (aS == bS)
            return true;
        if (aS->len != bS->len || aS->hash != bS->hash)
            return false;
        return memcmp(aS->chars, bS->chars, aS->len) == 0;
    }
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
    if (IS_FOREIGN(a) && IS_FOREIGN(b))
        return AS_FOREIGN_PTR(a) == AS_FOREIGN_PTR(b);

    return AS_OBJ(a) == AS_OBJ(b);
#else
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
        if (IS_STR(a) && IS_STR(b))
        {
            ObjString *aS = AS_STR(a);
            ObjString *bS = AS_STR(b);
            if (aS == bS)
                return true;
            if (aS->len != bS->len || aS->hash != bS->hash)
                return false;
            return memcmp(aS->chars, bS->chars, aS->len) == 0;
        }
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
        if (IS_FOREIGN(a) && IS_FOREIGN(b))
            return AS_FOREIGN_PTR(a) == AS_FOREIGN_PTR(b);

        return AS_OBJ(a) == AS_OBJ(b);
    }
    default:
        return false;
    }
#endif
}