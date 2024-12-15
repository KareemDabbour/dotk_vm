#define TB_IMPL
#include "include/vm.h"
#include "include/compiler.h"
#include "include/debug.h"
#include "include/io.h"
#include "include/memory.h"
#include "include/object.h"
#include "include/termbox2.h"
#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <regex.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

VM vm;
Value prefixStr = NIL_VAL;
FILE *file;
bool printBytecodeGlobal = false;
bool printExecStackGlobaL = false;

static bool invoke(ObjString *name, int argc);
static InterpretResult run(bool isRepl, int runUntilFrame);
static char *valueToString(Value val, char *buff, int *len);

static char *trim(char *str)
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

static void resetStack()
{
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
}

Value peek(int dist)
{
    return vm.stackTop[-1 - dist];
}

void runtimeError(const char *format, ...)
{
    // save error from format
    va_list args;
    va_start(args, format);
    char *buff = ALLOCATE(char, STR_BUFF);
    vsprintf(buff, format, args);
    vm.lastError = takeString(buff, (int)strlen(buff));
    va_end(args);
    int len = 0;
    char *trace = ALLOCATE(char, STR_BUFF);
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
            len += sprintf(trace + len, "Error in:\n");
        }

        len += sprintf(trace + len, "  %s:%d:%d", pos.file, pos.line, pos.col);
        if (function->name == NULL)
            len += sprintf(trace + len, " in script");
        else if (printFunc)
            len += sprintf(trace + len, " in %s()", function->name->chars);
        len += sprintf(trace + len, "\n");
    }
    vm.lastErrorTrace = takeString(trace, len);

    resetStack();
    if (vm.isInTryCatch)
        return;

    fprintf(stderr, "%s", vm.lastErrorTrace->chars);
    fprintf(stderr, "%s\n", vm.lastError->chars);
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
    fputs("\n", stderr);
}

static bool isBuiltinClass(Value value)
{
    return IS_OBJ(value) && (IS_LIST(value) || IS_MAP(value) || IS_STR(value)); //(AS_OBJ(value)->type & (OBJ_MAP | OBJ_STRING | OBJ_LIST)); // IS_BUILTIN(val); //
}

static bool isBuiltinClazz(ObjClass *clazz)
{
    return clazz == vm.listClass || clazz == vm.mapClass || clazz == vm.stringClass;
}

ObjClass *getVmClass(Value val)
{
    if (IS_LIST(val))
        return vm.listClass;
    if (IS_MAP(val))
        return vm.mapClass;
    if (IS_STR(val))
        return vm.stringClass;
    return NULL;
}

bool checkIfValuesEqual(Value a, Value b)
{
    if (a.type == VAL_OBJ && IS_INSTANCE(a) && !IS_NIL(AS_INSTANCE(a)->klass->equals))
    {
        int frameCount = vm.frameCount;
        push(a);
        push(b);
        if (!invoke(vm.eqStr, 1))
            return false;
        if (frameCount != vm.frameCount && run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
        {
            return false;
        }
        Value ret = pop();
        if (!IS_BOOL(ret))
            return false;
        return AS_BOOL(ret);
    }
    return valuesEqual(a, b);
}

static uint32_t hashValueShallow(Value value)
{
    switch (value.type)
    {
    case VAL_NUMBER:
        return value.as.number;
    case VAL_BOOL:
        return value.as.boolean;
    case VAL_NIL:
        return 0;
    case VAL_OBJ:
        switch (AS_OBJ(value)->type)
        {
        case OBJ_STRING:
            return AS_STR(value)->hash;
        case OBJ_LIST:
        {
            uint32_t hash = 0;
            ObjList *list = AS_LIST(value);
            for (int i = 0; i < list->count; i++)
            {
                hash += hashValueShallow(list->items[i]);
            }
            return hash;
        }
        case OBJ_CLASS:
            return AS_CLASS(value)->name->hash;
        case OBJ_INSTANCE:
            return AS_INSTANCE(value)->klass->name->hash;
        case OBJ_FUNCTION:
            return AS_FUN(value)->name == NULL ? 0 : AS_FUN(value)->name->hash;
        case OBJ_NATIVE:
            // hash the pointer
            return (uint32_t)(uintptr_t)AS_NATIVE(value);
        case OBJ_CLOSURE:
            return AS_CLOSURE(value)->function->name == NULL ? 0 : AS_CLOSURE(value)->function->name->hash;
        case OBJ_BOUND_METHOD:
            return AS_BOUND_METHOD(value)->method->function->name == NULL ? 0 : AS_BOUND_METHOD(value)->method->function->name->hash;
        case OBJ_BOUND_BUILTIN:
            // hash the pointer
            return (uint32_t)(uintptr_t)AS_BOUND_BUILTIN(value);
        case OBJ_UPVALUE:
            return (uint32_t)(uintptr_t)value.as.obj;
        case OBJ_MAP:
        {
            uint32_t hash = 0;
            ObjMap *map = AS_MAP(value);
            for (int i = 0; i < map->map.capacity; i++)
            {
                MapEntry *entry = &map->map.entries[i];
                if (entry->isUsed)
                {
                    hash += hashValueShallow(entry->key);
                    hash += hashValueShallow(entry->value);
                }
            }
            return hash;
        }
        case OBJ_SLICE:
        {
            ObjSlice *slice = AS_SLICE(value);
            return (slice->start + slice->end + slice->step) * 41;
        }
        }
    }
}

static uint32_t hashValueDeep(Value value)
{
    if (value.type == VAL_OBJ && IS_INSTANCE(value) && !IS_NIL(AS_INSTANCE(value)->klass->hashFn))
    {
        Value *stop = vm.stackTop;
        int frameCount = vm.frameCount;
        ObjUpvalue *upvalues = vm.openUpvalues;
        push(value);
        if (!invoke(vm.hashStr, 0))
            return 0;
        if (frameCount != vm.frameCount && run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
        {
            return 0;
        }
        Value ret = pop();
        if (!IS_NUM(ret))
            return 0;
        return (uint32_t)AS_NUM(ret);
    }
    else
        return hashValueShallow(value);
}

MapEntry *findMapEntry(MapEntry *entries, int capacity, Value key)
{
    uint32_t index = hashValueDeep(key) & (capacity - 1);
    for (;;)
    {
        MapEntry *entry = &entries[index];
        if (!entry->isUsed)
            return entry;
        else if (checkIfValuesEqual(entry->key, key))
            return entry;
        index = (index + 1) & (capacity - 1);
    }
}

bool mapGet(Map *map, Value key, Value *valueOut)
{
    if (map->count == 0)
        return false;
    MapEntry *entry = findMapEntry(map->entries, map->capacity, key);
    if (!entry->isUsed)
        return false;
    *valueOut = entry->value;
    return true;
}

static void adjustMapCapacity(Map *map, int capacity)
{
    MapEntry *entries = ALLOCATE(MapEntry, capacity);
    for (int i = 0; i < capacity; i++)
    {
        entries[i].key = NIL_VAL;
        entries[i].value = NIL_VAL;
        entries[i].isUsed = false;
    }
    map->count = 0;
    for (int i = 0; i < map->capacity; i++)
    {
        MapEntry *entry = &map->entries[i];
        if (!entry->isUsed)
            continue;

        MapEntry *dest = findMapEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        dest->isUsed = true;
        map->count++;
    }
    FREE_ARRAY(MapEntry, map->entries, map->capacity);
    map->entries = entries;
    map->capacity = capacity;
}

bool mapSet(Map *map, Value key, Value value)
{
    if ((map->count + 1) > (map->capacity * MAP_MAX_LOAD))
    {
        int newCap = GROW_CAPACITY(map->capacity);
        adjustMapCapacity(map, newCap);
    }

    MapEntry *entry = findMapEntry(map->entries, map->capacity, key);
    bool isNewKey = !entry->isUsed;
    if (isNewKey)
        map->count++;
    entry->key = key;
    entry->value = value;
    entry->isUsed = true;
    return isNewKey;
}

bool mapDelete(Map *map, Value key)
{
    if (map->count == 0)
        return false;
    MapEntry *entry = findMapEntry(map->entries, map->capacity, key);
    if (!entry->isUsed)
        return false;
    map->count--;
    entry->key = NIL_VAL;
    entry->value = BOOL_VAL(true);
    entry->isUsed = false;
    return true;
}

bool mapContainsKey(Map *map, Value key)
{
    if (map->count == 0)
        return false;
    MapEntry *entry = findMapEntry(map->entries, map->capacity, key);
    return entry->isUsed;
}

void mapRemoveWhite(Map *map)
{
    for (int i = 0; i < map->capacity; i++)
    {
        MapEntry *entry = &map->entries[i];
        if (entry->isUsed && !entry->key.as.obj->isMarked)
        {
            mapDelete(map, entry->key);
        }
    }
}

void printMap(Map *map)
{
    for (int i = 0; i < map->capacity; i++)
    {
        MapEntry *entry = &map->entries[i];
        if (entry->isUsed)
        {
            printf("Key: ");
            printValue(entry->key, 0);
            printf(" Value: ");
            printValue(entry->value, 0);
            printf("\n");
        }
    }
}

static char *mapToString(Map *map, char *buff, int *len)
{
    int index = 0;
    buff[index++] = '{';
    for (int i = 0; i < map->capacity; i++)
    {
        MapEntry *entry = &map->entries[i];
        if (!entry->isUsed)
            continue;
        char *key = valueToString(entry->key, buff + index, len);
        index += *len;
        buff[index++] = ':';
        char *value = valueToString(entry->value, buff + index, len);
        index += *len;
        buff[index++] = ',';
    }
    if (map->count > 0)
        buff[--index] = '}';
    else
        buff[index] = '}';
    buff[++index] = '\0';
    *len = index;
    return buff;
}

static Value clockNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    return NUM_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value fOpenNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'fopen()' expects 2 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]) || !IS_STR(argv[1]))
    {
        runtimeError("'fopen()' expects 2 strings as arguments but got '%s' and '%s'", VALUE_TYPES[argv[0].type], VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *mode = AS_CSTR(argv[1]);
    if (strcmp(mode, "r") != 0 && strcmp(mode, "w") != 0 && strcmp(mode, "a") != 0 && strcmp(mode, "r+") != 0 && strcmp(mode, "w+") != 0 && strcmp(mode, "a+") != 0)
    {
        runtimeError("Invalid mode '%s' for 'fopen()'", mode);
        *hasError = true;
        return NIL_VAL;
    }
    FILE *file = fopen(AS_CSTR(argv[0]), mode);
    if (file == NULL)
    {
        runtimeError("Could not open file '%s'", AS_CSTR(argv[0]));
        *hasError = true;
        return NIL_VAL;
    }

    return OBJ_VAL(newForeignObj(TYPE_FILE, file, false));
}

static Value fCloseNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (argc != 1)
    {
        runtimeError("'fclose()' expects 1 argument but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_FOREIGN_TYPE(argv[0], TYPE_FILE))
    {
        runtimeError("'fclose()' expects a file pointer as argument but got '%s'", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    FILE *file = AS_FOREIGN_PTR(argv[0]);

    return NUM_VAL(fclose(file));
}

static Value offsetNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    //     if (argc != 2)
    //     {
    //         runtimeError("'offset()' expects 2 arguments but %d were passed in", argc);
    //         *hasError = true;
    //         return NIL_VAL;
    //     }
    //     if (!IS_POINTER(argv[0]) || !IS_NUM(argv[1]))
    //     {
    //         runtimeError("'offset()' expects a foriegh pointer and a number as arguments but got '%s' and '%s'", VALUE_TYPES[argv[0].type], VALUE_TYPES[argv[1].type]);
    //         *hasError = true;
    //         return NIL_VAL;
    //     }
    //     void *p = AS_POINTER(argv[0]);
    //     long offset = (long)AS_NUM(argv[1]);
    //     return POINTER_VAL(p + offset * sizeof(char));
    return NIL_VAL;
}

NATIVE_FN(testRinitNative)
{
    int ret = tb_init();
    printf("WOP\n");
    return NUM_VAL(ret);
    // POINTER_VAL(vm.stackTop);
}

NATIVE_FN(testRClearNative)
{
    int ret = tb_clear();
    return NUM_VAL(ret);
}

NATIVE_FN(testRpresentNative)
{
    int ret = tb_present();
    return NUM_VAL(ret);
}

NATIVE_FN(testRcloseNative)
{
    int ret = tb_shutdown();

    return NUM_VAL(ret);
}

NATIVE_FN(testRPresentNative)
{
    int ret = tb_present();
    return NUM_VAL(ret);
}

NATIVE_FN(testRpollNative)
{
    struct tb_event ev;
    int ret = tb_poll_event(&ev);

    ObjInstance *evObj = newInstance(vm.baseObj);
    tableSet(&evObj->fields, copyString("type", 4), NUM_VAL(ev.type));
    tableSet(&evObj->fields, copyString("mod", 3), NUM_VAL(ev.mod));
    tableSet(&evObj->fields, copyString("key", 3), NUM_VAL(ev.key));
    tableSet(&evObj->fields, copyString("ch", 2), NUM_VAL(ev.ch));
    tableSet(&evObj->fields, copyString("w", 1), NUM_VAL(ev.w));
    tableSet(&evObj->fields, copyString("h", 1), NUM_VAL(ev.h));
    tableSet(&evObj->fields, copyString("x", 1), NUM_VAL(ev.x));
    tableSet(&evObj->fields, copyString("y", 1), NUM_VAL(ev.y));
    return OBJ_VAL(evObj);
}

NATIVE_FN(testRprintNative)
{
    // get args (x,y, foreground, background, text)
    if (argc != 5)
    {
        runtimeError("'print()' expects 5 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_NUM(argv[0]) || !IS_NUM(argv[1]) || !IS_NUM(argv[2]) || !IS_NUM(argv[3]) || !IS_STR(argv[4]))
    {
        runtimeError("'print()' expects 5 numbers and a string as arguments but got '%s', '%s', '%s', '%s', '%s'", VALUE_TYPES[argv[0].type], VALUE_TYPES[argv[1].type], VALUE_TYPES[argv[2].type], VALUE_TYPES[argv[3].type], VALUE_TYPES[argv[4].type]);
        *hasError = true;
        return NIL_VAL;
    }

    int x = (int)AS_NUM(argv[0]);
    int y = (int)AS_NUM(argv[1]);
    int fg = (uint16_t)AS_NUM(argv[2]);
    int bg = (uint16_t)AS_NUM(argv[3]);
    char *text = AS_CSTR(argv[4]);

    int ret = tb_print(x, y, fg, bg, text);
    return NUM_VAL(ret);
}
NATIVE_FN(testRsetCellNative)
{
    // get args (x,y, foreground, background, text)
    if (argc != 5)
    {
        runtimeError("'setCell()' expects 5 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_NUM(argv[0]) || !IS_NUM(argv[1]) || !IS_NUM(argv[2]) || !IS_NUM(argv[3]) || !IS_NUM(argv[4]))
    {
        runtimeError("'setCell()' expects 5 numbers as arguments but got '%s', '%s', '%s', '%s', '%s'", VALUE_TYPES[argv[0].type], VALUE_TYPES[argv[1].type], VALUE_TYPES[argv[2].type], VALUE_TYPES[argv[3].type], VALUE_TYPES[argv[4].type]);
        *hasError = true;
        return NIL_VAL;
    }

    int x = (int)AS_NUM(argv[0]);
    int y = (int)AS_NUM(argv[1]);
    int ch = (uint32_t)AS_NUM(argv[2]);
    int fg = (uint16_t)AS_NUM(argv[3]);
    int bg = (uint16_t)AS_NUM(argv[4]);

    int ret = tb_set_cell(x, y, ch, fg, bg);
    return NUM_VAL(ret);
}

// static Value testRcloseNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     CloseWindow();
//     return NIL_VAL;
// }

// static Value testRbeginNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     BeginDrawing();
//     return NIL_VAL;
// }

// static Value testRendNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     EndDrawing();
//     return NIL_VAL;
// }

// static Value testRdrawNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     ClearBackground(RAYWHITE);
//     DrawText("Congrats! You created your first window!", 190, 200, 20, LIGHTGRAY);
//     return NIL_VAL;
// }

NATIVE_FN(fileExistsNative)
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

NATIVE_FN(fileSizeNative)
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

NATIVE_FN(fileClassExistsNative)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for fileClassExists(path)", argc);
        *hasError = true;
        return BOOL_VAL(false);
    }
    if (!IS_CLASS(peek(argc)))
    {
        runtimeError("Expected a File to be caller but got '%s' for File.exists()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }

    char *path = AS_CSTR(argv[0]);
    struct stat buffer;
    return BOOL_VAL(stat(path, &buffer) == 0);
}

NATIVE_FN(fileFSizeNative)
{
    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.size()' expects a file pointer as argument but got '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjForeign *f = AS_FOREIGN(peek(argc));
    Value size;
    if (tableGet(&f->fields, copyString("size", 4), &size))
    {
        return size;
    }

    FILE *fp = f->ptr;
    if (fseek(fp, 0, SEEK_END) < 0)
    {
        runtimeError("'File.close()' expects a file pointer as argument but got '%s'", VALUE_TYPES[peek(argc).type]);
        tableSet(&f->fields, copyString("isClosed", 8), BOOL_VAL(true));
        fclose(fp);
        *hasError = true;
        return NIL_VAL;
    }
    long f_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    size = NUM_VAL(f_size);
    tableSet(&f->fields, copyString("size", 4), size);
    return size;
}

NATIVE_FN(fileCloseNative)
{
    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.close()' expects a file pointer as argument but got '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjForeign *file = AS_FOREIGN(peek(argc));
    Value closed = BOOL_VAL(true);
    if (!tableGet(&file->fields, copyString("isClosed", 8), &closed))
    {
        runtimeError("Could not check if file has been closed or not");
        *hasError = true;
        return NIL_VAL;
    }

    if (AS_BOOL(closed))
        return closed;

    closed = BOOL_VAL(true);

    tableSet(&file->fields, copyString("isClosed", 8), closed);
    fclose(file->ptr);
    file->ptr = NULL;
    return closed;
}

NATIVE_FN(fileResetCursorNative)
{
    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.resetCursor()' expects a file pointer to be it's caller but got called by: '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjForeign *file = AS_FOREIGN(peek(argc));
    Value name = OBJ_VAL(copyString("unknown", 7));
    tableGet(&file->fields, copyString("name", 4), &name);

    if (file->ptr == NULL)
    {
        runtimeError("Trying to get cursor position of a closed file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }

    if (fseek(file->ptr, 0, SEEK_SET) != 0)
    {
        runtimeError("Failed to reset cursor for file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    long position = ftell(file->ptr);
    if (position == -1)
    {
        runtimeError("Failed to get cursor position");
        *hasError = true;
        return NIL_VAL;
    }

    return NUM_VAL((double)position);
}

NATIVE_FN(fileCursorPosition)
{
    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.cursorPosition()' expects a file pointer to be its caller but got called by: '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjForeign *file = AS_FOREIGN(peek(argc));
    FILE *fp = file->ptr;

    if (fp == NULL)
    {
        runtimeError("Trying to get cursor position of a closed file");
        *hasError = true;
        return NIL_VAL;
    }

    long position = ftell(fp);
    if (position == -1)
    {
        runtimeError("Failed to get cursor position");
        *hasError = true;
        return NIL_VAL;
    }

    return NUM_VAL((double)position);
}

static Value fileWriteNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'File.write()' expects 1 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.write()' expects a file pointer to be it's caller but got called by: '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjForeign *file = AS_FOREIGN(peek(argc));
    Value mode;

    Value name = OBJ_VAL(copyString("unknown", 7));
    tableGet(&file->fields, copyString("name", 4), &name);

    if (!tableGet(&file->fields, copyString("mode", 4), &mode))
    {
        runtimeError("Could not get mode of file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    if (AS_CSTR(mode)[1] != '+' && AS_CSTR(mode)[0] == 'r')
    {
        runtimeError("Trying to write to a file opened in read only mode (You might want to open the file with a mode of 'r+', 'w', 'w+', 'a' or 'a+' instead)");
        *hasError = true;
        return NIL_VAL;
    }

    int strLen = 0;
    char str[STR_BUFF] = {0};
    valueToString(argv[0], str, &strLen);

    FILE *fp = file->ptr;
    if (fp == NULL)
    {

        runtimeError("Trying to write to a closed file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    if (AS_CSTR(mode)[0] == 'a')
    {
        fseek(fp, 0, SEEK_END);
    }
    size_t written = fwrite(str, sizeof(char), strLen / sizeof(char), fp);
    if (written != strLen)
    {
        runtimeError("Failed to write to file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    fflush(fp);
    return NUM_VAL((long)written);
}

static Value fileReadNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{

    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.read()' expects a file pointer to be it's caller but got called by: '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjForeign *file = AS_FOREIGN(peek(argc));

    Value name = OBJ_VAL(copyString("unknown", 7));
    tableGet(&file->fields, copyString("name", 4), &name);
    Value mode;
    if (!tableGet(&file->fields, copyString("mode", 4), &mode))
    {
        runtimeError("Could not get mode of file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    if (AS_CSTR(mode)[1] != '+' && AS_CSTR(mode)[0] == 'w')
    {
        runtimeError("Trying to read from a file opened in write only mode (You might want to open the file with a mode of 'w+', 'r', 'r+', 'a' or 'a+' instead)");
        *hasError = true;
        return NIL_VAL;
    }

    FILE *fp = file->ptr;
    if (fp == NULL)
    {
        runtimeError("Trying to read from a closed file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    fseek(fp, 0, SEEK_END);
    long f_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buff = ALLOCATE(char, f_size + 1);
    size_t read = fread(buff, sizeof(char), f_size, fp);
    buff[read] = '\0';
    Value str = OBJ_VAL(copyString(buff, (int)read));
    FREE_ARRAY(char, buff, f_size + 1);
    return str;
}

static Value fileReadLineNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.readLine()' expects a file pointer to be it's caller but got called by: '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjForeign *file = AS_FOREIGN(peek(argc));

    Value name = OBJ_VAL(copyString("unknown", 7));
    tableGet(&file->fields, copyString("name", 4), &name);
    Value mode;
    if (!tableGet(&file->fields, copyString("mode", 4), &mode))
    {
        runtimeError("Could not get mode of file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    if (AS_CSTR(mode)[1] != '+' && AS_CSTR(mode)[0] == 'w')
    {
        runtimeError("Trying to read from a file opened in write only mode (You might want to open the file with a mode of 'w+', 'r', 'r+', 'a' or 'a+' instead)");
        *hasError = true;
        return NIL_VAL;
    }

    FILE *fp = file->ptr;
    if (fp == NULL)
    {
        runtimeError("Trying to read from a closed file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    char *line = NULL;
    size_t len = 0;
    ssize_t read = getline(&line, &len, fp);
    if (read == -1)
    {
        free(line);
        return NIL_VAL;
    }
    Value str = OBJ_VAL(copyString(line, (int)read - 1));
    free(line);
    return str;
}

static Value fileReadLinesNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (!IS_FOREIGN_TYPE(peek(argc), TYPE_FILE))
    {
        runtimeError("'File.readLines(<optional: false> includeEmptyLines)' expects a file pointer to be it's caller but got called by: '%s'", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    bool includeEmptyLines = false;

    if (argc == 1)
    {
        if (!IS_BOOL(argv[0]))
        {
            runtimeError("'File.readLines(optional<includeEmptyLines: false>)' expects a boolean an argument but got '%s'", VALUE_TYPES[peek(argc).type]);
            *hasError = true;
            return NIL_VAL;
        }
        includeEmptyLines = AS_BOOL(argv[0]);
    }
    ObjForeign *file = AS_FOREIGN(peek(argc));

    Value name = OBJ_VAL(copyString("unknown", 7));
    tableGet(&file->fields, copyString("name", 4), &name);
    Value mode;
    if (!tableGet(&file->fields, copyString("mode", 4), &mode))
    {
        runtimeError("Could not get mode of file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    if (AS_CSTR(mode)[1] != '+' && AS_CSTR(mode)[0] == 'w')
    {
        runtimeError("Trying to read from a file opened in write only mode (You might want to open the file with a mode of 'w+', 'r', 'r+', 'a' or 'a+' instead)");
        *hasError = true;
        return NIL_VAL;
    }

    FILE *fp = file->ptr;
    if (fp == NULL)
    {
        runtimeError("Trying to read from a closed file '%s'", AS_CSTR(name));
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *lines = newList();

    char *line = NULL;
    size_t len = 0;
    ssize_t read = getline(&line, &len, fp);
    while (read != -1)
    {
        Value str = OBJ_VAL(copyString(line, (int)read));
        if (includeEmptyLines || *trim(AS_CSTR(str)) != '\0')
            appendToList(lines, str);

        read = getline(&line, &len, fp);
    }

    free(line);
    return OBJ_VAL(lines);
}

NATIVE_FN(fileInitNative)
{
    if (argc != 1 && argc != 2)
    {
        runtimeError("'File(<filepath>, optional<mode: 'r' >)' expects 1 or 2 argument(s) but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_STR(argv[0]) || (argc == 2 && !IS_STR(argv[1])))
    {
        runtimeError("'File(<file>, optional<mode>)' expects 2 strings as arguments but got '%s' and '%s'", VALUE_TYPES[argv[0].type], VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *mode = "r";
    if (argc == 2)
    {
        mode = AS_CSTR(argv[1]);
        if (strcmp(mode, "r") != 0 && strcmp(mode, "w") != 0 && strcmp(mode, "a") != 0 && strcmp(mode, "r+") != 0 && strcmp(mode, "w+") != 0 && strcmp(mode, "a+") != 0 && strcmp(mode, "rw") != 0 && strcmp(mode, "rw+") != 0)
        {
            runtimeError("Invalid mode '%s' for 'File(<file>, optional<mode>)'\nNOTE: Valid options are: 'r', 'w' and 'a'. You can add a '+' proceeding any mode to make create a file if it's non-exsistant.", mode);
            *hasError = true;
            return NIL_VAL;
        }
    }
    FILE *file = fopen(AS_CSTR(argv[0]), mode);
    if (file == NULL)
    {
        runtimeError("Could not open file '%s'", AS_CSTR(argv[0]));
        *hasError = true;
        return NIL_VAL;
    }
    ObjForeign *f = newForeignObj(TYPE_FILE, file, false);
    tableSet(&f->fields, copyString("mode", 4), OBJ_VAL(copyString(mode, (int)strlen(mode))));
    tableSet(&f->fields, copyString("name", 4), argv[0]);
    tableSet(&f->fields, copyString("isClosed", 8), BOOL_VAL(false));

    tableSet(&f->methods, copyString("close", 5), OBJ_VAL(newNative(fileCloseNative)));
    tableSet(&f->methods, copyString("size", 4), OBJ_VAL(newNative(fileFSizeNative)));
    tableSet(&f->methods, copyString("write", 5), OBJ_VAL(newNative(fileWriteNative)));
    tableSet(&f->methods, copyString("read", 4), OBJ_VAL(newNative(fileReadNative)));
    tableSet(&f->methods, copyString("readLine", 8), OBJ_VAL(newNative(fileReadLineNative)));
    tableSet(&f->methods, copyString("readLines", 9), OBJ_VAL(newNative(fileReadLinesNative)));
    tableSet(&f->methods, copyString("resetCursor", 11), OBJ_VAL(newNative(fileResetCursorNative)));
    tableSet(&f->methods, copyString("cursorPos", 9), OBJ_VAL(newNative(fileCursorPosition)));

    return OBJ_VAL(f);
}

// NATIVE_FN(regexExecNative)
// {
//     //     if (argc != 2)
//     //     {
//     //         runtimeError("'Regex.exec(<string>)' expects 2 arguments but %d were passed in", argc);
//     //         *hasError = true;
//     //         return NIL_VAL;
//     //     }
//     //     if (!IS_STR(argv[0]) || !IS_STR(argv[1]))
//     //     {
//     //         runtimeError("'Regex.exec(<string>)' expects 2 strings as arguments but got '%s' and '%s'", VALUE_TYPES[argv[0].type], VALUE_TYPES[argv[1].type]);
//     //         *hasError = true;
//     //         return NIL_VAL;
//     //     }
//     //     ObjString *patternObj = AS_STR(argv[0]);
//     //     const char *pattern = AS_CSTR(argv[0]);
//     //     regex_t *re = &AS_FOREIGN(argv[-1])->as.regex;
//     //     const char *text = AS_CSTR(argv[1]);
//     //     regmatch_t matches[1];
//     //     int ret = regexec(re, text, 1, matches, 0);
//     //     if (ret != 0)
//     //     {
//     //         return NIL_VAL;
//     //     }
//     //     int start = matches[0].rm_so;
//     //     int end = matches[0].rm_eo;
//     //     return OBJ_VAL(takeSlice(AS_STR(argv[1]), start, end));
// }

// NATIVE_FN(regexInitForeign)
// {
//     regex_t ree;
//     ObjString *patternObj = NULL;
//     if (argc == 1)
//     {
//         if (!IS_STR(argv[0]))
//         {
//             runtimeError("'Regex(<pattern>)' expects a string as argument but got '%s'", VALUE_TYPES[argv[0].type]);
//             *hasError = true;
//             return NIL_VAL;
//         }
//         patternObj = AS_STR(argv[0]);
//         const char *pattern = AS_CSTR(argv[0]);
//         int ret = regcomp(&ree, pattern, REG_EXTENDED);
//         if (ret != 0)
//         {
//             char *err = ALLOCATE(char, 1024);
//             regerror(ret, &ree, err, 1024);
//             runtimeError("Failed to compile regex pattern: %s", err);
//             FREE_ARRAY(char, err, 1024);
//             *hasError = true;
//             return NIL_VAL;
//         }
//     }

//     ObjForeign *r = newForeignObj(TYPE_REGEX, &ree, false);
//     tableSet(&r->fields, copyString("pattern", 7), OBJ_VAL(patternObj));
// }

NATIVE_FN(getcNative)
{
    int i = argc == 1 ? AS_NUM(argv[0]) : 0;
    return NUM_VAL(getc(stdin));
}

NATIVE_FN(kbhitNative)
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

static Value instanceOf(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'instanceof' expects 2 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    ObjClass *builtinClass = NULL;
    if (!IS_CLASS(argv[1]))
    {
        runtimeError("'instanceof' expects a class as second argument but got '%s'", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(argv[0]))
    {
        if (isBuiltinClass(argv[0]))
        {
            builtinClass = getVmClass(argv[0]);
        }
        else if (IS_CLASS(argv[0]))
        {
            return BOOL_VAL(AS_CLASS(argv[0]) == AS_CLASS(argv[1]));
        }
        else
        {
            return BOOL_VAL(false);
        }
    }

    // if (builtinClass != NULL)
    // {
    //     return BOOL_VAL(builtinClass == AS_CLASS(argv[1]));
    // }

    ObjInstance *instance = AS_INSTANCE(argv[0]);
    ObjClass *klass = AS_CLASS(argv[1]);
    ObjClass *tempClass = builtinClass == NULL ? instance->klass : builtinClass;
    while (tempClass != NULL)
    {
        if (tempClass == klass)
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
        type = val.as.number == round(val.as.number) ? "int" : "float";
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
        case OBJ_MAP:
            type = "map";
            break;
        case OBJ_CLASS:
            type = "class";
            break;
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
        case OBJ_BOUND_BUILTIN:
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

static Value gcNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    int before = vm.bytesAllocated;
    collectGarbage();
    return NUM_VAL((double)(before - vm.bytesAllocated));
}

NATIVE_FN(vmStatsNative)
{
    ObjInstance *stats = newInstance(vm.baseObj);
    tableSet(&stats->fields, copyString("bytesAllocated", 14), NUM_VAL((double)vm.bytesAllocated));
    tableSet(&stats->fields, copyString("nextGC", 6), NUM_VAL((double)vm.nextGC));
    return OBJ_VAL(stats);
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
    if (IS_MAP(argv[0]))
        return NUM_VAL((double)AS_MAP(argv[0])->map.count);
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
    if (round(AS_NUM(argv[0])) == AS_NUM(argv[0]))
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

char *valueToString(Value val, char *buff, int *len)
{
    switch (val.type)
    {
    case VAL_NUMBER:
        sprintf(buff, "%.15g", AS_NUM(val));
        *len = (int)strlen(buff);
        return buff;
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
                valueToString(list->items[i], buff + index, len);
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
        case OBJ_MAP:
        {
            ObjMap *map = AS_MAP(val);
            return mapToString(&map->map, buff, len);
        }
        case OBJ_FOREIGN:
        {
            ObjForeign *obj = AS_FOREIGN(val);
            sprintf(buff, "%s f<%p>", FOREIGN_TYPES[obj->type], obj->ptr);
            *len = (int)strlen(buff);
            return buff;
        }
        case OBJ_CLASS:
            sprintf(buff, "%s", AS_CLASS(val)->name->chars);
            *len = (int)strlen(buff);
            return buff;
        case OBJ_INSTANCE:
            // Use toStr if available?
            if (!IS_NIL(AS_INSTANCE(val)->klass->toStr))
            {
                // Make sure that the val is on the stack
                Value stackTop = peek(0);
                if (stackTop.type != VAL_OBJ || stackTop.as.obj != val.as.obj)
                    push(val);

                int frameCount = vm.frameCount;

                if (!invoke(vm.toStr, 0))
                {
                    *len = 0;
                    return buff;
                }
                bool invokedNative = frameCount == vm.frameCount;
                if (!invokedNative && run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
                {
                    *len = 0;
                    return buff;
                }
                Value str = invokedNative ? peek(0) : pop();
                if (!IS_STR(str))
                {
                    *len = 0;
                    return buff;
                }
                sprintf(buff, "%s", AS_STR(str)->chars);
                *len = AS_STR(str)->len;
                return buff;
            }
            sprintf(buff, "%s<%p>", AS_INSTANCE(val)->klass->name->chars, (void *)AS_INSTANCE(val));
            *len = (int)strlen(buff);
            return buff;
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
        case OBJ_BOUND_BUILTIN:
            sprintf(buff, "%s", "<bound native>");
            *len = (int)strlen(buff);
            return buff;
        case OBJ_UPVALUE:
            sprintf(buff, "%s", "<upvalue>");
            *len = (int)strlen(buff);
            return buff;
        case OBJ_SLICE:
        {
            ObjSlice *slice = AS_SLICE(val);
            sprintf(buff, "<slice[%d:%d:%d]>", slice->start, slice->end, slice->step);
            *len = (int)strlen(buff);
            return buff;
        }
        }
    }
    snprintf(buff, 10, "%s", "<unknown>");
    *len = (int)strnlen(buff, 10);
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
    char str[STR_BUFF] = {0};
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

    // if (IS_INSTANCE(argv[0]) && !IS_NIL(AS_INSTANCE(argv[0])->klass->hashFn))
    // {
    //     vm.stackTop -= 2;
    //     push(argv[0]);
    //     if (!invoke(vm.hashStr, 0))
    //     {
    //         *hasError = true;
    //         return NIL_VAL;
    //     }
    //     *pushedValue = true;
    //     return NIL_VAL;
    // }
    // ObjString *str;
    // if (!IS_STR(argv[0]))
    // {
    //     char s[STR_BUFF] = {0};
    //     int len = 0;
    //     valueToString(argv[0], s, &len);
    //     str = copyString(s, len);
    // }
    // else
    //     str = AS_STR(argv[0]);
    return NUM_VAL(hashValueDeep(argv[0]));
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
    // char str[STR_BUFF] = {0}; // ALLOCATE(char, 100);
    int strAlloc = 100;
    char *str = ALLOCATE(char, strAlloc);
    str[0] = '\0';
    int needed = 1;
    // sprintf(str, "%s", "");
    for (int i = 0; i < list->count; i++)
    {
        int len = 0;
        char item[STR_BUFF] = {0};
        valueToString(list->items[i], item, &len);
        needed += len + sep->len;

        if (needed >= strAlloc)
        {
            int old = strAlloc;
            strAlloc = (needed + 0.5 * needed);
            str = GROW_ARRAY(char, str, old, strAlloc);
        }

        sprintf(str, "%s%s%s", str, item, sep->chars);
    }
    if (list->count > 0)
        str[needed - sep->len - 1] = '\0';
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
        return NUM_VAL(round(AS_NUM(argv[0])));
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
    if (vm.frameCount > 1)
        vm.frameCount--;
    if (!IS_STR(argv[0]))
    {
        char buff[STR_BUFF] = {0};
        int len = 0;
        valueToString(argv[0], buff, &len);
        runtimeError(buff);
    }
    else
    {
        runtimeError(AS_CSTR(argv[0]));
    }
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
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
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
        freeaddrinfo(res);
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
        freeaddrinfo(res);
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
    u_int16_t port = (u_int16_t)AS_NUM(argv[1]);
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
    tableSet(&vm.globals, AS_STR(peek(1)), peek(0));
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
        case OBJ_BOUND_BUILTIN:
        {
            ObjBoundBuiltin *bound = AS_BOUND_BUILTIN(callee);
            vm.stackTop[-argC - 1] = bound->receiver;
            bool hasError = false;
            bool pushedValue = false;
            Value result = bound->native->function(argC, vm.stackTop - argC, &hasError, &pushedValue);
            if (hasError)
                return false;
            if (!pushedValue)
            {
                vm.stackTop -= argC + 1;
                push(result);
            }
            return true;
        }
        case OBJ_CLASS:
        {
            ObjClass *klass = AS_CLASS(callee);
            vm.stackTop[-argC - 1] = OBJ_VAL(newInstance(klass));
            if (!IS_NIL(klass->initializer))
            {
                return callValue(klass->initializer, argC);
            }
            else if (argC != 0)
            {
                runtimeError("Expected 0 arguments for init of class '%s' but got %d", klass->name->chars, argC);
                return false;
            }
            return true;
        }
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
        case OBJ_FOREIGN:
        {
            ObjForeign *foreign = AS_FOREIGN(callee);
            Value initMethod;
            if (!tableGet(&foreign->methods, vm.initStr, &initMethod))
            {
                runtimeError("This foreign object %s does not have an init method", FOREIGN_TYPES[foreign->type]);
                return false;
            }
            vm.stackTop[-argC - 1] = callee;
            return call(AS_CLOSURE(initMethod), argC);
        }
        case OBJ_CLOSURE:
            return call(AS_CLOSURE(callee), argC);
        default:
            break;
        }
    }
    runtimeError("Can only call functions and classes -- not '%s'", VALUE_TYPES[callee.type]);
    return false;
}

static bool invokeFromClass(ObjClass *klass, ObjString *name, int argc)
{
    if (klass == NULL)
        return false;

    Value method;
    if (!tableGet(&klass->methods, name, &method))
    {
        runtimeError("Undefined method '%s' for class '%s'", name->chars, klass->name->chars);
        return false;
    }
    return callValue(method, argc);
}

bool invoke(ObjString *name, int argc)
{
    Value receiver = peek(argc);

    if (IS_FOREIGN(receiver))
    {
        ObjForeign *foreign = AS_FOREIGN(receiver);
        Value method;
        if (!tableGet(&foreign->methods, name, &method))
        {
            runtimeError("Undefined method '%s' for foreign object of type '%s'", name->chars, FOREIGN_TYPES[foreign->type]);
            return false;
        }
        // vm.stackTop[-argc - 1] = method;
        return callValue(method, argc);
    }

    bool isInstance = IS_INSTANCE(receiver);
    bool isClass = IS_CLASS(receiver);
    if (!isInstance && !isClass)
    {
        if (isBuiltinClass(receiver))
            return invokeFromClass(getVmClass(receiver), name, argc);

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
    if (IS_NATIVE(method))
    {
        ObjBoundBuiltin *bound = newBoundBuiltin(peek(0), ((ObjNative *)AS_OBJ(method)));
        pop();
        push(OBJ_VAL(bound));
        return true;
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
    return IS_NIL(value) || (IS_BOOL(value) && !(AS_BOOL(value))) || (IS_NUM(value) && AS_NUM(value) == 0) || (IS_STR(value) && AS_STR(value)->len == 0) || (IS_LIST(value) && AS_LIST(value)->count == 0) || (IS_MAP(value) && AS_MAP(value)->count == 0);
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

static void *runCallableInNewThread(void *arg)
{
    acquireGVL();
    ObjClosure *callable = (ObjClosure *)arg;
    // if (!IS_CLOSURE(callable))
    // {
    //     runtimeError("Thread expects a closure as argument but got '%s'", VALUE_TYPES[callable.type]);
    //     return NULL;
    // }
    // push(OBJ_VAL(callable));
    if (!call(callable, 0))
    {
        releaseGVL();
        return NULL;
    }
    if (run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
    {
        releaseGVL();
        return NULL;
    }
    releaseGVL();
    return NULL; // pop();
}

// static Value newThreadNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     if (argc != 1)
//     {
//         runtimeError("'newThread()' expects one argument but %d were passed in", argc);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     if (!IS_CLOSURE(argv[0]))
//     {
//         runtimeError("'newThread()' expects a closure as argument but got '%s'", VALUE_TYPES[argv[0].type]);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     pthread_t thread;
//     ObjClosure *closure = AS_CLOSURE(argv[0]);
//     if (pthread_create(&thread, NULL, runCallableInNewThread, closure) == 0)
//     {
//         releaseGVL();
//         acquireGVL();
//         return NUM_VAL((unsigned long)thread);
//     }
//     else
//     {
//         runtimeError("Failed to create thread");
//         *hasError = true;
//         return NIL_VAL;
//     }
// }

// static Value joinThreadNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     if (argc != 1)
//     {
//         runtimeError("'joinThread()' expects one argument but %d were passed in", argc);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     if (!IS_NUM(argv[0]))
//     {
//         runtimeError("'joinThread()' expects a number as argument but got '%s'", VALUE_TYPES[argv[0].type]);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     pthread_t thread = (pthread_t)AS_NUM(argv[0]);
//     releaseGVL();
//     int ret;
//     if ((ret = pthread_join(thread, NULL)) != 0)
//     {
//         acquireGVL();
//         runtimeError("Failed to join thread");
//         *hasError = true;
//         return NIL_VAL;
//     }
//     acquireGVL();
//     return BOOL_VAL(ret == 0);
// }

static Value inputNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc == 1)
    {
        char str[STR_BUFF] = {0};
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

    char input[STR_BUFF] = {0};
    char *ret = fgets(input, STR_BUFF, stdin);
    input[strlen(input) - 1] = '\0';
    return OBJ_VAL(copyString(input, (int)strlen(input)));
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

static Value evalNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'eval()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for eval(str)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(argv[0]);
    ObjFunction *function = compile(str, "eval", vm.isRepl, printBytecodeGlobal);
    if (function == NULL)
    {
        *hasError = true;
        return NIL_VAL;
    }

    ObjClosure *closure = newClosure(function);
    push(OBJ_VAL(closure));

    if (!call(closure, 0))
        return NIL_VAL;
    if (run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
    {
        *hasError = true;
        return NIL_VAL;
    }
    return pop();
}

static Value invalidInitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    runtimeError("Cannot instantiate native class");
    *hasError = true;
    return NIL_VAL;
}

/////////////////////// String CLASS NATIVE METHODS ///////////////////////

static Value initStringNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc == 0)
    {
        return OBJ_VAL(copyString("", 0));
    }
    Value ret = argv[0];
    if (!IS_STR(ret))
    {
        char str[STR_BUFF] = {0};
        int len = 0;
        valueToString(ret, str, &len);
        ret = OBJ_VAL(copyString(str, len));
    }
    return ret;
}

NATIVE_FN(regexMatchStrNative)
{
    if (argc != 1)
    {
        runtimeError("'str.match(pattern)' expects 1 argument. %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string but got '%s' for str.match(pattern)", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for str.match(pattern)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    char *str = AS_CSTR(peek(argc));
    char *pattern = AS_CSTR(argv[0]);
    regex_t regex;
    int er;
    if ((er = regcomp(&regex, pattern, REG_EXTENDED)) != 0)
    {
        runtimeError("Invalid regex pattern");
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = newList();
    push(OBJ_VAL(list));
    int numGroup = regex.re_nsub + 1;
    regmatch_t match[numGroup];
    int status;
    while ((status = regexec(&regex, str, numGroup, match, 0)) == 0)
    {
        int start;
        int end;
        if (numGroup == 1)
        {
            start = match[numGroup - 1].rm_so;
            end = match[numGroup - 1].rm_eo;
            appendToList(list, OBJ_VAL(copyString(str + start, end - start)));
        }
        else
        {
            ObjList *inner = newList();
            push(OBJ_VAL(inner));
            for (int i = 0; i < numGroup; i++)
            {
                if (match[i].rm_so == -1)
                    break;
                start = match[i].rm_so;
                end = match[i].rm_eo;
                appendToList(inner, OBJ_VAL(copyString(str + start, end - start)));
            }
            appendToList(list, pop());
        }

        str += end;
        if (*str == '\0')
            break;
    }
    // pop();
    regfree(&regex);
    return pop();
}

NATIVE_FN(regexFindAllNative)
{
    if (argc != 1)
    {
        runtimeError("'str.findall(pattern) expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string but got '%s' for str.findall(pattern)", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("Expected a string but got '%s' for str.findall(pattern)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }

    char *str = AS_CSTR(peek(argc));
    char *pattern = AS_CSTR(argv[0]);
    regex_t regex;
    int er;
    if ((er = regcomp(&regex, pattern, REG_EXTENDED)) != 0)
    {
        runtimeError("Invalid regex pattern");
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = newList();
    push(OBJ_VAL(list));
    int numGroup = regex.re_nsub + 1;
    regmatch_t match[numGroup];
    int status;
    while ((status = regexec(&regex, str, numGroup, match, 0)) == 0)
    {
        int start;
        int end;

        if (numGroup == 1 || numGroup == 2)
        {
            start = match[numGroup - 1].rm_so;
            end = match[numGroup - 1].rm_eo;
            appendToList(list, OBJ_VAL(copyString(str + start, end - start)));
        }
        else
        {
            ObjList *inner = newList();
            push(OBJ_VAL(inner));

            for (int i = 1; i < numGroup; i++)
            {
                if (match[i].rm_so == -1)
                    break;
                start = match[i].rm_so;
                end = match[i].rm_eo;
                appendToList(inner, OBJ_VAL(copyString(str + start, end - start)));
            }
            appendToList(list, pop());
        }
        str += end;
        if (*str == '\0')
            break;
    }

    regfree(&regex);
    return pop();
}

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
    int strAlloc = 100;
    char *str = ALLOCATE(char, strAlloc);
    str[0] = '\0';
    int needed = 0;
    // sprintf(str, "%s", "");
    for (int i = 0; i < list->count; i++)
    {
        int len = 0;
        char item[STR_BUFF] = {0};
        valueToString(list->items[i], item, &len);
        // int strLen = strlen(str);
        needed += len + sep->len;

        if (needed >= strAlloc)
        {
            int old = strAlloc;
            strAlloc = (needed + 0.5 * needed);

            str = GROW_ARRAY(char, str, old, strAlloc);
        }

        // GROW_ARRAY(char, str, len + strLen + sep->len + 1, strLen);
        // char temp[STR_BUFF] = //ALLOCATE(char, strlen(str) + len + strlen(sep->chars) + 1);
        sprintf(str, "%s%s%s", str, item, sep->chars);
        // FREE_ARRAY(char, str, strlen(str) + 1);
        // str = temp;
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
    char result[STR_BUFF] = {0}; // ALLOCATE(char, STR_BUFF);
    int len = 0;
    int i = 0;
    int numFormats = 0;
    char item[STR_BUFF] = {0};
    while (str[i])
    {
        if (str[i] == '$' && str[i + 1] == '{')
        {
            i++;
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
                    runtimeError("Index out of range for string.format(). Expected a number between 0 & %d but got %ld", argc - 1, num);
                    // free(result);
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
    return OBJ_VAL(copyString(result, (int)strlen(result)));
}

static Value strHashNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'hash()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(argc)))
    {
        runtimeError("Expected a string to be caller but got '%s' for hash()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }

    return NUM_VAL(AS_STR(peek(argc))->hash);
}

// THIS IS A MAYBE
// static Value withColorNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     if (argc != 2)
//     {
//         runtimeError("'withColor()' expects 2 arguments %d were passed in", argc);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     if (!IS_STR(peek(argc)))
//     {
//         runtimeError("Expected a string to be caller but got '%s' for withColor()", VALUE_TYPES[peek(argc).type]);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     if (!IS_NUM(argv[0]))
//     {
//         runtimeError("Expected a number but got '%s' for withColor(color, str)", VALUE_TYPES[argv[0].type]);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     if (!IS_NUM(argv[1]))
//     {
//         runtimeError("Expected a string but got '%s' for withColor(color, str)", VALUE_TYPES[argv[1].type]);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     char *str = AS_CSTR(peek(argc));
//     int color = (int)AS_NUM(argv[0]);
//     char *result = ALLOCATE(char, strlen(str) + 100);
//     sprintf(result, "\033[%dm%s\033[0m", color, str);
//     return OBJ_VAL(takeString(result, (int)strlen(result)));
// }

///////////////////////////////////////////////////////////////////////////

//////////////////////// List CLASS NATIVE METHODS ////////////////////////

NATIVE_FN(initListNative)
{
    if (argc == 0)
    {
        return OBJ_VAL(newList());
    }
    if (argc == 1 && IS_NUM(argv[0]))
    {
        return OBJ_VAL(newListWithCapacity((int)AS_NUM(argv[0])));
    }
    runtimeError("Expected 0 or 1 argument but got %d for List()", argc);
    *hasError = true;
    return NIL_VAL;
}

NATIVE_FN(listOfNative)
{
    ObjList *list = newList();
    for (int i = 0; i < argc; i++)
        appendToList(list, argv[i]);
    return OBJ_VAL(list);
}

NATIVE_FN(appendListNative)
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

NATIVE_FN(insertListNative)
{
    if (argc != 2)
    {
        runtimeError("'insert(at, thing)' expects 2 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for insert()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_NUM(argv[0]))
    {
        runtimeError("Expected a number as the second argument but got '%s' for insert()", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    int index = AS_NUM(argv[0]);
    if (index < 0 || index > list->count)
    {
        runtimeError("Index out of range for insert(). List has size %d. However, %d was provided", list->count, index);
        *hasError = true;
        return NIL_VAL;
    }
    if (list->count == list->capacity)
    {
        int oldCapacity = list->capacity;
        list->capacity = GROW_CAPACITY(oldCapacity);
        list->items = GROW_ARRAY(Value, list->items, oldCapacity, list->capacity);
    }
    for (int i = list->count; i > index; i--)
    {
        list->items[i] = list->items[i - 1];
    }
    list->items[index] = argv[1];
    list->count++;
    return OBJ_VAL(list);
}

NATIVE_FN(extendListNative)
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

NATIVE_FN(prependListNative)
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

NATIVE_FN(containsListNative)
{
    if (argc != 1)
    {
        runtimeError("'contains()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for contains()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    for (int i = 0; i < list->count; i++)
        if (checkIfValuesEqual(list->items[i], argv[0]))
            return BOOL_VAL(true);
    return BOOL_VAL(false);
}

NATIVE_FN(indexOfListNative)
{
    if (argc != 1)
    {
        runtimeError("'contains()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for contains()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    for (int i = 0; i < list->count; i++)
        if (checkIfValuesEqual(list->items[i], argv[0]))
            return NUM_VAL(i);
    return NUM_VAL(-1);
}

NATIVE_FN(popListNative)
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

NATIVE_FN(foreachNative)
{
    if (argc != 1)
    {
        runtimeError("'foreach()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    bool isClosure = IS_CLOSURE(argv[0]);
    // if (!isClosure)
    // {
    //     runtimeError("Expected a callable but got '%s' for foreach(closure)", VALUE_TYPES[argv[0].type]);
    //     *hasError = true;
    //     return NIL_VAL;
    // }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for foreach()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    // ObjClosure *closure = AS_CLOSURE(peek(0));
    Value callable = pop();
    bool enumerate = false;
    if (isClosure)
    {
        ObjClosure *closure = AS_CLOSURE(callable);
        // push(OBJ_VAL(closure));
        if (closure->function->arity != 1 && closure->function->arity != 2)
        {
            runtimeError("Expected a closure with 1 or 2 arguments but got %d", closure->function->arity);
            *hasError = true;
            return NIL_VAL;
        }
        enumerate = closure->function->arity == 2;
    }
    for (int i = 0; i < list->count; i++)
    {
        push(callable);
        int frameCount = vm.frameCount;
        if (enumerate)
            push(NUM_VAL(i));

        push(list->items[i]);
        if (!callValue(callable, enumerate ? 2 : 1))
        {
            *hasError = true;
            return NIL_VAL;
        }
        if (isClosure && run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
        {
            *hasError = true;
            return NIL_VAL;
        }
        // list->items[i] = *(vm.stackTop - 1);
        pop();
    }
    push(OBJ_VAL(list));
    return OBJ_VAL(list);
}

NATIVE_FN(removeIfNative)
{
    if (argc != 1)
    {
        runtimeError("'removeIf()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_CLOSURE(argv[0]))
    {
        runtimeError("Expected a closure but got '%s' for removeIf(closure)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for removeIf()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    // ObjClosure *closure = AS_CLOSURE(peek(0));
    ObjClosure *closure = AS_CLOSURE(pop());
    // push(OBJ_VAL(closure));
    if (closure->function->arity != 1 && closure->function->arity != 2)
    {
        runtimeError("Expected a closure with 1 or 2 arguments but got %d", closure->function->arity);
        *hasError = true;
        return NIL_VAL;
    }
    bool enumerate = closure->function->arity == 2;
    int i = 0;
    while (i < list->count)
    {
        push(OBJ_VAL(closure));

        if (enumerate)
            push(NUM_VAL(i));

        push(list->items[i]);
        if (!call(closure, enumerate ? 2 : 1))
        {
            *hasError = true;
            return NIL_VAL;
        }
        if (run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
        {
            *hasError = true;
            return NIL_VAL;
        }
        if (IS_BOOL(peek(0)) && AS_BOOL(pop()))
        {
            for (int j = i; j < list->count - 1; j++)
                list->items[j] = list->items[j + 1];
            list->count--;
        }
        else
            i++;
    }
    push(OBJ_VAL(list));
    return OBJ_VAL(list);
}

NATIVE_FN(mapNative)
{
    if (argc != 1)
    {
        runtimeError("'map()' expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    bool isClosure = IS_CLOSURE(argv[0]);
    // if (!IS_CLOSURE(argv[0]))
    // {
    //     runtimeError("Expected a closure but got '%s' for map(closure)", VALUE_TYPES[argv[0].type]);
    //     *hasError = true;
    //     return NIL_VAL;
    // }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list to be caller but got '%s' for map()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(peek(argc));
    // ObjClosure *closure = AS_CLOSURE(peek(0));
    Value callable = pop();
    bool enumerate = false;
    if (isClosure)
    {
        ObjClosure *closure = AS_CLOSURE(callable);
        // push(OBJ_VAL(closure));
        if (closure->function->arity != 1 && closure->function->arity != 2)
        {
            runtimeError("Expected a closure with 1 or 2 arguments but got %d", closure->function->arity);
            *hasError = true;
            return NIL_VAL;
        }
        enumerate = closure->function->arity == 2;
    }
    for (int i = 0; i < list->count; i++)
    {
        push(callable);
        ObjUpvalue *upvalues = vm.openUpvalues;

        if (enumerate)
            push(NUM_VAL(i));

        push(list->items[i]);
        if (!callValue(callable, enumerate ? 2 : 1))
        {
            *hasError = true;
            return NIL_VAL;
        }
        if (isClosure && run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
        {
            *hasError = true;
            return NIL_VAL;
        }
        list->items[i] = pop();
    }
    push(OBJ_VAL(list));
    return OBJ_VAL(list);
}

///////////////////////////////////////////////////////////////////////////

/////////////////////// Map CLASS NATIVE METHODS //////////////////////////

static Value mapInitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'init()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.init()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    ObjMap *map = newMap();
    map->map.capacity = 64;
    map->capacity = 64;
    map->map.count = 0;
    map->count = 0;
    map->map.entries = ALLOCATE(MapEntry, map->capacity);
    for (int i = 0; i < map->map.capacity; i++)
    {
        map->map.entries[i].key = NIL_VAL;
        map->map.entries[i].value = NIL_VAL;
        map->map.entries[i].isUsed = false;
    }
    // tableSet(&instance->fields, copyString("entries", 8), OBJ_VAL(map));
    return OBJ_VAL(map);
}

static Value mapClearNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'clear()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.clear()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    map->count = 0;
    map->map.count = 0;
    for (int i = 0; i < map->map.capacity; i++)
    {
        map->map.entries[i].key = NIL_VAL;
        map->map.entries[i].value = NIL_VAL;
        map->map.entries[i].isUsed = false;
    }
    return NIL_VAL;
}

static Value mapKeysNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.keys()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    ObjList *list = newList();
    push(OBJ_VAL(list));
    for (int i = 0; i < map->map.capacity; i++)
    {
        if (map->map.entries[i].isUsed)
            appendToList(list, map->map.entries[i].key);
    }
    pop();
    return OBJ_VAL(list);
}

static Value mapValuesNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.keys()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    ObjList *list = newList();
    push(OBJ_VAL(list));
    for (int i = 0; i < map->map.capacity; i++)
    {
        if (map->map.entries[i].isUsed)
            appendToList(list, map->map.entries[i].value);
    }
    pop();
    return OBJ_VAL(list);
}

static Value mapEntriesNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.entries()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    ObjList *list = newList();
    push(OBJ_VAL(list));
    for (int i = 0; i < map->map.capacity; i++)
    {
        if (map->map.entries[i].isUsed)
        {
            ObjInstance *entry = newInstance(vm.baseObj);
            push(OBJ_VAL(entry));
            tableSet(&entry->fields, copyString("key", 3), map->map.entries[i].key);
            tableSet(&entry->fields, copyString("value", 5), map->map.entries[i].value);
            // ObjList *entry = newList();
            // appendToList(entry, map->map.entries[i].key);
            // appendToList(entry, map->map.entries[i].value);
            appendToList(list, OBJ_VAL(entry));
            pop();
        }
    }
    pop();
    return OBJ_VAL(list);
}

static Value mapGetNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1 && argc != 2)
    {
        runtimeError("'get(key, <optional> default)', expects 1 or 2 argument(s) %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.get()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    bool hasDefault = argc == 2;

    ObjMap *map = AS_MAP(peek(argc));
    Value val;
    if (!mapGet(&map->map, argv[0], &val))
        return hasDefault ? argv[1] : NIL_VAL;
    return val;
}

static Value mapToStrNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'toStr()', expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.toStr()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    char *str = ALLOCATE(char, STR_BUFF);
    int len = 0;
    mapToString(&map->map, str, &len);
    return OBJ_VAL(takeString(str, len));
}

static Value mapSetNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'set(key, value)', expects 2 argument(s) %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.set()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    mapSet(&map->map, argv[0], argv[1]);
    map->capacity = map->map.capacity;
    return argv[1];
}

static Value mapSizeNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'size()', expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.size()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    return NUM_VAL(map->count);
}

static Value mapRemoveNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'remove(key)', expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.remove()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    if (!mapDelete(&map->map, argv[0]))
        return BOOL_VAL(false);
    map->count--;
    return BOOL_VAL(true);
}

static Value mapContainsKeyNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 1)
    {
        runtimeError("'containsKey(key)', expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.containsKey()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    return BOOL_VAL(mapContainsKey(&map->map, argv[0]));
}

static Value mapComputeNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'compute(key, closure)', expects 2 argument(s) %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.compute()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_CLOSURE(argv[1]))
    {
        runtimeError("Expected a closure but got '%s' for map.compute(key, closure)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    ObjClosure *closure = AS_CLOSURE(argv[1]);
    if (closure->function->arity != 2)
    {
        runtimeError("Expected a closure with 2 arguments but got %d", closure->function->arity);
        *hasError = true;
        return NIL_VAL;
    }
    Value val;
    if (!mapGet(&map->map, argv[0], &val))
        val = NIL_VAL;

    push(OBJ_VAL(closure));
    push(argv[0]);
    push(val);
    if (!call(closure, 2))
        return NIL_VAL;
    if (run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
        return NIL_VAL;
    val = pop();
    mapSet(&map->map, argv[0], val);

    return OBJ_VAL(map);
}

static Value mapComputeIfAbsentNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'computeIfAbsent(key, closure)', expects 2 argument(s) %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.computeIfAbsent()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_CLOSURE(argv[1]))
    {
        runtimeError("Expected a closure but got '%s' for map.computeIfAbsent(key, closure)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    ObjClosure *closure = AS_CLOSURE(argv[1]);
    if (closure->function->arity != 1)
    {
        runtimeError("Expected a closure with 1 argument but got %d", closure->function->arity);
        *hasError = true;
        return NIL_VAL;
    }
    Value val;
    if (!mapGet(&map->map, argv[0], &val))
    {
        push(OBJ_VAL(closure));
        push(argv[0]);
        if (!call(closure, 1))
            return NIL_VAL;
        if (run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
            return NIL_VAL;
        val = pop();
        mapSet(&map->map, argv[0], val);
        map->capacity = map->map.capacity;
    }

    return OBJ_VAL(map);
}

static Value mapComputeIfPresentNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 2)
    {
        runtimeError("'computeIfPresent(key, closure)', expects 2 argument(s) %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_MAP(peek(argc)))
    {
        runtimeError("Expected a map to be caller but got '%s' for map.computeIfPresent()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_CLOSURE(argv[1]))
    {
        runtimeError("Expected a closure but got '%s' for map.computeIfPresent(key, closure)", VALUE_TYPES[argv[1].type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(peek(argc));
    ObjClosure *closure = AS_CLOSURE(argv[1]);
    if (closure->function->arity != 2)
    {
        runtimeError("Expected a closure with 2 arguments but got %d", closure->function->arity);
        *hasError = true;
        return NIL_VAL;
    }
    Value val;
    if (mapGet(&map->map, argv[0], &val))
    {
        push(OBJ_VAL(closure));
        push(argv[0]);
        push(val);
        if (!call(closure, 2))
            return NIL_VAL;
        if (run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
        {
            *hasError = true;
            return NIL_VAL;
        }
        val = pop();
        mapSet(&map->map, argv[0], val);
        map->capacity = map->map.capacity;
    }

    return OBJ_VAL(map);
}
/////////////////// StringBuilder CLASS NATIVE METHODS ////////////////////

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
    Value str = argv[0];
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    if (!IS_STR(str))
    {
        int len = 0;
        char buff[STR_BUFF] = {0}; // ALLOCATE(char, STR_BUFF);
        valueToString(str, buff, &len);
        str = OBJ_VAL(copyString(buff, len));
        // pop();
    }
    Value value;
    if (!tableGet(&instance->fields, copyString("contents", 8), &value))
    {
        runtimeError("SB instance has no 'contents' field");
        *hasError = true;
        return NIL_VAL;
    }

    if (!IS_LIST(value))
    {
        runtimeError("Expected StringBuilder to have a list but got '%s' for SB.append(str)", VALUE_TYPES[value.type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjList *list = AS_LIST(value);
    appendToList(list, str);
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
    char *str = ALLOCATE(char, STR_BUFF);
    sprintf(str, "%s", "");
    for (int i = 0; i < list->count; i++)
    {
        int len = 0;
        char item[STR_BUFF] = {0};
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

////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////

// TODO
//////////////////////// Regex CLASS NATIVE METHODS ////////////////////////

// static Value regexInitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
// {
//     if (argc != 1)
//     {
//         runtimeError("'init(pattern)' expects 1 argument %d were passed in", argc);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     if (!IS_INSTANCE(peek(argc)))
//     {
//         runtimeError("Expected a Regex to be caller but got '%s' for Regex.init()", VALUE_TYPES[peek(argc).type]);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     if (!IS_STR(argv[0]))
//     {
//         runtimeError("Expected a string but got '%s' for Regex.init(pattern)", VALUE_TYPES[argv[0].type]);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     ObjInstance *instance = AS_INSTANCE(peek(argc));
//     ObjString *pattern = AS_STR(argv[0]);
//     int errorOffset;
//     const char *error;
//     pcre *re = pcre_compile(pattern->chars, 0, &error, &errorOffset, NULL);
//     if (re == NULL)
//     {
//         runtimeError("Error compiling regex pattern: %s", error);
//         *hasError = true;
//         return NIL_VAL;
//     }
//     tableSet(&instance->fields, copyString("pattern", 7), argv[0]);
//     tableSet(&instance->fields, copyString("re", 2), OBJ_VAL(re));
//     return peek(argc);
// }

////////////////////////////////////////////////////////////////////////////

//////////////////////// Error CLASS NATIVE METHODS ////////////////////////

static Value errorInitNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0 && argc != 1 && argc != 2)
    {
        runtimeError("'init(<optional> message, <optional> stackTrace)', expects 0, 1 or 2 argument(s) %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected an Error to be caller but got '%s' for Error())", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    tableSet(&instance->fields, copyString("message", 7), argc >= 1 ? argv[0] : NIL_VAL);
    tableSet(&instance->fields, copyString("stackTrace", 10), argc == 2 ? argv[1] : NIL_VAL);
    return peek(argc);
}

static Value errorToStringNative(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    if (argc != 0)
    {
        runtimeError("'toStr()' expects 0 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected an Error to be caller but got '%s' for Error.toStr()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Value messageVal;
    if (!tableGet(&instance->fields, copyString("message", 7), &messageVal))
    {
        runtimeError("'Error' instance has no 'message' field");
        *hasError = true;
        return NIL_VAL;
    }

    // Get stack trace
    Value stackTrace;
    if (!tableGet(&instance->fields, copyString("stackTrace", 10), &stackTrace))
    {
        runtimeError("'Error' instance has no 'stackTrace' field");
        *hasError = true;
        return NIL_VAL;
    }

    char str[STR_BUFF] = {0};
    int len = 0;
    if (IS_STR(messageVal) && IS_STR(stackTrace))
    {
        ObjString *stack = AS_STR(stackTrace);
        ObjString *message = AS_STR(messageVal);
        sprintf(str, "%s%s", stack->chars, message->chars);
        len = stack->len + message->len + 1;
    }
    else
    {
        char message[STR_BUFF] = {0};
        int messageLen = 0;
        char stack[STR_BUFF] = {0};
        int stackLen = 0;
        valueToString(messageVal, message, &messageLen);
        valueToString(stackTrace, stack, &stackLen);
        sprintf(str, "Error: %.*s \nwith trace: %.*s", messageLen, message, stackLen, stack);
        len = messageLen + stackLen + 23;
    }
    return OBJ_VAL(copyString(str, len));
}

////////////////////////////////////////////////////////////////////////////

//////////////////////// BASE CLASS NATIVE METHODS ////////////////////////
NATIVE_FN(baseToStrNative)
{
    if (argc != 0)
    {
        runtimeError("'toStr()', expects 0 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_INSTANCE(peek(argc)))
    {
        runtimeError("Expected an Object to be caller but got '%s' for Object.toStr()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *instance = AS_INSTANCE(peek(argc));
    Table *fields = &instance->fields;
    int strAlloc = STR_BUFF;
    char *str = ALLOCATE(char, strAlloc);
    int len = 0;
    int initialLen = instance->klass->name->len + 2;
    sprintf(str, "%s {", instance->klass->name->chars);

    len = initialLen;

    for (int i = 0; i < fields->capacity; i++)
    {
        if (fields->entries[i].key != NULL && fields->entries[i].key != vm.clazzStr)
        {
            ObjString *key = fields->entries[i].key;
            Value value = fields->entries[i].value;

            char valueStr[STR_BUFF] = {0};
            int valueLen = 0;
            valueToString(value, valueStr, &valueLen);
            int tempLen = key->len + valueLen + 4; // 4 for ": " and ", "
            if (len + tempLen >= strAlloc)
            {
                strAlloc = GROW_CAPACITY(strAlloc);
                str = GROW_ARRAY(char, str, len, strAlloc);
            }
            sprintf(str + len, "%s: %s, ", key->chars, valueStr);
            len += tempLen;
        }
    }

    if (len > initialLen) // Remove the last ", " if there were any fields
        len -= 2;
    sprintf(str + len, "}");

    return OBJ_VAL(takeString(str, len + 1));
}

NATIVE_FN(loadLibNative)
{
    if (argc != 1)
    {
        runtimeError("'loadLib(path)', expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    void *handle = dlopen(AS_CSTR(argv[0]), RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        runtimeError("Failed to load library: %s", dlerror());
        *hasError = true;
        return NIL_VAL;
    }
    void (*init)(DefineNativeFn, DefineNativeClassFn);
    init = dlsym(handle, "init");
    bool ret = false;
    if (init)
    {
        init(defineNative, primativeClass);
        ret = true;
    }
    // dlclose(handle);
    return BOOL_VAL(ret);
}

NATIVE_FN(dirNative)
{
    // expect a class or instance or builtin class
    if (argc != 1)
    {
        runtimeError("'__dir__(clazz/instance)', expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    Value val = peek(0);
    Table table;

    if (IS_INSTANCE(val))
        table = AS_INSTANCE(val)->klass->methods;
    else if (IS_BUILTIN(val))
        table = getVmClass(val)->methods;
    else if (IS_CLASS(val)) // class
        table = AS_CLASS(val)->methods;
    else if (IS_FOREIGN(val))
        table = AS_FOREIGN(val)->methods;
    else
    {
        runtimeError("Expected a class or instance but got '%s' for __dir__(clazz/instance)", VALUE_TYPES[peek(0).type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjList *list = newList();
    // push(OBJ_VAL(list));
    for (int i = 0; i < table.capacity; i++)
    {
        if (table.entries[i].key != NULL)
        {
            appendToList(list, OBJ_VAL(table.entries[i].key));
        }
    }

    return OBJ_VAL(list);
}

// same as dir but for fields
NATIVE_FN(varsNative)
{
    // expect a class or instance
    if (argc != 1)
    {
        runtimeError("'__vars__(clazz/instance)', expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }

    Value val = peek(0);
    ObjInstance *instance = NULL;

    if (IS_INSTANCE(val))
        instance = AS_INSTANCE(val);
    else
    {
        runtimeError("Expected an instance but got '%s' for __vars__(instance)", VALUE_TYPES[peek(0).type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjList *list = newList();
    // push(OBJ_VAL(list));
    for (int i = 0; i < instance->fields.capacity; i++)
    {
        if (instance->fields.entries[i].key != NULL)
        {
            appendToList(list, OBJ_VAL(instance->fields.entries[i].key));
        }
    }

    return OBJ_VAL(list);
}

NATIVE_FN(deleteVarNative)
{
    if (argc != 1)
    {
        runtimeError("'__del__(var)', expects 1 argument %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(peek(0)))
    {
        runtimeError("Expected a string but got '%s' for __del__(var)", VALUE_TYPES[peek(0).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjString *name = AS_STR(peek(0));
    if (!tableDelete(&vm.globals, name))
    {
        runtimeError("Variable '%s' not found", name->chars);
        *hasError = true;
        return NIL_VAL;
    }
    return BOOL_VAL(true);
}

static int compareValuesAsc(const void *a, const void *b)
{
    Value va = *(Value *)a;
    Value vb = *(Value *)b;
    if (IS_NIL(va) && IS_NIL(vb))
        return 0;
    if (IS_NIL(va))
        return 1;
    if (IS_NIL(vb))
        return -1;
    if (IS_NUM(va) && IS_NUM(vb))
    {
        double na = AS_NUM(va);
        double nb = AS_NUM(vb);
        if (na == nb)
            return 0;
        return na > nb ? 1 : -1;
    }
    if (IS_BOOL(va) && IS_BOOL(vb))
    {
        bool ba = AS_BOOL(va);
        bool bb = AS_BOOL(vb);
        if (ba == bb)
            return 0;
        return ba > bb ? 1 : -1;
    }
    if (IS_STR(va) && IS_STR(vb))
    {
        ObjString *sa = AS_STR(va);
        ObjString *sb = AS_STR(vb);
        return strcmp(sa->chars, sb->chars);
    }
    return 0;
}

static int compareValuesDes(const void *a, const void *b)
{
    Value va = *(Value *)a;
    Value vb = *(Value *)b;
    if (IS_NIL(va) && IS_NIL(vb))
        return 0;
    if (IS_NIL(va))
        return -1;
    if (IS_NIL(vb))
        return 1;
    if (IS_NUM(va) && IS_NUM(vb))
    {
        double na = AS_NUM(va);
        double nb = AS_NUM(vb);
        if (na == nb)
            return 0;
        return na < nb ? 1 : -1;
    }
    if (IS_BOOL(va) && IS_BOOL(vb))
    {
        bool ba = AS_BOOL(va);
        bool bb = AS_BOOL(vb);
        if (ba == bb)
            return 0;
        return ba < bb ? 1 : -1;
    }
    if (IS_STR(va) && IS_STR(vb))
    {
        ObjString *sa = AS_STR(va);
        ObjString *sb = AS_STR(vb);
        return strcmp(sb->chars, sa->chars);
    }
    return 0;
}

static int customCompareValues(const void *a, const void *b)
{
    if (!IS_CLOSURE(peek(0)))
    {
        runtimeError("Expected a closure but got '%s' for List::sort(cmpFn)", VALUE_TYPES[peek(0).type]);
        return 0;
    }
    ObjClosure *cmpFn = AS_CLOSURE(peek(0));
    Value va = *(Value *)a;
    Value vb = *(Value *)b;
    // printf("customCompareValues\n");
    push(va);
    push(vb);
    if (!call(cmpFn, 2))
    {
        runtimeError("Error calling custom compare function");
        return 0;
    }
    if (run(false, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
    {
        runtimeError("Error calling custom compare function");
        return 0;
    }
    Value result = pop();
    if (!IS_NUM(result))
    {
        runtimeError("Expected a number but got '%s' for List::sort(cmpFn)", VALUE_TYPES[result.type]);
        return 0;
    }
    push(OBJ_VAL(cmpFn));
    return AS_NUM(result);
}

NATIVE_FN(sortListNative)
{
    // How would I be able to provide a custom compare function
    if (argc != 0 && argc != 1)
    {
        runtimeError("'list::sort(<optional> cmpFn | <optional> descending)', expects 0 or 1 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_LIST(peek(argc)))
    {
        runtimeError("Expected a list but got '%s' for list::sort()", VALUE_TYPES[peek(argc).type]);
        *hasError = true;
        return NIL_VAL;
    }
    ObjClosure *cmpFn = NULL;
    bool desc = false;
    if (argc == 1 && IS_CLOSURE(argv[0]))
    {
        cmpFn = AS_CLOSURE(argv[0]);
        if (cmpFn->function->arity != 2)
        {
            runtimeError("Expected a closure with 2 arguments but got %d", cmpFn->function->arity);
            *hasError = true;
            return NIL_VAL;
        }
    }
    else if (argc == 1 && IS_BOOL(argv[0]))
    {
        desc = AS_BOOL(argv[0]);
    }
    else if (argc == 1)
    {
        runtimeError("Expected a closure or boolean but got '%s' for List::sort(cmpFn|descending)", VALUE_TYPES[argv[0].type]);
        *hasError = true;
        return NIL_VAL;
    }

    ObjList *list = AS_LIST(peek(argc));
    if (list->count == 0)
        return peek(argc);
    Value *items = list->items;

    if (cmpFn == NULL)
    {
        qsort(items, list->count, sizeof(Value), desc ? compareValuesDes : compareValuesAsc);
    }
    else
    {
        push(OBJ_VAL(cmpFn));
        qsort(items, list->count, sizeof(Value), customCompareValues);
        pop();
    }

    return peek(argc);
}

////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////

NATIVE_FN(forkNative)
{
    if (argc != 0)
    {
        runtimeError("'fork()', expects 0 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    pid_t pid = fork();
    if (pid == -1)
    {
        runtimeError("Failed to fork");
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL(pid);
}

NATIVE_FN(waitNative)
{
    if (argc != 0)
    {
        runtimeError("'wait()', expects 0 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    int status;
    pid_t pid = wait(&status);
    if (pid == -1)
    {
        runtimeError("Failed to wait");
        *hasError = true;
        return NIL_VAL;
    }
    return NUM_VAL(status);
}

NATIVE_FN(pipeNative)
{
    if (argc != 0)
    {
        runtimeError("'pipe()', expects 0 arguments %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    int fd[2];
    if (pipe(fd) == -1)
    {
        runtimeError("Failed to create pipe");
        *hasError = true;
        return NIL_VAL;
    }
    ObjInstance *entry = newInstance(vm.baseObj);
    push(OBJ_VAL(entry));
    tableSet(&entry->fields, copyString("send", 4), NUM_VAL(fd[1]));
    tableSet(&entry->fields, copyString("read", 4), NUM_VAL(fd[0]));

    return pop();
}

////////////////////////////////////////////////////////////////////////////

void initVM(bool printBytecode, bool printExecStack)
{
    if (signal(SIGPIPE, sigpipeHandler) == SIG_ERR)
    {
        fprintf(stderr, "[ERROR]::Pipe signal handler setup failed!!");
    }
    // if (signal(SIGSEGV, sigSegvHandler) == SIG_ERR)
    // {
    //     fprintf(stderr, "[ERROR]::SegV signal handler setup failed!!");
    // }
    acquireGVL();
    printBytecodeGlobal = printBytecode;
    printExecStackGlobaL = printExecStack;
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

    vm.lastError = NULL;
    vm.isInTryCatch = false;
    vm.isRepl = false;

    // Base Object
    vm.baseObj = NULL;
    ObjClass *baseObj = primativeClass("Object");
    vm.baseObj = baseObj;
    // baseObj->initializer = OBJ_VAL(newNative(initObjectNative));
    baseObj->toStr = OBJ_VAL(newNative(baseToStrNative));
    tableSet(&baseObj->methods, copyString("toStr", 5), baseObj->toStr);

    // newClass
    vm.errorClass = NULL;
    ObjClass *errorClass = primativeClass("Error");
    vm.errorClass = errorClass;
    errorClass->initializer = OBJ_VAL(newNative(errorInitNative));
    errorClass->toStr = OBJ_VAL(newNative(errorToStringNative));
    tableSet(&errorClass->methods, copyString("toStr", 5), errorClass->toStr);
    tableSet(&errorClass->methods, copyString("init", 4), errorClass->initializer);

    vm.stringClass = NULL;
    ObjClass *stringClass = primativeClass("String");
    vm.stringClass = stringClass;
    stringClass->initializer = OBJ_VAL(newNative(initStringNative));
    stringClass->hashFn = OBJ_VAL(newNative(strHashNative));
    tableSet(&stringClass->methods, copyString("split", 5), OBJ_VAL(newNative(split2Native)));
    tableSet(&stringClass->methods, copyString("match", 5), OBJ_VAL(newNative(regexMatchStrNative)));
    tableSet(&stringClass->methods, copyString("findall", 7), OBJ_VAL(newNative(regexFindAllNative)));
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
    tableSet(&stringClass->methods, copyString("f", 1), OBJ_VAL(newNative(formatNative)));
    tableSet(&stringClass->methods, copyString("init", 4), stringClass->initializer);
    tableSet(&stringClass->methods, copyString("_hash_", 6), stringClass->hashFn);

    vm.listClass = NULL;
    ObjClass *listClass = primativeClass("List");
    vm.listClass = listClass;
    listClass->initializer = OBJ_VAL(newNative(initListNative));
    tableSet(&listClass->methods, copyString("append", 6), OBJ_VAL(newNative(appendListNative)));
    tableSet(&listClass->methods, copyString("of", 2), OBJ_VAL(newNative(listOfNative)));
    tableSet(&listClass->methods, copyString("extend", 6), OBJ_VAL(newNative(extendListNative)));
    tableSet(&listClass->methods, copyString("prepend", 7), OBJ_VAL(newNative(prependListNative)));
    tableSet(&listClass->methods, copyString("contains", 8), OBJ_VAL(newNative(containsListNative)));
    tableSet(&listClass->methods, copyString("sort", 4), OBJ_VAL(newNative(sortListNative)));

    // tableSet(&listClass->methods, copyString("remove", 6), OBJ_VAL(newNative(removeListNative)));
    // tableSet(&listClass->methods, copyString("clear", 5), OBJ_VAL(newNative(clearListNative)));
    tableSet(&listClass->methods, copyString("indexOf", 7), OBJ_VAL(newNative(indexOfListNative)));
    tableSet(&listClass->methods, copyString("insert", 6), OBJ_VAL(newNative(insertListNative)));
    tableSet(&listClass->methods, copyString("pop", 3), OBJ_VAL(newNative(popListNative)));
    tableSet(&listClass->methods, copyString("foreach", 7), OBJ_VAL(newNative(foreachNative)));
    tableSet(&listClass->methods, copyString("map", 3), OBJ_VAL(newNative(mapNative)));
    tableSet(&listClass->methods, copyString("filter", 6), OBJ_VAL(newNative(removeIfNative)));
    tableSet(&listClass->methods, copyString("init", 4), listClass->initializer);

    vm.mapClass = NULL;
    ObjClass *mapClass = primativeClass("HashMap");
    vm.mapClass = mapClass;
    mapClass->initializer = OBJ_VAL(newNative(mapInitNative));
    mapClass->indexFn = OBJ_VAL(newNative(mapGetNative));
    mapClass->setFn = OBJ_VAL(newNative(mapSetNative));
    mapClass->sizeFn = OBJ_VAL(newNative(mapSizeNative));
    mapClass->toStr = OBJ_VAL(newNative(mapToStrNative));

    tableSet(&mapClass->methods, copyString("get", 3), mapClass->indexFn);
    tableSet(&mapClass->methods, copyString("_get_", 5), mapClass->indexFn);
    tableSet(&mapClass->methods, copyString("set", 3), mapClass->setFn);
    tableSet(&mapClass->methods, copyString("put", 3), mapClass->setFn);
    tableSet(&mapClass->methods, copyString("_set_", 5), mapClass->setFn);
    tableSet(&mapClass->methods, copyString("toStr", 5), mapClass->toStr);
    tableSet(&mapClass->methods, copyString("_size_", 7), mapClass->sizeFn);
    tableSet(&mapClass->methods, copyString("remove", 6), OBJ_VAL(newNative(mapRemoveNative)));
    tableSet(&mapClass->methods, copyString("clear", 5), OBJ_VAL(newNative(mapClearNative)));
    tableSet(&mapClass->methods, copyString("keys", 4), OBJ_VAL(newNative(mapKeysNative)));
    tableSet(&mapClass->methods, copyString("values", 6), OBJ_VAL(newNative(mapValuesNative)));
    tableSet(&mapClass->methods, copyString("entries", 7), OBJ_VAL(newNative(mapEntriesNative)));
    tableSet(&mapClass->methods, copyString("containsKey", 11), OBJ_VAL(newNative(mapContainsKeyNative)));
    tableSet(&mapClass->methods, copyString("compute", 7), OBJ_VAL(newNative(mapComputeNative)));
    tableSet(&mapClass->methods, copyString("computeIfAbsent", 15), OBJ_VAL(newNative(mapComputeIfAbsentNative)));
    tableSet(&mapClass->methods, copyString("computeIfPresent", 16), OBJ_VAL(newNative(mapComputeIfPresentNative)));

    ObjClass *sb = primativeClass("StringBuilder");
    sb->initializer = OBJ_VAL(newNative(sbInitNative));
    sb->sizeFn = OBJ_VAL(newNative(sbSizeNative));
    sb->toStr = OBJ_VAL(newNative(sbToStringNative));

    tableSet(&sb->methods, copyString("_size_", 7), sb->sizeFn);
    tableSet(&sb->methods, copyString("toStr", 5), sb->toStr);
    tableSet(&sb->methods, copyString("append", 6), OBJ_VAL(newNative(sbAppendNative)));
    tableSet(&sb->methods, copyString("clear", 5), OBJ_VAL(newNative(sbClearNative)));
    tableSet(&sb->methods, copyString("pop", 3), OBJ_VAL(newNative(sbPopNative)));
    tableSet(&sb->methods, copyString("toArray", 7), OBJ_VAL(newNative(sbToArrayNative)));

    ObjClass *file = primativeClass("File");
    file->initializer = OBJ_VAL(newNative(fileInitNative));
    tableSet(&file->methods, copyString("exists", 6), OBJ_VAL(newNative(fileClassExistsNative)));

    defineNative("tI", testRinitNative);
    defineNative("tC", testRcloseNative);
    defineNative("tPres", testRPresentNative);
    defineNative("tP", testRprintNative);
    defineNative("tPoll", testRpollNative);
    defineNative("tClear", testRClearNative);
    defineNative("tSetCell", testRsetCellNative);

    defineNative("offset", offsetNative);

    defineNative("clock", clockNative);
    defineNative("input", inputNative);
    defineNative("getc", getcNative);
    defineNative("kbhit", kbhitNative);
    defineNative("chr", chrNative);
    defineNative("exit", exitNative);
    defineNative("eval", evalNative);
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

    defineNative("fork", forkNative);
    defineNative("wait", waitNative);
    defineNative("pipe", pipeNative);

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

    // GC
    defineNative("gc", gcNative);
    defineNative("vmStats", vmStatsNative);

    defineNative("__dir__", dirNative);
    defineNative("__vars__", varsNative);
    defineNative("__del__", deleteVarNative);

    // Threads // SOON TO BE A BUILT-IN CLASS
    // defineNative("newThread", newThreadNative);
    // defineNative("joinThread", joinThreadNative);

    // defineNative("loadLib", loadLibNative);
}

void freeVM()
{
    releaseGVL();
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
    vm.indexStr = NULL;
    vm.setStr = NULL;
    vm.sizeStr = NULL;
    vm.hashStr = NULL;
    vm.ltStr = NULL;
    vm.gtStr = NULL;
    vm.errorClass = NULL;
    vm.stringClass = NULL;
    vm.listClass = NULL;
    vm.mapClass = NULL;
    vm.baseObj = NULL;
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

InterpretResult run(bool isRepl, int runUntilFrame)
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
        // #if DEBUG_TRACE_EXEC
        if (unlikely(printExecStackGlobaL))
        {
            printf("        ");
            for (Value *slot = vm.stack; slot < vm.stackTop; slot++)
            {
                printf("[ ");
                printValue(*slot, 0);
                printf(" ]");
            }
            printf("\n");
            disassembleInst(&frame->closure->function->chunk, (int)(frame->ip - frame->closure->function->chunk.code));
        }
        // #endif
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
                runtimeError("Operands must be numbers, strings or objects with a <_lt_> method. Not %s and %s", VALUE_TYPES[a.type], VALUE_TYPES[b.type]);
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
                else if (isBuiltinClass(listVal) && !IS_NIL(getVmClass(listVal)->indexFn))
                {
                    push(listVal);
                    push(indexVal);
                    if (!invoke(vm.indexStr, 1))
                        return INTERPRET_RUNTIME_ERROR;
                    frame = &vm.frames[vm.frameCount - 1];
                    break;
                }
                runtimeError("'%s' is not subscriptable", VALUE_TYPES[listVal.type]);
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
                else if (isBuiltinClass(listVal) && !IS_NIL(getVmClass(listVal)->setFn))
                {
                    push(listVal);
                    push(indexVal);
                    push(itemVal);
                    if (!invoke(vm.setStr, 2))
                        return INTERPRET_RUNTIME_ERROR;
                    frame = &vm.frames[vm.frameCount - 1];
                    break;
                }

                runtimeError("'%s' is not subscriptable", VALUE_TYPES[listVal.type]);
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
            if (index < 0)
                index -= 1;
            storeToList(list, getValidListIndex(list, index), itemVal);
            push(itemVal);
            break;
        }
        case OP_INHERIT:
        {
            Value superclass = peek(1);
            if (!IS_CLASS(superclass))
            {
                runtimeError("Superclass must be a class but got type: '%s''", VALUE_TYPES[superclass.type]);
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjClass *superclazz = AS_CLASS(superclass);
            if (isBuiltinClazz(superclazz))
            {
                runtimeError("Cannot inherit from a builtin class '%s'", superclazz->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjClass *subclass = AS_CLASS(peek(0));
            subclass->initializer = superclazz->initializer;
            subclass->toStr = superclazz->toStr;
            subclass->equals = superclazz->equals;
            subclass->lessThan = superclazz->lessThan;
            subclass->greaterThan = superclazz->greaterThan;
            subclass->indexFn = superclazz->indexFn;
            subclass->setFn = superclazz->setFn;
            subclass->sizeFn = superclazz->sizeFn;
            subclass->superclass = superclazz;
            tableAddAll(&superclazz->methods, &subclass->methods);
            tableAddAll(&superclazz->staticVars, &subclass->staticVars);

            tableSet(&subclass->staticVars, copyString("superClass", 10), OBJ_VAL(superclazz));
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
            ObjClass *klass;
            ObjString *name = READ_STRING();
            if (!IS_INSTANCE(peek(0)) && !IS_CLASS(peek(0)))
            {
                if (!isBuiltinClass(peek(0)))
                {
                    if (IS_FOREIGN(peek(0)))
                    {
                        Value value;
                        if (tableGet(&AS_FOREIGN(peek(0))->fields, name, &value))
                        {
                            pop();
                            push(value);
                            break;
                        }
                        else
                        {
                            runtimeError("Cannot find property %s in foreign object", name->chars);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    // handle foreign objects?
                    runtimeError("Cannot dereference %s, only Classes and their instances can be dereferenced.", VALUE_TYPES[peek(0).type]);
                    return INTERPRET_RUNTIME_ERROR;
                }
                klass = getVmClass(peek(0));
                Value native;
                if (tableGet(&klass->methods, name, &native))
                {
                    ObjBoundBuiltin *bound = newBoundBuiltin(peek(0), ((ObjNative *)AS_OBJ(native)));
                    pop();
                    push(OBJ_VAL(bound));
                    break;
                }
                if (tableGet(&klass->staticVars, name, &native))
                {
                    pop();
                    push(native);
                    break;
                }
                runtimeError("Cannot find property %s in %s\n", name->chars, klass->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }

            Table *table;
            if (IS_INSTANCE(peek(0)))
            {
                ObjInstance *instance = AS_INSTANCE(peek(0));
                klass = instance->klass;
                table = &instance->fields;
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
            if ((vm.stackTop - vm.stack) <= 1)
                push(val);
            if (IS_OBJ(val) && AS_OBJ(val)->type == OBJ_INSTANCE && !IS_NIL(AS_INSTANCE(val)->klass->toStr))
            {
                *(frame->ip)--;
                if (!invoke(vm.toStr, 0))
                    return INTERPRET_RUNTIME_ERROR;
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            int strLen = 0;
            char str[STR_BUFF] = {0};
            valueToString(pop(), str, &strLen);
            printf("%.*s", strLen, str);
            // printValue(pop(), PRINT_VERBOSE_OBJECTS_DEPTH);

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

                    char valStr[STR_BUFF] = {0};
                    int len = 0;
                    valueToString(notStr, valStr, &len);
                    push(OBJ_VAL(addTwoStrings(str->chars, valStr, str->len, len)));
                    break;
                }
            }
            else if (IS_STR(peek(0)))
            {
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
                    break;
                }
                else
                {
                    ObjString *str = AS_STR(pop());
                    Value notStr = pop();

                    char valStr[STR_BUFF] = {0};
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

            bool test = checkIfValuesEqual(a, b);
            push(BOOL_VAL(test));
            break;
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
            if (vm.frameCount < runUntilFrame)
                return INTERPRET_OK;

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
                // How can I check a working directory?

                ObjString *filePath = AS_STR(file);

                if (filePath->chars[filePath->len - 1] != 'k' || filePath->chars[filePath->len - 2] != '.')
                {
                    char *newPath = (char *)malloc(filePath->len + 3);
                    memcpy(newPath, filePath->chars, filePath->len);
                    newPath[filePath->len] = '.';
                    newPath[filePath->len + 1] = 'k';
                    newPath[filePath->len + 2] = '\0';
                    filePath = takeString(newPath, filePath->len + 2);
                }

                Value value;
                if (tableGet(&vm.imports, filePath, &value))
                    break;

                // Check known working directories
                char *workingDirs[4] = {
                    "./",
                    "/usr/local/lib/dotk/",
                    "/usr/lib/dotk/",
                    NULL};
                char *source = NULL;
                for (int i = 0; workingDirs[i] != NULL; i++)
                {
                    char fullPath[PATH_MAX];
                    snprintf(fullPath, sizeof(fullPath), "%s%s", workingDirs[i], filePath->chars);
                    source = readFile(fullPath);
                    if (source != NULL)
                    {
                        break;
                    }
                }

                if (source == NULL)
                {
                    runtimeError("Cannot import file:\"%s\"\n", filePath->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }

                vm.importSources = (char **)realloc(vm.importSources, sizeof(char *) * ++vm.importCount);
                vm.importSources[vm.importCount - 1] = source;

                CallFrame preImport = *frame;
                vm.frameCount = 0;

                InterpretResult res = interpret(source, filePath->chars, false, 0, NULL);
                if (res == INTERPRET_RUNTIME_ERROR)
                {
                    tableDelete(&vm.imports, filePath);
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

        case OP_TRY:
        {
            Value *stop = vm.stackTop;
            int frameCount = vm.frameCount;
            ObjUpvalue *upvalues = vm.openUpvalues;
            bool b4 = vm.isInTryCatch;
            if (!IS_CLOSURE(peek(0)))
            {
                runtimeError("Expected a closure as the first argument to the try block");
                return INTERPRET_RUNTIME_ERROR;
            }
            vm.isInTryCatch = true;
            ObjClosure *tryBlock = AS_CLOSURE(peek(0));
            call(tryBlock, 0);
            if (run(isRepl, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
            {
                vm.stackTop = stop;
                vm.frameCount = frameCount;
                vm.openUpvalues = upvalues;
                pop();
                push(BOOL_VAL(false));
            }
            else
            {
                pop();
                push(BOOL_VAL(true));
            }
            vm.isInTryCatch = b4;
            break;
        }
        case OP_CATCH:
        {
            if (!IS_CLOSURE(peek(0)))
            {
                runtimeError("Expected a closure as the first argument to the catch block");
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjClosure *catchBlock = AS_CLOSURE(peek(0));
            ObjInstance *error = newInstance(vm.errorClass);
            tableSet(&error->fields, copyString("message", 7), OBJ_VAL(vm.lastError));
            tableSet(&error->fields, copyString("stackTrace", 10), OBJ_VAL(vm.lastErrorTrace));
            push(OBJ_VAL(error));
            call(catchBlock, 1);
            if (run(isRepl, vm.frameCount) == INTERPRET_RUNTIME_ERROR)
            {
                pop();
                return INTERPRET_RUNTIME_ERROR;
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

InterpretResult interpret(const char *source, char *file, bool printExpressions, int argC, char **argV)
{
    bool b4 = vm.isRepl;
    vm.isRepl = printExpressions;
    if (argC > 0)
    {
        ObjList *args = newList();
        for (int i = 0; i < argC; i++)
            appendToList(args, OBJ_VAL(copyString(argV[i], (int)strlen(argV[i]))));

        tableSet(&vm.globals, copyString("ARG_V", 5), OBJ_VAL(args));
        tableSet(&vm.globals, copyString("ARG_C", 5), NUM_VAL(argC));
    }
    else
    {
        // check to see if the ARG_V and ARG_C are already defined
        Value argV;
        if (!tableGet(&vm.globals, copyString("ARG_V", 5), &argV))
        {
            tableSet(&vm.globals, copyString("ARG_V", 5), OBJ_VAL(newList()));
            tableSet(&vm.globals, copyString("ARG_C", 5), NUM_VAL(0));
        }
    }
    ObjString *file_ = copyString(file, (int)strlen(file));
    tableSet(&vm.imports, file_, NUM_VAL(0));
    ObjFunction *function = compile(source, file, printExpressions, printBytecodeGlobal);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;
    push(OBJ_VAL(function));
    ObjClosure *closure = newClosure(function);
    // tableSet(&vm.importFuncs, file_, OBJ_VAL(closure)); This may be due to some nonesense btw I am using this table to free imports after vm is closed
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);
    InterpretResult result = run(printExpressions, -1);
    vm.isRepl = b4;
    return result;
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
