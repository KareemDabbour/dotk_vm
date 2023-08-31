#include "include/vm.h"
#include "include/compiler.h"
#include "include/debug.h"
#include "include/io.h"
#include "include/memory.h"
#include "include/object.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
VM vm;
FILE *file;
static FILE *getFile()
{
    if (file == NULL)
    {
        file = fopen("test.k", "rb");
    }
    return file;
}
static Value clockNative(int argc, Value *argv)
{
    return NUM_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value getcNative(int argc, Value *argv)
{
    int i = argc == 1 ? AS_NUM(argv[0]) : 0;
    return NUM_VAL(getc(i == 0 ? stdin : getFile()));
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

static void resetStack()
{
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
}

static Value peek(int dist)
{
    return vm.stackTop[-1 - dist];
}

static void runtimeError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frameCount - 1; i >= 0; i--)
    {
        CallFrame *frame = &vm.frames[i];
        ObjFunction *function = frame->closure->function;
        size_t inst = frame->ip - function->chunk.code - 1;
        Position pos = getPos(&function->chunk, inst);
        fprintf(stderr, "%s:%d:%d in ", pos.file, pos.line, pos.col);
        if (function->name == NULL)
            fprintf(stderr, "script\n");
        else
            fprintf(stderr, "%s\n", function->name->chars);
    }

    resetStack();
    // vm.frameCount = 1;
}

static Value exitNative(int argc, Value *argv)
{

    if (argc != 1)
    {
        runtimeError("'exit()' expects one argument %d were passed in", argc);
        return NIL_VAL;
    }
    exit((int)AS_NUM(argv[0]));
}

static Value printErrNative(int argc, Value *argv)
{

    if (argc != 1)
    {
        runtimeError("'print_error()' expects one argument %d were passed in", argc);
        return NIL_VAL;
    }
    runtimeError(AS_CSTR(argv[0]));
    return argv[0];
}

static Value chrNative(int argc, Value *argv)
{
    if (argc != 1)
    {
        runtimeError("Expected 1 argument but %d passed in for chr(ch)", argc);
        return NIL_VAL;
    }
    int num = (int)AS_NUM(argv[0]);

    char *chr = ALLOCATE(char, 2);
    chr[0] = (char)num;
    chr[1] = '\0';
    return OBJ_VAL(takeString(chr, 1));
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
            Value result = native(argC, vm.stackTop - argC);
            vm.stackTop -= argC + 1;
            push(result);
            return true;
        }
        default:
            break;
        }
    }
    runtimeError("Can only call functions and classes");
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
    return call(AS_CLOSURE(method), argc);
}

static bool invoke(ObjString *name, int argc)
{
    Value receiver = peek(argc);
    bool isInstance = IS_INSTANCE(receiver);
    bool isClass = IS_CLASS(receiver);
    if (!isInstance && !isClass)
    {
        runtimeError("Only Classes and their instances have methods.");
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

static void defineMethod(ObjString *name)
{
    Value method = peek(0);
    ObjClass *klass = AS_CLASS(peek(1));
    tableSet(&klass->methods, name, method);
    if (name == vm.initStr)
        klass->initializer = method;
    if (name == vm.toStr)
        klass->toString = method;
    pop();
}

static bool isFalsey(Value value)
{
    return IS_NIL(value) || (IS_BOOL(value) && !(AS_BOOL(value)));
}

static void concatenate()
{
    ObjString *b = AS_STR(peek(0));
    ObjString *a = AS_STR(peek(1));

    int length = a->len + b->len;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->len);
    memcpy(chars + a->len, b->chars, b->len);
    chars[length] = '\0';

    ObjString *result = takeString(chars, length);
    pop();
    pop();
    push(OBJ_VAL(result));
}

void initVM()
{
    resetStack();
    vm.objects = NULL;
    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024 * 1024;

    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

    vm.nextWideOp = -1;

    initTable(&vm.globals);
    initTable(&vm.strings);
    vm.initStr = NULL;
    vm.initStr = copyString("init", 4);

    vm.toStr = NULL;
    vm.toStr = copyString("toStr", 5);

    defineNative("clock", clockNative);
    defineNative("getc", getcNative);
    defineNative("chr", chrNative);
    defineNative("exit", exitNative);
    defineNative("print_error", printErrNative);
}

void freeVM()
{
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.initStr = NULL;
    freeObjects();
}

static InterpretResult run()
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
            printValue(*slot);
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
            BIN_OP(BOOL_VAL, >);
            break;
        case OP_LESS:
            BIN_OP(BOOL_VAL, <);
            break;
        case OP_CLASS:
        {
            push(OBJ_VAL(newClass(READ_STRING())));
            break;
        }

        case OP_TRUE:
            push(BOOL_VAL(true));
            break;
        case OP_METHOD:
        {
            defineMethod(READ_STRING());
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
        case OP_INDEX_SUBSCR:
        {
            Value indexVal = pop();
            Value listVal = pop();
            Value result;
            // if(IS_STR(listVal)){} TODO
            if (!IS_LIST(listVal))
            {
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

            result = indexFromList(list, index);
            push(result);
            break;
        }
        case OP_STORE_SUBSCR:
        {
            Value itemVal = pop();
            Value indexVal = pop();
            Value listVal = pop();
            if (!IS_LIST(listVal))
            {
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
            storeToList(list, index, itemVal);
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
            Value v = peek(0);

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
            if (!IS_INSTANCE(peek(0)) && !IS_CLASS(peek(0))) // TODO STATIC VARS
            {
                runtimeError("Only Classes and their instances can be dereferenced.");
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
                runtimeError("Only Classes and their Instances can have fields.");
                return INTERPRET_RUNTIME_ERROR;
            }
            Table *table;
            ObjString *name = READ_STRING();
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
            printValue(pop());
            printf("\n");
            break;
        }
        case OP_ADD:
        {
            if (IS_STR(peek(0)) && IS_STR(peek(1)))
            {
                concatenate();
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
        case OP_EQUAL:
        {
            Value a = pop();
            Value b = pop();
            push(BOOL_VAL(valuesEqual(a, b)));
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
                char *source = readFile(filePath->chars);
                if (source == NULL)
                {
                    runtimeError("Cannot import file:\"%s\"\n", filePath->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                CallFrame preImport = *frame;
                vm.frameCount = 0;
                interpret(source, filePath->chars);
                vm.frameCount = 1;
                vm.frames[0] = preImport;
                free(source);
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

InterpretResult interpret(const char *source, char *file)
{
    ObjFunction *function = compile(source, file);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;
    push(OBJ_VAL(function));
    ObjClosure *closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}

void push(Value value)
{
    *vm.stackTop++ = value;
}

Value pop()
{
    return *(--vm.stackTop);
}
