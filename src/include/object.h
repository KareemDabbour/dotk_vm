#ifndef dotk_object_h
#define dotk_object_h

#include "chunk.h"
#include "common.h"
#include "table.h"
#include "value.h"

#define OBJ_TYPE(value) (AS_OBJ(value)->type)

#define IS_CLASS(value) isObjType(value, OBJ_CLASS)
#define IS_INSTANCE(value) isObjType(value, OBJ_INSTANCE)
#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define IS_STR(value) isObjType(value, OBJ_STRING)
#define IS_FUN(value) isObjType(value, OBJ_FUNCTION)
#define IS_CLOSURE(value) isObjType(value, OBJ_CLOSURE)
#define IS_NATIVE(value) isObjType(value, OBJ_NATIVE)
#define IS_LIST(value) isObjType(value, OBJ_LIST)
#define IS_SLICE(value) isObjType(value, OBJ_SLICE)

#define AS_CLASS(value) ((ObjClass *)AS_OBJ(value))
#define AS_INSTANCE(value) ((ObjInstance *)AS_OBJ(value))
#define AS_BOUND_METHOD(value) ((ObjBoundMethod *)AS_OBJ(value))
#define AS_STR(value) ((ObjString *)AS_OBJ(value))
#define AS_CSTR(value) (((ObjString *)AS_OBJ(value))->chars)
#define AS_FUN(value) ((ObjFunction *)AS_OBJ(value))
#define AS_CLOSURE(value) ((ObjClosure *)AS_OBJ(value))
#define AS_NATIVE(value) (((ObjNative *)AS_OBJ(value))->function)
#define AS_LIST(value) ((ObjList *)AS_OBJ(value))
#define AS_SLICE(value) ((ObjSlice *)AS_OBJ(value))
typedef enum
{
    OBJ_STRING = 0,
    OBJ_LIST,
    OBJ_SLICE,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_NATIVE,
    OBJ_UPVALUE,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_BOUND_METHOD
} ObjType;

static const char *OBJECT_TYPES[10] = {
    "STRING",
    "LIST",
    "SLICE",
    "FUNCTION",
    "CLOSURE",
    "NATIVE",
    "UPVALUE",
    "CLASS",
    "INSTANCE",
    "BOUND METHOD"};

struct Obj
{
    ObjType type;
    bool isMarked;
    struct Obj *next;
};

typedef struct
{
    Obj obj;
    int arity;
    int upValueCount;
    Chunk chunk;
    ObjString *name;
} ObjFunction;

struct ObjString
{
    Obj obj;
    int len;
    char *chars;
    uint32_t hash;
};

typedef struct ObjUpvalue
{
    Obj obj;
    Value *location;
    Value closed;
    struct ObjUpvalue *next;
} ObjUpvalue;

typedef struct ObjClosure
{
    Obj obj;
    ObjFunction *function;
    ObjUpvalue **upvalues;
    int upvalueCount;
} ObjClosure;

typedef Value (*NativeFn)(int argC, Value *argV, bool *hasError);

typedef struct ObjNative
{
    Obj obj;
    NativeFn function;
} ObjNative;

typedef struct ObjClass
{
    Obj obj;
    ObjString *name;
    Value initializer;
    Value toStr;
    Value equals;
    Value greaterThan;
    Value lessThan;
    struct ObjClass *superclass;
    Table methods;
    Table staticVars;
} ObjClass;

typedef struct _ObjList
{
    Obj obj;
    int count;
    int capacity;
    Value *items;
} ObjList;

typedef struct _ObjSlice
{
    Obj obj;
    int start;
    int end;
    int step;
} ObjSlice;

typedef struct ObjInstance
{
    Obj obj;
    ObjClass *klass;
    Table fields;
} ObjInstance;

typedef struct ObjBoundMethod
{
    Obj obj;
    Value receiver;
    ObjClosure *method;
} ObjBoundMethod;

ObjClass *newClass(ObjString *name);
ObjClosure *newClosure(ObjFunction *function);
ObjFunction *newFunction();
ObjList *newList();
ObjSlice *newSlice(int start, int end, int step);
void appendToList(ObjList *list, Value value);
void storeToList(ObjList *list, int index, Value value);
Value indexFromList(ObjList *list, int index);
void deleteFromList(ObjList *list, int index);
bool isValidListIndex(ObjList *list, int index);
ObjInstance *newInstance(ObjClass *klass);
ObjBoundMethod *newBoundMethod(Value receiver, ObjClosure *method);
ObjNative *newNative(NativeFn function);
ObjString *takeString(char *chars, int len);
ObjString *copyString(const char *chars, int len);
ObjUpvalue *newUpvalue(Value *slot);
void printObj(Value value, int depth);

static inline bool isObjType(Value value, ObjType type)
{
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#endif