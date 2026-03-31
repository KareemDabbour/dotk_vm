#include "include/memory.h"
#include "include/compiler.h"
#include "include/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEBUG_LOG_GC
#include "include/debug.h"
#endif

#define GC_HEAP_GROW_FACTOR 2


static bool gcDebugEnabled(void)
{
    static int initialized = 0;
    static bool enabled = false;
    if (!initialized)
    {
        const char *value = getenv("DOTK_DEBUG_GC");
        enabled = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
        initialized = 1;
    }
    return enabled;
}

void *reallocateDebug(void *pointer, size_t oldSize, size_t newSize, const char *file, int line, const char *type, const char *op)
{
    if (gcDebugEnabled())
    {
        if (type != NULL)
            fprintf(stderr, "[gc-alloc][%s] %s:%d old=%zu new=%zu ptr=%p type=%s\n", op, file, line, oldSize, newSize, pointer, type);
        else
            fprintf(stderr, "[gc-alloc][%s] %s:%d old=%zu new=%zu ptr=%p\n", op, file, line, oldSize, newSize, pointer);
    }
    return reallocate(pointer, oldSize, newSize);
}


void *reallocate(void *pointer, size_t oldSize, size_t newSize)
{
    vm.bytesAllocated += newSize - oldSize;
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "%p realloc old:%zu new: %zu vmtot: %ld\n", (void *)pointer, oldSize, newSize, vm.bytesAllocated);
#endif
    if (newSize > oldSize)
    {
#if DEBUG_STRESS_GC
        if (!vm.gcDisabled)
            collectGarbage();
#endif
        if (vm.bytesAllocated > vm.maxHeapSize)
        {

            // collectGarbage();

            // if (vm.bytesAllocated > vm.maxHeapSize)
            // {
            //     fprintf(stderr, "KAREEM -- OUT OF MEMORY (%ld)\n", vm.bytesAllocated);
            //     exit(1);
            // }
        }
        if (vm.bytesAllocated > vm.nextGC && !vm.gcDisabled)
        {
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
        markObj((Obj *)instance->klass);
        markTable(&instance->fields);
        break;
    }
    case OBJ_FUNCTION:
    {
        ObjFunction *function = (ObjFunction *)object;
        markObj((Obj *)function->name);
        for (int i = 0; i < function->paramCount; i++)
        {
            markValue(function->chunk.constants.values[function->paramNameConsts[i]]);
        }
        for (int i = 0; i < function->localNameCount; i++)
        {
            uint16_t idx = function->localNameConsts[i];
            if (idx != UINT16_MAX)
                markValue(function->chunk.constants.values[idx]);
        }
        markArray(&function->chunk.constants);
        break;
    }
    case OBJ_UPVALUE:
    {
        ObjUpvalue *upvalue = (ObjUpvalue *)object;

        markValue(((ObjUpvalue *)object)->closed);
        break;
    }
    case OBJ_FOREIGN:
    {
        ObjForeign *foreign = (ObjForeign *)object;
        markTable(&foreign->fields);
        markTable(&foreign->methods);
        break;
    }
    case OBJ_STRING:
        break;
    case OBJ_NATIVE:
    {
        ObjNative *native = (ObjNative *)object;
        for (int i = 0; i < native->paramCount; i++)
        {
            if (native->params[i].hasDefault)
                markValue(native->params[i].defaultVal);
        }
        break;
    }
    default:
        break;
    }
}

static void freeObject(Obj *object)
{
#ifdef DEBUG_LOG_GC
    fprintf(stderr, "%p free type %s\n", (void *)object, OBJECT_TYPES[object->type]);
#endif
    switch (object->type)
    {
    case OBJ_STRING:
    {
        ObjString *string = (ObjString *)object;
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_STRING: %s\n", (void *)object, string->chars);
#endif

        FREE_ARRAY(char, string->chars, string->len + 1);
        FREE(ObjString, object);
        break;
    }
    case OBJ_FUNCTION:
    {
        ObjFunction *func = (ObjFunction *)object;
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_FUNCTION (%s)\n", (void *)object, func->name ? func->name->chars : "anonymous");
#endif
        FREE_ARRAY(uint16_t, func->paramNameConsts, func->paramCount);
        FREE_ARRAY(uint16_t, func->defaultConsts, func->defaultCount);
        FREE_ARRAY(uint16_t, func->localNameConsts, func->localNameCount);
        // FREE(ObjString, func->name);
        freeChunk(&func->chunk);
        FREE(ObjFunction, object);
        break;
    }
    case OBJ_LIST:
    {
        ObjList *list = (ObjList *)object;
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_LIST of %d elements\n", (void *)object, list->count);
#endif

        FREE_ARRAY(Value, list->items, list->capacity);
        FREE(ObjList, object);
        break;
    }
    case OBJ_SLICE:
    {
        ObjSlice *slice = (ObjSlice *)object;
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_SLICE %d:%d:%d\n", (void *)object, slice->start, slice->end, slice->step);
#endif
        FREE(ObjSlice, object);
        break;
    }
    case OBJ_BOUND_METHOD:
    {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_BOUND_METHOD\n", (void *)object);
#endif
        ObjBoundMethod *boundMethod = (ObjBoundMethod *)object;
        FREE(ObjBoundMethod, boundMethod);

        break;
    }
    case OBJ_BOUND_BUILTIN:
    {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_BOUND_BUILTIN\n", (void *)object);
#endif
        FREE(ObjBoundBuiltin, object);
        break;
    }
    case OBJ_CLOSURE:
    {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_CLOSURE\n", (void *)object);
#endif
        ObjClosure *closure = (ObjClosure *)object;
        FREE_ARRAY(ObjUpvalue *, closure->upvalues, closure->upvalueCount);
        FREE(ObjClosure, object);
        break;
    }
    case OBJ_MAP:
    {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_MAP\n", (void *)object);
#endif
        ObjMap *map = (ObjMap *)object;
        freeMap(&map->map);
        FREE(ObjMap, object);
        break;
    }
    case OBJ_CLASS:
    {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_CLASS\n", (void *)object);
#endif
        ObjClass *clazz = (ObjClass *)object;
        freeTable(&clazz->methods);
        freeTable(&clazz->staticVars);
        FREE(ObjClass, clazz);
        break;
    }
    case OBJ_INSTANCE:
    {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_INSTANCE\n", (void *)object);
#endif
        ObjInstance *instance = (ObjInstance *)object;
        freeTable(&instance->fields);
        FREE(ObjInstance, instance);
        break;
    }
    case OBJ_UPVALUE:
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_UPVALUE\n", (void *)object);
#endif
        FREE(ObjUpvalue, object);
        break;
    case OBJ_NATIVE:
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_NATIVE\n", (void *)object);
#endif
        FREE_ARRAY(NativeParamDef, ((ObjNative *)object)->params, ((ObjNative *)object)->paramCount);
        FREE(ObjNative, object);
        break;
    case OBJ_FOREIGN:
    {
#ifdef DEBUG_LOG_GC
        fprintf(stderr, "%p freeing OBJ_FOREIGN\n", (void *)object);
#endif
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
    markTable(&vm.constGlobals);
    markTable(&vm.strings);
    markTable(&vm.imports);
    markTable(&vm.importFuncs);
    markObj((Obj *)vm.currentModuleExports);
    markCompilerRoots();
    markObj((Obj *)vm.initStr);
    markObj((Obj *)vm.strDunderStr);
    markObj((Obj *)vm.setitemDunderStr);
    markObj((Obj *)vm.hashDunderStr);
    markObj((Obj *)vm.lenDunderStr);
    markObj((Obj *)vm.eqDunderStr);
    markObj((Obj *)vm.ltDunderStr);
    markObj((Obj *)vm.gtDunderStr);
    markObj((Obj *)vm.getitemDunderStr);
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
    /* Optional runtime GC debug output when DOTK_DEBUG_GC is set. */
    if (getenv("DOTK_DEBUG_GC") != NULL)
    {
        size_t objCount = 0;
        size_t cnt_string = 0, cnt_function = 0, cnt_closure = 0, cnt_list = 0, cnt_map = 0, cnt_class = 0, cnt_instance = 0, cnt_upvalue = 0, cnt_foreign = 0, cnt_native = 0, cnt_bound_method = 0, cnt_bound_builtin = 0, cnt_slice = 0, cnt_other = 0;
        for (Obj *o = vm.objects; o != NULL; o = o->next)
        {
            objCount++;
            switch (o->type)
            {
            case OBJ_STRING:
                cnt_string++;
                break;
            case OBJ_FUNCTION:
                cnt_function++;
                break;
            case OBJ_CLOSURE:
                cnt_closure++;
                break;
            case OBJ_LIST:
                cnt_list++;
                break;
            case OBJ_MAP:
                cnt_map++;
                break;
            case OBJ_CLASS:
                cnt_class++;
                break;
            case OBJ_INSTANCE:
                cnt_instance++;
                break;
            case OBJ_UPVALUE:
                cnt_upvalue++;
                break;
            case OBJ_FOREIGN:
                cnt_foreign++;
                break;
            case OBJ_NATIVE:
                cnt_native++;
                break;
            case OBJ_BOUND_METHOD:
                cnt_bound_method++;
                break;
            case OBJ_BOUND_BUILTIN:
                cnt_bound_builtin++;
                break;
            case OBJ_SLICE:
                cnt_slice++;
                break;
            default:
                cnt_other++;
                break;
            }
        }
        fprintf(stderr, "[DOTK_DEBUG_GC] gc begin bytes=%zu objects=%zu strings=%d/%d importCount=%d nextGC=%zu types: str=%zu func=%zu clos=%zu list=%zu map=%zu class=%zu inst=%zu upv=%zu foreign=%zu native=%zu bmethod=%zu bbuiltin=%zu slice=%zu other=%zu\n",
                vm.bytesAllocated, objCount, vm.strings.count, vm.strings.capacity, vm.importCount, vm.nextGC,
                cnt_string, cnt_function, cnt_closure, cnt_list, cnt_map, cnt_class, cnt_instance, cnt_upvalue, cnt_foreign, cnt_native, cnt_bound_method, cnt_bound_builtin, cnt_slice, cnt_other);
    }
    markRoots();
    traceReferences();
    tableRemoveWhite(&vm.strings);
    sweep();

    vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;
#ifdef DEBUG_LOG_GC
    printf("-- gc end\n");
    fprintf(stderr, "   collected %zu bytes (from %zu to %zu) next at %zu\n",
            before - vm.bytesAllocated, before, vm.bytesAllocated,
            vm.nextGC);
#endif
    if (getenv("DOTK_DEBUG_GC") != NULL)
    {
        size_t objCount2 = 0;
        size_t cnt_string2 = 0, cnt_function2 = 0, cnt_closure2 = 0, cnt_list2 = 0, cnt_map2 = 0, cnt_class2 = 0, cnt_instance2 = 0, cnt_upvalue2 = 0, cnt_foreign2 = 0, cnt_native2 = 0, cnt_bound_method2 = 0, cnt_bound_builtin2 = 0, cnt_slice2 = 0, cnt_other2 = 0;
        for (Obj *o = vm.objects; o != NULL; o = o->next)
        {
            objCount2++;
            switch (o->type)
            {
            case OBJ_STRING:
                cnt_string2++;
                break;
            case OBJ_FUNCTION:
                cnt_function2++;
                break;
            case OBJ_CLOSURE:
                cnt_closure2++;
                break;
            case OBJ_LIST:
                cnt_list2++;
                break;
            case OBJ_MAP:
                cnt_map2++;
                break;
            case OBJ_CLASS:
                cnt_class2++;
                break;
            case OBJ_INSTANCE:
                cnt_instance2++;
                break;
            case OBJ_UPVALUE:
                cnt_upvalue2++;
                break;
            case OBJ_FOREIGN:
                cnt_foreign2++;
                break;
            case OBJ_NATIVE:
                cnt_native2++;
                break;
            case OBJ_BOUND_METHOD:
                cnt_bound_method2++;
                break;
            case OBJ_BOUND_BUILTIN:
                cnt_bound_builtin2++;
                break;
            case OBJ_SLICE:
                cnt_slice2++;
                break;
            default:
                cnt_other2++;
                break;
            }
        }
        fprintf(stderr, "[DOTK_DEBUG_GC] gc end bytes=%zu objects=%zu strings=%d/%d nextGC=%zu types: str=%zu func=%zu clos=%zu list=%zu map=%zu class=%zu inst=%zu upv=%zu foreign=%zu native=%zu bmethod=%zu bbuiltin=%zu slice=%zu other=%zu\n",
                vm.bytesAllocated, objCount2, vm.strings.count, vm.strings.capacity, vm.nextGC,
                cnt_string2, cnt_function2, cnt_closure2, cnt_list2, cnt_map2, cnt_class2, cnt_instance2, cnt_upvalue2, cnt_foreign2, cnt_native2, cnt_bound_method2, cnt_bound_builtin2, cnt_slice2, cnt_other2);
    }
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