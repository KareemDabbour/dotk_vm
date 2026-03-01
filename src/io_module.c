#include "include/builtin_module.h"
#include "include/io.h"
#include "include/vm.h"

#include <string.h>

NATIVE_FN(ioReadTextNative)
{
    if (argc != 1)
    {
        runtimeError("'io_read_text(path)' expects 1 argument but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_STR(argv[0]))
    {
        runtimeError("'io_read_text(path)' expects a string path but got '%s'", valueTypeName(argv[0]));
        *hasError = true;
        return NIL_VAL;
    }

    char *source = readFile(AS_CSTR(argv[0]));
    if (source == NULL)
    {
        runtimeError("Failed to read file '%s'", AS_CSTR(argv[0]));
        *hasError = true;
        return NIL_VAL;
    }

    return OBJ_VAL(takeString(source, (int)strlen(source)));
}

NATIVE_FN(ioWriteTextNative)
{
    if (argc != 2)
    {
        runtimeError("'io_write_text(path, content)' expects 2 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_STR(argv[0]) || !IS_STR(argv[1]))
    {
        runtimeError("'io_write_text(path, content)' expects two string arguments");
        *hasError = true;
        return NIL_VAL;
    }

    FILE *f = fopen(AS_CSTR(argv[0]), "wb");
    if (f == NULL)
    {
        runtimeError("Failed to open file '%s'", AS_CSTR(argv[0]));
        *hasError = true;
        return NIL_VAL;
    }

    const char *content = AS_CSTR(argv[1]);
    size_t contentLen = (size_t)AS_STR(argv[1])->len;
    size_t written = fwrite(content, 1, contentLen, f);
    fclose(f);

    if (written != contentLen)
    {
        runtimeError("Failed to write full content to '%s'", AS_CSTR(argv[0]));
        *hasError = true;
        return NIL_VAL;
    }

    return BOOL_VAL(true);
}

static void initIOModule()
{
    vmDefineNative("io_read_text", ioReadTextNative);
    vmDefineNative("io_write_text", ioWriteTextNative);
}

void registerBuiltinIOModule()
{
    registerBuiltinModule("io", initIOModule);
}
