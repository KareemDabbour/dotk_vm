#ifndef dotk_value_h
#define dotk_value_h

#include "common.h"
#include <string.h>

typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef enum
{
    VAL_BOOL,
    VAL_NIL,
    VAL_NUMBER,
    VAL_OBJ
} ValueType;

static char *VALUE_TYPES[4] = {
    "Boolean",
    "Null",
    "Number",
    "Object"};

#ifdef NAN_BOXING
typedef uint64_t Value;

#define SIGN_BIT ((uint64_t)0x8000000000000000)
#define QNAN ((uint64_t)0x7ffc000000000000)

#define TAG_NIL 1
#define TAG_FALSE 2
#define TAG_TRUE 3

static inline Value numToValue(double num)
{
    Value value;
    memcpy(&value, &num, sizeof(double));
    return value;
}

static inline double valueToNum(Value value)
{
    double num;
    memcpy(&num, &value, sizeof(Value));
    return num;
}

#define BOOL_VAL(val) ((val) ? (Value)(QNAN | TAG_TRUE) : (Value)(QNAN | TAG_FALSE))
#define NIL_VAL ((Value)(QNAN | TAG_NIL))
#define NUM_VAL(val) numToValue(val)
#define OBJ_VAL(object) ((Value)(SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(Obj *)(object)))

#define IS_BOOL(value) (((value) | 1) == (QNAN | TAG_TRUE))
#define IS_NIL(value) ((value) == NIL_VAL)
#define IS_NUM(value) (((value) & QNAN) != QNAN)
#define IS_OBJ(value) (((value) & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))

#define AS_BOOL(value) ((value) == (QNAN | TAG_TRUE))
#define AS_NUM(value) valueToNum(value)
#define AS_OBJ(value) ((Obj *)(uintptr_t)((value) & ~(SIGN_BIT | QNAN)))

static inline ValueType VALUE_TYPE(Value value)
{
    if (IS_BOOL(value))
        return VAL_BOOL;
    if (IS_NIL(value))
        return VAL_NIL;
    if (IS_NUM(value))
        return VAL_NUMBER;
    return VAL_OBJ;
}
#else

typedef struct
{
    ValueType type;
    union
    {
        bool boolean;
        double number;
        Obj *obj;
    } as;
} Value;

#define IS_BOOL(value) ((value).type == VAL_BOOL)
#define IS_NIL(value) ((value).type == VAL_NIL)
#define IS_NUM(value) ((value).type == VAL_NUMBER)
#define IS_OBJ(value) ((value).type == VAL_OBJ)

#define AS_OBJ(val) ((val).as.obj)
#define AS_BOOL(val) ((val).as.boolean)
#define AS_NUM(val) ((val).as.number)

#define OBJ_VAL(object) ((Value){.type = VAL_OBJ, {.obj = (Obj *)object}})
#define BOOL_VAL(val) ((Value){.type = VAL_BOOL, {.boolean = val}})
#define NIL_VAL ((Value){.type = VAL_NIL, {.number = 0}})
#define NUM_VAL(val) ((Value){.type = VAL_NUMBER, {.number = val}})

static inline ValueType VALUE_TYPE(Value value)
{
    return value.type;
}

#endif

static inline const char *valueTypeName(Value value)
{
    return VALUE_TYPES[VALUE_TYPE(value)];
}

typedef struct
{
    int capacity;
    int size;
    Value *values;
} ValueArray;

bool valuesEqual(Value a, Value b);
void initValueArray(ValueArray *array);
void writeValueArray(ValueArray *array, Value value);
void freeValueArray(ValueArray *array);
void printValue(Value value, int depth);

#endif