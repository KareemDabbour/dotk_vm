#include <string.h>

#include "include/memory.h"
#include "include/object.h"
#include "include/table.h"
#include "include/value.h"
#include "include/vm.h"

#define ALLOCATE_OBJ(type, objectType) \
    (type *)allocateObject(sizeof(type), objectType)

static Obj *allocateObject(size_t size, ObjType type)
{
    Obj *object = (Obj *)reallocate(NULL, 0, size);
    object->type = type;
    object->isMarked = false;
    object->next = vm.objects;
    vm.objects = object;
#ifdef DEBUG_LOG_GC
    printf("%p allocate %zu for %s\n", (void *)object, size, OBJECT_TYPES[type]);
#endif
    return object;
}

ObjBoundMethod *newBoundMethod(Value receiver, ObjClosure *method)
{
    ObjBoundMethod *bound = ALLOCATE_OBJ(ObjBoundMethod, OBJ_BOUND_METHOD);

    bound->receiver = receiver;
    bound->method = method;
    return bound;
}

ObjBoundBuiltin *newBoundBuiltin(Value receiver, ObjNative *method)
{
    ObjBoundBuiltin *bound = ALLOCATE_OBJ(ObjBoundBuiltin, OBJ_BOUND_BUILTIN);

    bound->receiver = receiver;
    bound->native = method;
    return bound;
}

ObjMap *newMap()
{
    ObjMap *map = ALLOCATE_OBJ(ObjMap, OBJ_MAP);
    initMap(&map->map);
    map->map.entries = NULL;
    map->count = 0;
    map->capacity = 0;
    return map;
}

ObjList *newList()
{
    ObjList *list = ALLOCATE_OBJ(ObjList, OBJ_LIST);
    list->items = NULL;
    list->capacity = 0;
    list->count = 0;
    return list;
}

ObjSlice *newSlice(int start, int end, int step)
{
    ObjSlice *slice = ALLOCATE_OBJ(ObjSlice, OBJ_SLICE);
    slice->start = start;
    slice->end = end;
    slice->step = step;
    return slice;
}

void appendToList(ObjList *list, Value value)
{
    // Grow the array if necessary
    if (list->capacity < list->count + 1)
    {
        int oldCapacity = list->capacity;
        list->capacity = GROW_CAPACITY(oldCapacity);
        list->items = GROW_ARRAY(Value, list->items, oldCapacity, list->capacity);
    }
    list->items[list->count] = value;
    list->count++;
    return;
}

void storeToList(ObjList *list, int index, Value value)
{
    list->items[index] = value;
}

Value indexFromList(ObjList *list, int index)
{
    if (index < 0)
        index = list->count + index;

    return list->items[index];
}

void deleteFromList(ObjList *list, int index)
{
    for (int i = index; i < list->count - 1; i++)
    {
        list->items[i] = list->items[i + 1];
    }
    list->items[list->count - 1] = NIL_VAL;
    list->count--;
}

inline bool isValidListIndex(ObjList *list, int index) { return !(index < -list->count || index > list->count - 1); }

ObjClass *newClass(ObjString *name)
{
    ObjClass *clazz = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
    clazz->name = name;
    clazz->initializer = NIL_VAL;
    clazz->toStr = NIL_VAL;
    clazz->equals = NIL_VAL;
    clazz->greaterThan = NIL_VAL;
    clazz->lessThan = NIL_VAL;
    clazz->indexFn = NIL_VAL;
    clazz->setFn = NIL_VAL;
    clazz->sizeFn = NIL_VAL;
    clazz->hashFn = NIL_VAL;

    clazz->superclass = NULL;

    initTable(&clazz->methods);
    initTable(&clazz->staticVars);
    tableSet(&clazz->staticVars, vm.clazzStr, (Value){.type = VAL_OBJ, .as.obj = &clazz->name->obj});
    return clazz;
}

ObjClosure *newClosure(ObjFunction *function)
{
    ObjUpvalue **upvalues = ALLOCATE(ObjUpvalue *, function->upValueCount);
    for (int i = 0; i < function->upValueCount; i++)
        upvalues[i] = NULL;

    ObjClosure *closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalueCount = function->upValueCount;
    return closure;
}

ObjFunction *newFunction()
{
    ObjFunction *func = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
    func->arity = 0;
    func->upValueCount = 0;
    func->name = NULL;
    initChunk(&func->chunk);
    return func;
}

ObjInstance *newInstance(ObjClass *klass)
{
    ObjInstance *instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
    instance->klass = klass;
    initTable(&instance->fields);
    return instance;
}

ObjNative *newNative(NativeFn function)
{
    ObjNative *native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
    native->function = function;
    return native;
}

static ObjString *allocateString(char *chars, int len, uint32_t hash)
{
    ObjString *string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    string->len = len;
    string->chars = chars;
    string->hash = hash;
    push(OBJ_VAL(string));
    tableSet(&vm.strings, string, NIL_VAL);
    pop();
    return string;
}

static uint32_t hashString(const char *key, int len)
{
    uint32_t hash = 2166136261u;
    for (int i = 0; i < len; i++)
    {
        hash ^= (uint32_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

ObjString *takeString(char *chars, int len)
{
    uint32_t hash = hashString(chars, len);
    ObjString *interned = tableFindStr(&vm.strings, chars, len, hash);
    if (interned != NULL)
    {
        FREE_ARRAY(char, chars, len + 1);
        return interned;
    }
    return allocateString(chars, len, hash);
}

ObjString *copyString(const char *chars, int length)
{
    uint32_t hash = hashString(chars, length);
    ObjString *interned = tableFindStr(&vm.strings, chars, length, hash);
    if (interned != NULL)
        return interned;
    char *heapChars = ALLOCATE(char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    return allocateString(heapChars, length, hash);
}

ObjUpvalue *newUpvalue(Value *slot)
{
    ObjUpvalue *upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = NIL_VAL;
    upvalue->next = NULL;
    return upvalue;
}

static void printFunction(ObjFunction *function)
{
    if (function->name == NULL)
    {
        printf("<script>");
        return;
    }
    printf("<%s>", function->name->chars);
}

static void printList(ObjList *list, int depth)
{
    printf("[");
    for (int i = 0; i < list->count - 1; i++)
    {
        if (list->items[i].as.obj == &list->obj)
            printf("[...]");
        else
            printValue(list->items[i], depth - 1);
        printf(", ");
    }
    if (list->count != 0)
    {
        if (list->items[list->count - 1].as.obj == &list->obj)
            printf("[...]");
        else
            printValue(list->items[list->count - 1], depth - 1);
    }
    printf("]");
}
static void printSlice(ObjSlice *slice)
{
    printf("Slice[%d:%d@%d]", slice->start, slice->end, slice->step);
}

static void printTable(Table table, int depth)
{
    int fields = 0;
    for (int i = 0; i < table.capacity; i++)
    {
        if (table.entries[i].key != NULL)
        {
            printf("%s: ", table.entries[i].key->chars);
            printValue(table.entries[i].value, depth - 1);

            if (++fields < table.count)
                printf(", ");
        }
    }
}

static void printMap(ObjMap *map, int depth)
{
    int fields = 0;
    printf("{");
    for (int i = 0; i < map->map.capacity; i++)
    {
        if (map->map.entries[i].isUsed)
        {
            printValue(map->map.entries[i].key, depth - 1);
            printf(": ");
            printValue(map->map.entries[i].value, depth - 1);

            if (++fields < map->count)
                printf(", ");
        }
    }
    printf("}");
}

static void printCustomObj(ObjInstance *instance, int depth)
{
    if (instance == NULL)
    {
        printf("{}");
        return;
    }
    printf("%s@%p:{", instance->klass->name->chars, instance);
    // printf("static: {");
    // printTable(instance->klass->staticVars);
    // printf("}, ");
    // printf("fields: {");

    printTable(instance->fields, depth);

    printf("}");
}

void printObj(Value value, int depth)
{
    switch (OBJ_TYPE(value))
    {
    case OBJ_STRING:
        printf("%s", AS_CSTR(value));
        break;
    case OBJ_FUNCTION:
        printFunction(AS_FUN(value));
        break;
    case OBJ_CLOSURE:
        printFunction(AS_CLOSURE(value)->function);
        break;
    case OBJ_NATIVE:
        printf("<native fn>");
        break;
    case OBJ_CLASS:
        printf("%s class", AS_CLASS(value)->name->chars);
        break;
    case OBJ_INSTANCE:
    {
#if DEBUG_PRINT_VERBOSE_CUSTOM_OBJECTS
        if (depth > 0)
            printCustomObj((AS_INSTANCE(value)), depth - 1);
        else
            printf("%s@%p", AS_INSTANCE(value)->klass->name->chars, (void *)AS_INSTANCE(value));
#else
        printf("%s@%p", AS_INSTANCE(value)->klass->name->chars, (void *)AS_INSTANCE(value));
#endif
        break;
    }
    case OBJ_BOUND_METHOD:
        printFunction(AS_BOUND_METHOD(value)->method->function);
        break;
    case OBJ_BOUND_BUILTIN:
        printf("<native method bound to ");
        printObj(AS_BOUND_BUILTIN(value)->receiver, depth - 1);
        printf(">");
        break;
    case OBJ_LIST:
        printList(AS_LIST(value), depth);
        break;
    case OBJ_SLICE:
        printSlice(AS_SLICE(value));
        break;
    case OBJ_MAP:
        printMap(AS_MAP(value), depth);
        break;
    case OBJ_UPVALUE:
        printf("upvalue");
        break;
    default:
        break;
    }
}