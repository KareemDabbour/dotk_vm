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
#include <sys/stat.h>

#define BUFFER_SIZE 104857
#define STR_BUFF BUFFER_SIZE
#define FRAMES_MAX 64 * 2 * 2 * 2 * 2
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct _CallFrame
{
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
} CallFrame;

typedef struct _VM
{
    CallFrame frames[FRAMES_MAX];
    int frameCount;
    Value stack[STACK_MAX];
    Value *stackTop;
    Table strings;
    Table globals;
    Table imports;
    Table importFuncs;
    ObjString *initStr;
    ObjString *toStr;
    ObjString *eqStr;
    ObjString *ltStr;
    ObjString *gtStr;
    ObjString *indexStr;
    ObjString *setStr;
    ObjString *sizeStr;
    ObjString *hashStr;
    ObjString *clazzStr;
    ObjClass *stringClass;
    ObjClass *listClass;
    ObjClass *mapClass;
    ObjClass *errorClass;
    ObjUpvalue *openUpvalues;
    uint8_t nextWideOp;

    size_t bytesAllocated;
    size_t nextGC;
    Obj *objects;
    int grayCount;
    int grayCapacity;
    Obj **grayStack;
    int importCount;
    char **importSources;
    bool isInTryCatch;
    bool isRepl;
    ObjString *lastError;
    ObjString *lastErrorTrace;
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
void rotateStack();

// This is so I can use the equal override in classes for the builtin Map class
void markMap(Map *map);
void mapRemoveWhite(Map *map);
#endif