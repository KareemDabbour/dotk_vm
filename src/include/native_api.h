#ifndef dotk_native_api_h
#define dotk_native_api_h

#include "object.h"

#define DOTK_NATIVE_API_VERSION 2

typedef struct DotKNativeApi
{
    int version;
    void (*defineNative)(const char *name, NativeFn function);
    ObjClass *(*defineClass)(const char *name);
    void (*defineGlobalValue)(const char *name, Value value);
    ObjClass *(*getClassByName)(const char *name);
    void (*defineClassMethod)(ObjClass *clazz, const char *name, NativeFn function);
    void (*defineClassStaticMethod)(ObjClass *clazz, const char *name, NativeFn function);
    Value (*makeString)(const char *chars, int len, bool interned);
    Value (*makeList)(void);
    bool (*listAppend)(Value list, Value item);
    Value (*makeMap)(void);
    bool (*mapSet)(Value map, Value key, Value value);
    Value (*makeForeign)(ForeignType type, void *ptr, bool ownsPtr);
    Value (*makeForeignWithClass)(ObjClass *klass, void *ptr, bool ownsPtr);
    void (*pushValue)(Value value);
    Value (*popValue)(void);
    void (*raiseError)(const char *format, ...);
    const char *(*valueTypeName)(Value value);
    bool (*setTableValue)(Table *table, ObjString *key, Value value);
    void (*setNativeMethod)(Table *table, const char *name, NativeFn fn);
} DotKNativeApi;

typedef bool (*DotKInitModuleFn)(const DotKNativeApi *api);

#endif
