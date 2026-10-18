#include "include/builtin_module.h"
#include "include/vm.h"

#include <string.h>

#define BUILTIN_MODULE_MAX 128

typedef struct
{
    const char *name;
    BuiltinModuleInitFn initFn;
} BuiltinModuleEntry;

static BuiltinModuleEntry modules[BUILTIN_MODULE_MAX];
static int moduleCount = 0;
static bool initialized = false;

void registerBuiltinIOModule();
void registerBuiltinPrimitiveClassesModule();
void registerBuiltinFileClassModule();
void registerBuiltinSQLClassModule();
void registerBuiltinCoreModule();
void registerBuiltinHttpModule();

static void registerDefaultBuiltinModules()
{
    registerBuiltinPrimitiveClassesModule();
    registerBuiltinFileClassModule();
    registerBuiltinSQLClassModule();
    registerBuiltinCoreModule();
    registerBuiltinIOModule();
    registerBuiltinHttpModule();
}

void registerBuiltinModule(const char *name, BuiltinModuleInitFn initFn)
{
    if (name == NULL || initFn == NULL)
        return;

    for (int i = 0; i < moduleCount; i++)
    {
        if (strcmp(modules[i].name, name) == 0)
            return;
    }

    if (moduleCount >= BUILTIN_MODULE_MAX)
        return;

    modules[moduleCount].name = name;
    modules[moduleCount].initFn = initFn;
    vm.builtinModulesLoaded[moduleCount] = false;
    moduleCount++;
}

void initBuiltinModules()
{
    if (!initialized)
    {
        registerDefaultBuiltinModules();
        initialized = true;
    }

    for (int i = 0; i < moduleCount; i++)
        vm.builtinModulesLoaded[i] = false;
}

bool importBuiltinModule(ObjString *requestedName)
{
    if (requestedName == NULL)
        return false;

    const char *name = requestedName->chars;
    int nameLen = requestedName->len;

    for (int i = 0; i < moduleCount; i++)
    {
        if ((int)strlen(modules[i].name) == nameLen && strncmp(modules[i].name, name, nameLen) == 0)
        {
            if (!vm.builtinModulesLoaded[i])
            {
                modules[i].initFn();
                vm.builtinModulesLoaded[i] = true;
            }
            return true;
        }
    }

    if (nameLen > 2 && name[nameLen - 2] == '.' && name[nameLen - 1] == 'k')
    {
        int trimmedLen = nameLen - 2;
        for (int i = 0; i < moduleCount; i++)
        {
            if ((int)strlen(modules[i].name) == trimmedLen && strncmp(modules[i].name, name, trimmedLen) == 0)
            {
                if (!vm.builtinModulesLoaded[i])
                {
                    modules[i].initFn();
                    vm.builtinModulesLoaded[i] = true;
                }
                return true;
            }
        }
    }

    return false;
}
