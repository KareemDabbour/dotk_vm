#include "include/dotk.h"
#include "include/compiler.h"
#include "include/scanner.h"
#include "include/memory.h"
#include "include/vm.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_ALLOCATION_BUCKETS 1024

static size_t allocationBucket(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    return ((address >> 4) ^ (address >> 14) ^ (address >> 24)) & (HOST_ALLOCATION_BUCKETS - 1);
}

typedef struct HostSource
{
    char *text;
    char *file;
    struct HostSource *next;
} HostSource;

typedef struct HostAllocation
{
    void *pointer;
    size_t size;
    bool system;
    struct HostAllocation *next;
} HostAllocation;

struct DotKHandle
{
    DotKVM *owner;
    Value value;
    struct DotKHandle *next;
};

typedef struct HostNativeBinding
{
    DotKVM *owner;
    DotKHostNativeFn function;
    void *userdata;
    char *name;
    struct HostNativeBinding *next;
} HostNativeBinding;

struct DotKNativeCall
{
    DotKVM *owner;
    Value *arguments;
    size_t count;
    Value *result;
    DotKStatus status;
    char error[512];
};

struct DotKTask
{
    DotKVM *owner;
    ObjGenerator *generator;
    DotKTaskState state;
    Value value;
    struct DotKTask *next;
};

struct DotKVM
{
    VM runtime;
    pthread_mutex_t executionLock;
    struct DotKVM *next;
    DotKConfig config;
    DotKDiagnostic diagnostic;
    char message[2048];
    char trace[4096];
    char file[512];
    pthread_t thread;
    DotKHandle *handles;
    HostSource *sources;
    HostAllocation *allocations[HOST_ALLOCATION_BUCKETS];
    HostNativeBinding *bindings;
    DotKNativeCall *nativeCall;
    bool initializingModule;
    DotKTask *tasks;
    DotKTask *taskTail;
    DotKTask *nextTask;
    DotKProfileStats profile;
    jmp_buf *escape;
    bool recovering;
    bool poisoned;
    uint64_t remaining;
    unsigned interrupt_ticks;
};

static _Thread_local DotKVM *activeContext;
static DotKVM *contexts;
static pthread_mutex_t hostMutex = PTHREAD_MUTEX_INITIALIZER;

static void activateContext(DotKVM *context)
{
    activeContext = context;
    vmSelectInstance(&context->runtime, &context->executionLock);
}

static void deactivateContext(void)
{
    activeContext = NULL;
    vmSelectInstance(NULL, NULL);
}

static bool registeredContext(const DotKVM *context)
{
    for (DotKVM *candidate = contexts; candidate; candidate = candidate->next)
        if (candidate == context)
            return true;
    return false;
}

static bool validNativeCall(const DotKNativeCall *call)
{
    return activeContext && call && call == activeContext->nativeCall;
}

DotKVM *dotkNativeContext(const DotKNativeCall *call)
{
    return validNativeCall(call) ? call->owner : NULL;
}

size_t dotkNativeArgumentCount(const DotKNativeCall *call)
{
    return validNativeCall(call) ? call->count : 0;
}

DotKStatus dotkNativeGetNumber(const DotKNativeCall *call, size_t index, double *result)
{
    if (!validNativeCall(call) || !result || index >= call->count || !IS_NUM(call->arguments[index]))
        return DOTK_INVALID_ARGUMENT;
    *result = AS_NUM(call->arguments[index]);
    return DOTK_OK;
}

DotKStatus dotkNativeGetString(const DotKNativeCall *call, size_t index, const char **bytes, size_t *length)
{
    if (!validNativeCall(call) || !bytes || !length || index >= call->count || !IS_STR(call->arguments[index]))
        return DOTK_INVALID_ARGUMENT;
    *bytes = AS_CSTR(call->arguments[index]);
    *length = (size_t)AS_STR(call->arguments[index])->len;
    return DOTK_OK;
}

DotKStatus dotkNativeReturnNumber(DotKNativeCall *call, double number)
{
    if (!validNativeCall(call))
        return DOTK_INVALID_ARGUMENT;
    *call->result = NUM_VAL(number);
    return DOTK_OK;
}

DotKStatus dotkNativeReturnString(DotKNativeCall *call, const char *bytes, size_t length)
{
    if (!validNativeCall(call) || !bytes || length > INT_MAX)
        return DOTK_INVALID_ARGUMENT;
    if (activeContext->poisoned)
        return DOTK_POISONED;
    jmp_buf escape;
    jmp_buf *previous = activeContext->escape;
    activeContext->escape = &escape;
    DotKStatus status;
    if (setjmp(escape) == 0)
    {
        *call->result = OBJ_VAL(copyString(bytes, (int)length));
        status = DOTK_OK;
    }
    else
        status = DOTK_OUT_OF_MEMORY;
    activeContext->escape = previous;
    return status;
}

DotKStatus dotkNativeReturnHandle(DotKNativeCall *call, const DotKHandle *value)
{
    if (!validNativeCall(call) || !value || value->owner != call->owner)
        return DOTK_INVALID_ARGUMENT;
    *call->result = value->value;
    return DOTK_OK;
}

DotKStatus dotkNativeError(DotKNativeCall *call, const char *message)
{
    if (!validNativeCall(call) || !message)
        return DOTK_INVALID_ARGUMENT;
    call->status = DOTK_RUNTIME_ERROR;
    snprintf(call->error, sizeof(call->error), "%s", message);
    return DOTK_RUNTIME_ERROR;
}

Value vmHostInvokeNative(ObjNative *native, int argumentCount, Value *arguments, bool *hasError)
{
    activeContext->profile.native_calls++;
    HostNativeBinding *binding = (HostNativeBinding *)native->hostBinding;
    if (!binding || binding->owner != activeContext)
    {
        runtimeError("Native callable belongs to another VM");
        *hasError = true;
        return NIL_VAL;
    }
    DotKNativeCall call;
    memset(&call, 0, sizeof(call));
    call.owner = activeContext;
    call.arguments = arguments;
    call.count = (size_t)argumentCount;
    call.result = vm.stackTop;
    push(NIL_VAL);
    activeContext->nativeCall = &call;
    DotKStatus status = binding->function(&call, binding->userdata);
    activeContext->nativeCall = NULL;
    if (activeContext->poisoned)
        vmHostAllocationFailure();
    Value result = pop();
    if (status != DOTK_OK || call.status != DOTK_OK)
    {
        runtimeError("%s", call.error[0] ? call.error : "Host native callback failed");
        *hasError = true;
        return NIL_VAL;
    }
    return result;
}

static DotKStatus registerNative(DotKVM *context, const char *name, DotKHostNativeFn function, void *userdata)
{
    if (!name || !name[0] || !function)
        return DOTK_INVALID_ARGUMENT;
    Value constant;
    if (tableGet(&vm.constGlobals, copyString(name, (int)strlen(name)), &constant))
        return DOTK_INVALID_ARGUMENT;
    HostNativeBinding *binding = (HostNativeBinding *)calloc(1, sizeof(HostNativeBinding));
    if (!binding)
        vmHostAllocationFailure();
    binding->next = context->bindings;
    context->bindings = binding;
    binding->owner = context;
    binding->function = function;
    binding->userdata = userdata;
    size_t length = strlen(name);
    binding->name = (char *)malloc(length + 1);
    if (!binding->name)
        vmHostAllocationFailure();
    memcpy(binding->name, name, length + 1);
    ObjNative *native = newNative(NULL);
    native->hostBinding = binding;
    native->name = binding->name;
    push(OBJ_VAL(native));
    vmDefineGlobalValue(name, OBJ_VAL(native));
    pop();
    return DOTK_OK;
}

static DotKStatus moduleDefineNative(const DotKContextNativeApi *api, const char *name, DotKHostNativeFn function, void *userdata)
{
    if (!activeContext || !activeContext->initializingModule || !api || api->context != activeContext ||
        api->version != DOTK_CONTEXT_NATIVE_API_VERSION || api->size < sizeof(*api))
        return DOTK_INVALID_ARGUMENT;
    if (activeContext->poisoned)
        return DOTK_POISONED;
    jmp_buf escape;
    jmp_buf *previous = activeContext->escape;
    activeContext->escape = &escape;
    DotKStatus status;
    if (setjmp(escape) == 0)
        status = registerNative(activeContext, name, function, userdata);
    else
        status = DOTK_OUT_OF_MEMORY;
    activeContext->escape = previous;
    return status;
}

static void clearDiagnostic(DotKVM *context)
{
    context->message[0] = '\0';
    context->trace[0] = '\0';
    context->file[0] = '\0';
    context->diagnostic.status = DOTK_OK;
    context->diagnostic.message = context->message;
    context->diagnostic.stack_trace = context->trace;
    context->diagnostic.file = context->file;
    context->diagnostic.line = 0;
    context->diagnostic.column = 0;
}

static void setDiagnostic(DotKVM *context, DotKStatus status, const char *message,
                          const char *file, int line, int column)
{
    context->diagnostic.status = status;
    snprintf(context->message, sizeof(context->message), "%s", message);
    snprintf(context->file, sizeof(context->file), "%s", file ? file : "<host>");
    context->diagnostic.line = line;
    context->diagnostic.column = column;
}

void vmHostMarkRoots(void)
{
    if (activeContext)
    {
        for (DotKHandle *handle = activeContext->handles; handle; handle = handle->next)
            markValue(handle->value);
        for (DotKTask *task = activeContext->tasks; task; task = task->next)
        {
            markObj((Obj *)task->generator);
            markValue(task->value);
        }
    }
}

void vmHostWrite(const char *bytes, size_t length)
{
    if (activeContext && activeContext->config.write)
        activeContext->config.write(activeContext->config.userdata, bytes, length);
}

void vmHostCompileError(const char *file, int line, int column, const char *message)
{
    if (activeContext && activeContext->diagnostic.status == DOTK_OK)
        setDiagnostic(activeContext, DOTK_COMPILE_ERROR, message, file, line, column);
}

void vmHostRuntimeError(const char *message, const char *trace, Position position)
{
    DotKStatus status = activeContext->diagnostic.status;
    if (status == DOTK_OK)
        status = DOTK_RUNTIME_ERROR;
    setDiagnostic(activeContext, status, message, position.file, position.line, position.col);
    snprintf(activeContext->trace, sizeof(activeContext->trace), "%s", trace);
}

void *vmHostReallocate(void *pointer, size_t oldSize, size_t newSize)
{
    activeContext->profile.allocation_calls++;
    HostAllocation **link = &activeContext->allocations[allocationBucket(pointer)];
    while (pointer && *link && (*link)->pointer != pointer)
    {
        activeContext->profile.allocation_probes++;
        link = &(*link)->next;
    }
    HostAllocation *allocation = pointer ? *link : NULL;
    if (allocation)
        oldSize = allocation->size;
    if (newSize == 0)
    {
        if (pointer)
        {
            if (activeContext->config.reallocate && (!allocation || !allocation->system))
                activeContext->config.reallocate(activeContext->config.userdata, pointer, oldSize, 0);
            else
                free(pointer);
        }
        if (allocation)
        {
            *link = allocation->next;
            free(allocation);
        }
        return NULL;
    }
    HostAllocation *pending = NULL;
    if (!allocation)
    {
        pending = (HostAllocation *)malloc(sizeof(HostAllocation));
        if (!pending)
            return NULL;
    }
    void *result = activeContext->config.reallocate && (!allocation || !allocation->system) ?
        activeContext->config.reallocate(activeContext->config.userdata, pointer, oldSize, newSize) :
        realloc(pointer, newSize);
    if (!result)
    {
        free(pending);
        return NULL;
    }
    if (!allocation)
    {
        allocation = pending;
        allocation->system = false;
        size_t bucket = allocationBucket(result);
        allocation->next = activeContext->allocations[bucket];
        activeContext->allocations[bucket] = allocation;
    }
    else if (result != pointer)
    {
        *link = allocation->next;
        size_t bucket = allocationBucket(result);
        allocation->next = activeContext->allocations[bucket];
        activeContext->allocations[bucket] = allocation;
    }
    allocation->pointer = result;
    allocation->size = newSize;
    return result;
}

char *vmHostOwnStringBuffer(char *buffer, size_t size)
{
    size_t bucket = allocationBucket(buffer);
    for (HostAllocation *allocation = activeContext->allocations[bucket]; allocation; allocation = allocation->next)
    {
        activeContext->profile.allocation_probes++;
        if (allocation->pointer == buffer)
            return buffer;
    }
    HostAllocation *allocation = (HostAllocation *)malloc(sizeof(HostAllocation));
    if (!allocation)
    {
        free(buffer);
        vmHostAllocationFailure();
    }
    allocation->pointer = buffer;
    allocation->size = 0;
    allocation->system = true;
    allocation->next = activeContext->allocations[bucket];
    activeContext->allocations[bucket] = allocation;
    char *owned = ALLOCATE(char, size);
    memcpy(owned, buffer, size);
    vmHostReallocate(buffer, 0, 0);
    return owned;
}

size_t vmHostHeapLimit(void)
{
    return activeContext->config.heap_limit ? activeContext->config.heap_limit : SIZE_MAX;
}

void vmHostAllocationFailure(void)
{
    if (activeContext && activeContext->recovering)
    {
        activeContext->poisoned = true;
        setDiagnostic(activeContext, DOTK_OUT_OF_MEMORY, "Managed heap limit or allocation failure", NULL, 0, 0);
        longjmp(*activeContext->escape, 1);
    }
    abort();
}

bool vmHostCheckExecution(void)
{
    activeContext->profile.dispatches++;
    if (activeContext->diagnostic.status == DOTK_LIMIT_ERROR ||
        activeContext->diagnostic.status == DOTK_INTERRUPTED)
    {
        runtimeError("%s", activeContext->diagnostic.message);
        return false;
    }
    if (activeContext->config.instruction_limit && activeContext->remaining == 0)
    {
        setDiagnostic(activeContext, DOTK_LIMIT_ERROR, "Instruction budget exceeded", NULL, 0, 0);
        runtimeError("Instruction budget exceeded");
        return false;
    }
    if (activeContext->config.instruction_limit)
        activeContext->remaining--;
    if (activeContext->interrupt_ticks++ % 1024 == 0 && activeContext->config.interrupted &&
        activeContext->config.interrupted(activeContext->config.userdata))
    {
        setDiagnostic(activeContext, DOTK_INTERRUPTED, "Execution interrupted by host", NULL, 0, 0);
        runtimeError("Execution interrupted by host");
        return false;
    }
    return true;
}

void vmHostRecordCollection(void)
{
    activeContext->profile.collections++;
}

char *vmHostResolveModule(const char *name)
{
    if (!activeContext->config.resolve_module)
        return NULL;
    const char *source = activeContext->config.resolve_module(activeContext->config.userdata, name);
    if (!source)
        return NULL;
    size_t length = strlen(source);
    char *copy = (char *)malloc(length + 1);
    if (!copy)
        vmHostAllocationFailure();
    memcpy(copy, source, length + 1);
    return copy;
}

char *vmHostRetainFilename(const char *name)
{
    HostSource *source = (HostSource *)calloc(1, sizeof(HostSource));
    if (!source)
        vmHostAllocationFailure();
    source->next = activeContext->sources;
    activeContext->sources = source;
    size_t length = strlen(name);
    source->file = (char *)malloc(length + 1);
    if (!source->file)
        vmHostAllocationFailure();
    memcpy(source->file, name, length + 1);
    return source->file;
}

bool vmHostNativeAllowed(const char *name)
{
    static const char *allowed[] = {
        "clock", "time", "chr", "exit", "randInt", "rand", "str", "hash", "join", "round",
        "int", "float", "bool", "list", "type", "len", "instanceof", "ord", "split", "splitOnWs",
        "replace", "find", "contains", "lower", "upper", "trim", "trimLeft", "trimRight",
        "isWhitespace", "sin", "cos", "gc", "vmStats", "__dir__", "__vars__", "__del__"};
    for (size_t index = 0; index < sizeof(allowed) / sizeof(allowed[0]); index++)
        if (strcmp(name, allowed[index]) == 0)
            return true;
    return false;
}

void dotkConfigInit(DotKConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->version = DOTK_API_VERSION;
    config->instruction_limit = 1000000;
    config->heap_limit = 64 * 1024 * 1024;
}

static void freeContext(DotKVM *context)
{
    while (context->tasks)
    {
        DotKTask *task = context->tasks;
        context->tasks = task->next;
        free(task);
    }
    while (context->bindings)
    {
        HostNativeBinding *binding = context->bindings;
        context->bindings = binding->next;
        free(binding->name);
        free(binding);
    }
    for (size_t bucket = 0; bucket < HOST_ALLOCATION_BUCKETS; bucket++)
    {
        while (context->allocations[bucket])
        {
            HostAllocation *allocation = context->allocations[bucket];
            context->allocations[bucket] = allocation->next;
            if (context->config.reallocate && !allocation->system)
                context->config.reallocate(context->config.userdata, allocation->pointer, allocation->size, 0);
            else
                free(allocation->pointer);
            free(allocation);
        }
    }
    while (context->handles)
    {
        DotKHandle *handle = context->handles;
        context->handles = handle->next;
        free(handle);
    }
    while (context->sources)
    {
        HostSource *source = context->sources;
        context->sources = source->next;
        free(source->text);
        free(source->file);
        free(source);
    }
    free(context->runtime.compilerState);
    free(context->runtime.scannerState);
    free(context);
}

DotKVM *dotkCreate(const DotKConfig *config)
{
    DotKConfig defaults;
    dotkConfigInit(&defaults);
    if (!config)
        config = &defaults;
    if (config->size < sizeof(DotKConfig) || config->version != DOTK_API_VERSION ||
        pthread_mutex_trylock(&hostMutex) != 0)
        return NULL;
    if (activeContext || vm.mainThread)
    {
        pthread_mutex_unlock(&hostMutex);
        return NULL;
    }
    DotKVM *context = (DotKVM *)calloc(1, sizeof(DotKVM));
    if (!context)
    {
        pthread_mutex_unlock(&hostMutex);
        return NULL;
    }
    context->config = *config;
    context->thread = pthread_self();
    clearDiagnostic(context);
    context->runtime.compilerState = createCompilerState();
    context->runtime.scannerState = createScannerState();
    if (!context->runtime.compilerState || !context->runtime.scannerState ||
        pthread_mutex_init(&context->executionLock, NULL) != 0)
    {
        freeContext(context);
        pthread_mutex_unlock(&hostMutex);
        return NULL;
    }
    activateContext(context);
    context->recovering = true;
    jmp_buf escape;
    context->escape = &escape;
    if (setjmp(escape) == 0)
        initVMForHost();
    context->recovering = false;
    context->escape = NULL;
    if (context->poisoned)
    {
        if (context->config.diagnostic)
            context->config.diagnostic(context->config.userdata, &context->diagnostic);
        freeVM();
        pthread_mutex_destroy(&context->executionLock);
        deactivateContext();
        freeContext(context);
        context = NULL;
    }
    else
    {
        context->next = contexts;
        contexts = context;
        deactivateContext();
    }
    pthread_mutex_unlock(&hostMutex);
    return context;
}

DotKStatus dotkDestroy(DotKVM *context)
{
    if (pthread_mutex_trylock(&hostMutex) != 0)
        return DOTK_BUSY;
    if (!context || !registeredContext(context))
    {
        pthread_mutex_unlock(&hostMutex);
        return DOTK_INVALID_ARGUMENT;
    }
    if (!pthread_equal(context->thread, pthread_self()))
    {
        pthread_mutex_unlock(&hostMutex);
        return DOTK_BUSY;
    }
    activateContext(context);
    freeVM();
    pthread_mutex_destroy(&context->executionLock);
    resetCompilerState();
    DotKVM **link = &contexts;
    while (*link != context)
        link = &(*link)->next;
    *link = context->next;
    deactivateContext();
    freeContext(context);
    pthread_mutex_unlock(&hostMutex);
    return DOTK_OK;
}

const DotKDiagnostic *dotkGetDiagnostic(const DotKVM *context)
{
    return context ? &context->diagnostic : NULL;
}

typedef enum
{
    HOST_EVAL, HOST_NUMBER, HOST_STRING, HOST_GET_GLOBAL, HOST_SET_GLOBAL,
    HOST_GET_NUMBER, HOST_GET_STRING, HOST_CALL, HOST_RELEASE, HOST_COLLECT, HOST_COMPILE,
    HOST_DEFINE_NATIVE, HOST_MODULE_INIT, HOST_SCHEDULE, HOST_PUMP, HOST_TASK_STATE,
    HOST_TASK_VALUE, HOST_TASK_CANCEL, HOST_TASK_RELEASE, HOST_PROFILE
} HostOperation;

typedef struct
{
    HostOperation operation;
    const char *text;
    const char *file;
    size_t length;
    double number;
    const DotKHandle *value;
    const DotKHandle *const *arguments;
    DotKHandle **result;
    double *numberResult;
    const char **stringResult;
    size_t *lengthResult;
    DotKHostNativeFn nativeFunction;
    DotKContextModuleInitFn initializeModule;
    void *userdata;
    DotKTask *task;
    DotKTask **taskResult;
    DotKTaskState *taskState;
    size_t *resumeCount;
    DotKProfileStats *profileResult;
} HostRequest;

static DotKStatus makeHandle(DotKVM *context, Value value, DotKHandle **result)
{
    DotKHandle *handle = (DotKHandle *)malloc(sizeof(DotKHandle));
    if (!handle)
        vmHostAllocationFailure();
    handle->owner = context;
    handle->value = value;
    handle->next = context->handles;
    context->handles = handle;
    *result = handle;
    return DOTK_OK;
}

static DotKStatus executeRequest(DotKVM *context, HostRequest *request)
{
    Value value;
    switch (request->operation)
    {
    case HOST_PROFILE:
        *request->profileResult = context->profile;
        request->profileResult->size = sizeof(DotKProfileStats);
        request->profileResult->heap_bytes = vm.bytesAllocated;
        return DOTK_OK;
    case HOST_SCHEDULE:
    {
        if (!IS_CLOSURE(request->value->value) || AS_CLOSURE(request->value->value)->function->isAsync)
            return DOTK_INVALID_ARGUMENT;
        for (size_t argument = 0; argument < request->length; argument++)
            if (!request->arguments[argument] || request->arguments[argument]->owner != context)
                return DOTK_INVALID_ARGUMENT;
        ObjClosure *closure = AS_CLOSURE(request->value->value);
        if (request->length != (size_t)closure->function->arity)
            return DOTK_INVALID_ARGUMENT;
        DotKTask *task = (DotKTask *)calloc(1, sizeof(DotKTask));
        if (!task)
            vmHostAllocationFailure();
        task->owner = context;
        task->value = NIL_VAL;
        if (context->taskTail)
            context->taskTail->next = task;
        else
            context->tasks = task;
        context->taskTail = task;
        if (!context->nextTask)
            context->nextTask = task;
        Value arguments[UINT8_COUNT];
        for (size_t argument = 0; argument < request->length; argument++)
            arguments[argument] = request->arguments[argument]->value;
        task->generator = newGenerator(closure, arguments, (int)request->length);
        *request->taskResult = task;
        return DOTK_OK;
    }
    case HOST_PUMP:
    {
        *request->resumeCount = 0;
        while (*request->resumeCount < request->length && context->tasks)
        {
            DotKTask *start = context->nextTask ? context->nextTask : context->tasks;
            DotKTask *task = start;
            while (task->state != DOTK_TASK_READY)
            {
                task = task->next ? task->next : context->tasks;
                if (task == start)
                    return DOTK_OK;
            }
            context->nextTask = task->next ? task->next : context->tasks;
            (*request->resumeCount)++;
            context->profile.task_resumes++;
            bool succeeded = vmResumeHostTask(task->generator, &task->value);
            if (!succeeded)
            {
                task->state = DOTK_TASK_ERROR;
                return DOTK_RUNTIME_ERROR;
            }
            if (task->generator->finished)
            {
                task->state = DOTK_TASK_DONE;
                task->generator = NULL;
            }
        }
        return DOTK_OK;
    }
    case HOST_TASK_STATE:
        *request->taskState = request->task->state;
        return DOTK_OK;
    case HOST_TASK_VALUE:
        return makeHandle(context, request->task->value, request->result);
    case HOST_TASK_CANCEL:
        if (request->task->state == DOTK_TASK_READY)
        {
            request->task->state = DOTK_TASK_CANCELLED;
            request->task->generator = NULL;
            request->task->value = NIL_VAL;
        }
        return DOTK_OK;
    case HOST_TASK_RELEASE:
    {
        DotKTask **link = &context->tasks;
        DotKTask *previous = NULL;
        while (*link && *link != request->task)
        {
            previous = *link;
            link = &(*link)->next;
        }
        if (!*link)
            return DOTK_INVALID_ARGUMENT;
        DotKTask *task = *link;
        *link = task->next;
        if (context->taskTail == task)
            context->taskTail = previous;
        if (context->nextTask == task)
            context->nextTask = task->next ? task->next : context->tasks;
        free(task);
        return DOTK_OK;
    }
    case HOST_DEFINE_NATIVE:
        return registerNative(context, request->text, request->nativeFunction, request->userdata);
    case HOST_MODULE_INIT:
    {
        const DotKContextNativeApi api = {
            sizeof(DotKContextNativeApi), DOTK_CONTEXT_NATIVE_API_VERSION, context,
            moduleDefineNative, dotkNativeContext, dotkNativeArgumentCount,
            dotkNativeGetNumber, dotkNativeGetString, dotkNativeReturnNumber,
            dotkNativeReturnString, dotkNativeError};
        context->initializingModule = true;
        DotKStatus status = request->initializeModule(&api, request->userdata);
        context->initializingModule = false;
        return status;
    }
    case HOST_COMPILE:
    case HOST_EVAL:
    {
        HostSource *source = (HostSource *)calloc(1, sizeof(HostSource));
        if (!source)
            vmHostAllocationFailure();
        source->next = context->sources;
        context->sources = source;
        size_t textLength = strlen(request->text);
        size_t fileLength = strlen(request->file);
        source->text = (char *)malloc(textLength + 1);
        source->file = (char *)malloc(fileLength + 1);
        if (!source->text || !source->file)
            vmHostAllocationFailure();
        memcpy(source->text, request->text, textLength + 1);
        memcpy(source->file, request->file, fileLength + 1);
        if (request->operation == HOST_COMPILE)
        {
            vmEnsureArgumentGlobals();
            ObjFunction *function = compile(source->text, source->file, false, false);
            if (!function)
                return DOTK_COMPILE_ERROR;
            push(OBJ_VAL(function));
            ObjClosure *closure = newClosure(function);
            pop();
            return makeHandle(context, OBJ_VAL(closure), request->result);
        }
        InterpretResult result = interpret(source->text, source->file, false, 0, NULL);
        return result == INTERPRET_OK ? DOTK_OK :
               result == INTERPRET_COMPILE_ERROR ? DOTK_COMPILE_ERROR : DOTK_RUNTIME_ERROR;
    }
    case HOST_NUMBER:
        return makeHandle(context, NUM_VAL(request->number), request->result);
    case HOST_STRING:
        return makeHandle(context, OBJ_VAL(copyString(request->text, (int)request->length)), request->result);
    case HOST_GET_GLOBAL:
        if (!tableGet(&vm.globals, copyString(request->text, (int)strlen(request->text)), &value))
            return DOTK_INVALID_ARGUMENT;
        return makeHandle(context, value, request->result);
    case HOST_SET_GLOBAL:
    {
        ObjString *name = copyString(request->text, (int)strlen(request->text));
        Value constant;
        if (tableGet(&vm.constGlobals, name, &constant))
            return DOTK_INVALID_ARGUMENT;
        tableSet(&vm.globals, name, request->value->value);
        return DOTK_OK;
    }
    case HOST_GET_NUMBER:
        if (!IS_NUM(request->value->value))
            return DOTK_INVALID_ARGUMENT;
        *request->numberResult = AS_NUM(request->value->value);
        return DOTK_OK;
    case HOST_GET_STRING:
        if (!IS_STR(request->value->value))
            return DOTK_INVALID_ARGUMENT;
        *request->stringResult = AS_CSTR(request->value->value);
        *request->lengthResult = (size_t)AS_STR(request->value->value)->len;
        return DOTK_OK;
    case HOST_CALL:
        push(request->value->value);
        for (size_t argument = 0; argument < request->length; argument++)
        {
            if (!request->arguments[argument] || request->arguments[argument]->owner != context)
                return DOTK_INVALID_ARGUMENT;
            push(request->arguments[argument]->value);
        }
        if (vmCallFromHost((int)request->length, &value) != INTERPRET_OK)
            return DOTK_RUNTIME_ERROR;
        return makeHandle(context, value, request->result);
    case HOST_RELEASE:
    {
        DotKHandle **link = &context->handles;
        while (*link && *link != request->value)
            link = &(*link)->next;
        if (!*link)
            return DOTK_INVALID_ARGUMENT;
        DotKHandle *handle = *link;
        *link = handle->next;
        free(handle);
        return DOTK_OK;
    }
    case HOST_COLLECT:
        collectGarbage();
        return DOTK_OK;
    }
    return DOTK_INVALID_ARGUMENT;
}

static DotKStatus perform(DotKVM *context, HostRequest *request)
{
    if (request->result)
        *request->result = NULL;
    if (request->taskResult)
        *request->taskResult = NULL;
    if (pthread_mutex_trylock(&hostMutex) != 0)
        return DOTK_BUSY;
    if (!context || !registeredContext(context) || (request->value && request->value->owner != context) ||
        (request->task && request->task->owner != context))
    {
        pthread_mutex_unlock(&hostMutex);
        return DOTK_INVALID_ARGUMENT;
    }
    if (!pthread_equal(context->thread, pthread_self()))
    {
        pthread_mutex_unlock(&hostMutex);
        return DOTK_BUSY;
    }
    if (context->poisoned)
    {
        pthread_mutex_unlock(&hostMutex);
        return DOTK_POISONED;
    }
    activateContext(context);
    bool executing = request->operation == HOST_EVAL || request->operation == HOST_CALL ||
                     request->operation == HOST_COMPILE || request->operation == HOST_MODULE_INIT ||
                     request->operation == HOST_PUMP;
    if (executing)
    {
        clearDiagnostic(context);
        context->remaining = context->config.instruction_limit;
        context->interrupt_ticks = 0;
    }
    context->recovering = true;
    DotKStatus status;
    jmp_buf escape;
    context->escape = &escape;
    if (setjmp(escape) == 0)
        status = executeRequest(context, request);
    else
        status = DOTK_OUT_OF_MEMORY;
    context->recovering = false;
    context->escape = NULL;
    context->nativeCall = NULL;
    context->initializingModule = false;
    if (executing || context->poisoned || status == DOTK_INVALID_ARGUMENT)
        vmResetHostExecution(context->poisoned);
    if (executing && context->diagnostic.status != DOTK_OK)
        status = context->diagnostic.status;
    if ((executing || status == DOTK_OUT_OF_MEMORY) && status != DOTK_OK &&
        context->diagnostic.status != DOTK_OK && context->config.diagnostic)
        context->config.diagnostic(context->config.userdata, &context->diagnostic);
    deactivateContext();
    pthread_mutex_unlock(&hostMutex);
    return status;
}

DotKStatus dotkEval(DotKVM *context, const char *source, const char *file)
{
    if (!source)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_EVAL;
    request.text = source;
    request.file = file ? file : "<host>";
    return perform(context, &request);
}

DotKStatus dotkDefineNative(DotKVM *context, const char *name, DotKHostNativeFn function, void *userdata)
{
    if (!name || !function)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_DEFINE_NATIVE;
    request.text = name;
    request.nativeFunction = function;
    request.userdata = userdata;
    return perform(context, &request);
}

DotKStatus dotkSchedule(DotKVM *context, const DotKHandle *function, size_t argumentCount,
                       const DotKHandle *const *arguments, DotKTask **task)
{
    if (!function || !task || argumentCount > UINT8_MAX || (argumentCount && !arguments))
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_SCHEDULE;
    request.value = function;
    request.length = argumentCount;
    request.arguments = arguments;
    request.taskResult = task;
    return perform(context, &request);
}

DotKStatus dotkPump(DotKVM *context, size_t maximumResumes, size_t *resumed)
{
    if (!resumed)
        return DOTK_INVALID_ARGUMENT;
    *resumed = 0;
    HostRequest request = {0};
    request.operation = HOST_PUMP;
    request.length = maximumResumes;
    request.resumeCount = resumed;
    return perform(context, &request);
}

DotKStatus dotkGetProfileStats(DotKVM *context, DotKProfileStats *stats)
{
    if (!stats || stats->size < sizeof(DotKProfileStats))
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_PROFILE;
    request.profileResult = stats;
    return perform(context, &request);
}

static DotKStatus performTask(const DotKTask *task, HostOperation operation, DotKTaskState *state, DotKHandle **result)
{
    if (!task)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = operation;
    request.task = (DotKTask *)task;
    request.taskState = state;
    request.result = result;
    return perform(task->owner, &request);
}

DotKStatus dotkGetTaskState(const DotKTask *task, DotKTaskState *state)
{
    return state ? performTask(task, HOST_TASK_STATE, state, NULL) : DOTK_INVALID_ARGUMENT;
}

DotKStatus dotkGetTaskValue(const DotKTask *task, DotKHandle **result)
{
    return result ? performTask(task, HOST_TASK_VALUE, NULL, result) : DOTK_INVALID_ARGUMENT;
}

DotKStatus dotkCancelTask(DotKTask *task)
{
    return performTask(task, HOST_TASK_CANCEL, NULL, NULL);
}

DotKStatus dotkReleaseTask(DotKTask *task)
{
    return performTask(task, HOST_TASK_RELEASE, NULL, NULL);
}

DotKStatus dotkLoadNativeModule(DotKVM *context, DotKContextModuleInitFn initialize, void *userdata)
{
    if (!initialize)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_MODULE_INIT;
    request.initializeModule = initialize;
    request.userdata = userdata;
    return perform(context, &request);
}

DotKStatus dotkMakeNumber(DotKVM *context, double number, DotKHandle **result)
{
    if (!result)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_NUMBER;
    request.number = number;
    request.result = result;
    return perform(context, &request);
}

DotKStatus dotkCompile(DotKVM *context, const char *source, const char *file, DotKHandle **program)
{
    if (!source || !program)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_COMPILE;
    request.text = source;
    request.file = file ? file : "<host>";
    request.result = program;
    return perform(context, &request);
}

DotKStatus dotkMakeString(DotKVM *context, const char *bytes, size_t length, DotKHandle **result)
{
    if (!bytes || !result || length > INT_MAX)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_STRING;
    request.text = bytes;
    request.length = length;
    request.result = result;
    return perform(context, &request);
}

DotKStatus dotkGetGlobal(DotKVM *context, const char *name, DotKHandle **result)
{
    if (!name || !result)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_GET_GLOBAL;
    request.text = name;
    request.result = result;
    return perform(context, &request);
}

DotKStatus dotkSetGlobal(DotKVM *context, const char *name, const DotKHandle *value)
{
    if (!name || !value)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_SET_GLOBAL;
    request.text = name;
    request.value = value;
    return perform(context, &request);
}

DotKStatus dotkGetNumber(const DotKHandle *value, double *result)
{
    if (!value || !result)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_GET_NUMBER;
    request.value = value;
    request.numberResult = result;
    return perform(value->owner, &request);
}

DotKStatus dotkGetString(const DotKHandle *value, const char **bytes, size_t *length)
{
    if (!value || !bytes || !length)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_GET_STRING;
    request.value = value;
    request.stringResult = bytes;
    request.lengthResult = length;
    return perform(value->owner, &request);
}

DotKStatus dotkCall(DotKVM *context, const DotKHandle *function, size_t argumentCount,
                    const DotKHandle *const *arguments, DotKHandle **result)
{
    if (!function || !result || argumentCount > UINT8_MAX || (argumentCount && !arguments))
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_CALL;
    request.value = function;
    request.arguments = arguments;
    request.length = argumentCount;
    request.result = result;
    return perform(context, &request);
}

DotKStatus dotkRelease(DotKHandle *value)
{
    if (!value)
        return DOTK_INVALID_ARGUMENT;
    HostRequest request = {0};
    request.operation = HOST_RELEASE;
    request.value = value;
    return perform(value->owner, &request);
}

DotKStatus dotkCollect(DotKVM *context)
{
    HostRequest request = {0};
    request.operation = HOST_COLLECT;
    return perform(context, &request);
}