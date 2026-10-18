#include "dotk.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    DotKVM *context;
    char output[4096];
    size_t length;
    int diagnostics;
    int allocations;
    int fail_after;
    bool interrupted;
    const char *moduleSource;
    double nativeBias;
    int callbackCleanup;
    DotKStatus reentry;
} HostState;

static void writeOutput(void *userdata, const char *bytes, size_t length)
{
    HostState *state = (HostState *)userdata;
    assert(state->length + length < sizeof(state->output));
    memcpy(state->output + state->length, bytes, length);
    state->length += length;
    state->output[state->length] = '\0';
    if (state->context)
        state->reentry = dotkEval(state->context, "print(999)", "reentry.k");
}

static void reportDiagnostic(void *userdata, const DotKDiagnostic *diagnostic)
{
    HostState *state = (HostState *)userdata;
    assert(diagnostic->status != DOTK_OK);
    assert(diagnostic->message && diagnostic->file);
    state->diagnostics++;
}

static bool interrupted(void *userdata)
{
    return ((HostState *)userdata)->interrupted;
}

static const char *resolveModule(void *userdata, const char *name)
{
    if (strcmp(name, "host_math") == 0)
    {
        HostState *state = (HostState *)userdata;
        return state->moduleSource ? state->moduleSource : "export const answer = 42\n";
    }
    return NULL;
}

static void *allocateMemory(void *userdata, void *pointer, size_t oldSize, size_t newSize)
{
    (void)oldSize;
    HostState *state = (HostState *)userdata;
    if (newSize == 0)
    {
        if (pointer)
            state->allocations--;
        free(pointer);
        return NULL;
    }
    if (state->fail_after == 0)
        return NULL;
    if (state->fail_after > 0)
        state->fail_after--;
    void *result = realloc(pointer, newSize);
    if (result && !pointer)
        state->allocations++;
    return result;
}

static void hostSignal(int signalNumber)
{
    (void)signalNumber;
}

static DotKStatus hostAdd(DotKNativeCall *call, void *userdata)
{
    HostState *state = (HostState *)userdata;
    double argument = 0;
    assert(dotkNativeContext(call) == state->context);
    if (dotkNativeArgumentCount(call) != 1 || dotkNativeGetNumber(call, 0, &argument) != DOTK_OK)
        return dotkNativeError(call, "host_add requires one number");
    return dotkNativeReturnNumber(call, argument + state->nativeBias);
}

static DotKStatus hostEcho(DotKNativeCall *call, void *userdata)
{
    (void)userdata;
    const char *bytes = NULL;
    size_t length = 0;
    if (dotkNativeGetString(call, 0, &bytes, &length) != DOTK_OK)
        return dotkNativeError(call, "host_echo requires a string");
    return dotkNativeReturnString(call, bytes, length);
}

static DotKStatus initializeContextModule(const DotKContextNativeApi *api, void *userdata)
{
    assert(api->version == DOTK_CONTEXT_NATIVE_API_VERSION);
    assert(api->size >= sizeof(*api));
    return api->define_native(api, "host_add", hostAdd, userdata);
}

static DotKStatus nativeAllocationFailure(DotKNativeCall *call, void *userdata)
{
    HostState *state = (HostState *)userdata;
    state->fail_after = 1;
    DotKStatus status = dotkNativeReturnString(call, "native-allocation-failure", 25);
    state->callbackCleanup++;
    return status;
}

static void *wrongThread(void *userdata)
{
    HostState *state = (HostState *)userdata;
    assert(dotkEval(state->context, "print(0)", "thread.k") == DOTK_BUSY);
    return NULL;
}

int main(void)
{
    HostState state;
    memset(&state, 0, sizeof(state));
    state.fail_after = -1;
    DotKConfig config;
    dotkConfigInit(&config);
    config.userdata = &state;
    config.write = writeOutput;
    config.diagnostic = reportDiagnostic;
    config.resolve_module = resolveModule;
    config.interrupted = interrupted;
    config.instruction_limit = 1000;
    void (*previousSignal)(int) = signal(SIGINT, hostSignal);
    state.context = dotkCreate(&config);
    assert(state.context);
    assert(signal(SIGINT, hostSignal) == hostSignal);
    HostState otherState;
    memset(&otherState, 0, sizeof(otherState));
    otherState.fail_after = -1;
    otherState.moduleSource = "export const answer = 84\n";
    DotKConfig otherConfig = config;
    otherConfig.userdata = &otherState;
    otherState.context = dotkCreate(&otherConfig);
    assert(otherState.context);
    state.nativeBias = 100;
    otherState.nativeBias = 200;
    assert(dotkLoadNativeModule(state.context, initializeContextModule, &state) == DOTK_OK);
    assert(dotkLoadNativeModule(otherState.context, initializeContextModule, &otherState) == DOTK_OK);
    assert(dotkDefineNative(state.context, "host_echo", hostEcho, &state) == DOTK_OK);
    assert(dotkEval(state.context, "var native_result = host_add(1)\nvar native_string = host_echo('kept')", "native-first.k") == DOTK_OK);
    assert(dotkEval(otherState.context, "var native_result = host_add(1)", "native-second.k") == DOTK_OK);
    assert(dotkEval(state.context, "var isolated = 11", "first.k") == DOTK_OK);
    assert(dotkEval(otherState.context, "var isolated = 22\nfrom host_math import answer", "second.k") == DOTK_OK);
    DotKHandle *firstValue = NULL;
    DotKHandle *secondValue = NULL;
    double isolatedValue = 0;
    assert(dotkGetGlobal(state.context, "native_result", &firstValue) == DOTK_OK);
    assert(dotkGetNumber(firstValue, &isolatedValue) == DOTK_OK && isolatedValue == 101);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkGetGlobal(otherState.context, "native_result", &secondValue) == DOTK_OK);
    assert(dotkGetNumber(secondValue, &isolatedValue) == DOTK_OK && isolatedValue == 201);
    assert(dotkRelease(secondValue) == DOTK_OK);
    assert(dotkEval(state.context, "host_add('bad')", "native-error.k") == DOTK_RUNTIME_ERROR);
    assert(strcmp(dotkGetDiagnostic(state.context)->message, "host_add requires one number") == 0);
    assert(dotkGetGlobal(state.context, "isolated", &firstValue) == DOTK_OK);
    assert(dotkGetGlobal(otherState.context, "isolated", &secondValue) == DOTK_OK);
    assert(dotkSetGlobal(otherState.context, "foreign_value", firstValue) == DOTK_INVALID_ARGUMENT);
    assert(dotkCollect(state.context) == DOTK_OK);
    assert(dotkCollect(otherState.context) == DOTK_OK);
    assert(dotkGetNumber(firstValue, &isolatedValue) == DOTK_OK && isolatedValue == 11);
    assert(dotkGetNumber(secondValue, &isolatedValue) == DOTK_OK && isolatedValue == 22);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkRelease(secondValue) == DOTK_OK);
    assert(dotkEval(state.context, "fn steps() { yield 10\nyield 20 }", "steps.k") == DOTK_OK);
    assert(dotkGetGlobal(state.context, "steps", &firstValue) == DOTK_OK);
    DotKTask *task = NULL;
    DotKTask *cancelledTask = NULL;
    DotKTaskState taskState;
    size_t resumed = 0;
    assert(dotkSchedule(state.context, firstValue, 0, NULL, &task) == DOTK_OK);
    assert(dotkSchedule(state.context, firstValue, 0, NULL, &cancelledTask) == DOTK_OK);
    assert(dotkCancelTask(cancelledTask) == DOTK_OK);
    assert(dotkGetTaskState(cancelledTask, &taskState) == DOTK_OK && taskState == DOTK_TASK_CANCELLED);
    assert(dotkPump(otherState.context, 1, &resumed) == DOTK_OK && resumed == 0);
    assert(dotkCollect(state.context) == DOTK_OK);
    assert(dotkPump(state.context, 1, &resumed) == DOTK_OK && resumed == 1);
    assert(dotkGetTaskValue(task, &secondValue) == DOTK_OK);
    assert(dotkGetNumber(secondValue, &isolatedValue) == DOTK_OK && isolatedValue == 10);
    assert(dotkRelease(secondValue) == DOTK_OK);
    assert(dotkEval(otherState.context, "gc()", "between-frames.k") == DOTK_OK);
    assert(dotkPump(state.context, 1, &resumed) == DOTK_OK && resumed == 1);
    assert(dotkGetTaskValue(task, &secondValue) == DOTK_OK);
    assert(dotkGetNumber(secondValue, &isolatedValue) == DOTK_OK && isolatedValue == 20);
    assert(dotkRelease(secondValue) == DOTK_OK);
    assert(dotkPump(state.context, 1, &resumed) == DOTK_OK && resumed == 1);
    assert(dotkGetTaskState(task, &taskState) == DOTK_OK && taskState == DOTK_TASK_DONE);
    assert(dotkReleaseTask(task) == DOTK_OK);
    assert(dotkReleaseTask(cancelledTask) == DOTK_OK);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkGetGlobal(otherState.context, "answer", &secondValue) == DOTK_OK);
    assert(dotkGetNumber(secondValue, &isolatedValue) == DOTK_OK && isolatedValue == 84);
    assert(dotkRelease(secondValue) == DOTK_OK);
    DotKProfileStats firstStats;
    DotKProfileStats secondStats;
    firstStats.size = sizeof(firstStats);
    secondStats.size = sizeof(secondStats);
    assert(dotkGetProfileStats(state.context, &firstStats) == DOTK_OK);
    assert(dotkGetProfileStats(otherState.context, &secondStats) == DOTK_OK);
    assert(firstStats.task_resumes == 3 && secondStats.task_resumes == 0);
    assert(firstStats.native_calls > 0 && firstStats.dispatches > 0);
    assert(dotkEval(state.context, "fn firstTask() { yield 1\nyield 3 }\nfn secondTask() { yield 2\nyield 4 }\nfn taskError() { missing() }\nfn noYield() { while (true) {} }", "task-policies.k") == DOTK_OK);
    DotKTask *firstTask = NULL;
    DotKTask *secondTask = NULL;
    assert(dotkGetGlobal(state.context, "firstTask", &firstValue) == DOTK_OK);
    assert(dotkSchedule(state.context, firstValue, 0, NULL, &firstTask) == DOTK_OK);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkGetGlobal(state.context, "secondTask", &firstValue) == DOTK_OK);
    assert(dotkSchedule(state.context, firstValue, 0, NULL, &secondTask) == DOTK_OK);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkPump(state.context, 2, &resumed) == DOTK_OK && resumed == 2);
    assert(dotkGetTaskValue(firstTask, &firstValue) == DOTK_OK);
    assert(dotkGetNumber(firstValue, &isolatedValue) == DOTK_OK && isolatedValue == 1);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkGetTaskValue(secondTask, &firstValue) == DOTK_OK);
    assert(dotkGetNumber(firstValue, &isolatedValue) == DOTK_OK && isolatedValue == 2);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkCollect(state.context) == DOTK_OK);
    assert(dotkPump(state.context, 2, &resumed) == DOTK_OK && resumed == 2);
    assert(dotkGetTaskValue(firstTask, &firstValue) == DOTK_OK);
    assert(dotkGetNumber(firstValue, &isolatedValue) == DOTK_OK && isolatedValue == 3);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkGetTaskValue(secondTask, &firstValue) == DOTK_OK);
    assert(dotkGetNumber(firstValue, &isolatedValue) == DOTK_OK && isolatedValue == 4);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkReleaseTask(firstTask) == DOTK_OK);
    assert(dotkReleaseTask(secondTask) == DOTK_OK);
    assert(dotkGetGlobal(state.context, "taskError", &firstValue) == DOTK_OK);
    assert(dotkSchedule(state.context, firstValue, 0, NULL, &task) == DOTK_OK);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkPump(state.context, 1, &resumed) == DOTK_RUNTIME_ERROR && resumed == 1);
    assert(dotkGetTaskState(task, &taskState) == DOTK_OK && taskState == DOTK_TASK_ERROR);
    assert(dotkReleaseTask(task) == DOTK_OK);
    assert(dotkGetGlobal(state.context, "noYield", &firstValue) == DOTK_OK);
    assert(dotkSchedule(state.context, firstValue, 0, NULL, &task) == DOTK_OK);
    assert(dotkRelease(firstValue) == DOTK_OK);
    assert(dotkPump(state.context, 1, &resumed) == DOTK_LIMIT_ERROR);
    assert(dotkGetTaskState(task, &taskState) == DOTK_OK && taskState == DOTK_TASK_ERROR);
    assert(dotkReleaseTask(task) == DOTK_OK);
    assert(dotkEval(otherState.context, "host_add(2)", "after-task-error.k") == DOTK_OK);
    assert(dotkDestroy(otherState.context) == DOTK_OK);
    otherState.context = NULL;
    DotKHandle *initialProgram = NULL;
    DotKHandle *initialResult = NULL;
    DotKHandle *argumentGlobal = NULL;
    double argumentCount = -1;
    assert(dotkCompile(state.context, "var initialArgumentCount = ARG_C", "initial.k", &initialProgram) == DOTK_OK);
    assert(dotkCall(state.context, initialProgram, 0, NULL, &initialResult) == DOTK_OK);
    assert(dotkGetGlobal(state.context, "initialArgumentCount", &argumentGlobal) == DOTK_OK);
    assert(dotkGetNumber(argumentGlobal, &argumentCount) == DOTK_OK && argumentCount == 0);
    assert(dotkRelease(initialProgram) == DOTK_OK);
    assert(dotkRelease(initialResult) == DOTK_OK);
    assert(dotkRelease(argumentGlobal) == DOTK_OK);
    assert(dotkEval(state.context, "print(42)", "hello.k") == DOTK_OK);
    assert(strcmp(state.output, "42\n") == 0);
    assert(state.reentry == DOTK_BUSY);

    assert(dotkEval(state.context, "var =", "syntax.k") == DOTK_COMPILE_ERROR);
    const DotKDiagnostic *diagnostic = dotkGetDiagnostic(state.context);
    assert(diagnostic->line == 1 && diagnostic->column > 0);
    assert(strcmp(diagnostic->file, "syntax.k") == 0);
    assert(dotkEval(state.context, "missing()", "runtime.k") == DOTK_RUNTIME_ERROR);
    assert(strcmp(dotkGetDiagnostic(state.context)->file, "runtime.k") == 0);
    assert(dotkEval(state.context, "exit(0)", "exit.k") == DOTK_RUNTIME_ERROR);
    assert(dotkEval(state.context, "open('/tmp/no-write', 'w')", "io.k") == DOTK_RUNTIME_ERROR);
    assert(dotkEval(state.context, "import 'file'", "module.k") == DOTK_RUNTIME_ERROR);
    assert(dotkEval(state.context, "async fn job() { return 1 }\njob()", "async.k") == DOTK_RUNTIME_ERROR);
    assert(dotkEval(state.context, "Thread.currentThread()", "thread-denied.k") == DOTK_RUNTIME_ERROR);

    DotKHandle *number = NULL;
    DotKHandle *function = NULL;
    DotKHandle *result = NULL;
    assert(dotkMakeNumber(state.context, 41, &number) == DOTK_OK);
    assert(dotkSetGlobal(state.context, "host_value", number) == DOTK_OK);
    char source[] = "fn increment(value) { return value + 1 }";
    char fileName[] = "owned.k";
    assert(dotkEval(state.context, source, fileName) == DOTK_OK);
    memset(source, 0, sizeof(source));
    memset(fileName, 0, sizeof(fileName));
    assert(dotkGetGlobal(state.context, "increment", &function) == DOTK_OK);
    assert(dotkCollect(state.context) == DOTK_OK);
    const DotKHandle *arguments[] = {number};
    assert(dotkCall(state.context, function, 1, arguments, &result) == DOTK_OK);
    double outputNumber = 0;
    assert(dotkGetNumber(result, &outputNumber) == DOTK_OK && outputNumber == 42);
    assert(dotkRelease(result) == DOTK_OK);
    assert(dotkCall(state.context, function, 0, NULL, &result) == DOTK_RUNTIME_ERROR);
    assert(strcmp(dotkGetDiagnostic(state.context)->file, "<host>") == 0);

    DotKHandle *text = NULL;
    assert(dotkMakeString(state.context, "a\0b", 3, &text) == DOTK_OK);
    assert(dotkCollect(state.context) == DOTK_OK);
    const char *bytes = NULL;
    size_t length = 0;
    assert(dotkGetString(text, &bytes, &length) == DOTK_OK);
    assert(length == 3 && memcmp(bytes, "a\0b", 3) == 0);
    assert(dotkRelease(text) == DOTK_OK);
    assert(dotkRelease(number) == DOTK_OK);
    assert(dotkRelease(function) == DOTK_OK);

    DotKHandle *program = NULL;
    size_t beforeCompile = state.length;
    assert(dotkCompile(state.context, "print(77)", "compiled.k", &program) == DOTK_OK);
    assert(state.length == beforeCompile);
    assert(dotkCollect(state.context) == DOTK_OK);
    assert(dotkCall(state.context, program, 0, NULL, &result) == DOTK_OK);
    assert(dotkRelease(result) == DOTK_OK);
    assert(dotkCall(state.context, program, 0, NULL, &result) == DOTK_OK);
    assert(dotkRelease(result) == DOTK_OK);
    assert(strcmp(state.output + beforeCompile, "77\n77\n") == 0);
    assert(dotkRelease(program) == DOTK_OK);

    assert(dotkEval(state.context, "from host_math import answer\nprint(answer)", "import.k") == DOTK_OK);
    assert(dotkEval(state.context, "fn nestedImport() { from host_math import answer\nreturn answer }", "nested.k") == DOTK_OK);
    assert(dotkGetGlobal(state.context, "nestedImport", &function) == DOTK_OK);
    assert(dotkCall(state.context, function, 0, NULL, &result) == DOTK_OK);
    assert(dotkGetNumber(result, &outputNumber) == DOTK_OK && outputNumber == 42);
    assert(dotkRelease(result) == DOTK_OK);
    assert(dotkRelease(function) == DOTK_OK);
    assert(dotkEval(state.context, "while (true) {}", "budget.k") == DOTK_LIMIT_ERROR);
    size_t beforeBudgetCatch = state.length;
    assert(dotkEval(state.context, "try { while (true) {} } catch (error) { print('continued') }", "budget-catch.k") == DOTK_LIMIT_ERROR);
    assert(state.length == beforeBudgetCatch);
    assert(dotkEval(state.context, "print(host_value)", "recovery.k") == DOTK_OK);
    state.interrupted = true;
    assert(dotkEval(state.context, "print(0)", "interrupt.k") == DOTK_INTERRUPTED);
    state.interrupted = false;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, wrongThread, &state) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(dotkDestroy(state.context) == DOTK_OK);
    state.context = NULL;
    signal(SIGINT, previousSignal);

    for (int iteration = 0; iteration < 3; iteration++)
    {
        state.context = dotkCreate(&config);
        assert(state.context);
        assert(dotkEval(state.context, "gc()\nvar keep = 'alive'", "repeat.k") == DOTK_OK);
        assert(dotkDestroy(state.context) == DOTK_OK);
        state.context = NULL;
    }

    config.reallocate = allocateMemory;
    state.context = dotkCreate(&config);
    assert(state.context);
    assert(dotkEval(state.context, "print(1)", "allocator.k") == DOTK_OK);
    assert(dotkEval(state.context, "var converted = 'ABC'.lower()\ngc()\nprint(converted)", "native-buffer.k") == DOTK_OK);
    assert(dotkEval(state.context, "var replaced = replace('ABC', 'A', 'Z')\ngc()\nprint(replaced)", "raw-buffer.k") == DOTK_OK);
    state.fail_after = 1;
    assert(dotkMakeString(state.context, "allocation-failure", 18, &text) == DOTK_OUT_OF_MEMORY);
    assert(dotkEval(state.context, "print(1)", "poisoned.k") == DOTK_POISONED);
    assert(dotkDestroy(state.context) == DOTK_OK);
    state.context = NULL;
    assert(state.allocations == 0);
    state.fail_after = -1;
    state.context = dotkCreate(&config);
    assert(state.context);
    assert(dotkDefineNative(state.context, "native_oom", nativeAllocationFailure, &state) == DOTK_OK);
    assert(dotkEval(state.context, "native_oom()", "native-oom.k") == DOTK_OUT_OF_MEMORY);
    assert(state.callbackCleanup == 1);
    assert(dotkDestroy(state.context) == DOTK_OK);
    state.context = NULL;
    assert(state.allocations == 0);
    state.fail_after = -1;
    config.heap_limit = 32;
    assert(dotkCreate(&config) == NULL);
    assert(state.allocations == 0);
    config.heap_limit = 64 * 1024 * 1024;
    for (int failure = 0; failure < 160; failure++)
    {
        state.fail_after = failure;
        state.context = dotkCreate(&config);
        if (state.context)
        {
            assert(dotkDestroy(state.context) == DOTK_OK);
            state.context = NULL;
        }
        assert(state.allocations == 0);
    }
    puts("embedding host: PASS");
    return 0;
}