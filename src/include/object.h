#ifndef dotk_object_h
#define dotk_object_h

#include "chunk.h"
#include "common.h"
#include "map.h"
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
#define IS_MAP(value) isObjType(value, OBJ_MAP)
#define IS_SLICE(value) isObjType(value, OBJ_SLICE)
#define IS_BUILTIN(value) isAnyObjType(value, OBJ_MAP | OBJ_LIST | OBJ_STRING)

#define AS_CLASS(value) ((ObjClass *)AS_OBJ(value))
#define AS_INSTANCE(value) ((ObjInstance *)AS_OBJ(value))
#define AS_BOUND_METHOD(value) ((ObjBoundMethod *)AS_OBJ(value))
#define AS_BOUND_BUILTIN(value) ((ObjBoundBuiltin *)AS_OBJ(value))
#define AS_STR(value) ((ObjString *)AS_OBJ(value))
#define AS_CSTR(value) (((ObjString *)AS_OBJ(value))->chars)
#define AS_FUN(value) ((ObjFunction *)AS_OBJ(value))
#define AS_CLOSURE(value) ((ObjClosure *)AS_OBJ(value))
#define AS_NATIVE(value) (((ObjNative *)AS_OBJ(value))->function)
#define AS_LIST(value) ((ObjList *)AS_OBJ(value))
#define AS_MAP(value) ((ObjMap *)AS_OBJ(value))
#define AS_SLICE(value) ((ObjSlice *)AS_OBJ(value))
typedef enum
{
    OBJ_STRING = 1,
    OBJ_MAP = 2,
    OBJ_LIST = 4,
    OBJ_SLICE = 8,
    OBJ_FUNCTION = 16,
    OBJ_CLOSURE = 32,
    OBJ_NATIVE = 64,
    OBJ_UPVALUE = 128,
    OBJ_CLASS = 256,
    OBJ_INSTANCE = 512,
    OBJ_BOUND_METHOD = 1024,
    OBJ_BOUND_BUILTIN = 2048
} ObjType;

static const char *OBJECT_TYPES[11] = {
    "STRING",
    "LIST",
    "SLICE",
    "FUNCTION",
    "CLOSURE",
    "NATIVE",
    "UPVALUE",
    "CLASS",
    "INSTANCE",
    "BOUND METHOD",
    "BOUND BUILTIN"};

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

typedef Value (*NativeFn)(int argC, Value *argV, bool *hasError, bool *pushedValue);

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
    Value indexFn;
    Value setFn;
    Value sizeFn;
    Value hashFn;
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

typedef struct _ObjMap
{
    Obj obj;
    int count;
    int capacity;
    Map map;
} ObjMap;

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

typedef struct ObjBoundBuiltin
{
    Obj obj;
    Value receiver;
    ObjNative *native;
} ObjBoundBuiltin;

ObjClass *newClass(ObjString *name);
ObjClosure *newClosure(ObjFunction *function);
ObjFunction *newFunction();
ObjList *newList();
ObjMap *newMap();
ObjSlice *newSlice(int start, int end, int step);
void appendToList(ObjList *list, Value value);
void storeToList(ObjList *list, int index, Value value);
Value indexFromList(ObjList *list, int index);
void deleteFromList(ObjList *list, int index);
bool isValidListIndex(ObjList *list, int index);
ObjInstance *newInstance(ObjClass *klass);
ObjBoundMethod *newBoundMethod(Value receiver, ObjClosure *method);
ObjBoundBuiltin *newBoundBuiltin(Value receiver, ObjNative *native);
ObjNative *newNative(NativeFn function);
ObjString *takeString(char *chars, int len);
ObjString *copyString(const char *chars, int len);
ObjUpvalue *newUpvalue(Value *slot);
void printObj(Value value, int depth);

static inline bool isObjType(Value value, ObjType type)
{
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

static inline bool isAnyObjType(Value value, ObjType combinedTypes)
{
    return IS_OBJ(value) && (AS_OBJ(value)->type & combinedTypes);
}

#endif