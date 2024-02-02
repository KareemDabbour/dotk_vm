#include "include/vm.h"
#include "include/compiler.h"
#include "include/debug.h"
#include "include/io.h"
#include "include/memory.h"
#include "include/object.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
VM vm;
Value prefixStr = NIL_VAL;
FILE *file;

static bool invoke(ObjString *name, int argc);
static FILE *getFile()
{
    if (file == NULL)
    {
        file = fopen("test.k", "rb");
    }
    return file;
}

static void resetStack()
{
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
}

static void runtimeError(const char *format, ...)
{
    va_list args;
    Position pos;
    for (int i = 0; i < vm.frameCount; i++)
    {
        CallFrame *frame = &vm.frames[i];
        ObjFunction *function = frame->closure->function;
        size_t inst = frame->ip - function->chunk.code - 1;
        pos = getPos(&function->chunk, inst);
        bool printFunc = true;
        if (strcmp(function->name->chars, pos.file) == 0)
        {
            printFunc = false;
            fprintf(stderr, "Error in:\n");
        }

        fprintf(stderr, "  %s:%d:%d", pos.file, pos.line, pos.col);
        if (function->name == NULL)
            fprintf(stderr, " in script");
        else if (printFunc)
            fprintf(stderr, " in %s()", function->name->chars);
        fputs("\n", stderr);
    }
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputs("\n", stderr);
    int lineLen = 0;
    while (pos.lineStart[lineLen] != '\n' && pos.lineStart[lineLen] != '\0')
    {
        fputc('~', stderr);
        lineLen++;
    }
    fprintf(stderr, "~\n%.*s", lineLen, pos.lineStart);
    fputs("\n", stderr);
    for (int i = 0; i < pos.col - 1; i++)
        fputc('-', stderr);
    fputs("^\n", stderr);
    for (int i = 0; i <= lineLen; i++)
        fputc('~', stderr);
    va_end(args);
    fputs("\n", stderr);
    resetStack();
}

static Value clockNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    return NUM_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value getcNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    int i = argc == 1 ? AS_NUM(argv[0]) : 0;
    return NUM_VAL(getc(stdin));
}

static Value kbhitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    return BOOL_VAL(_kbhit());
}

static bool isWide()
{
    if (vm.nextWideOp == 1)
    {
        vm.nextWideOp--;
        return true;
    }
    return false;
}

static inline Value peek(int dist)
{
    return vm.stackTop[-1 - dist];
}

static Value instanceOf(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'instanceof' expects 2 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(argv[0]))
    {
        return BOOL_VAL(false);
    }
    if (!IS_CLASS(argv[1]))
    {
        runtimeError("'instanceof' expects a class as second argument but got '%s'", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjInstance *instance = AS_INSTANCE(argv[0]);
    ObjClass *klass = AS_CLASS(argv[1]);
    ObjString *className = klass->name;
    ObjClass *tempClass = instance->klass;
    while (tempClass != NULL)
    {
        if (tempClass->name == className)
            return BOOL_VAL(true);
        tempClass = tempClass->superclass;
    }
    return BOOL_VAL(false);
}

static Value typeOf(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    Value val = argc == 0 ? NIL_VAL : argv[0];
    char *type;
    switch (val.type)
    {
    case VAL_NUMBER:
        type = val.as.number == roundf(val.as.number) ? "int" : "float";
        break;
    case VAL_BOOL:
        type = "bool";
        break;
    case VAL_NIL:
        type = "null";
        break;
    case VAL_OBJ:
        switch (AS_OBJ(val)->type)
        {
        case OBJ_STRING:
            type = "string";
            break;
        case OBJ_LIST:
            type = "list";
            break;
        case OBJ_CLASS:
        case OBJ_INSTANCE:
            type = "object";
            break;
            // type = AS_CLASS(val)->name->chars;
            // break;
            // type = AS_INSTANCE(val)->klass->name->chars;
            // break;
        case OBJ_SLICE:
            type = "slice";
            break;
        case OBJ_NATIVE:
        case OBJ_FUNCTION:
        case OBJ_CLOSURE:
        case OBJ_BOUND_METHOD:
            type = "function";
            break;
        case OBJ_UPVALUE:
            type = "upvalue";
            break;
        default:
            type = "unknown";
            break;
        }
    }

    return OBJ_VAL(copyString(type, (int)strlen(type)));
}

static Value sinNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'sin()' expects one argument but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("'sin()' expects a number as argument but got '%s'", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL(sin(AS_NUM(argv[0])));
}

static Value cosNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'cos()' expects one argument but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("'cos()' expects a number as argument but got '%s'", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL(cos(AS_NUM(argv[0])));
}

static Value lenNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 1)
    {
        runtimeError("'len()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (IS_STR(argv[0]))
        return NUM_VAL((double)AS_STR(argv[0])->len);
    if (IS_LIST(argv[0]))
        return NUM_VAL((double)AS_LIST(argv[0])->count);
    if (IS_INSTANCE(argv[0]) && !IS_NIL(AS_INSTANCE(argv[0])->klass->sizeFn))
    {
        vm.stackTop -= 2;
        push(argv[0]);
        if (!invoke(vm.sizeStr, 0))
        {
            *hasError = true;
            return NIL_VAL;
        }
        *pushedValue = true;
        return NIL_VAL;
    }
    runtimeError("'len()' expects a string or list as argument but got '%s'", VALUE_TYPES[argv[0].type]);
    *hasError = true;
    return NIL_VAL;
}

static Value exitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    disableRawMode();
    if (argc != 1)
        exit(0);

    exit((int)AS_NUM(argv[0]));
}

static Value sleepNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 1)
    {
        runtimeError("'sleep()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("'sleep()' expects a number as argument but got '%s'", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (AS_NUM(argv[0]) < 0)
    {
        runtimeError("'sleep()' expects a positive number as argument but got '%g'", AS_NUM(argv[0]));
        *hasError = true;
        return NIL_VAL;
    }
    if (roundf(AS_NUM(argv[0])) == AS_NUM(argv[0]))
        sleep(AS_NUM(argv[0]));
    else
        usleep(AS_NUM(argv[0]) * 1000000);
    return NIL_VAL;
}

static Value clearNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 0)
    {
        runtimeError("'clear()' expects no arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    clrscr();
    return NIL_VAL;
}

static char *valueToString(Value val, char *buff, int *len)
{
    switch (val.type)
    {
    case VAL_NUMBER:
    {
        sprintf(buff, "%.15g", AS_NUM(val));
        *len = (int)strlen(buff);
        return buff;
    }
    case VAL_BOOL:
        sprintf(buff, "%s", AS_BOOL(val) ? "true" : "false");
        *len = (int)strlen(buff);
        return buff;
    case VAL_NIL:
        sprintf(buff, "%s", "null");
        *len = (int)strlen(buff);
        return buff;
    case VAL_OBJ:
        switch (AS_OBJ(val)->type)
        {
        case OBJ_STRING:
            sprintf(buff, "%s", AS_STR(val)->chars);
            *len = (int)strlen(buff);
            return buff;
        case OBJ_LIST:
        {
            ObjList *list = AS_LIST(val);
            int index = 0;
            buff[index++] = '[';
            for (int i = 0; i < list->count; i++)
            {
                char *item = valueToString(list->items[i], buff + index, len);
                index += *len;
                buff[index++] = ',';
            }
            if (list->count > 0)
                buff[--index] = ']';
            else
                buff[index] = ']';
            buff[++index] = '\0';
            *len = index;
            return buff;
        }
        case OBJ_CLASS:
            sprintf(buff, "%s", AS_CLASS(val)->name->chars);
            *len = (int)strlen(buff);
            return buff;
        case OBJ_INSTANCE:
        {
            sprintf(buff, "%s<%p>", AS_INSTANCE(val)->klass->name->chars, (void *)AS_INSTANCE(val));
            *len = (int)strlen(buff);
            return buff;
        }
        case OBJ_FUNCTION:
            sprintf(buff, "%s", AS_FUN(val)->name == NULL ? "<script>" : AS_FUN(val)->name->chars);
            *len = (int)strlen(buff);
            return buff;
        case OBJ_NATIVE:
            sprintf(buff, "%s", "<native>");
            *len = (int)strlen(buff);
            return buff;
        case OBJ_CLOSURE:
            sprintf(buff, "%s", AS_CLOSURE(val)->function->name == NULL ? "<script>" : AS_CLOSURE(val)->function->name->chars);
            *len = (int)strlen(buff);
            return buff;
        case OBJ_BOUND_METHOD:
            sprintf(buff, "%s", AS_BOUND_METHOD(val)->method->function->name == NULL ? "<script>" : AS_BOUND_METHOD(val)->method->function->name->chars);
            *len = (int)strlen(buff);
            return buff;
        case OBJ_UPVALUE:
            sprintf(buff, "%s", "<upvalue>");
            *len = (int)strlen(buff);
            return buff;
        }
    }
    sprintf(buff, "%s", "<unknown>");
    *len = (int)strlen(buff);
    return buff;
}

static Value strCastNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 1)
    {
        runtimeError("'str()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (argv[0].type == VAL_OBJ && AS_OBJ(argv[0])->type == OBJ_STRING)
        return argv[0];
    int strLen = 0;
    char str[10000] = {0};
    valueToString(argv[0], str, &strLen);

    return OBJ_VAL(copyString(str, (int)strlen(str)));
}

static Value hashNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 1)
    {
        runtimeError("'hash()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (IS_INSTANCE(argv[0]) && !IS_NIL(AS_INSTANCE(argv[0])->klass->hashFn))
    {
        vm.stackTop -= 2;
        push(argv[0]);
        if (!invoke(vm.hashStr, 0))
        {
            *hasError = true;
            return NIL_VAL;
        }
        *pushedValue = true;
        return NIL_VAL;
    }
    ObjString *str;
    if (!IS_STR(argv[0]))
    {
        char s[10000] = {0};
        int len = 0;
        valueToString(argv[0], s, &len);
        str = copyString(s, len);
    }
    else
        str = AS_STR(argv[0]);
    return NUM_VAL((double)str->hash);
}

static Value joinNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 2)
    {
        runtimeError("'join()' expects two arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(argv[0]))
    {
        runtimeError("'join()' expects a list as first argument but got '%s'", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("'join()' expects a string as second argument but got '%s'", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(argv[0]);
    ObjString *sep = AS_STR(argv[1]);
    char *str = ALLOCATE(char, 100);
    sprintf(str, "%s", "");
    for (int i = 0; i < list->count; i++)
    {
        int len = 0;
        char item[10000] = {0};
        valueToString(list->items[i], item, &len);
        char *temp = ALLOCATE(char, strlen(str) + len + strlen(sep->chars) + 1);
        sprintf(temp, "%s%s%s", str, item, sep->chars);
        FREE_ARRAY(char, str, strlen(str) + 1);
        // FREE_ARRAY(char, item, strlen(item) + 1);
        str = temp;
    }
    if (list->count > 0)
        str[strlen(str) - strlen(sep->chars)] = '\0';
    return OBJ_VAL(takeString(str, (int)strlen(str)));
}

static Value intCastNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'int()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (argv[0].type == VAL_OBJ && AS_OBJ(argv[0])->type == OBJ_STRING)
    {
        char *str = AS_STR(argv[0])->chars;
        char *end;
        long int num = strtol(str, &end, 10);
        if (end == str)
        {
            runtimeError("Expected a number but got '%s' for int()", str);
            *hasError = true;
            return NIL_VAL;
        }
        return NUM_VAL((double)num);
    }
    if (argv[0].type == VAL_NUMBER)
        return NUM_VAL(roundf(AS_NUM(argv[0])));
    if (argv[0].type == VAL_BOOL)
        return NUM_VAL(AS_BOOL(argv[0]) ? 1 : 0);
    if (argv[0].type == VAL_NIL)
        return NUM_VAL(0);
    runtimeError("Expected a string or number but got '%s' for int()", VALUE_TYPES[argv[0].type]);
    *hasError = true;
    return NIL_VAL;
}
static Value floatCastNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'float()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (argv[0].type == VAL_OBJ && AS_OBJ(argv[0])->type == OBJ_STRING)
    {
        char *str = AS_STR(argv[0])->chars;
        char *end;
        double num = strtod(str, &end);
        if (end == str)
        {
            runtimeError("Expected a number but got '%s' for float()", str);
            *hasError = true;
            return NIL_VAL;
        }
        return NUM_VAL(num);
    }
    if (argv[0].type == VAL_NUMBER)
        return argv[0];
    if (argv[0].type == VAL_BOOL)
        return NUM_VAL(AS_BOOL(argv[0]) ? 1 : 0);
    if (argv[0].type == VAL_NIL)
        return NUM_VAL(0);
    runtimeError("Expected a string or number but got '%s' for float()", VALUE_TYPES[argv[0].type]);
    *hasError = true;
    return NIL_VAL;
}

static Value boolCastNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'bool()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (argv[0].type == VAL_OBJ && AS_OBJ(argv[0])->type == OBJ_STRING)
    {
        char *str = AS_STR(argv[0])->chars;
        if (strcmp(str, "true") == 0)
            return BOOL_VAL(true);
        if (strcmp(str, "false") == 0)
            return BOOL_VAL(false);
        return BOOL_VAL(AS_STR(argv[0])->len);
    }
    if (argv[0].type == VAL_NUMBER)
        return BOOL_VAL(AS_NUM(argv[0]) != 0);
    if (argv[0].type == VAL_BOOL)
        return argv[0];
    if (argv[0].type == VAL_NIL)
        return BOOL_VAL(false);
    runtimeError("Expected a string or number but got '%s' for bool()", VALUE_TYPES[argv[0].type]);
    *hasError = true;
    return NIL_VAL;
}

static Value listCastNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'list()' expects one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (argv[0].type == VAL_OBJ && AS_OBJ(argv[0])->type == OBJ_LIST)
        return argv[0];
    if (argv[0].type == VAL_OBJ && AS_OBJ(argv[0])->type == OBJ_STRING)
    {
        ObjList *list = newList();
        for (int i = 0; i < AS_STR(argv[0])->len; i++)
        {
            char *chr = ALLOCATE(char, 2);
            chr[0] = AS_STR(argv[0])->chars[i];
            chr[1] = '\0';
            // char *s = AS_STR(argv[0])->chars + i;
            appendToList(list, OBJ_VAL(takeString(chr, 1)));
        }
        return OBJ_VAL(list);
    }
    if (argv[0].type == VAL_NIL)
        return OBJ_VAL(newList());
    runtimeError("Expected a string or list but got '%s' for list()", VALUE_TYPES[argv[0].type]);
    *hasError = true;
    return NIL_VAL;
}

static Value randomIntNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 2)
    {
        runtimeError("'randInt()' expects two arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("'randInt()' expects a number as first argument but got '%s'", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[1]))
    {
        runtimeError("'randInt()' expects a number as second argument but got '%s'", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (AS_NUM(argv[0]) > AS_NUM(argv[1]))
    {
        runtimeError("'randInt()' expects the first argument to be smaller than the second argument but got '%g' and '%g'", AS_NUM(argv[0]), AS_NUM(argv[1]));
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL(rand() % (int)(AS_NUM(argv[1]) - AS_NUM(argv[0]) + 1) + AS_NUM(argv[0]));
}

static Value randNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 0)
    {
        runtimeError("'rand()' expects no arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL((double)rand() / RAND_MAX);
}

static Value printErrNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    (*hasError) = true;
    if (argc != 1)
    {
        runtimeError("'print_error()' expects one argument %d were passed in", argc);
        return NIL_VAL;
    }
    runtimeError(AS_CSTR(argv[0]));
    return argv[0];
}

static Value chrNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for chr(ch)", argc);
        *hasError = true;
        return NIL_VAL;
    }
    int num = (int)AS_NUM(argv[0]);

    char *chr = ALLOCATE(char, 2);
    chr[0] = (char)num;
    chr[1] = '\0';
    return OBJ_VAL(takeString(chr, 1));
}

static Value ordNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for ord(ch)", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for ord(ch)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjString *str = AS_STR(argv[0]);
    if (str->len != 1)
    {
        runtimeError("Expected a string of length 1 but got '%d' for ord(ch)", str->len);
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL((double)str->chars[0]);
}

static Value socketNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    // if (argc != 2)
    // {
    //     runtimeError("Expected 2 arguments but %d passed in for socket(host, port)", argc);
    //     *hasError = true;
    //     return NIL_VAL;
    // }
    // if (!IS_NUM(argv[0]))
    // {
    //     runtimeError("Expected a string but got '%s' for socket(host, port)", VALUE_TYPES[argv[0].type]);
    //     *hasError = true;
    //     return NIL_VAL;
    // }
    // if (!IS_NUM(argv[1]))
    // {
    //     runtimeError("Expected a number but got '%s' for socket(host, port)", VALUE_TYPES[argv[1].type]);
    //     *hasError = true;
    //     return NIL_VAL;
    // }
    // if (!IS_NUM(argv[2]))
    // {
    //     runtimeError("Expected a number but got '%s' for socket(host, port)", VALUE_TYPES[argv[1].type]);
    //     *hasError = true;
    //     return NIL_VAL;
    // }
    // char *host = AS_CSTR(argv[0]);
    // int port = (int)AS_NUM(argv[1]);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        runtimeError("Failed to create socket");
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL(sock);
}

static Value closeFDNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for close(socket)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for close(socket)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int sock = (int)AS_NUM(argv[0]);
    if (close(sock) < 0)
        return BOOL_VAL(false);

    return BOOL_VAL(true);
}

static Value setSockOptionsNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 3)
    {
        runtimeError("Expected 3 arguments but %d passed in for setSockOptions(socket, level, optname)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for setSockOptions(socket, level, optname)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[1]))
    {
        runtimeError("Expected a number but got '%s' for setSockOptions(socket, level, optname)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[2]))
    {
        runtimeError("Expected a number but got '%s' for setSockOptions(socket, level, optname)", VALUE_TYPES[argv[2].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int sock = (int)AS_NUM(argv[0]);
    int level = (int)AS_NUM(argv[1]);
    int optname = (int)AS_NUM(argv[2]);
    int optval = 1;
    if (setsockopt(sock, level, optname, &optval, sizeof(optval)) < 0)
    {
        runtimeError("Failed to set socket options");
        *hasError = true;
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

static Value connectNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 3)
    {
        runtimeError("Expected 3 arguments but %d passed in for connect(socket, host, port)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for connect(socket, host, port)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for connect(socket, host, port)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[2]))
    {
        runtimeError("Expected a number but got '%s' for connect(socket, host, port)", VALUE_TYPES[argv[2].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }

    int sock = (int)AS_NUM(argv[0]);
    char *host = AS_CSTR(argv[1]);
    int port = (int)AS_NUM(argv[2]);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, NULL, &hints, &res) != 0)
    {
        runtimeError("Failed to get address info for host '%s'", host);
        *hasError = true;
        return BOOL_VAL(false);
    }

    struct sockaddr_in *serv_addr = (struct sockaddr_in *)res->ai_addr;
    // serv_addr.sin_family = AF_INET;

    serv_addr->sin_port = htons(port);

    // if (inet_pton(AF_INET, host, &serv_addr.sin_addr) != 1)
    // {
    //     runtimeError("Invalid address %s Address not supported", host);
    //     *hasError = true;
    //     return BOOL_VAL(false);
    // }

    if (connect(sock, (struct sockaddr *)serv_addr, sizeof(*serv_addr)) < 0)
    {
        runtimeError("Failed to connect to host '%s' on port '%d'", host, port);
        *hasError = true;
        return BOOL_VAL(false);
    }
    freeaddrinfo(res);
    return BOOL_VAL(true);
}

static Value bindSocketPortNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("Expected 2 arguments but %d passed in for bindSocketPort(socket, port)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for bindSocketPort(socket, port)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[1]))
    {
        runtimeError("Expected a number but got '%s' for bindSocketPort(socket, port)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int sock = (int)AS_NUM(argv[0]);
    int port = (int)AS_NUM(argv[1]);
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        runtimeError("Failed to bind socket to port '%d'", port);
        *hasError = true;
        return BOOL_VAL(false);
    }

    return BOOL_VAL(true);
}

static Value listenNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for listen(socket)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for listen(socket)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int sock = (int)AS_NUM(argv[0]);
    if (listen(sock, 3) < 0)
    {
        runtimeError("Failed to listen on socket");
        *hasError = true;
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

static Value acceptNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("Expected 2 argument but %d passed in for accept(socket, port)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for accept(socket, port)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[1]))
    {
        runtimeError("Expected a number but got '%s' for accept(socket, port)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int sock = (int)AS_NUM(argv[0]);
    int port = (int)AS_NUM(argv[1]);
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    socklen_t addrlen = sizeof(address);
    int newSock = accept(sock, (struct sockaddr *)&address, &addrlen);
    if (newSock < 0)
    {
        runtimeError("Failed to accept connection");
        *hasError = true;
        return BOOL_VAL(false);
    }
    return NUM_VAL(newSock);
}

static Value readNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for read(socket)", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for read(socket)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    int sock = (int)AS_NUM(argv[0]);
    char buffer[BUFFER_SIZE] = {0};
    // int bytesRead;
    // while ((bytesRead = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0)
    // {
    //     buffer[bytesRead] = '\0';
    // }
    int valread = read(sock, buffer, BUFFER_SIZE);
    if (valread < 0)
    {
        runtimeError("Failed to read data");
        *hasError = true;
        return NIL_VAL;
    }
    return OBJ_VAL(copyString(buffer, valread));
}

static Value readSizeNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("Expected 2 argument but %d passed in for readSize(socket, size)", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for readSize(socket, size)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[1]))
    {
        runtimeError("Expected a number but got '%s' for readSize(socket, size)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    int sock = (int)AS_NUM(argv[0]);
    int size = (int)AS_NUM(argv[1]);
    char buffer[BUFFER_SIZE] = {0};
    int valread = read(sock, buffer, size);
    if (valread < 0)
    {
        runtimeError("Failed to read data");
        *hasError = true;
        return NIL_VAL;
    }
    return OBJ_VAL(copyString(buffer, valread));
}

static Value seekNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 3)
    {
        runtimeError("Expected 3 argument but %d passed in for seek(file, offset, whence)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for seek(file, offset, whence)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[1]))
    {
        runtimeError("Expected a number but got '%s' for seek(file, offset, whence)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[2]))
    {
        runtimeError("Expected a number but got '%s' for seek(file, offset, whence)", VALUE_TYPES[argv[2].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int file = (int)AS_NUM(argv[0]);
    int offset = (int)AS_NUM(argv[1]);
    int whence = (int)AS_NUM(argv[2]);
    if (lseek(file, offset, whence) < 0)
    {
        runtimeError("Failed to seek");
        *hasError = true;
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

static Value writeNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("Expected 2 argument but %d passed in for write(file, data)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for write(file, data)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for write(file, data)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int file = (int)AS_NUM(argv[0]);
    char *data = AS_CSTR(argv[1]);
    int dataLen = strlen(data);
    if (write(file, data, dataLen) != dataLen)
    {
        runtimeError("Failed to write data");
        *hasError = true;
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

static Value fileExistsNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for fileExists(path)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for fileExists(path)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    char *path = AS_CSTR(argv[0]);
    struct stat buffer;
    return BOOL_VAL(stat(path, &buffer) == 0);
}

static Value fileSizeNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for fileSize(path)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for fileSize(path)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    char *path = AS_CSTR(argv[0]);
    struct stat buffer;
    if (stat(path, &buffer) != 0)
    {
        runtimeError("Failed to get file size");
        *hasError = true;
        return BOOL_VAL(false);
    }
    return NUM_VAL((double)buffer.st_size);
}

static Value sendNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("Expected 2 argument but %d passed in for send(socket, data)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for send(socket, data)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for send(socket, data)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    int sock = (int)AS_NUM(argv[0]);
    char *data = AS_CSTR(argv[1]);
    int dataLen = strlen(data);
    if (send(sock, data, dataLen, 0) != dataLen)
    {
        runtimeError("Failed to send data");
        *hasError = true;
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

const char *get_mime_type(char *file_ext)
{
    if (strcasecmp(file_ext, "html") == 0 || strcasecmp(file_ext, "htm") == 0)
        return "text/html";
    else if (strcasecmp(file_ext, "txt") == 0)
        return "text/plain";
    else if (strcasecmp(file_ext, "jpg") == 0 || strcasecmp(file_ext, "jpeg") == 0)
        return "image/jpeg";
    else if (strcasecmp(file_ext, "png") == 0)
        return "image/png";
    else if (strcasecmp(file_ext, "css") == 0)
        return "text/css";
    else if (strcasecmp(file_ext, "js") == 0)
        return "application/javascript";
    else if (strcasecmp(file_ext, "json") == 0)
        return "application/json";
    else if (strcasecmp(file_ext, "pdf") == 0)
        return "application/pdf";
    else if (strcasecmp(file_ext, "xml") == 0)
        return "application/xml";
    else if (strcasecmp(file_ext, "zip") == 0)
        return "application/zip";
    else if (strcasecmp(file_ext, "gz") == 0)
        return "application/gzip";
    else if (strcasecmp(file_ext, "tar") == 0)
        return "application/x-tar";
    else if (strcasecmp(file_ext, "mp3") == 0)
        return "audio/mpeg";
    else if (strcasecmp(file_ext, "wav") == 0)
        return "audio/wav";
    else if (strcasecmp(file_ext, "ogg") == 0)
        return "audio/ogg";
    else if (strcasecmp(file_ext, "mid") == 0 || strcasecmp(file_ext, "midi") == 0)
        return "audio/midi";
    else if (strcasecmp(file_ext, "mp4") == 0)
        return "video/mp4";
    else if (strcasecmp(file_ext, "webm") == 0)
        return "video/webm";
    else if (strcasecmp(file_ext, "avi") == 0)
        return "video/x-msvideo";
    else if (strcasecmp(file_ext, "mpeg") == 0)
        return "video/mpeg";
    else if (strcasecmp(file_ext, "ico") == 0)
        return "image/x-icon";
    else if (strcasecmp(file_ext, "svg") == 0)
        return "image/svg+xml";
    else if (strcasecmp(file_ext, "gif") == 0)
        return "image/gif";

    else
        return "application/octet-stream";
}

static Value sendFileWithFileDescriptorNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("Expected 2 argument but %d passed in for sendFileWithFileDescriptor(socket, path)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for sendFileWithFileDescriptor(socket, path)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for sendFileWithFileDescriptor(socket, path)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return BOOL_VAL(false);
    }
    char *path = AS_CSTR(argv[1]);
    char *file_ext = strrchr(path, '.') + 1;

    char *header = (char *)malloc(BUFFER_SIZE * sizeof(char));
    snprintf(header, BUFFER_SIZE,
             "HTTP/2.0 200 OK\r\n"
             "Content-Type: %s \r\n"
             "\r\n",
             get_mime_type(file_ext));
    char response[BUFFER_SIZE];
    int response_len = 0;
    memcpy(response, header, strlen(header));
    response_len += strlen(header);

    int sock = (int)AS_NUM(argv[0]);

    int file = open(path, O_RDONLY);
    if (file < 0)
    {
        free(header);

        // runtimeError("Failed to open file '%s'", path);
        // *hasError = true;
        return BOOL_VAL(false);
    }
    int valread;

    while ((valread = read(file, response + response_len, BUFFER_SIZE - response_len)) > 0)
    {
        response_len += valread;
    }
    if (send(sock, response, response_len, 0) < 0)
    {
        runtimeError("Failed to send data");
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (valread < 0)
    {
        runtimeError("Failed to read data");
        *hasError = true;
        return BOOL_VAL(false);
    }
    free(header);
    close(file);
    return BOOL_VAL(true);
}

int DecimalToBase(int n, int b)
{
    int rslt = 0, digitPos = 1;
    while (n)
    {
        rslt += (n % b) * digitPos;
        n /= b;
        digitPos *= 10;
    }
    return rslt;
}

static Value openFileNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2 && argc != 3)
    {
        runtimeError("Expected 2 or 3 argument but %d passed in for openFile(path, flags, mode)", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for openFile(path, flags, mode)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[1]))
    {
        runtimeError("Expected an int but got '%s' for openFile(path, flags, mode)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (argc == 3 && !IS_NUM(argv[2]))
    {
        runtimeError("Expected an int but got '%s' for openFile(path, flags, mode)", VALUE_TYPES[argv[2].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *path = AS_CSTR(argv[0]);
    int flag = AS_NUM(argv[1]);
    int mode;
    if (argc == 3)
        mode = AS_NUM(argv[2]);
    int file = argc == 3 ? open(path, flag, mode) : open(path, flag);
    if (file < 0)
    {
        runtimeError("Failed to open file '%s'", path);
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL(file);
}

static void defineNative(const char *name, NativeFn function)
{
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function)));
    tableSet(&vm.globals, AS_STR(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

static bool call(ObjClosure *closure, int argC)
{
    if (argC != closure->function->arity)
    {
        runtimeError("Expected %d arguments but %d passed in for %s", closure->function->arity, argC, closure->function->name == NULL ? "<script>" : closure->function->name->chars);
        return false;
    }

    if (vm.frameCount == FRAMES_MAX)
    {
        runtimeError("StackOverFlow Error");
        return false;
    }

    CallFrame *frame = &vm.frames[vm.frameCount++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm.stackTop - argC - 1;
    return true;
}

static bool callValue(Value callee, int argC)
{
    if (IS_OBJ(callee))
    {
        switch (OBJ_TYPE(callee))
        {
        case OBJ_BOUND_METHOD:
        {
            ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
            vm.stackTop[-argC - 1] = bound->receiver;
            return call(bound->method, argC);
        }
        case OBJ_CLASS:
        {
            ObjClass *klass = AS_CLASS(callee);
            vm.stackTop[-argC - 1] = OBJ_VAL(newInstance(klass));
            if (!IS_NIL(klass->initializer))
            {
                if (IS_NATIVE(klass->initializer))
                    return callValue(klass->initializer, argC);
                return call(AS_CLOSURE(klass->initializer), argC);
            }
            else if (argC != 0)
            {
                runtimeError("Expected 0 arguments for init of class '%s' but got %d", klass->name->chars, argC);
                return false;
            }
            return true;
        }
        case OBJ_CLOSURE:
            return call(AS_CLOSURE(callee), argC);
        case OBJ_NATIVE:
        {
            NativeFn native = AS_NATIVE(callee);
            bool hasError = false;
            bool pushedValue = false;
            Value result = native(argC, vm.stackTop - argC, &hasError, &pushedValue);
            if (hasError)
                return false;
            if (!pushedValue)
            {
                vm.stackTop -= argC + 1;
                push(result);
            }
            return true;
        }
        default:
            break;
        }
    }
    runtimeError("Can only call functions and classes -- not '%s'", VALUE_TYPES[callee.type]);
    return false;
}

static bool invokeFromClass(ObjClass *klass, ObjString *name, int argc)
{
    Value method;
    if (!tableGet(&klass->methods, name, &method))
    {
        runtimeError("Undefined method '%s' for class '%s'", name->chars, klass->name->chars);
        return false;
    }
    if (IS_NATIVE(method))
        return callValue(method, argc); // maybe for custom classes with native methods
    else
        return call(AS_CLOSURE(method), argc);
}

bool invoke(ObjString *name, int argc)
{
    Value receiver = peek(argc);
    bool isInstance = IS_INSTANCE(receiver);
    bool isClass = IS_CLASS(receiver);
    if (!isInstance && !isClass)
    {
        if (IS_STR(receiver))
            return invokeFromClass(vm.stringClass, name, argc);
        else if (IS_LIST(receiver))
            return invokeFromClass(vm.listClass, name, argc);
        else
            runtimeError("Only Classes and their instances have methods. -- not '%s'", VALUE_TYPES[receiver.type]);
        return false;
    }

    // Static methods call
    if (isClass)
        return invokeFromClass(AS_CLASS(receiver), name, argc);

    ObjInstance *
        instance = AS_INSTANCE(receiver);
    Value value;
    if (tableGet(&instance->fields, name, &value))
    {
        vm.stackTop[-argc - 1] = value;
        return callValue(value, argc);
    }
    return invokeFromClass(instance->klass, name, argc);
}

static bool bindMethod(ObjClass *klass, ObjString *name)
{
    Value method;
    if (!tableGet(&klass->methods, name, &method))
    {
        runtimeError("Undefined method '%s' for class '%s'", name->chars, klass->name->chars);
        return false;
    }
    ObjBoundMethod *bound = newBoundMethod(peek(0), AS_CLOSURE(method));

    pop();
    push(OBJ_VAL(bound));
    return true;
}

static ObjUpvalue *captureUpvalues(Value *local)
{
    ObjUpvalue *prevUpval = NULL;
    ObjUpvalue *upval = vm.openUpvalues;
    while (upval != NULL && upval->location > local)
    {
        prevUpval = upval;
        upval = upval->next;
    }

    if (upval != NULL && upval->location == local)
        return upval;

    ObjUpvalue *createdUpval = newUpvalue(local);

    createdUpval->next = upval;

    if (prevUpval == NULL)
        vm.openUpvalues = createdUpval;
    else
        prevUpval->next = createdUpval;

    return createdUpval;
}

static void closeUpvalues(Value *last)
{
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last)
    {
        ObjUpvalue *upval = vm.openUpvalues;
        upval->closed = *upval->location;
        upval->location = &upval->closed;
        vm.openUpvalues = vm.openUpvalues->next;
    }
}

static bool defineMethod(ObjString *name)
{
    Value method = peek(0);
    ObjClass *klass = AS_CLASS(peek(1));
    tableSet(&klass->methods, name, method);
    if (name == vm.initStr)
        klass->initializer = method;
    else if (name == vm.toStr)
        klass->toStr = method;
    else if (name == vm.eqStr)
    {
        if (AS_CLOSURE(method)->function->arity != 1)
        {
            runtimeError("<_eq_> method override needs to have 1 argument only. Was defined with %d ", AS_CLOSURE(method)->function->arity);
            return false;
        }
        klass->equals = method;
    }
    else if (name == vm.ltStr)
    {
        if (AS_CLOSURE(method)->function->arity != 1)
        {
            runtimeError("<_lt_> method override needs to have 1 argument only. Was defined with %d ", AS_CLOSURE(method)->function->arity);
            return false;
        }
        klass->lessThan = method;
    }
    else if (name == vm.gtStr)
    {
        if (AS_CLOSURE(method)->function->arity != 1)
        {
            runtimeError("<_gt_> method override needs to have 1 argument only. Was defined with %d ", AS_CLOSURE(method)->function->arity);
            return false;
        }
        klass->greaterThan = method;
    }
    else if (name == vm.indexStr)
    {
        if (AS_CLOSURE(method)->function->arity != 1)
        {
            runtimeError("<_get_> method override needs to have 1 argument only. Was defined with %d ", AS_CLOSURE(method)->function->arity);
            return false;
        }
        klass->indexFn = method;
    }
    else if (name == vm.setStr)
    {
        if (AS_CLOSURE(method)->function->arity != 2)
        {
            runtimeError("<_set_> method override needs to have 2 arguments only. Was defined with %d ", AS_CLOSURE(method)->function->arity);
            return false;
        }
        klass->setFn = method;
    }
    else if (name == vm.sizeStr)
    {
        if (AS_CLOSURE(method)->function->arity != 0)
        {
            runtimeError("<_size_> method override needs to have 0 arguments only. Was defined with %d ", AS_CLOSURE(method)->function->arity);
            return false;
        }
        klass->sizeFn = method;
    }
    else if (name == vm.hashStr)
    {
        if (AS_CLOSURE(method)->function->arity != 0)
        {
            runtimeError("<_hash_> method override needs to have 0 arguments only. Was defined with %d ", AS_CLOSURE(method)->function->arity);
            return false;
        }
        klass->hashFn = method;
    }

    pop();
    return true;
}

static bool isFalsey(Value value)
{
    return IS_NIL(value) || (IS_BOOL(value) && !(AS_BOOL(value))) || (IS_NUM(value) && AS_NUM(value) == 0) || (IS_STR(value) && AS_STR(value)->len == 0) || (IS_LIST(value) && AS_LIST(value)->count == 0);
}

static ObjString *addTwoStrings(char *a, char *b, int lenA, int lenB)
{
    int length = lenA + lenB;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a, lenA);
    memcpy(chars + lenA, b, lenB);
    chars[length] = '\0';

    return takeString(chars, length);
}

static void concatenate()
{
    ObjString *b = AS_STR(peek(0));
    ObjString *a = AS_STR(peek(1));
    ObjString *result = addTwoStrings(a->chars, b->chars, a->len, b->len);
    pop();
    pop();
    push(OBJ_VAL(result));
}

static Value concatenateList(ObjList *a, ObjList *b)
{
    // ObjList *b = AS_LIST(peek(0));
    // ObjList *a = AS_LIST(peek(1));

    int capacity = a->capacity + b->capacity;
    int count = a->count + b->count;

    ObjList *result = newList();
    result->capacity = capacity;
    // result->count = count;
    result->items = ALLOCATE(Value, count);
    for (int i = 0; i < a->count; i++)
    {
        appendToList(result, a->items[i]);
    }
    for (int i = 0; i < b->count; i++)
    {
        appendToList(result, b->items[i]);
    }
    // pop();
    // pop();
    return OBJ_VAL(result);
}

ObjClass *primativeClass(char *name)
{
    ObjClass *clazz = newClass(copyString(name, (int)strlen(name)));
    tableSet(&vm.globals, clazz->name, OBJ_VAL(clazz));
    return clazz;
}

static Value inputNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc == 1)
    {
        char str[10000] = {0};
        int len = 0;
        valueToString(argv[0], str, &len);
        printf("%s", str);
    }
    else if (argc > 1)
    {
        runtimeError("'input()' expects zero or one argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    char *input = ALLOCATE(char, 100);
    char *ret = fgets(input, 100, stdin);
    input[strlen(input) - 1] = '\0';
    return OBJ_VAL(takeString(input, (int)strlen(input)));
}

static Value splitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1 && argc != 2)
    {
        runtimeError("'split(string, <optional> delim)' expects 1 or 2 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for split(str, sep)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (argc == 2 && !IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for split(str, sep)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str1 = AS_CSTR(argv[0]);
    char *str = ALLOCATE(char, strlen(str1) + 1);
    char *toFree = str;
    strcpy(str, str1);

    char *sep = argc == 2 ? AS_CSTR(argv[1]) : " ";
    int sepLen = (int)strlen(sep);
    char *token = strstr(str, sep);

    ObjList *list = newList();
    push(OBJ_VAL(list));
    while (token != NULL)
    {
        appendToList(list, OBJ_VAL(copyString(str, token - str)));
        str = token + sepLen;
        token = strstr(str, sep);
    }
    appendToList(list, OBJ_VAL(copyString(str, strlen(str))));
    pop();
    free(toFree);
    return OBJ_VAL(list);
}

char *replace(char *str, char *old, char *new)
{
    char *result;
    int i, cnt = 0;
    int newLen = strlen(new);
    int oldLen = strlen(old);

    // Counting the number of times old word
    // occur in the string
    for (i = 0; str[i] != '\0'; i++)
    {
        if (strstr(&str[i], old) == &str[i])
        {
            cnt++;
            // Jumping to index after the old word.
            i += oldLen - 1;
        }
    }

    // Making new string of enough length
    result = (char *)malloc(i + cnt * (newLen - oldLen) + 1);

    i = 0;
    while (*str)
    {
        // compare the substring with the result
        if (strstr(str, old) == str)
        {
            strcpy(&result[i], new);
            i += newLen;
            str += oldLen;
        }
        else
            result[i++] = *str++;
    }

    result[i] = '\0';
    return result;
}

static Value replaceNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 3)
    {
        runtimeError("'replace()' expects 3 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for replace(str, old, new)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for replace(str, old, new)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[2]))
    {
        runtimeError("Expected a string but got '%s' for replace(str, old, new)", VALUE_TYPES[argv[2].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    char *old = AS_CSTR(argv[1]);
    char *new = AS_CSTR(argv[2]);
    char *result = replace(str, old, new);
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

static Value splitByWhitespaceNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'splitByWhitespace()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for splitByWhitespace(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str1 = AS_CSTR(argv[0]);
    char *str = ALLOCATE(char, strlen(str1) + 1);
    // char *toFree = str;
    strcpy(str, str1);

    char *sep = " \t\r\n\v\f";
    char *token = strtok(str, sep);
    ObjList *list = newList();
    while (token != NULL)
    {
        appendToList(list, OBJ_VAL(copyString(token, (int)strlen(token))));
        token = strtok(NULL, sep);
    }
    free(str);
    return OBJ_VAL(list);
}

char *trim(char *str)
{
    char *end;
    while (isspace((unsigned char)*str))
        str++;
    if (*str == 0)
        return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;
    end[1] = '\0';
    return str;
}

static Value trimNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'trim()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for trim(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    char *result = ALLOCATE(char, strlen(str) + 1);
    char *t = result;
    strcpy(result, str);
    result = trim(result);
    ObjString *res = copyString(result, (int)strlen(result));
    free(t);
    return OBJ_VAL(res);
}

static Value findNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'find()' expects 2 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for find(str, sub)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for find(str, sub)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    char *sub = AS_CSTR(argv[1]);
    char *result = strstr(str, sub);
    if (result == NULL)
        return NUM_VAL(-1);
    return NUM_VAL(result - str);
}

char toLower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    return c;
}
static Value lowerNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'lower()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for lower(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    char *result = ALLOCATE(char, strlen(str) + 1);
    strcpy(result, str);
    for (int i = 0; result[i]; i++)
        result[i] = tolower(result[i]);
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

char toUpper(char c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    return c;
}

static Value upperNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'upper()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for upper(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    char *result = ALLOCATE(char, strlen(str) + 1);
    strcpy(result, str);
    for (int i = 0; result[i]; i++)
        result[i] = toUpper(result[i]);
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

static Value trimLeftNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'trimLeft()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for trimLeft(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    int i = 0;
    while (isspace((unsigned char)str[i]))
        i++;
    return OBJ_VAL(copyString(str + i, (int)strlen(str) - i));
}

static Value trimRightNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'trimRight()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for trimRight(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);

    int i = strlen(str) - 1;
    while (isspace((unsigned char)str[i]))
        i--;

    return OBJ_VAL(copyString(str, i + 1));
}

static Value isWhitespaceNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'isWhitespace()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for isWhitespace(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);

    for (int i = 0; str[i]; i++)
        if (!isspace((unsigned char)str[i]))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}

static Value containsNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'contains()' expects 2 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for contains(str, sub)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for contains(str, sub)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    char *sub = AS_CSTR(argv[1]);
    if (strstr(str, sub) == NULL)
        return BOOL_VAL(false);
    return BOOL_VAL(true);
}

/////////////////////// STRING CLASS NATIVE METHODS ///////////////////////

static Value split2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0 && argc != 1)
    {
        runtimeError("'split(<optional> delim)' expects 1 or 2 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string but got '%s' for split(sep)", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (argc == 1 && !IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for split(str, sep)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str1 = AS_CSTR(peek(argc));
    char *str = ALLOCATE(char, strlen(str1) + 1);
    char *toFree = str;
    strcpy(str, str1);

    char *sep = argc == 1 ? AS_CSTR(argv[0]) : " ";
    int sepLen = (int)strlen(sep);
    char *token = strstr(str, sep);

    ObjList *list = newList();
    push(OBJ_VAL(list));
    while (token != NULL)
    {
        appendToList(list, OBJ_VAL(copyString(str, token - str)));
        str = token + sepLen;
        token = strstr(str, sep);
    }
    appendToList(list, OBJ_VAL(copyString(str, strlen(str))));
    pop();
    free(toFree);
    return OBJ_VAL(list);
}

static Value replace2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 2)
    {
        runtimeError("'replace()' expects 3 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string as caller but got '%s' for replace(old, new)", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for replace(old, new)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[1]))
    {
        runtimeError("Expected a string but got '%s' for replace(old, new)", VALUE_TYPES[argv[2].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *old = AS_CSTR(argv[0]);
    char *new = AS_CSTR(argv[1]);
    char *result = replace(str, old, new);
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

static Value splitByWhitespace2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'splitByWhitespace()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for splitByWhitespace()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str1 = AS_CSTR(peek(argc));
    char *str = ALLOCATE(char, strlen(str1) + 1);
    // char *toFree = str;
    strcpy(str, str1);

    char *sep = " \t\r\n\v\f";
    char *token = strtok(str, sep);
    ObjList *list = newList();
    while (token != NULL)
    {
        appendToList(list, OBJ_VAL(copyString(token, (int)strlen(token))));
        token = strtok(NULL, sep);
    }
    free(str);
    return OBJ_VAL(list);
}

static Value trim2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'trim()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for trim()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *result = ALLOCATE(char, strlen(str) + 1);
    char *t = result;
    strcpy(result, str);
    result = trim(result);
    ObjString *res = copyString(result, (int)strlen(result));
    free(t);
    return OBJ_VAL(res);
}

static Value find2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'find()' expects 2 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for find(sub)", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for find(sub)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *sub = AS_CSTR(argv[0]);
    char *result = strstr(str, sub);
    if (result == NULL)
        return NUM_VAL(-1);
    return NUM_VAL(result - str);
}

static Value lower2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'lower()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for lower()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *result = ALLOCATE(char, strlen(str) + 1);
    strcpy(result, str);
    for (int i = 0; result[i]; i++)
        result[i] = tolower(result[i]);
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

static Value upper2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'upper()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for upper()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *result = ALLOCATE(char, strlen(str) + 1);
    strcpy(result, str);
    for (int i = 0; result[i]; i++)
        result[i] = toUpper(result[i]);
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

static Value titleNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'title()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for title()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *result = ALLOCATE(char, strlen(str) + 1);
    strcpy(result, str);
    bool cap = true;
    for (int i = 0; result[i]; i++)
    {
        if (cap && isalpha(result[i]))
        {
            result[i] = toUpper(result[i]);
            cap = false;
        }
        else if (isspace(result[i]))
            cap = true;
        else
            result[i] = toLower(result[i]);
    }
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

static Value trimLeft2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'trimLeft()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for trimLeft()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    int i = 0;
    while (isspace((unsigned char)str[i]))
        i++;
    return OBJ_VAL(copyString(str + i, (int)strlen(str) - i));
}

static Value trimRight2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'trimRight()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for trimRight()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));

    int i = strlen(str) - 1;
    while (isspace((unsigned char)str[i]))
        i--;

    return OBJ_VAL(copyString(str, i + 1));
}

static Value isWhitespace2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'isWhitespace()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for isWhitespace()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));

    for (int i = 0; str[i]; i++)
        if (!isspace((unsigned char)str[i]))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}

static Value contains2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'contains()' expects 1 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for contains(sub)", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for contains(sub)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *sub = AS_CSTR(argv[0]);
    if (strstr(str, sub) == NULL)
        return BOOL_VAL(false);
    return BOOL_VAL(true);
}

static Value isDigitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'isDigit()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for isDigit()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));

    for (int i = 0; str[i]; i++)
        if (!isdigit((unsigned char)str[i]))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}

static Value join2Native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 1)
    {
        runtimeError("'join(list)' expects 1 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(argv[0]))
    {
        runtimeError("'join(list)' expects a list as first argument but got '%s'", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("'join(list)' expects a string as second argument but got '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(argv[0]);
    ObjString *sep = AS_STR(peek(argc));
    char *str = ALLOCATE(char, 100);
    sprintf(str, "%s", "");
    for (int i = 0; i < list->count; i++)
    {
        int len = 0;
        char item[10000] = {0};
        valueToString(list->items[i], item, &len);
        char *temp = ALLOCATE(char, strlen(str) + len + strlen(sep->chars) + 1);
        sprintf(temp, "%s%s%s", str, item, sep->chars);
        FREE_ARRAY(char, str, strlen(str) + 1);
        // FREE_ARRAY(char, item, strlen(item) + 1);
        str = temp;
    }
    if (list->count > 0)
        str[strlen(str) - strlen(sep->chars)] = '\0';
    return OBJ_VAL(takeString(str, (int)strlen(str)));
}

static Value formatNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc < 1)
    {
        runtimeError("'format()' expects at least 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for format()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *result = ALLOCATE(char, 10000);
    int len = 0;
    int i = 0;
    int numFormats = 0;
    char item[10000] = {0};
    while (str[i])
    {
        if (str[i] == '{')
        {
            if (str[i + 1] != '}')
            {
                char index[100] = {0};
                char *temp = str + i + 1;
                while (*temp != '}')
                    temp++;
                strncpy(index, str + i + 1, temp - str - i - 1);
                i += temp - str - i - 1;
                char *end;
                long int num = strtol(index, &end, 10);
                if (end == str)
                {
                    runtimeError("Expected a number but got '%s' for indexed format with string.format()", str);
                    *hasError = true;
                    return NIL_VAL;
                }
                if (num < 0 || num >= argc)
                {
                    runtimeError("Index out of range for string format. Expected a number between 0 and %d but got %ld", argc - 1, num);
                    *hasError = true;
                    return NIL_VAL;
                }
                int a;
                valueToString(argv[num], item, &a);
                for (int k = 0; k < a; k++)
                    result[len++] = item[k];
            }
            else if (str[i + 1] == '}')
            {
                if (numFormats >= argc)
                    numFormats = argc - 1;

                int a;
                valueToString(argv[numFormats], item, &a);
                for (int k = 0; k < a; k++)
                    result[len++] = item[k];

                numFormats++;
            }
            i++;
        }
        else
        {
            result[len++] = str[i];
        }
        i++;
    }
    result[len] = '\0';
    return OBJ_VAL(takeString(result, (int)strlen(result)));
}

///////////////////////////////////////////////////////////////////////////

//////////////////////// LIST CLASS NATIVE METHODS ////////////////////////

static Value appendListNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'append()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for append()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    appendToList(list, argv[0]);
    return OBJ_VAL(list);
}

static Value extendListNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'extend()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for extend()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(argv[0]))
    {
        runtimeError("Expected a list but got '%s' for extend(list)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *a = AS_LIST(peek(argc));
    ObjList *b = AS_LIST(argv[0]);
    for (int i = 0; i < b->count; i++)
        appendToList(a, b->items[i]);
    // return a;
    return OBJ_VAL(a);
}

static void prependToList(ObjList *list, Value value)
{
    if (list->count == list->capacity)
    {
        int oldCapacity = list->capacity;
        list->capacity = GROW_CAPACITY(oldCapacity);
        list->items = GROW_ARRAY(Value, list->items, oldCapacity, list->capacity);
    }
    for (int i = list->count; i > 0; i--)
        list->items[i] = list->items[i - 1];
    list->items[0] = value;
    list->count++;
}

static Value prependListNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'prepend()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for prepend()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    prependToList(list, argv[0]);
    return OBJ_VAL(list);
}

static Value popListNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0 && argc != 1)
    {
        runtimeError("'pop(<optional> index)' expects 0 or 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (argc && !IS_NUM(argv[0]))
    {
        runtimeError("Expected a number but got '%s' for pop(index)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for pop()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    if (list->count == 0)
    {
        runtimeError("Cannot pop from an empty list");
        *hasError = true;
        return NIL_VAL;
    }
    if (argc)
    {
        int index = AS_NUM(argv[0]);
        if (index < 0 || index >= list->count)
        {
            runtimeError("Index out of range");
            *hasError = true;
            return NIL_VAL;
        }
        Value value = list->items[index];
        for (int i = index; i < list->count - 1; i++)
            list->items[i] = list->items[i + 1];
        list->count--;
        return value;
    }
    return list->items[--list->count];
}

///////////////////////////////////////////////////////////////////////////

// SB

static Value sbInitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0 && argc != 1)
    {
        runtimeError("'init(<optional> initailString)' expects 0 or 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a SB to be caller but got '%s' for SB.init()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjInstance *instance = AS_INSTANCE(peek(argc));
    ObjList *elem = newList();
    if (argc && IS_STR(argv[0]))
        appendToList(elem, argv[0]);
    tableSet(&instance->fields, copyString("contents", 8), OBJ_VAL(elem));
    return peek(argc);
}

static Value sbAppendNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'append()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a SB to be caller but got '%s' for SB.append()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for SB.append(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Value value;
    if (!tableGet(&instance->fields, copyString("contents", 8), &value))
    {
        runtimeError("SB instance has no 'contents' field");
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_LIST(value))
    {
        runtimeError("Expected a list but got '%s' for SB.append(str)", VALUE_TYPES[value.type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(value);
    appendToList(list, argv[0]);
    return peek(argc);
}

static Value sbSizeNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'size()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a SB to be caller but got '%s' for SB.size()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Value value;
    if (!tableGet(&instance->fields, copyString("contents", 8), &value))
    {
        runtimeError("SB instance has no 'contents' field");
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_LIST(value))
    {
        runtimeError("Expected a list but got '%s' for SB.size()", VALUE_TYPES[value.type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(value);
    return NUM_VAL(list->count);
}

static Value sbToStringNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'toStr()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a SB to be caller but got '%s' for SB.toStr()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Value value;
    if (!tableGet(&instance->fields, copyString("contents", 8), &value))
    {
        runtimeError("SB instance has no 'contents' field");
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_LIST(value))
    {
        runtimeError("Expected a list but got '%s' for SB.toStr()", VALUE_TYPES[value.type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(value);
    char *str = ALLOCATE(char, 100);
    sprintf(str, "%s", "");
    for (int i = 0; i < list->count; i++)
    {
        int len = 0;
        char item[10000] = {0};
        valueToString(list->items[i], item, &len);
        char *temp = ALLOCATE(char, strlen(str) + len + 1);
        sprintf(temp, "%s%s", str, item);
        FREE_ARRAY(char, str, strlen(str) + 1);
        str = temp;
    }
    return OBJ_VAL(takeString(str, (int)strlen(str)));
}

static Value sbClearNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'clear()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a SB to be caller but got '%s' for SB.clear()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Value value;
    if (!tableGet(&instance->fields, copyString("contents", 8), &value))
    {
        runtimeError("SB instance has no 'contents' field");
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_LIST(value))
    {
        runtimeError("Expected a list but got '%s' for SB.clear()", VALUE_TYPES[value.type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(value);
    list->count = 0;
    return peek(argc);
}

static Value sbPopNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'pop()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a SB to be caller but got '%s' for SB.pop()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Value value;
    if (!tableGet(&instance->fields, copyString("contents", 8), &value))
    {
        runtimeError("SB instance has no 'contents' field");
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_LIST(value))
    {
        runtimeError("Expected a list but got '%s' for SB.pop()", VALUE_TYPES[value.type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(value);
    if (list->count == 0)
    {
        runtimeError("Cannot pop from an empty SB");
        *hasError = true;
        return NIL_VAL;
    }
    return list->items[--list->count];
}

static Value sbToArrayNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'toArray()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a SB to be caller but got '%s' for SB.toArray()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Value value;
    if (!tableGet(&instance->fields, copyString("contents", 8), &value))
    {
        runtimeError("SB instance has no 'contents' field");
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_LIST(value))
    {
        runtimeError("Expected a list but got '%s' for SB.toArray()", VALUE_TYPES[value.type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(value);
    return OBJ_VAL(list);
}
//

void initVM()
{
    srand(time(NULL));
    resetStack();
    vm.objects = NULL;
    vm.bytesAllocated = 0;
    vm.nextGC = 1024L * 1024L * 1024L;

    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

    vm.importCount = 0;
    vm.importSources = NULL;

    vm.nextWideOp = -1;

    initTable(&vm.globals);
    initTable(&vm.strings);
    initTable(&vm.imports);
    initTable(&vm.importFuncs);
    vm.initStr = NULL;
    vm.initStr = copyString("init", 4);

    vm.toStr = NULL;
    vm.toStr = copyString("toStr", 5);

    vm.eqStr = NULL;
    vm.eqStr = copyString("_eq_", 4);

    vm.ltStr = NULL;
    vm.ltStr = copyString("_lt_", 4);

    vm.gtStr = NULL;
    vm.gtStr = copyString("_gt_", 4);

    vm.indexStr = NULL;
    vm.indexStr = copyString("_get_", 5);

    vm.setStr = NULL;
    vm.setStr = copyString("_set_", 5);

    vm.sizeStr = NULL;
    vm.sizeStr = copyString("_size_", 6);

    vm.hashStr = NULL;
    vm.hashStr = copyString("_hash_", 6);

    vm.clazzStr = NULL;
    vm.clazzStr = copyString("clazz", 5);

    // newClass
    vm.stringClass = NULL;
    ObjClass *stringClass = primativeClass("String");
    vm.stringClass = stringClass;
    tableSet(&stringClass->methods, copyString("split", 5), OBJ_VAL(newNative(split2Native)));
    tableSet(&stringClass->methods, copyString("replace", 7), OBJ_VAL(newNative(replace2Native)));
    tableSet(&stringClass->methods, copyString("splitOnWs", 9), OBJ_VAL(newNative(splitByWhitespace2Native)));
    tableSet(&stringClass->methods, copyString("trim", 4), OBJ_VAL(newNative(trim2Native)));
    tableSet(&stringClass->methods, copyString("find", 4), OBJ_VAL(newNative(find2Native)));
    tableSet(&stringClass->methods, copyString("lower", 5), OBJ_VAL(newNative(lower2Native)));
    tableSet(&stringClass->methods, copyString("upper", 5), OBJ_VAL(newNative(upper2Native)));
    tableSet(&stringClass->methods, copyString("trimLeft", 8), OBJ_VAL(newNative(trimLeft2Native)));
    tableSet(&stringClass->methods, copyString("trimRight", 9), OBJ_VAL(newNative(trimRight2Native)));
    tableSet(&stringClass->methods, copyString("isWhitespace", 12), OBJ_VAL(newNative(isWhitespace2Native)));
    tableSet(&stringClass->methods, copyString("contains", 8), OBJ_VAL(newNative(contains2Native)));
    tableSet(&stringClass->methods, copyString("isDigit", 7), OBJ_VAL(newNative(isDigitNative)));
    tableSet(&stringClass->methods, copyString("title", 5), OBJ_VAL(newNative(titleNative)));
    tableSet(&stringClass->methods, copyString("join", 4), OBJ_VAL(newNative(join2Native)));
    tableSet(&stringClass->methods, copyString("format", 6), OBJ_VAL(newNative(formatNative)));

    vm.listClass = NULL;
    ObjClass *listClass = primativeClass("List");
    vm.listClass = listClass;
    tableSet(&listClass->methods, copyString("append", 6), OBJ_VAL(newNative(appendListNative)));
    tableSet(&listClass->methods, copyString("extend", 6), OBJ_VAL(newNative(extendListNative)));
    tableSet(&listClass->methods, copyString("prepend", 7), OBJ_VAL(newNative(prependListNative)));
    tableSet(&listClass->methods, copyString("pop", 3), OBJ_VAL(newNative(popListNative)));

    ObjClass *sb = primativeClass("StringBuilder");
    sb->initializer = OBJ_VAL(newNative(sbInitNative));
    sb->sizeFn = OBJ_VAL(newNative(sbSizeNative));
    sb->toStr = OBJ_VAL(newNative(sbToStringNative));
    // tableSet(&sb->methods, copyString("init", 4), sb->initializer);
    tableSet(&sb->methods, copyString("_size_", 4), sb->sizeFn);
    tableSet(&sb->methods, copyString("toStr", 5), sb->toStr);
    tableSet(&sb->methods, copyString("append", 6), OBJ_VAL(newNative(sbAppendNative)));
    tableSet(&sb->methods, copyString("clear", 5), OBJ_VAL(newNative(sbClearNative)));
    tableSet(&sb->methods, copyString("pop", 3), OBJ_VAL(newNative(sbPopNative)));
    tableSet(&sb->methods, copyString("toArray", 7), OBJ_VAL(newNative(sbToArrayNative)));

    ObjClass *mapEntryClass = primativeClass("MapEntry");

    defineNative("clock", clockNative);
    defineNative("input", inputNative);
    defineNative("getc", getcNative);
    defineNative("kbhit", kbhitNative);
    defineNative("chr", chrNative);
    defineNative("exit", exitNative);
    defineNative("print_error", printErrNative);
    defineNative("sleep", sleepNative);
    defineNative("clear", clearNative);
    defineNative("randInt", randomIntNative);
    defineNative("rand", randNative);
    defineNative("str", strCastNative);
    defineNative("hash", hashNative);
    defineNative("join", joinNative);
    defineNative("int", intCastNative);
    defineNative("float", floatCastNative);
    defineNative("bool", boolCastNative);
    defineNative("list", listCastNative);
    defineNative("type", typeOf);
    defineNative("len", lenNative);
    defineNative("instanceof", instanceOf);
    defineNative("ord", ordNative);
    // Strings
    defineNative("split", splitNative);
    defineNative("splitOnWs", splitByWhitespaceNative);
    defineNative("replace", replaceNative);
    defineNative("find", findNative);
    defineNative("contains", containsNative);
    defineNative("lower", lowerNative);
    defineNative("upper", upperNative);
    defineNative("trim", trimNative);
    defineNative("trimLeft", trimLeftNative);
    defineNative("trimRight", trimRightNative);
    defineNative("isWhitespace", isWhitespaceNative);

    // Sockets
    defineNative("socket", socketNative);
    defineNative("close", closeFDNative);
    defineNative("connect", connectNative);
    defineNative("bind", bindSocketPortNative);
    defineNative("setSockOpt", setSockOptionsNative);
    defineNative("listen", listenNative);
    defineNative("accept", acceptNative);
    defineNative("read", readNative);
    defineNative("send", sendNative);
    defineNative("sendFile", sendFileWithFileDescriptorNative);
    // File
    defineNative("open", openFileNative);
    defineNative("readN", readSizeNative);
    defineNative("seek", seekNative);
    defineNative("write", writeNative);
    // defineNative("appendFile", appendFileNative);
    defineNative("fileExists", fileExistsNative);
    defineNative("fileSize", fileSizeNative);

    // Math
    defineNative("sin", sinNative);
    defineNative("cos", cosNative);
}

void freeVM()
{
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    freeTable(&vm.imports);
    freeTable(&vm.importFuncs);
    for (int i = 0; i < vm.importCount; i++)
        free(vm.importSources[i]);
    FREE_ARRAY(char, vm.importSources, vm.importCount);
    vm.initStr = NULL;
    vm.toStr = NULL;
    vm.eqStr = NULL;
    vm.clazzStr = NULL;
    freeObjects();
}

bool indexByNum(Value indexVal, ObjList *list)
{
    Value result;
    int index = AS_NUM(indexVal);

    if (!isValidListIndex(list, index))
    {
        runtimeError("List index out of range. List has size %d. However, %d was provided", list->count, index);
        return true;
    }

    result = indexFromList(list, index);
    push(result);
    return false;
}

int getValidListIndex(ObjList *list, int index)
{
    if (index < 0)
        index = list->count + index + 1;
    if (index > list->count)
        index = list->count;
    return index;
}

int getValidStringIndex(ObjString *string, int index)
{
    if (index < 0)
        index = string->len + index + 1;
    if (index > string->len)
        index = string->len;
    return index;
}

void indexStringBySlice(Value sliceVal, ObjString *str)
{
    Value *result;
    ObjSlice *slice = AS_SLICE(sliceVal);
    int start = getValidStringIndex(str, slice->start);
    int end = getValidStringIndex(str, slice->end);
    int step = slice->step;
    bool reversed = step < 0;
    char substring[BUFFER_SIZE];
    int subIndex = 0;
    if (reversed)
    {
        int low = start < end ? start : end;
        int high = start < end ? end : start;
        for (int i = high - 1; i >= low; i += step)
        {
            substring[subIndex++] = str->chars[i];
        }
    }
    else
    {
        for (int i = start; i < end; i += step)
        {
            substring[subIndex++] = str->chars[i];
        }
    }
    substring[subIndex] = '\0';
    if (subIndex == 0)
    {
        push(OBJ_VAL(copyString("", 0)));
        return;
    }
    ObjString *ret = copyString(substring, subIndex);

    push(OBJ_VAL(ret));
}

void indexBySlice(Value sliceVal, ObjList *list)
{
    Value *result;
    ObjSlice *slice = AS_SLICE(sliceVal);
    int start = getValidListIndex(list, slice->start);
    int end = getValidListIndex(list, slice->end);
    int step = slice->step;
    bool reversed = step < 0;
    ObjList *ret = newList();
    push(OBJ_VAL(ret));
    if (reversed)
    {
        int low = start < end ? start : end;
        int high = start < end ? end : start;
        for (int i = high - 1; i >= low; i += step)
        {
            appendToList(ret, indexFromList(list, i));
        }
    }
    else
    {
        for (int i = start; i < end; i += step)
        {
            appendToList(ret, indexFromList(list, i));
        }
    }
}

static InterpretResult run(bool isRepl)
{
    CallFrame *frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[isWide() ? READ_SHORT() : READ_BYTE()])
#define READ_CONSTANT_SHORT() (frame->closure->function->chunk.constants.values[READ_SHORT()])
#define READ_STRING() AS_STR(READ_CONSTANT())
#define READ_STRING_SHORT() AS_STR(READ_CONSTANT_SHORT())
#define BIN_OP(valueType, op)                          \
    do                                                 \
    {                                                  \
        if (!IS_NUM(peek(0)) || !IS_NUM(peek(1)))      \
        {                                              \
            runtimeError("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR;            \
        }                                              \
        double b = AS_NUM(pop());                      \
        double a = AS_NUM(pop());                      \
        push(valueType(a op b));                       \
    } while (false);

    for (;;)
    {
#if DEBUG_TRACE_EXEC
        printf("        ");
        for (Value *slot = vm.stack; slot < vm.stackTop; slot++)
        {
            printf("[ ");
            printValue(*slot, 0);
            printf(" ]");
        }
        printf("\n");
        disassembleInst(&frame->closure->function->chunk, (int)(frame->ip - frame->closure->function->chunk.code));
#endif
        uint8_t inst;
        switch (inst = READ_BYTE())
        {
        case OP_NIL:
            push(NIL_VAL);
            break;
        case OP_FALSE:
            push(BOOL_VAL(false));
            break;
        case OP_POP:
            pop();
            break;
        case OP_DUP:
            push(peek(0));
            break;
        case OP_SUB:
            BIN_OP(NUM_VAL, -);
            break;
        case OP_DIV:
            BIN_OP(NUM_VAL, /);
            break;
        case OP_INT_DIV:
        {
            if (!IS_NUM(peek(0)) || !IS_NUM(peek(1)))
            {
                runtimeError("Operands must be numbers.");
                return INTERPRET_RUNTIME_ERROR;
            }
            double b = AS_NUM(pop());
            double a = AS_NUM(pop());
            push(NUM_VAL((long)a / (long)b));
            break;
        }
        case OP_MULT:
            BIN_OP(NUM_VAL, *);
            break;

        case OP_MOD:
        {
            if (!IS_NUM(peek(0)) || !IS_NUM(peek(1)))
            {
                runtimeError("Operands must be numbers.");
                return INTERPRET_RUNTIME_ERROR;
            }
            double b = AS_NUM(pop());
            double a = AS_NUM(pop());
            push(NUM_VAL(fmod(a, b)));
            break;
        }
        case OP_POW:
        {
            if (!IS_NUM(peek(0)) || !IS_NUM(peek(1)))
            {
                runtimeError("Operands must be numbers.");
                return INTERPRET_RUNTIME_ERROR;
            }
            double b = AS_NUM(pop());
            double a = AS_NUM(pop());
            push(NUM_VAL(pow(a, b)));
            break;
        }
        case OP_NOT:
            push(BOOL_VAL(isFalsey(pop())));
            break;
        case OP_GREATER:
        {
            Value b = pop();
            Value a = pop();
            if (IS_NUM(a) && IS_NUM(b))
            {
                double bD = AS_NUM(b);
                double aD = AS_NUM(a);
                push(BOOL_VAL(aD > bD));
            }
            else if (IS_STR(a) && IS_STR(b))
            {
                ObjString *bS = AS_STR(b);
                ObjString *aS = AS_STR(a);
                push(BOOL_VAL(strcmp(aS->chars, bS->chars) > 0));
            }
            else if (a.type == VAL_OBJ && IS_INSTANCE(a) && !IS_NIL(AS_INSTANCE(a)->klass->greaterThan))
            {
                push(a);
                push(b);

                if (!invoke(vm.gtStr, 1))
                    return INTERPRET_RUNTIME_ERROR;
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            else
            {
                runtimeError("Operands must be numbers, strings or objects with a <_gt_> method.");
                return INTERPRET_RUNTIME_ERROR;
            }
            // BIN_OP(BOOL_VAL, >);
            break;
        }
        case OP_LESS:
        {
            Value b = pop();
            Value a = pop();
            if (IS_NUM(a) && IS_NUM(b))
            {
                double bD = AS_NUM(b);
                double aD = AS_NUM(a);
                push(BOOL_VAL(aD < bD));
                break;
            }
            else if (IS_STR(a) && IS_STR(b))
            {
                ObjString *bS = AS_STR(b);
                ObjString *aS = AS_STR(a);
                push(BOOL_VAL(strcmp(aS->chars, bS->chars) < 0));
                break;
            }
            else if (a.type == VAL_OBJ && IS_INSTANCE(a) && !IS_NIL(AS_INSTANCE(a)->klass->lessThan))
            {
                push(a);
                push(b);

                if (!invoke(vm.ltStr, 1))
                    return INTERPRET_RUNTIME_ERROR;
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            else
            {
                runtimeError("Operands must be numbers, strings or objects with a <_lt_> method.");
                return INTERPRET_RUNTIME_ERROR;
            }

            // BIN_OP(BOOL_VAL, <);
            break;
        }
        case OP_CLASS:
            push(OBJ_VAL(newClass(READ_STRING())));
            break;
        case OP_TRUE:
            push(BOOL_VAL(true));
            break;
        case OP_METHOD:
            if (!defineMethod(READ_STRING()))
                return INTERPRET_RUNTIME_ERROR;
            break;
        case OP_BUILD_SLICE:
        {
            if (!IS_NUM(peek(0)))
            {
                runtimeError("Expected number as slice step but got '%s'", VALUE_TYPES[peek(0).type]);
                return INTERPRET_RUNTIME_ERROR;
            }
            if (!IS_NUM(peek(1)))
            {
                runtimeError("Expected number as slice end but got '%s'", VALUE_TYPES[peek(1).type]);
                return INTERPRET_RUNTIME_ERROR;
            }
            if (!IS_NUM(peek(2)))
            {
                runtimeError("Expected number as slice start but got '%s'", VALUE_TYPES[peek(2).type]);
                return INTERPRET_RUNTIME_ERROR;
            }
            int step = (int)AS_NUM(pop());
            int end = (int)AS_NUM(pop());
            int start = (int)AS_NUM(pop());
            ObjSlice *slice = newSlice(start, end, step);
            push(OBJ_VAL(slice));
            break;
        }
        case OP_BUILD_LIST:
        {
            ObjList *list = newList();
            uint16_t itemCount = isWide() ? READ_SHORT() : READ_BYTE();

            push(OBJ_VAL(list));
            for (int i = itemCount; i > 0; i--)
            {
                appendToList(list, peek(i));
                // READ_STRING();
            }
            pop();
            while (itemCount-- > 0)
            {
                pop();
            }

            push(OBJ_VAL(list));
            break;
        }
        case OP_BUILD_DEFAULT_LIST:
        {
            bool hasDefault = (bool)READ_BYTE();
            Value defaultValue = NIL_VAL;
            if (hasDefault)
                defaultValue = pop();
            Value size = pop();
            if (!IS_NUM(size))
                runtimeError("Cannot create a default list with a non-number size");
            ObjList *list = newList();
            push(OBJ_VAL(list));
            int count = AS_NUM(size);
            for (int i = 0; i < count; i++)
            {
                appendToList(list, defaultValue);
            }
            pop();
            push(OBJ_VAL(list));
            break;
        }
        case OP_INDEX_SUBSCR:
        {
            Value indexVal = pop();
            Value listVal = pop();
            Value result;
            if (IS_STR(listVal))
            {
                ObjString *str = AS_STR(listVal);
                if (IS_NUM(indexVal))
                {
                    int index = AS_NUM(indexVal);
                    if (index < 0)
                        index = str->len + index;
                    if (index >= str->len || index < 0)
                    {
                        runtimeError("String index out of range. String has size %d. However, %d was provided", str->len, index);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    result = OBJ_VAL(copyString(str->chars + index, 1));
                    push(result);
                }
                else if (IS_SLICE(indexVal))
                {

                    indexStringBySlice(indexVal, str);
                    // runtimeError("String indexing with slicing is not supported yet :(");
                    // return INTERPRET_RUNTIME_ERROR;
                }
                else
                {
                    runtimeError("Expected number or slice as string index but got '%s'", VALUE_TYPES[indexVal.type]);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            if (!IS_LIST(listVal))
            {
                if (IS_INSTANCE(listVal))
                {
                    if (!IS_NIL(AS_INSTANCE(listVal)->klass->indexFn))
                    {
                        push(listVal);
                        push(indexVal);
                        if (!invoke(vm.indexStr, 1))
                            return INTERPRET_RUNTIME_ERROR;
                        frame = &vm.frames[vm.frameCount - 1];
                        break;
                    }
                    else
                    {
                        runtimeError("Class '%s' is not subscriptable.\nHINT -- You would need to override the <_get_(i)> method", AS_INSTANCE(listVal)->klass->name->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }
                runtimeError("'%s' is not subsciptable", VALUE_TYPES[listVal.type]);
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjList *list = AS_LIST(listVal);
            bool hadError = false;
            if (IS_NUM(indexVal))
            {
                hadError = indexByNum(indexVal, list);
            }
            else if (IS_SLICE(indexVal))
            {
                indexBySlice(indexVal, list);
            }
            else
            {
                runtimeError("Expected number or slice as list index but got '%s'", VALUE_TYPES[indexVal.type]);
                return INTERPRET_RUNTIME_ERROR;
            }
            if (hadError)
                return INTERPRET_RUNTIME_ERROR;
            break;
        }
        case OP_STORE_SUBSCR:
        {
            Value itemVal = pop();
            Value indexVal = pop();
            Value listVal = pop();
            if (!IS_LIST(listVal))
            {
                if (IS_INSTANCE(listVal))
                {
                    if (!IS_NIL(AS_INSTANCE(listVal)->klass->setFn))
                    {
                        push(listVal);
                        push(indexVal);
                        push(itemVal);
                        if (!invoke(vm.setStr, 2))
                            return INTERPRET_RUNTIME_ERROR;
                        frame = &vm.frames[vm.frameCount - 1];
                        break;
                    }
                    else
                    {
                        runtimeError("Class '%s' is not subscriptable.\nHINT -- You would need to override the <_set_(i, v)> method", AS_INSTANCE(listVal)->klass->name->chars);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }
                runtimeError("'%s' is not subsciptable", VALUE_TYPES[listVal.type]);
                return INTERPRET_RUNTIME_ERROR;
            }

            ObjList *list = AS_LIST(listVal);

            if (!IS_NUM(indexVal))
            {
                runtimeError("Expected number as list index but got '%s'", VALUE_TYPES[indexVal.type]);
                return INTERPRET_RUNTIME_ERROR;
            }

            int index = AS_NUM(indexVal);

            if (!isValidListIndex(list, index))
            {
                runtimeError("List index out of range. List has size %d. However, %d was provided", list->count, index);
                return INTERPRET_RUNTIME_ERROR;
            }
            storeToList(list, getValidListIndex(list, index), itemVal);
            push(itemVal);
            break;
        }
        case OP_INHERIT:
        {
            Value superclass = peek(1);
            if (!IS_CLASS(superclass))
            {
                runtimeError("Superclass must be a class");
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjClass *subclass = AS_CLASS(peek(0));
            subclass->initializer = AS_CLASS(superclass)->initializer;
            subclass->toStr = AS_CLASS(superclass)->toStr;
            subclass->equals = AS_CLASS(superclass)->equals;
            subclass->lessThan = AS_CLASS(superclass)->lessThan;
            subclass->greaterThan = AS_CLASS(superclass)->greaterThan;
            subclass->indexFn = AS_CLASS(superclass)->indexFn;
            subclass->setFn = AS_CLASS(superclass)->setFn;
            subclass->sizeFn = AS_CLASS(superclass)->sizeFn;
            subclass->superclass = AS_CLASS(superclass);
            tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
            tableAddAll(&AS_CLASS(superclass)->staticVars, &subclass->staticVars);

            pop();
            break;
        }
        case OP_GET_SUPER:
        {
            ObjString *name = READ_STRING();
            ObjClass *super = AS_CLASS(pop());
            if (!bindMethod(super, name))
                return INTERPRET_RUNTIME_ERROR;
            break;
        }
        case OP_DEF_GLOBAL:
        {
            tableSet(&vm.globals, READ_STRING(), peek(0));
            pop();
            break;
        }
        case OP_STATIC_VAR:
        {
            ObjClass *c = AS_CLASS(peek(1));

            tableSet(&c->staticVars, READ_STRING(), peek(0));
            pop();
            break;
        }
        case OP_GET_LOCAL:
        {
            uint16_t slot = isWide() ? READ_SHORT() : READ_BYTE();
            push(frame->slots[slot]);
            break;
        }
        case OP_SET_LOCAL:
        {
            uint16_t slot = isWide() ? READ_SHORT() : READ_BYTE();
            frame->slots[slot] = peek(0);
            break;
        }
        case OP_GET_GLOBAL:
        {
            ObjString *name = READ_STRING();

            Value value;
            if (!tableGet(&vm.globals, name, &value))
            {
                runtimeError("Undefined variable '%s'.", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            push(value);
            break;
        }
        case OP_SET_GLOBAL:
        {
            ObjString *name = READ_STRING();
            if (tableSet(&vm.globals, name, peek(0)))
            {
                tableDelete(&vm.globals, name);
                runtimeError("Undefined variable '%s'.", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_GET_PROPERTY:
        {
            if (!IS_INSTANCE(peek(0)) && !IS_CLASS(peek(0)))
            {
                runtimeError("Cannot dereference %s, only Classes and their instances can be dereferenced.", VALUE_TYPES[peek(0).type]);
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjString *name = READ_STRING();
            ObjClass *klass;
            Table *table;
            if (IS_INSTANCE(peek(0)))
            {
                ObjInstance *instance = AS_INSTANCE(peek(0));
                klass = instance->klass;
                table = &instance->fields;
                // tableAddAll(&klass->staticVars, &instance->fields); TODO:
            }
            else
            {
                klass = AS_CLASS(peek(0));
                table = &klass->staticVars;
            }
            Value s = peek(0);
            Value value;
            if (tableGet(table, name, &value))
            {
                pop(); // instance or class off the stack
                push(value);
                break;
            }
            if (!bindMethod(klass, name))
                return INTERPRET_RUNTIME_ERROR;
            break;
        }
        case OP_SET_PROPERTY:
        {
            if (!IS_INSTANCE(peek(1)) && !IS_CLASS(peek(1)))
            {
                runtimeError("%s Doesn't have any fields -- only custom Classes and their Instances can have fields.");
                return INTERPRET_RUNTIME_ERROR;
            }
            Table *table;
            ObjString *name = READ_STRING();
            Value s0 = peek(0);
            Value s1 = peek(1);
            if (IS_INSTANCE(peek(1)))
            {
                ObjInstance *instance = AS_INSTANCE(peek(1));
                table = &instance->fields;
                Value v;
                if (tableGet(&instance->klass->staticVars, name, &v))
                {
                    // tableSet(table, name, peek(0));
                    table = &instance->klass->staticVars;
                }
            }
            else
            {
                ObjClass *klass = AS_CLASS(peek(1));
                table = &klass->staticVars;
            }

            tableSet(table, name, peek(0));
            Value value = pop();
            pop();
            push(value);
            break;
        }
        case OP_GET_UPVALUE:
        {
            uint8_t slot = READ_BYTE();
            push(*frame->closure->upvalues[slot]->location);
            break;
        }
        case OP_SET_UPVALUE:
        {
            uint8_t slot = READ_BYTE();
            *frame->closure->upvalues[slot]->location = peek(0);
            break;
        }
        case OP_CLOSE_UPVALUE:
        {
            closeUpvalues(vm.stackTop - 1);
            pop();
            break;
        }
        case OP_PRINT:
        {
            Value val = peek(0);
            if (IS_OBJ(val) && AS_OBJ(val)->type == OBJ_INSTANCE && !IS_NIL(AS_INSTANCE(val)->klass->toStr))
            {
                *(frame->ip)--;
                if (!invoke(vm.toStr, 0))
                    return INTERPRET_RUNTIME_ERROR;
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }

            printValue(pop(), PRINT_VERBOSE_OBJECTS_DEPTH);

            printf("\n");
            break;
        }
        case OP_ADD:
        {
            if (IS_STR(peek(1)))
            {
                if (IS_STR(peek(0)))
                    concatenate();
                else if (IS_INSTANCE(peek(0)) && !IS_NIL(AS_INSTANCE(peek(0))->klass->toStr))
                {
                    *(frame->ip)--;
                    if (!invoke(vm.toStr, 0))
                        return INTERPRET_RUNTIME_ERROR;
                    frame = &vm.frames[vm.frameCount - 1];
                    break;
                }
                else
                {
                    Value notStr = pop();
                    ObjString *str = AS_STR(pop());

                    char valStr[10000] = {0};
                    int len = 0;
                    valueToString(notStr, valStr, &len);
                    push(OBJ_VAL(addTwoStrings(str->chars, valStr, str->len, len))); // strlen(valStr))));
                    break;
                }
            }
            else if (IS_STR(peek(0)))
            {
                // rotateStack();
                if (prefixStr.type != VAL_NIL)
                {
                    push(prefixStr);
                    prefixStr = NIL_VAL;
                }
                if (IS_INSTANCE(peek(1)) && !IS_NIL(AS_INSTANCE(peek(1))->klass->toStr))
                {
                    prefixStr = pop();
                    *(frame->ip)--;
                    if (!invoke(vm.toStr, 0))
                        return INTERPRET_RUNTIME_ERROR;
                    frame = &vm.frames[vm.frameCount - 1];
                    // push(s);
                    break;
                }
                else
                {
                    // rotateStack();
                    ObjString *str = AS_STR(pop());
                    Value notStr = pop();

                    char valStr[10000] = {0};
                    int len = 0;
                    valueToString(notStr, valStr, &len);
                    push(OBJ_VAL(addTwoStrings(valStr, str->chars, len, str->len)));
                    break;
                }
            }
            else if (IS_LIST(peek(0)) && IS_LIST(peek(1)))
            {
                push(concatenateList(AS_LIST(pop()), AS_LIST(pop())));
            }
            else if (IS_NUM(peek(0)) && IS_NUM(peek(1)))
            {
                double b = AS_NUM(pop());
                double a = AS_NUM(pop());
                push(NUM_VAL(a + b));
            }
            else
            {
                runtimeError(
                    "Operands must be two numbers or two strings.");
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_BIN_AND:
        {
            if (!IS_NUM(peek(0)) || !IS_NUM(peek(1)))
            {
                runtimeError(
                    "Operands must be two numbers");
                return INTERPRET_RUNTIME_ERROR;
            }
            int b = AS_NUM(pop());
            int a = AS_NUM(pop());
            push(NUM_VAL((a & b)));
            break;
        }
        case OP_BIN_OR:
        {
            if (!IS_NUM(peek(0)) || !IS_NUM(peek(1)))
            {
                runtimeError(
                    "Operands must be two numbers");
                return INTERPRET_RUNTIME_ERROR;
            }
            int b = AS_NUM(pop());
            int a = AS_NUM(pop());
            push(NUM_VAL((a | b)));
            break;
        }
        case OP_EQUAL:
        {
            Value b = pop();
            Value a = pop();
            if (a.type == VAL_OBJ && IS_INSTANCE(a) && !IS_NIL(AS_INSTANCE(a)->klass->equals))
            {
                push(a);
                push(b);

                if (!invoke(vm.eqStr, 1))
                    return INTERPRET_RUNTIME_ERROR;
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            else
            {
                push(BOOL_VAL(valuesEqual(a, b)));
            }
            break;
        }
        case OP_NEGATE:
        {
            if (!IS_NUM(peek(0)))
            {
                runtimeError("Operand must be a number");
                return INTERPRET_RUNTIME_ERROR;
            }
            *(vm.stackTop - 1) = NUM_VAL(-AS_NUM(peek(0)));
            break;
        }
        case OP_JUMP_IF_FALSE:
        {
            uint16_t offset = READ_SHORT();
            if (isFalsey(peek(0)))
                frame->ip += offset;
            break;
        }
        case OP_JUMP:
        {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
            break;
        }
        case OP_LOOP:
        {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
            break;
        }
        case OP_CALL:
        {
            int argC = READ_BYTE();
            if (!callValue(peek(argC), argC))
                return INTERPRET_RUNTIME_ERROR;
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_INVOKE:
        {
            ObjString *methodName = READ_STRING();
            int argc = READ_BYTE();
            if (!invoke(methodName, argc))
                return INTERPRET_RUNTIME_ERROR;
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_SUPER_INVOKE:
        {
            ObjString *method = READ_STRING();
            int argc = READ_BYTE();
            ObjClass *super = AS_CLASS(pop());
            if (!invokeFromClass(super, method, argc))
                return INTERPRET_RUNTIME_ERROR;
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_CLOSURE:
        {
            ObjFunction *function = AS_FUN(READ_CONSTANT());

            ObjClosure *closure = newClosure(function);
            push(OBJ_VAL(closure));
            for (int i = 0; i < closure->upvalueCount; i++)
            {
                uint8_t isLocal = READ_BYTE();
                uint8_t index = READ_BYTE();
                if (isLocal)
                    closure->upvalues[i] = captureUpvalues(frame->slots + index);
                else
                    closure->upvalues[i] = frame->closure->upvalues[index];
            }
            break;
        }
        case OP_RETURN:
        {
            Value result = pop();
            closeUpvalues(frame->slots);
            vm.frameCount--;
            if (vm.frameCount == 0)
            {
                pop();
                return INTERPRET_OK;
            }
            vm.stackTop = frame->slots;
            push(result);
            frame = &vm.frames[vm.frameCount - 1];
            break;
        }
        case OP_CONSTANT:
        {
            Value constant = READ_CONSTANT();

            push(constant);
            break;
        }
        case OP_IMPORT:
        {
            Value file = pop();
            if (IS_STR(file))
            {
                ObjString *filePath = AS_STR(file);
                Value value;
                if (tableGet(&vm.imports, filePath, &value))
                    break;
                char *source = readFile(filePath->chars);
                if (source == NULL)
                {
                    runtimeError("Cannot import file:\"%s\"\n", filePath->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm.importSources = (char **)realloc(vm.importSources, sizeof(char *) * ++vm.importCount);
                vm.importSources[vm.importCount - 1] = source;

                CallFrame preImport = *frame;
                vm.frameCount = 0;

                InterpretResult res = interpret(source, filePath->chars, false);
                if (res == INTERPRET_RUNTIME_ERROR)
                {
                    tableDelete(&vm.imports, filePath);
                    runtimeError("Cannot import file:\"%s\" As it had the previous errors.\n", filePath->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                else if (res == INTERPRET_COMPILE_ERROR)
                {
                    tableDelete(&vm.imports, filePath);
                    runtimeError("Cannot import file:\"%s\" As it had the previous compiler errors.\n", filePath->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                tableSet(&vm.imports, filePath, NUM_VAL(1));
                vm.frameCount = 1;
                vm.frames[0] = preImport;
            }
            break;
        }
        case OP_CONSTANT_LONG:
        {
            Value constant = frame->closure->function->chunk.constants.values[(READ_BYTE() & 0xff) | ((READ_BYTE() << 8) & 0xff) | ((READ_BYTE() << 16) & 0xff)];
            push(constant);
            break;
        }
        case OP_WIDE:
            vm.nextWideOp = 2;
            break;
        default:
            break;
        }

        if (vm.nextWideOp == 1)
        {
            runtimeError("OP_WIDE was used on an invalide opcode.");
            return INTERPRET_RUNTIME_ERROR;
        }
        else if (vm.nextWideOp == 2)
        {
            vm.nextWideOp--;
        }
    }

#undef BIN_OP
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
}

InterpretResult interpret(const char *source, char *file, bool printExpressions)
{
    ObjString *file_ = copyString(file, (int)strlen(file));
    tableSet(&vm.imports, file_, NUM_VAL(0));
    ObjFunction *function = compile(source, file, printExpressions);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;
    push(OBJ_VAL(function));
    ObjClosure *closure = newClosure(function);
    // tableSet(&vm.importFuncs, file_, OBJ_VAL(closure)); This may be due to some nonesense btw I am using this table to free imports after vm is closed
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run(printExpressions);
}

inline void push(Value value)
{
    *vm.stackTop++ = value;
}

inline Value pop()
{
    return *(--vm.stackTop);
}

void rotateStack()
{
    Value a = pop();
    Value b = pop();
    push(a);
    push(b);
}
