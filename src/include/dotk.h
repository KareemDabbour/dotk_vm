#ifndef DOTK_H
#define DOTK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOTK_API_VERSION 1

#if defined(__GNUC__) || defined(__clang__)
#define DOTK_API __attribute__((visibility("default")))
#else
#define DOTK_API
#endif

typedef struct DotKVM DotKVM;
typedef struct DotKHandle DotKHandle;
typedef struct DotKNativeCall DotKNativeCall;
typedef struct DotKContextNativeApi DotKContextNativeApi;
typedef struct DotKTask DotKTask;

typedef enum
{
    DOTK_TASK_READY,
    DOTK_TASK_DONE,
    DOTK_TASK_ERROR,
    DOTK_TASK_CANCELLED
} DotKTaskState;

typedef struct
{
    size_t size;
    uint64_t dispatches;
    uint64_t native_calls;
    uint64_t task_resumes;
    uint64_t collections;
    uint64_t allocation_calls;
    uint64_t allocation_probes;
    size_t heap_bytes;
} DotKProfileStats;

typedef enum
{
    DOTK_OK,
    DOTK_COMPILE_ERROR,
    DOTK_RUNTIME_ERROR,
    DOTK_INVALID_ARGUMENT,
    DOTK_BUSY,
    DOTK_OUT_OF_MEMORY,
    DOTK_LIMIT_ERROR,
    DOTK_INTERRUPTED,
    DOTK_POISONED
} DotKStatus;

typedef DotKStatus (*DotKHostNativeFn)(DotKNativeCall *call, void *userdata);
typedef DotKStatus (*DotKContextModuleInitFn)(const DotKContextNativeApi *api, void *userdata);

#define DOTK_CONTEXT_NATIVE_API_VERSION 3

struct DotKContextNativeApi
{
    size_t size;
    unsigned version;
    DotKVM *context;
    DotKStatus (*define_native)(const DotKContextNativeApi *api, const char *name, DotKHostNativeFn function, void *userdata);
    DotKVM *(*context_of)(const DotKNativeCall *call);
    size_t (*argument_count)(const DotKNativeCall *call);
    DotKStatus (*get_number)(const DotKNativeCall *call, size_t index, double *result);
    DotKStatus (*get_string)(const DotKNativeCall *call, size_t index, const char **bytes, size_t *length);
    DotKStatus (*return_number)(DotKNativeCall *call, double number);
    DotKStatus (*return_string)(DotKNativeCall *call, const char *bytes, size_t length);
    DotKStatus (*raise_error)(DotKNativeCall *call, const char *message);
};

typedef struct
{
    DotKStatus status;
    const char *message;
    const char *stack_trace;
    const char *file;
    int line;
    int column;
} DotKDiagnostic;

typedef void (*DotKWriteFn)(void *userdata, const char *bytes, size_t length);
typedef void (*DotKDiagnosticFn)(void *userdata, const DotKDiagnostic *diagnostic);
typedef void *(*DotKReallocateFn)(void *userdata, void *pointer, size_t old_size, size_t new_size);
typedef bool (*DotKInterruptFn)(void *userdata);
typedef const char *(*DotKResolveModuleFn)(void *userdata, const char *name);

typedef struct
{
    size_t size;
    unsigned version;
    void *userdata;
    DotKWriteFn write;
    DotKDiagnosticFn diagnostic;
    DotKReallocateFn reallocate;
    DotKInterruptFn interrupted;
    DotKResolveModuleFn resolve_module;
    uint64_t instruction_limit;
    size_t heap_limit;
} DotKConfig;

DOTK_API void dotkConfigInit(DotKConfig *config);
DOTK_API DotKVM *dotkCreate(const DotKConfig *config);
DOTK_API DotKStatus dotkDestroy(DotKVM *context);
DOTK_API DotKStatus dotkEval(DotKVM *context, const char *source, const char *file);
DOTK_API DotKStatus dotkCompile(DotKVM *context, const char *source, const char *file, DotKHandle **program);
DOTK_API const DotKDiagnostic *dotkGetDiagnostic(const DotKVM *context);
DOTK_API DotKStatus dotkMakeNumber(DotKVM *context, double number, DotKHandle **result);
DOTK_API DotKStatus dotkMakeString(DotKVM *context, const char *bytes, size_t length, DotKHandle **result);
DOTK_API DotKStatus dotkGetGlobal(DotKVM *context, const char *name, DotKHandle **result);
DOTK_API DotKStatus dotkSetGlobal(DotKVM *context, const char *name, const DotKHandle *value);
DOTK_API DotKStatus dotkGetNumber(const DotKHandle *value, double *result);
DOTK_API DotKStatus dotkGetString(const DotKHandle *value, const char **bytes, size_t *length);
DOTK_API DotKStatus dotkCall(DotKVM *context, const DotKHandle *function, size_t argument_count,
                    const DotKHandle *const *arguments, DotKHandle **result);
DOTK_API DotKStatus dotkRelease(DotKHandle *value);
DOTK_API DotKStatus dotkCollect(DotKVM *context);
DOTK_API DotKStatus dotkDefineNative(DotKVM *context, const char *name, DotKHostNativeFn function, void *userdata);
DOTK_API DotKStatus dotkLoadNativeModule(DotKVM *context, DotKContextModuleInitFn initialize, void *userdata);
DOTK_API DotKVM *dotkNativeContext(const DotKNativeCall *call);
DOTK_API size_t dotkNativeArgumentCount(const DotKNativeCall *call);
DOTK_API DotKStatus dotkNativeGetNumber(const DotKNativeCall *call, size_t index, double *result);
DOTK_API DotKStatus dotkNativeGetString(const DotKNativeCall *call, size_t index, const char **bytes, size_t *length);
DOTK_API DotKStatus dotkNativeReturnNumber(DotKNativeCall *call, double number);
DOTK_API DotKStatus dotkNativeReturnString(DotKNativeCall *call, const char *bytes, size_t length);
DOTK_API DotKStatus dotkNativeReturnHandle(DotKNativeCall *call, const DotKHandle *value);
DOTK_API DotKStatus dotkNativeError(DotKNativeCall *call, const char *message);
DOTK_API DotKStatus dotkSchedule(DotKVM *context, const DotKHandle *function, size_t argument_count,
                                const DotKHandle *const *arguments, DotKTask **task);
DOTK_API DotKStatus dotkPump(DotKVM *context, size_t maximum_resumes, size_t *resumed);
DOTK_API DotKStatus dotkGetTaskState(const DotKTask *task, DotKTaskState *state);
DOTK_API DotKStatus dotkGetTaskValue(const DotKTask *task, DotKHandle **result);
DOTK_API DotKStatus dotkCancelTask(DotKTask *task);
DOTK_API DotKStatus dotkReleaseTask(DotKTask *task);
DOTK_API DotKStatus dotkGetProfileStats(DotKVM *context, DotKProfileStats *stats);

#ifdef __cplusplus
}
#endif

#endif