#ifndef dotk_vm_h
#define dotk_vm_h

#include "chunk.h"
#include "io.h"
#include "object.h"
#include "table.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define BUFFER_SIZE 104855
#define STR_BUFF BUFFER_SIZE
#define FRAMES_MAX 64 * 2 * 2
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)
#define NATIVE_FN(fn) static Value fn(int argc, Value *argv, bool *hasError, bool *pushedValue)

typedef struct _CallFrame
{
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
    uint64_t startTimeNs;
} CallFrame;

/* Per-thread execution state for the GVL-based threading model. */
typedef enum {
    THREAD_CREATED,
    THREAD_RUNNING,
    THREAD_FINISHED,
    THREAD_ERROR,
} DotKThreadStatus;

typedef struct DotKThread
{
    pthread_t handle;
    DotKThreadStatus status;

    /* Per-thread VM state (swapped into global vm when running) */
    CallFrame frames[FRAMES_MAX];
    int frameCount;
    Value stack[STACK_MAX];
    Value *stackTop;
    ObjUpvalue *openUpvalues;
    bool isInTryCatch;

    /* Initial callable + args */
    ObjClosure *closure;
    Value *args;
    int argCount;

    /* Result after completion */
    Value result;
    ObjString *errorMsg;

    /* Synchronization */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} DotKThread;

typedef struct _VM
{
    /* Threading — every execution context lives in a DotKThread.
       mainThread is allocated once in initVM().
       currentThread always points to whoever is active. */
    DotKThread *mainThread;
    DotKThread *currentThread;
    CallFrame frames[FRAMES_MAX];
    int frameCount;
    Value stack[STACK_MAX];
    Value *stackTop;
    Table strings;
    Table globals;
    Table constGlobals;
    Table imports;
    Table importFuncs;
    ObjNamespace *currentModuleExports;
    ObjNamespace *currentLoadingModule;
    bool currentModuleHasExplicitExports;
    ObjString *initStr;
    ObjString *toStr;
    ObjString *strDunderStr;
    ObjString *eqStr;
    ObjString *eqDunderStr;
    ObjString *ltStr;
    ObjString *ltDunderStr;
    ObjString *gtStr;
    ObjString *gtDunderStr;
    ObjString *indexStr;
    ObjString *getitemDunderStr;
    ObjString *setStr;
    ObjString *setitemDunderStr;
    ObjString *sizeStr;
    ObjString *lenDunderStr;
    ObjString *hashStr;
    ObjString *hashDunderStr;
    ObjString *iterStr;
    ObjString *iterDunderStr;
    ObjString *nextStr;
    ObjString *nextDunderStr;
    ObjString *clazzStr;
    ObjClass *stringClass;
    ObjClass *listClass;
    ObjClass *mapClass;
    ObjClass *generatorClass;
    ObjClass *listIteratorClass;
    ObjClass *mapIteratorClass;
    ObjClass *stringIteratorClass;
    ObjClass *errorClass;
    ObjClass *fileClass;
    ObjClass *sqlClass;
    ObjClass *threadClass;
    ObjClass *baseObj;
    ObjUpvalue *openUpvalues;
    uint8_t nextWideOp;

    /* Profiler for language-level timings */
    struct ProfilerEntry
    {
        ObjFunction *function;
        size_t callCount;
        uint64_t totalNs;
    } *profilerEntries;
    int profilerEntryCount;
    int profilerEntryCapacity;
    bool profilerEnabled;
    /* Call-graph profiler entries (caller -> callee) */
    struct CallGraphEntry
    {
        ObjFunction *caller;
        ObjFunction *callee;
        size_t callCount;
        uint64_t totalNs;
    } *callGraphEntries;
    int callGraphEntryCount;
    int callGraphEntryCapacity;

    size_t bytesAllocated;
    size_t nextGC;
    size_t maxHeapSize;
    Obj *objects;
    int grayCount;
    int grayCapacity;
    Obj **grayStack;
    int importCount;
    char **importSources;
    bool gcDisabled;
    bool isInTryCatch;
    bool isRepl;
    ObjString *lastError;
    ObjString *lastErrorTrace;
    /* Monomorphic inline cache for method dispatch */
#define IC_SIZE 1024
    struct InlineCacheEntry {
        uint8_t *ip;        /* bytecode IP that produced this entry */
        ObjClass *klass;    /* cached class pointer */
        Value method;       /* cached method value */
    } inlineCache[IC_SIZE];
} VM;

typedef enum
{
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

void initVM(bool printBytecode, bool printExecStack);
void freeVM();

InterpretResult interpret(const char *source, char *file, bool printExpressions, int argC, char **argV);
extern VM vm;
void push(Value value);
Value pop();
Value popN(int n);
void rotateStack();
Value peek(int distance);
void runtimeError(const char *format, ...);
ObjClass *getVmClass(Value val);
// static void defineNative(const char *name, NativeFn function);

typedef void (*DefineNativeFn)(const char *name, NativeFn function);
typedef ObjClass *(*DefineNativeClassFn)(char *name);

void vmDefineNative(const char *name, NativeFn function);
ObjClass *vmDefineClass(const char *name);
void vmDefineGlobalValue(const char *name, Value value);
ObjClass *vmGetClassByName(const char *name);
void vmDefineClassMethod(ObjClass *clazz, const char *name, NativeFn function);
void vmDefineClassStaticMethod(ObjClass *clazz, const char *name, NativeFn function);
void vmEnableDebugger(bool enabled);

/* Blocking I/O helpers: save thread state + release GVL, reacquire + restore.
   Must be used in matched pairs around any blocking syscall in native functions.
   Returns an opaque handle that must be passed to vmEndBlockingIO(). */
DotKThread *vmBeginBlockingIO(void);
void vmEndBlockingIO(DotKThread *saved);

// This is so I can use the equal override in classes for the builtin Map class
void markMap(Map *map);
void mapRemoveWhite(Map *map);
#endif