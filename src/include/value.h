#ifndef dotk_value_h
#define dotk_value_h

#include "common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;

#ifdef NAN_BOXING
typedef uint64_t Value;
#else

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

#endif

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
void printValue(Value value);

#endif