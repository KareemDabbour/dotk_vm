#ifndef dotk_builtin_module_h
#define dotk_builtin_module_h

#include "object.h"

typedef void (*BuiltinModuleInitFn)(void);

void registerBuiltinModule(const char *name, BuiltinModuleInitFn initFn);
void initBuiltinModules();
bool importBuiltinModule(ObjString *requestedName);

#endif
