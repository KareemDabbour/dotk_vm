#include "include/memory.h"
#include "include/compiler.h"
#include "include/vm.h"
#include <stdlib.h>

#ifdef DEBUG_LOG_GC
#include "include/debug.h"
#endif

#define GC_HEAP_GROW_FACTOR 2;

void *reallocate(void *pointer, size_t oldSize, size_t newSize)
{
    vm.bytesAllocated += newSize - oldSize;
    if (newSize > oldSize)
    {
#if DEBUG_STRESS_GC
        collectGarbage();
#endif
        if (vm.bytesAllocated > vm.nextGC)
        {
            // fprintf(stderr, "KAREEM -- COLLECTING GARBAGE from %ld. Next GC: %ld\n", vm.bytesAllocated, vm.nextGC);
            // printf("KAREEM -- CALLLINGGNGNNG\n");
            collectGarbage();
        }
    }

    if (newSize == 0)
    {
        free(pointer);
        return NULL;
    }
    void *result = realloc(pointer, newSize);
    if (result == NULL)
        exit(1);
    return result;
}

void markObj(Obj *object)
{
    if (object == NULL || object->isMarked)
        return;
#ifdef DEBUG_LOG_GC
    printf("%p mark ", (void *)object);
    printValue(OBJ_VAL(object), 1);
    printf("\n");
#endif
    object->isMarked = true;

    if (vm.grayCapacity < vm.grayCount + 1)
    {
        vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
        vm.grayStack = (Obj **)realloc(vm.grayStack, sizeof(Obj *) * vm.grayCapacity);

        if (vm.grayStack == NULL)
            exit(1);
    }
    vm.grayStack[vm.grayCount++] = object;
}

void markValue(Value value)
{
    if (IS_OBJ(value))
        markObj(AS_OBJ(value));
}

static void markArray(ValueArray *array)
{
    for (int i = 0; i < array->size; i++)
    {
        markValue(array->values[i]);
    }
}

static void blackenObject(Obj *object)
{
#ifdef DEBUG_LOG_GC
    printf("%p blacken ", (void *)object);
    printValue(OBJ_VAL(object), 1);
    printf("\n");
#endif
    markObj(object);
    switch (object->type)
    {
    case OBJ_CLOSURE:
    {
        ObjClosure *closure = (ObjClosure *)object;
        markObj((Obj *)closure->function);
        for (int i = 0; i < closure->upvalueCount; i++)
        {
            markObj((Obj *)closure->upvalues[i]);
        }
        break;
    }
    case OBJ_MAP:
    {
        ObjMap *map = (ObjMap *)object;
        markMap(&map->map);
        break;
    }
    case OBJ_LIST:
    {
        ObjList *list = (ObjList *)object;
        for (int i = 0; i < list->count; i++)
        {
            markValue(list->items[i]);
        }
        break;
    }
    case OBJ_BOUND_METHOD:
    {
        ObjBoundMethod *bound = (ObjBoundMethod *)object;
        markValue(bound->receiver);
        markObj((Obj *)bound->method);
        break;
    }
    case OBJ_BOUND_BUILTIN:
    {
        ObjBoundBuiltin *bound = (ObjBoundBuiltin *)object;
        markValue(bound->receiver);
        break;
    }
    case OBJ_SLICE:
    {
        ObjSlice *slice = (ObjSlice *)object;
        break;
    }
    case OBJ_CLASS:
    {
        ObjClass *klass = (ObjClass *)object;
        markObj((Obj *)klass->name);
        markObj((Obj *)klass->superclass);
        markTable(&klass->methods);
        markTable(&klass->staticVars);
        markValue(klass->setFn);
        markValue(klass->sizeFn);
        markValue(klass->hashFn);
        markValue(klass->indexFn);
        markValue(klass->initializer);
        markValue(klass->toStr);
        markValue(klass->equals);
        markValue(klass->greaterThan);
        markValue(klass->lessThan);

        break;
    }
    case OBJ_INSTANCE:
    {
        ObjInstance *instance = (ObjInstance *)object;
        markObj((Obj *)instance);
        markObj((Obj *)instance->klass);
        markTable(&instance->fields);
        break;
    }
    case OBJ_FUNCTION:
    {
        ObjFunction *function = (ObjFunction *)object;
        markObj((Obj *)function->name);
        markArray(&function->chunk.constants);
        break;
    }
    case OBJ_UPVALUE:
    {
        ObjUpvalue *upvalue = (ObjUpvalue *)object;

        markValue(((ObjUpvalue *)object)->closed);
        break;
    }
    case OBJ_STRING:
    case OBJ_NATIVE:
    case OBJ_FOREIGN:
        break;
    default:
        break;
    }
}

static void freeObject(Obj *object)
{
#ifdef DEBUG_LOG_GC
    printf("%p free type %s\n", (void *)object, OBJECT_TYPES[object->type]);
#endif
    switch (object->type)
    {
    case OBJ_STRING:
    {
        ObjString *string = (ObjString *)object;
        FREE_ARRAY(char, string->chars, string->len + 1);
        FREE(ObjString, object);
        break;
    }
    case OBJ_FUNCTION:
    {
        ObjFunction *func = (ObjFunction *)object;
        freeChunk(&func->chunk);
        FREE(ObjFunction, object);
        break;
    }
    case OBJ_LIST:
    {
        ObjList *list = (ObjList *)object;
        FREE_ARRAY(Value *, list->items, list->count);
        FREE(ObjList, object);
        break;
    }
    case OBJ_SLICE:
    {
        ObjSlice *slice = (ObjSlice *)object;
        FREE(ObjSlice, object);
        break;
    }
    case OBJ_BOUND_METHOD:
    {
        FREE(ObjBoundMethod, object);
        break;
    }
    case OBJ_BOUND_BUILTIN:
    {
        FREE(ObjBoundBuiltin, object);
        break;
    }
    case OBJ_CLOSURE:
    {
        ObjClosure *closure = (ObjClosure *)object;
        FREE_ARRAY(ObjUpvalue *, closure->upvalues, closure->upvalueCount);
        FREE(ObjClosure, object);
        break;
    }
    case OBJ_MAP:
    {
        ObjMap *map = (ObjMap *)object;
        freeMap(&map->map);
        FREE(ObjMap, object);
        break;
    }
    case OBJ_CLASS:
    {
        ObjClass *clazz = (ObjClass *)object;
        freeTable(&clazz->methods);
        freeTable(&clazz->staticVars);
        FREE(ObjClass, clazz);
        break;
    }
    case OBJ_INSTANCE:
    {
        ObjInstance *instance = (ObjInstance *)object;
        freeTable(&instance->fields);
        FREE(ObjInstance, instance);
        break;
    }
    case OBJ_UPVALUE:
        FREE(ObjUpvalue, object);
        break;
    case OBJ_NATIVE:
        FREE(ObjNative, object);
        break;
    case OBJ_FOREIGN:
    {
        ObjForeign *foreign = (ObjForeign *)object;
        if (foreign->ownsPtr)
            free(foreign->ptr);

        freeTable(&foreign->fields);
        freeTable(&foreign->methods);
        FREE(ObjForeign, foreign);
        break;
    }
    }
}

static void markRoots()
{
    for (Value *slot = vm.stack; slot < vm.stackTop; slot++)
    {
        markValue(*slot);
    }

    for (int i = 0; i < vm.frameCount; i++)
    {
        markObj((Obj *)vm.frames[i].closure);
    }
    for (ObjUpvalue *upvalue = vm.openUpvalues; upvalue != NULL; upvalue = upvalue->next)
    {
        markObj((Obj *)upvalue);
    }

    markTable(&vm.globals);
    // markTable(&vm.strings);
    markTable(&vm.imports);
    markTable(&vm.importFuncs);
    markCompilerRoots();
    markObj((Obj *)vm.initStr);
    markObj((Obj *)vm.toStr);
    markObj((Obj *)vm.eqStr);
    markObj((Obj *)vm.ltStr);
    markObj((Obj *)vm.gtStr);
    markObj((Obj *)vm.indexStr);
    markObj((Obj *)vm.setStr);
    markObj((Obj *)vm.sizeStr);
    markObj((Obj *)vm.hashStr);
    markObj((Obj *)vm.stringClass);
    markObj((Obj *)vm.listClass);
    markObj((Obj *)vm.mapClass);
    markObj((Obj *)vm.lastError);
    markObj((Obj *)vm.errorClass);
    markObj((Obj *)vm.baseObj);
}

void traceReferences()
{
    while (vm.grayCount > 0)
    {
        Obj *object = vm.grayStack[--vm.grayCount];
        blackenObject(object);
    }
}

static void sweep()
{
    Obj *previous = NULL;
    Obj *object = vm.objects;
    while (object != NULL)
    {
        if (object->isMarked)
        {
            object->isMarked = false;
            previous = object;
            object = object->next;
        }
        else
        {
            Obj *unreached = object;
            object = object->next;
            if (previous != NULL)
                previous->next = object;
            else
                vm.objects = object;

            freeObject(unreached);
        }
    }
}

void collectGarbage()
{
#ifdef DEBUG_LOG_GC
    printf("-- gc begin\n");
    size_t before = vm.bytesAllocated;
#endif
    markRoots();
    traceReferences();
    tableRemoveWhite(&vm.strings);
    sweep();

    vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;
#ifdef DEBUG_LOG_GC
    printf("-- gc end\n");
    printf("   collected %zu bytes (from %zu to %zu) next at %zu\n",
           before - vm.bytesAllocated, before, vm.bytesAllocated,
           vm.nextGC);
#endif
}

void freeObjects()
{
    Obj *object = vm.objects;
    while (object != NULL)
    {
        Obj *next = object->next;
        freeObject(object);
        object = next;
    }
    free(vm.grayStack);
}