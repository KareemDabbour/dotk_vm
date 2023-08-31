#include <stdio.h>
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

ObjList *newList()
{
    ObjList *list = ALLOCATE_OBJ(ObjList, OBJ_LIST);
    list->items = NULL;
    list->capacity = 0;
    list->count = 0;
    return list;
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

bool isValidListIndex(ObjList *list, int index)
{
    if (index < -list->count || index > list->count - 1)
    {
        return false;
    }
    return true;
}

ObjClass *newClass(ObjString *name)
{
    ObjClass *clazz = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
    clazz->name = name;
    clazz->initializer = NIL_VAL;
    clazz->toString = NIL_VAL;
    initTable(&clazz->methods);
    initTable(&clazz->staticVars);
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

static void printList(ObjList *list)
{
    printf("[");
    for (int i = 0; i < list->count - 1; i++)
    {
        printValue(list->items[i]);
        printf(", ");
    }
    if (list->count != 0)
        printValue(list->items[list->count - 1]);
    printf("]");
}

static void printTable(Table table)
{
    int fields = 0;
    for (int i = 0; i < table.capacity; i++)
    {
        if (table.entries[i].key != NULL)
        {
            printf("%s: ", table.entries[i].key->chars);
            printValue(table.entries[i].value);

            if (++fields < table.count)
                printf(", ");
        }
    }
}

static void printCustomObj(ObjInstance *instance)
{
    if (instance == NULL)
    {
        printf("{}");
        return;
    }
    printf("%s<%p>: {", instance->klass->name->chars, instance);
    printf("static: {");
    printTable(instance->klass->staticVars);
    printf("}, ");
    printf("fields: {");
    printTable(instance->fields);
    printf("}");
    printf("}");
}

void printObj(Value value)
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
        printCustomObj((AS_INSTANCE(value)));
#else
        printf("%s instance", AS_INSTANCE(value)->klass->name->chars);
#endif
        break;
    }
    case OBJ_BOUND_METHOD:
        printFunction(AS_BOUND_METHOD(value)->method->function);
        break;
    case OBJ_LIST:
        printList(AS_LIST(value));
        break;
    case OBJ_UPVALUE:
        printf("upvalue");
        break;
    default:
        break;
    }
}