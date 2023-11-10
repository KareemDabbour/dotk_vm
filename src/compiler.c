#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/common.h"
#include "include/compiler.h"
#include "include/memory.h"
#include "include/object.h"
#include "include/scanner.h"

#ifdef DEBUG_PRINT_CODE
#include "include/debug.h"
#endif

typedef struct _Parser
{
    Token prev;
    Token current;
    char *file;
    bool hadError;
    bool panicMode;
} Parser;

typedef enum _Precedence
{
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_OR,         // or
    PREC_AND,        // and
    PREC_EQUALITY,   // == !=
    PREC_COMPARISON, // < > <= >=
    PREC_TERM,       // + -
    PREC_FACTOR,     // * /
    PREC_UNARY,      // ! -
    PREC_POWER,      // **
    PREC_CALL,       // . ()
    PREC_SUBSCRIPT,  // [] .
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct _ParseRule
{
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct _Local
{
    Token name;
    int depth;
    bool isCaptured;
} Local;

typedef struct _UpValue
{
    uint8_t index;
    bool isLocal;
} UpValue;

typedef enum _FunctionType
{
    TYPE_FUNCTION,
    TYPE_METHOD,
    TYPE_STATIC_METHOD,
    TYPE_INITIALIZER,
    TYPE_TO_STR,
    TYPE_SCRIPT,
    TYPE_ANONYMOUS
} FunctionType;

typedef struct _Compiler
{
    struct _Compiler *enclosing;
    ObjFunction *function;
    FunctionType type;
    bool isRepl;
    Local locals[UINT16_COUNT];
    int localCount;
    UpValue upValues[UINT16_COUNT];
    int scopeDepth;
} Compiler;

typedef struct _ClassCompiler
{
    struct _ClassCompiler *enclosing;
    bool hasSuperClass;
} ClassCompiler;

Parser parser;
Compiler *current = NULL;
ClassCompiler *currentClass = NULL;

int innermostLoopStart = -1;
int innermostLoopScopeDepth = 0;
int innermostLoopEnd = -1;
int breakAddress = -1;

static void expression();
static void declareVar();
static void namedVariable(Token name, bool canAssign);
static void statement();
static bool identifierEqual(Token *a, Token *b);
static void variable(bool canAssign);
static void varDeclaration();
static void importStatement();
static uint16_t identifierConst(Token *name);
static void defineVar(uint16_t global);
static void addLocal(Token name);
static void markInitialized();
static void declaration();
static ParseRule *getRule(TokenType type);
static void parsePrecedence(Precedence precedence);
static uint16_t parseVariable(const char *errMsg);
static int resolveLocal(Compiler *compiler, Token *name);
static int resolveUpValue(Compiler *compiler, Token *name);

static Chunk *currentChunk()
{
    return &current->function->chunk;
}

static void errorAt(Token *token, const char *message)
{
    if (parser.panicMode)
        return;
    parser.panicMode = true;
    fprintf(stderr, "%s:%d:%d Error", parser.file, token->line, token->col);
    if (token->type == TOKEN_EOF)
    {
        fprintf(stderr, " at end");
    }
    else if (token->type != TOKEN_ERROR)
    {
        fprintf(stderr, " at '%.*s'", token->len, token->start);
    }
    fprintf(stderr, ": %s\n", message);
}

static void error(const char *message)
{
    errorAt(&parser.prev, message);
}

static void errorAtCurrent(const char *message)
{
    errorAt(&parser.current, message);
}

static void advance()
{
    parser.prev = parser.current;
    for (;;)
    {
        parser.current = scanToken();
        // printf("TOKEN: '%.*s', TOKEN TYPE: \"%s\", LINE: %d, COL: %d\n", parser.current.len, parser.current.start, TOKEN_NAMES[parser.current.type], parser.current.line, parser.current.col);
        if (parser.current.type != TOKEN_ERROR)
            break;
        errorAtCurrent(parser.current.start);
    }
}

static void consume(TokenType type, const char *message)
{
    if (parser.current.type == type)
    {
        advance();
        return;
    }
    errorAtCurrent(message);
}

static bool check(TokenType type)
{
    return parser.current.type == type;
}

static bool match(TokenType type)
{
    if (!check(type))
        return false;
    advance();
    return true;
}

static void emitByte(uint8_t byte)
{
    writeChunk(currentChunk(), byte, parser.prev.line, parser.prev.col, parser.file);
}

static void emitWide(uint8_t byte, uint16_t constant)
{
    emitByte(OP_WIDE);
    emitByte(byte);
    emitByte((uint8_t)(constant >> 8));
    emitByte((uint8_t)constant);
}

static void emitBytes(uint8_t byte1, uint16_t byte2)
{
    if (byte2 <= UINT8_MAX)
    {
        emitByte(byte1);
        emitByte((uint8_t)byte2);
    }
    else
    {
        emitWide(byte1, byte2);
    }
}

static void emitLoop(int loopStart)
{
    emitByte(OP_LOOP);

    int offset = currentChunk()->size - loopStart + 2;
    if (offset > UINT16_MAX)
        error("Loop body too large");
    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

static int emitJump(uint8_t inst)
{
    emitByte(inst);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->size - 2;
}

static void emitReturn()
{
    if (current->type == TYPE_INITIALIZER)
        emitBytes(OP_GET_LOCAL, 0);
    else
        emitByte(OP_NIL);
    emitByte(OP_RETURN);
}

static uint16_t makeConstant(Value value)
{
    int constant = addConst(currentChunk(), value);
    // int constant = writeConst(currentChunk(), value, parser.current.line);
    if (constant > UINT16_MAX)
    {
        error("Too many constants in one chunk.");
        return 0;
    }
    return (uint16_t)constant;
}

static inline void emitConst(Value value)
{
    emitBytes(OP_CONSTANT, (uint16_t)makeConstant(value));
}

static void patchJump(int offset)
{
    int jump = currentChunk()->size - offset - 2;

    if (jump > UINT16_MAX)
    {
        error("Too much code to jump over");
    }

    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler *compiler, FunctionType type, char *file, bool isRepl)
{
    parser.file = file;
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->isRepl = isRepl;
    compiler->function = newFunction();
    current = compiler;
    if (type == TYPE_ANONYMOUS)
    {
        current->function->name = copyString("anonymous_fn", 13);
    }
    else if (type != TYPE_SCRIPT)
    {
        current->function->name = copyString(parser.prev.start, parser.prev.len);
    }
    else
    {
        current->function->name = copyString(file, strlen(file));
    }

    Local *local = &current->locals[current->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->name.len = 0;
    local->isCaptured = false;
    if (type != TYPE_FUNCTION)
    {
        local->name.start = "this";
        local->name.len = 4;
    }
}

static ObjFunction *endCompiler()
{
    emitReturn();
    ObjFunction *func = current->function;
#if DEBUG_PRINT_CODE
    if (!parser.hadError)
        disassembleChunk(currentChunk(), func->name != NULL ? func->name->chars : "<script>");
#endif
    current = current->enclosing;
    return func;
}

static void list(bool canAssign)
{
    int itemCount = 0;
    if (!check(TOKEN_RIGHT_BRACE))
    {
        do
        {
            if (check(TOKEN_RIGHT_BRACE))
                break;

            parsePrecedence(PREC_OR);

            if (itemCount == UINT16_COUNT)
                error("Cannot have more than 256 items in a list literal");
            itemCount++;
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after list literal");

    emitBytes(OP_BUILD_LIST, (uint8_t)itemCount);
    return;
}

static void defaultSizeList(bool canAssign)
{
    if (!check(TOKEN_RIGHT_BRACKET))
    {
        parsePrecedence(PREC_OR);
    }
    consume(TOKEN_RIGHT_BRACKET, "Expect ']' after default list builder");
    if (check(TOKEN_LEFT_BRACE))
    {
        consume(TOKEN_LEFT_BRACE, "Expect '{' after default list builder");
        expression();
        emitBytes(OP_BUILD_DEFAULT_LIST, (uint8_t)1);
        consume(TOKEN_RIGHT_BRACE, "Expect '}' after default list builder");
    }
    else
    {
        emitBytes(OP_BUILD_DEFAULT_LIST, (uint8_t)0);
    }
}

static void subscript(bool canAssign)
{
    parsePrecedence(PREC_OR);
    consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index");

    if (canAssign && match(TOKEN_EQUAL))
    {
        expression();
        emitByte(OP_STORE_SUBSCR);
    }
    else
    {
        emitByte(OP_INDEX_SUBSCR);
    }
    return;
}

static void beginScope()
{
    current->scopeDepth++;
}

static void endScope()
{
    current->scopeDepth--;
    while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth)
    {
        if (current->locals[current->localCount - 1].isCaptured)
            emitByte(OP_CLOSE_UPVALUE);
        else
            emitByte(OP_POP);
        current->localCount--;
    }
}

static void binary(bool canAssign)
{
    TokenType opType = parser.prev.type;
    ParseRule *rule = getRule(opType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (opType)
    {
    case TOKEN_AMP:
        emitByte(OP_BIN_AND);
        break;
    case TOKEN_PIPE:
        emitByte(OP_BIN_OR);
        break;
    case TOKEN_PLUS:
        emitByte(OP_ADD);
        break;
    case TOKEN_MINUS:
        emitByte(OP_SUB);
        break;
    case TOKEN_STAR:
        emitByte(OP_MULT);
        break;
    case TOKEN_STAR_STAR:
        emitByte(OP_POW);
        break;
    case TOKEN_SLASH:
        emitByte(OP_DIV);
        break;
    case TOKEN_SLASH_SLASH:
        emitByte(OP_INT_DIV);
        break;
    case TOKEN_PERCENT:
        emitByte(OP_MOD);
        break;
    case TOKEN_BANG_EQUAL:
        emitBytes(OP_EQUAL, OP_NOT);
        break;
    case TOKEN_EQUAL_EQUAL:
        emitByte(OP_EQUAL);
        break;
    case TOKEN_GREATER:
        emitByte(OP_GREATER);
        break;
    case TOKEN_GREATER_EQUAL:
        emitBytes(OP_LESS, OP_NOT);
        break;
    case TOKEN_LESS:
        emitByte(OP_LESS);
        break;
    case TOKEN_LESS_EQUAL:
        emitBytes(OP_GREATER, OP_NOT);
        break;
    default:
        break;
    }
}

static uint8_t argList()
{
    uint8_t argC = 0;
    if (!check(TOKEN_RIGHT_PAREN))
    {
        do
        {
            expression();
            if (argC == 255)
                error("Can't have more than 255 arguments");
            argC++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments");
    return argC;
}

static void call(bool canAssign)
{
    uint8_t argCount = argList();
    emitBytes(OP_CALL, argCount);
}

static void dot(bool canAssign)
{
    consume(TOKEN_IDENTIFIER, "Expect property name afer '.'");
    uint16_t name = identifierConst(&parser.prev);

    if (canAssign && match(TOKEN_EQUAL))
    {
        expression();
        emitBytes(OP_SET_PROPERTY, name);
    }
    else if (match(TOKEN_LEFT_PAREN))
    {
        uint8_t argc = argList();

        emitBytes(OP_INVOKE, name);
        emitByte(argc);
    }
    else
        emitBytes(OP_GET_PROPERTY, name);
}

static void literal(bool canAssign)
{
    switch (parser.prev.type)
    {
    case TOKEN_FALSE:
        emitByte(OP_FALSE);
        break;
    case TOKEN_TRUE:
        emitByte(OP_TRUE);
        break;
    case TOKEN_NIL:
        emitByte(OP_NIL);
        break;
    default:
        return;
    }
}

static void grouping(bool canAssign)
{
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression");
}

static void expression()
{
    parsePrecedence(PREC_ASSIGNMENT);
}

static void block()
{
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
    {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block");
}

static void function(FunctionType type)
{
    Compiler compiler;
    initCompiler(&compiler, type, parser.file, current->isRepl);
    beginScope();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    if (!check(TOKEN_RIGHT_PAREN))
    {
        do
        {
            current->function->arity++;
            if (current->function->arity >= UINT16_MAX)
                errorAtCurrent("Can't have more than 65535 parameters");
            uint16_t constant = parseVariable("Expect parameter name.");
            defineVar(constant);
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block();

    ObjFunction *function = endCompiler();
    uint16_t constant = makeConstant(OBJ_VAL(function));
    emitBytes(OP_CLOSURE, constant);

    for (int i = 0; i < function->upValueCount; i++)
    {
        emitByte(compiler.upValues[i].isLocal ? 1 : 0);
        emitByte(compiler.upValues[i].index);
    }
}

static void method()
{
    consume(TOKEN_IDENTIFIER, "Expect name of method");
    uint16_t constant = identifierConst(&parser.prev);
    FunctionType type = *parser.prev.start == '_' ? TYPE_STATIC_METHOD : TYPE_METHOD;
    if (parser.prev.len == 4 && memcmp(parser.prev.start, "init", 4) == 0)
        type = TYPE_INITIALIZER;
    if (check(TOKEN_LEFT_PAREN))
    {
        function(type);
        emitBytes(OP_METHOD, constant);
    }
    else
    {
        if (match(TOKEN_EQUAL))
            expression();
        else
            emitByte(OP_NIL);

        consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
        emitBytes(OP_STATIC_VAR, constant);
    }
}

static Token synthToken(const char *text)
{
    Token token;
    token.start = text;
    token.len = (int)strlen(text);
    return token;
}

static void super_(bool canAssign)
{
    if (currentClass == NULL)
        error("Can't use 'super' outside of class");
    else if (!currentClass->hasSuperClass)
        error("Can't use 'super' in a class that has no superclass");

    consume(TOKEN_DOT, "Expect '.' after 'super'");
    consume(TOKEN_IDENTIFIER, "Expect superclass method name");
    uint8_t name = identifierConst(&parser.prev);
    namedVariable(synthToken("this"), false);
    if (match(TOKEN_LEFT_PAREN))
    {
        uint8_t argCount = argList();
        namedVariable(synthToken("super"), false);
        emitBytes(OP_SUPER_INVOKE, name);
        emitByte(argCount);
    }
    else
    {
        namedVariable(synthToken("super"), false);
        emitBytes(OP_GET_SUPER, name);
    }
    // emitBytes(OP_GET_SUPER, name);
}

static void classDeclaration()
{
    consume(TOKEN_IDENTIFIER, "Expect class name");
    Token className = parser.prev;
    uint16_t nameConst = identifierConst(&parser.prev);
    declareVar();

    emitBytes(OP_CLASS, nameConst);

    defineVar(nameConst);

    ClassCompiler classCompiler;
    classCompiler.enclosing = currentClass;
    classCompiler.hasSuperClass = false;
    currentClass = &classCompiler;

    if (match(TOKEN_LESS))
    {
        consume(TOKEN_IDENTIFIER, "Expect superclass");
        variable(false);
        if (identifierEqual(&className, &parser.prev))
            error("A class can't inherit from itself");
        beginScope();
        addLocal(synthToken("super"));
        defineVar(0);
        namedVariable(className, false);
        // namedVariable(parser.prev, false);
        consume(TOKEN_GREATER, "Expect '>' after superclassname");
        classCompiler.hasSuperClass = true;
        emitByte(OP_INHERIT);
    }

    namedVariable(className, false);
    consume(TOKEN_LEFT_BRACE, "Expect '{' before class body");
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
    {
        method();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body");
    emitByte(OP_POP);

    if (classCompiler.hasSuperClass)
        endScope();

    currentClass = currentClass->enclosing;
}
static void funDeclaration()
{
    uint16_t global = parseVariable("Expect function name");
    markInitialized();
    function(TYPE_FUNCTION);
    defineVar(global);
}

static void varDeclaration()
{
    uint16_t global = parseVariable("Expect variable name");

    if (match(TOKEN_EQUAL))
    {
        expression();
    }
    else
    {
        emitByte(OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    defineVar(global);
}

static void constDeclaration()
{
    Token name = parser.current;
    uint16_t global = parseVariable("Expect constant name");
    consume(TOKEN_EQUAL, "Expect '=' after constant name");
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after constant declaration.");
    defineVar(global);
}

static void expressionStatement()
{
    expression();
    if (current->isRepl)
    {
        if (match(TOKEN_SEMICOLON))
            advance();
        emitByte(OP_PRINT);
    }
    else
    {
        consume(TOKEN_SEMICOLON, "Expect ';' after expression");
        emitByte(OP_POP);
    }
}

#define MAX_CASES 256
static void switchStatement()
{
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'switch'");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after value");
    consume(TOKEN_LEFT_BRACE, "Expect '{' before switch cases");

    int state = 0;
    int caseEnds[MAX_CASES];
    int caseCount = 0;
    int previousCaseSkip = -1;

    while (!match(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
    {
        if (match(TOKEN_CASE) || match(TOKEN_DEFAULT))
        {
            TokenType type = parser.prev.type;
            if (state == 2)
                error("Can't have another case or default after default case");
            if (state == 1)
            {
                caseEnds[caseCount++] = emitJump(OP_JUMP);
                patchJump(previousCaseSkip);
                emitByte(OP_POP);
            }

            if (type == TOKEN_CASE)
            {
                state = 1;

                emitByte(OP_DUP);
                expression();

                emitByte(OP_EQUAL);
                previousCaseSkip = emitJump(OP_JUMP_IF_FALSE);

                emitByte(OP_POP);
            }
            else
            {
                state = 2;
                previousCaseSkip = -1;
            }
        }
        else
        {
            if (state == 0)
            {
                error("Can't have statements before any case.");
            }
            statement();
        }
    }

    if (state == 1)
    {
        patchJump(previousCaseSkip);
        emitByte(OP_POP);
    }

    for (int i = 0; i < caseCount; i++)
    {
        patchJump(caseEnds[i]);
    }
    emitByte(OP_POP);
}

static void forStatement()
{
    beginScope();

    int loopVar = -1;
    Token loopVarName;
    loopVarName.start = NULL;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after for.");
    if (match(TOKEN_SEMICOLON))
    {
    }
    else if (match(TOKEN_VAR))
    {
        loopVarName = parser.current;
        varDeclaration();
        loopVar = current->localCount - 1;
    }
    else
        expressionStatement();

    int surroundingLoopStart = innermostLoopStart;
    int surroundingLoopScopeDepth = innermostLoopScopeDepth;
    int lastBreakAddr = breakAddress;
    innermostLoopStart = currentChunk()->size;
    innermostLoopScopeDepth = current->scopeDepth;

    int exitJump = -1;
    if (!match(TOKEN_SEMICOLON))
    {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

        // This is for the following condition: for(var i = 0;)
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP); // pop the condition
    }

    if (!match(TOKEN_RIGHT_PAREN))
    {
        int bodyJump = emitJump(OP_JUMP);
        int incrementStart = currentChunk()->size;
        expression();
        emitByte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after clauses");

        emitLoop(innermostLoopStart);
        innermostLoopStart = incrementStart;
        patchJump(bodyJump);
    }

    int innerVar = -1;
    if (loopVar != -1)
    {
        beginScope();
        emitBytes(OP_GET_LOCAL, (uint8_t)loopVar);
        addLocal(loopVarName);
        markInitialized();

        innerVar = current->localCount - 1;
    }

    statement();

    if (loopVar != -1)
    {
        emitBytes(OP_GET_LOCAL, (uint8_t)innerVar);
        emitBytes(OP_SET_LOCAL, (uint8_t)loopVar);
        emitByte(OP_POP);
        endScope();
    }

    emitLoop(innermostLoopStart);

    if (exitJump != -1)
    {
        patchJump(exitJump);
        emitByte(OP_POP); // pop the condition off the stack
    }
    if (breakAddress != -1)
        patchJump(breakAddress);

    breakAddress = lastBreakAddr;
    innermostLoopStart = surroundingLoopStart;
    innermostLoopScopeDepth = surroundingLoopScopeDepth;

    endScope();
}

static void ifStatement()
{
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression");
    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();

    int elseJump = emitJump(OP_JUMP);

    patchJump(thenJump);
    emitByte(OP_POP);

    if (match(TOKEN_ELSE))
        statement();
    patchJump(elseJump);
}

static void printStatement()
{
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void returnStatement()
{
    if (current->type == TYPE_SCRIPT)
        error("Can't return from top-level code");

    if (match(TOKEN_SEMICOLON))
        emitReturn();
    else
    {
        if (current->type == TYPE_INITIALIZER)
            error("Can't return a value from a class's constructor.");

        expression();
        consume(TOKEN_SEMICOLON, "Exepect ';' after return value");
        emitByte(OP_RETURN);
    }
}

static void whileStatement()
{
    beginScope();

    int surroundingLoopStart = innermostLoopStart;
    int surroundingBreakAddr = breakAddress;
    int surroundingLoopScopeDepth = innermostLoopScopeDepth;
    int surroundingLoopEnd = innermostLoopEnd;
    innermostLoopStart = currentChunk()->size;
    innermostLoopScopeDepth = current->scopeDepth;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while' ");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition");

    innermostLoopEnd = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();
    emitLoop(innermostLoopStart);
    patchJump(innermostLoopEnd);
    emitByte(OP_POP);
    if (breakAddress != -1)
        patchJump(breakAddress);
    breakAddress = surroundingBreakAddr;
    innermostLoopStart = surroundingLoopStart;
    innermostLoopScopeDepth = surroundingLoopScopeDepth;
    innermostLoopEnd = surroundingLoopEnd;
    endScope();
}

static void synchronize()
{
    parser.panicMode = false;

    while (parser.current.type != TOKEN_EOF)
    {
        if (parser.prev.type == TOKEN_SEMICOLON)
            return;
        switch (parser.current.type)
        {
        case TOKEN_CLASS:
        case TOKEN_FUN:
        case TOKEN_VAR:
        case TOKEN_FOR:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_PRINT:
        case TOKEN_RETURN:
            return;

        default:;
        }
        advance();
    }
}

static void declaration()
{
    if (match(TOKEN_CLASS))
        classDeclaration();
    else if (match(TOKEN_FUN))
        funDeclaration();
    else if (match(TOKEN_VAR))
        varDeclaration();
    else if (match(TOKEN_CONST))
        constDeclaration();
    else if (match(TOKEN_IMPORT))
        importStatement();
    else
        statement();
    if (parser.panicMode)
        synchronize();
}

static void continueStatement()
{
    if (innermostLoopStart == -1)
    {
        error("Can't use 'continue' outside of a loop.");
        return;
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");

    // Discard any locals created inside the loop.
    for (int i = current->localCount - 1;
         i >= 0 && current->locals[i].depth > innermostLoopScopeDepth;
         i--)
    {
        emitByte(OP_POP);
    }

    // Jump to top of current innermost loop.
    emitLoop(innermostLoopStart);
}

static void breakStatement()
{
    if (innermostLoopStart == -1)
    {
        error("Can't use 'break' outside of a loop.");
        return;
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after 'break'.");
    for (int i = current->localCount - 1;
         i >= 0 && current->locals[i].depth > innermostLoopScopeDepth;
         i--)
    {
        emitByte(OP_POP);
    }
    breakAddress = emitJump(OP_JUMP);
}

static void statement()
{
    if (match(TOKEN_PRINT))
        printStatement();
    else if (match(TOKEN_FOR))
        forStatement();
    else if (match(TOKEN_IF))
        ifStatement();
    else if (match(TOKEN_SWITCH))
        switchStatement();
    else if (match(TOKEN_WHILE))
        whileStatement();
    else if (match(TOKEN_RETURN))
        returnStatement();
    else if (match(TOKEN_CONTINUE))
        continueStatement();
    else if (match(TOKEN_BREAK))
        breakStatement();
    else if (match(TOKEN_LEFT_BRACE))
    {
        beginScope();
        block();
        endScope();
    }

    else
        expressionStatement();
}

static void number(bool canAssign)
{
    double value = strtod(parser.prev.start, NULL);
    emitConst(NUM_VAL(value));
}

static void or_(bool canAssign)
{
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    int endJump = emitJump(OP_JUMP);

    patchJump(elseJump);
    emitByte(OP_POP);

    parsePrecedence(PREC_OR);
    patchJump(endJump);
}

static void and_(bool canAssign)
{
    int endJump = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP);
    parsePrecedence(PREC_AND);

    patchJump(endJump);
}

static void rawString(bool canAssign)
{
    emitConst(OBJ_VAL(copyString(parser.prev.start + 1, parser.prev.len - 2)));
}

static void templateString(bool canAssign)
{
    // Template strings can have escape sequences. Need to scan token chars and build up new string with converted escape sequences.

    int tokenLen = parser.prev.len - 2;
    int realLen = 0;

    char *new = ALLOCATE(char, tokenLen);

    for (int i = 1; i < tokenLen + 1; ++i)
    {
        char c = parser.prev.start[i];

        // Can't have escape sequence with only 1 char left
        if (i < tokenLen && c == '\\')
        {
            char next = parser.prev.start[++i];
            switch (next)
            {
            case '\n':
                break;
            case '\\':
                new[realLen++] = '\\';
                break;
            case '\'':
                new[realLen++] = '\'';
                break;
            case '\"':
                new[realLen++] = '\"';
                break;
            case 'n':
                new[realLen++] = '\n';
                break;
            case 't':
                new[realLen++] = '\t';
                break;
            case 'r':
                new[realLen++] = '\r';
                break;
            default:
                break;
            }
            continue;
        }

        new[realLen++] = c;
    }
    emitConst(OBJ_VAL(copyString(new, realLen)));
    FREE(char, new);
}

static void basicString(bool canAssign)
{
    templateString(canAssign);
}

static void namedVariable(Token name, bool canAssign)
{
#define SHORT_HAND_ASSIGN(op)            \
    do                                   \
    {                                    \
        emitBytes(getOp, (uint16_t)arg); \
        expression();                    \
        emitByte(op);                    \
        emitBytes(setOp, (uint16_t)arg); \
    } while (false)

    uint8_t getOp, setOp;
    int arg = resolveLocal(current, &name);

    if (arg != -1)
    {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    }
    else if ((arg = resolveUpValue(current, &name)) != -1)
    {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    }
    else
    {
        arg = identifierConst(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(TOKEN_EQUAL))
    {
        expression();
        emitBytes(setOp, (uint16_t)arg);
    }
    else if (canAssign && match(TOKEN_PLUS_EQUAL))
        SHORT_HAND_ASSIGN(OP_ADD);
    else if (canAssign && match(TOKEN_MINUS_EQUAL))
        SHORT_HAND_ASSIGN(OP_SUB);
    else if (canAssign && match(TOKEN_SLASH_EQUAL))
        SHORT_HAND_ASSIGN(OP_DIV);
    else if (canAssign && match(TOKEN_SLASH_SLASH_EQUAL))
        SHORT_HAND_ASSIGN(OP_INT_DIV);
    else if (canAssign && match(TOKEN_STAR_EQUAL))
        SHORT_HAND_ASSIGN(OP_MULT);
    else
        emitBytes(getOp, arg);
}

static void variable(bool canAssign)
{
    namedVariable(parser.prev, canAssign);
}

static void _this(bool canAssign)
{
    if (currentClass == NULL)
    {
        error("Can't use 'this' outside of a class");
        return;
    }
    variable(false);
}

static void _const(bool canAssign)
{
}

static void unary(bool canAssign)
{
    TokenType opType = parser.prev.type;

    parsePrecedence(PREC_UNARY);

    switch (opType)
    {
    case TOKEN_MINUS:
        emitByte(OP_NEGATE);
        break;
    case TOKEN_BANG:
        emitByte(OP_NOT);
        break;
    default:
        return;
    }
}

static void slice(bool canAssign)
{
    if (check(TOKEN_RIGHT_BRACKET))
        emitConst(NUM_VAL(-1.0));
    else
        expression();
    if (match(TOKEN_AT))
        expression();
    else
        emitConst(NUM_VAL(1.0));

    emitByte(OP_BUILD_SLICE);
}

static void sliceNoStart(bool canAssign)
{
    emitConst(NUM_VAL(0.0));
    if (check(TOKEN_RIGHT_BRACKET))
        emitConst(NUM_VAL(-1.0));
    else
        expression();

    if (match(TOKEN_AT))
        expression();
    else
        emitConst(NUM_VAL(1.0));

    emitByte(OP_BUILD_SLICE);
}

static void sliceJustStep(bool canAssign)
{
    emitConst(NUM_VAL(0.0));
    emitConst(NUM_VAL(-1.0));
    expression();

    emitByte(OP_BUILD_SLICE);
}

static void sliceNoEnd(bool canAssign)
{
    emitConst(NUM_VAL(-1.0));
    if (check(TOKEN_RIGHT_BRACKET))
        emitConst(NUM_VAL(1.0));
    else
        expression();
    emitByte(OP_BUILD_SLICE);
}

static void anonFunction(bool canAssign)
{
    markInitialized();
    function(TYPE_ANONYMOUS);
}

ParseRule rules[] = {
    [TOKEN_LEFT_BRACKET] = {defaultSizeList, subscript, PREC_SUBSCRIPT},
    [TOKEN_COLON] = {sliceNoStart, slice, PREC_OR},
    [TOKEN_COLON_AT] = {sliceJustStep, sliceNoEnd, PREC_OR},
    [TOKEN_RIGHT_BRACKET] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE] = {list, NULL, PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, dot, PREC_SUBSCRIPT},
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},
    [TOKEN_AMP] = {NULL, binary, PREC_TERM},
    [TOKEN_PIPE] = {NULL, binary, PREC_TERM},
    [TOKEN_MINUS_EQUAL] = {NULL, binary, PREC_TERM},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_PLUS_EQUAL] = {NULL, binary, PREC_TERM},
    [TOKEN_PERCENT] = {NULL, binary, PREC_FACTOR},
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_SLASH_EQUAL] = {NULL, binary, PREC_FACTOR},
    [TOKEN_SLASH_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_SLASH_SLASH_EQUAL] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR_EQUAL] = {NULL, binary, PREC_FACTOR},
    [TOKEN_STAR_STAR] = {NULL, binary, PREC_POWER},
    [TOKEN_BANG] = {unary, NULL, PREC_NONE},
    [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_EQUALITY},
    [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_RAW_STRING] = {rawString, NULL, PREC_NONE},
    [TOKEN_TEMPLATE_STRING] = {templateString, NULL, PREC_NONE},
    [TOKEN_BASIC_STRING] = {basicString, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {anonFunction, NULL, PREC_ASSIGNMENT},
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, or_, PREC_OR},
    [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
    [TOKEN_SUPER] = {super_, NULL, PREC_NONE},
    [TOKEN_THIS] = {_this, NULL, PREC_NONE},
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},
    [TOKEN_CONST] = {NULL, NULL, PREC_NONE},
    [TOKEN_WHILE] = {NULL, NULL, PREC_NONE},
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
    [TOKEN_IMPORT] = {NULL, NULL, PREC_NONE},
};

static void parsePrecedence(Precedence precedence)
{
    advance();
    ParseFn prefixRule = getRule(parser.prev.type)->prefix;
    if (prefixRule == NULL)
    {
        error("Expect expression");
        // emitByte(OP_NIL);
        return;
    }
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(parser.current.type)->precedence)
    {
        advance();
        ParseFn infixRule = getRule(parser.prev.type)->infix;
        infixRule(canAssign);
    }

    if (canAssign && match(TOKEN_EQUAL))
    {
        error("Invalid assignment target.");
    }
}

static uint16_t identifierConst(Token *name)
{
    return makeConstant(OBJ_VAL(copyString(name->start, name->len)));
}

static bool identifierEqual(Token *a, Token *b)
{
    if (a->len != b->len)
        return false;
    return memcmp(a->start, b->start, a->len) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name)
{
    for (int i = compiler->localCount - 1; i >= 0; i--)
    {
        Local *local = &compiler->locals[i];
        if (identifierEqual(name, &local->name))
        {
            if (local->depth == -1)
                error("Can't read local variable in its own initializer.");
            return i;
        }
    }
    return -1;
}

static int addUpValue(Compiler *compiler, uint8_t index, bool isLocal)
{
    int upValueCount = compiler->function->upValueCount;

    for (int i = 0; i < upValueCount; i++)
    {
        UpValue *upValue = &compiler->upValues[i];
        if (upValue->index == index && upValue->isLocal == isLocal)
            return i;
    }

    if (upValueCount == UINT16_COUNT)
    {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upValues[upValueCount].isLocal = isLocal;
    compiler->upValues[upValueCount].index = index;
    return compiler->function->upValueCount++;
}

static int resolveUpValue(Compiler *compiler, Token *name)
{
    if (compiler->enclosing == NULL)
        return -1;
    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1)
    {
        compiler->enclosing->locals[local].isCaptured = true;
        return addUpValue(compiler, (uint8_t)local, true);
    }

    int upValue = resolveUpValue(compiler->enclosing, name);
    if (upValue != -1)
        return addUpValue(compiler, (uint8_t)upValue, false);
    return -1;
}

static void addLocal(Token name)
{
    if (current->localCount == UINT16_COUNT)
    {
        error("Too many local variables in function.");
        return;
    }

    Local *local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = -1;
    local->isCaptured = false;
}

static void declareVar()
{
    if (current->scopeDepth == 0)
        return;
    Token *name = &parser.prev;
    for (int i = current->localCount - 1; i >= 0; i--)
    {
        Local *local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth)
        {
            break;
        }
        if (identifierEqual(name, &local->name))
        {
            error("There is already a variable with this name defined");
        }
    }

    addLocal(*name);
}

// static void declareConst()
// {
//     Token *name = &parser.prev;
//     if (current->scopeDepth == 0)
//     {
//         for (int i = currentChunk()->constants.size - 1; i >= 0; i--)
//         {
//             Value val = currentChunk()->constants.values[i];
//             if (identifierEqual(name, &(val)))
//                 error("There is already a constant with this name defined");
//         }
//     }
//     for (int i = current->localCount - 1; i >= 0; i--)
//     {
//         Local *local = &current->locals[i];
//         if (local->depth != -1 && local->depth < current->scopeDepth)
//         {
//             break;
//         }
//         if (identifierEqual(name, &local->name))
//         {
//             error("There is already a variable with this name defined");
//         }
//     }

//     addLocal(*name);
// }

static uint16_t parseVariable(const char *errMsg)
{
    consume(TOKEN_IDENTIFIER, errMsg);

    declareVar();
    if (current->scopeDepth > 0)
        return 0;

    return identifierConst(&parser.prev);
}

static void markInitialized()
{
    if (current->scopeDepth == 0)
        return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVar(uint16_t global)
{
    if (current->scopeDepth > 0)
    {
        markInitialized();
        return;
    }

    emitBytes(OP_DEF_GLOBAL, global);
}

static ParseRule *getRule(TokenType type)
{
    return &rules[type];
}

static void importStatement()
{

    if (match(TOKEN_TEMPLATE_STRING))
    {
        templateString(true);
        consume(TOKEN_SEMICOLON, "Expect ';' after import");
        emitByte(OP_IMPORT);
    }
}

ObjFunction *compile(const char *source, char *file, bool isRepl)
{
    initScanner(source);
    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT, file, isRepl);
    compiler.isRepl = isRepl;
    parser.panicMode = false;
    parser.hadError = false;
    advance();

    while (!match(TOKEN_EOF))
    {
        declaration();
    }

    consume(TOKEN_EOF, "Expect end of expr");
    ObjFunction *function = endCompiler();
    return parser.hadError ? NULL : function;
}

void markCompilerRoots()
{
    Compiler *compiler = current;
    while (compiler != NULL)
    {
        markObj((Obj *)compiler->function);
        compiler = compiler->enclosing;
    }
}