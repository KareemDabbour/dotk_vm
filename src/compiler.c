#include <string.h>

#include "include/common.h"
#include "include/compiler.h"
#include "include/memory.h"
#include "include/object.h"
#include "include/optimizer.h"
#include "include/scanner.h"

// #ifdef DEBUG_PRINT_CODE
#include "include/debug.h"
// #endif

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
    bool isConst;
} Local;

typedef struct _UpValue
{
    uint8_t index;
    bool isLocal;
    bool isConst;
} UpValue;

typedef enum _FunctionType
{
    TYPE_FUNCTION,
    TYPE_METHOD,
    TYPE_STATIC_METHOD,
    TYPE_INITIALIZER,
    TYPE_SCRIPT,
    TYPE_TRY,
    TYPE_CATCH,
    TYPE_ANONYMOUS
} FunctionType;

typedef struct _Compiler
{
    struct _Compiler *enclosing;
    ObjFunction *function;
    FunctionType type;
    bool isRepl;
    bool printBytecode;
    bool isInTryCatch;
    Local locals[LOCALS_MAX];
    int localCount;
    UpValue upValues[UINT8_COUNT];
    int scopeDepth;
    bool hasYield;
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
int numberOfBreaks = 0;
int breakAddresses[UINT8_COUNT] = {0};
bool inTernary = false;
int ternaryThen = -1;
int ternaryElse = -1;
static int gCompilerOptimizationLevel = 1;

static void expression();
static void declareVar(bool isConst);
static void namedVariable(Token name, bool canAssign);
static void statement();
static void yieldStatement();
static bool identifierEqual(Token *a, Token *b);
static void variable(bool canAssign);
static void varDeclaration();
static void importStatement();
static void fromImportStatement();
static void moduleDeclaration();
static void exportStatement();
static uint16_t identifierConst(Token *name);
static void defineVar(uint16_t global);
static void addLocal(Token name, bool isConst);
static void markInitialized();
static void declaration();
static ParseRule *getRule(TokenType type);
static void parsePrecedence(Precedence precedence);
static uint16_t parseVariable(const char *errMsg, bool isConst);
static uint16_t declareVariableFromToken(Token name, bool isConst);
static bool emitModuleSpecifier(Token *implicitAlias);
static int resolveLocal(Compiler *compiler, Token *name);
static int resolveUpValue(Compiler *compiler, Token *name);
static void beginScope();
static void endScope();
static Token synthToken(const char *text);
// static void optimizeFunction(ObjFunction *fn);

static inline Chunk *currentChunk()
{
    return &current->function->chunk;
}

static void errorAt(Token *token, const char *message)
{
    if (parser.panicMode)
        return;
    parser.panicMode = true;
    parser.hadError = true;
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
        // printf("TOKEN: %s - '%.*s'\n", TOKEN_NAMES[parser.current.type], parser.current.len, parser.current.start);
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
    writeChunk(currentChunk(), byte, parser.prev.line, parser.prev.col, parser.file, parser.prev.start - parser.prev.col + 1);
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
    Chunk *chunk = currentChunk();
    if (chunk != NULL && chunk->size > 0 && (chunk->code[chunk->size - 1] == OP_RETURN || chunk->code[chunk->size - 1] == OP_RETURN_NIL || chunk->code[chunk->size - 1] == OP_RETURN_THIS))
        return;
    if (current->type == TYPE_INITIALIZER)
        emitByte(OP_RETURN_THIS);
    else
        emitByte(OP_RETURN_NIL);
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

static void initCompiler(Compiler *compiler, FunctionType type, char *file, bool isRepl, bool printBytecode)
{
    parser.file = file;
    compiler->enclosing = current;
    compiler->function = NULL;
    compiler->type = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->hasYield = false;
    compiler->isRepl = isRepl;
    compiler->printBytecode = printBytecode;
    compiler->isInTryCatch = type == TYPE_TRY || type == TYPE_CATCH;
    compiler->function = newFunction();
    current = compiler;
    if (type == TYPE_ANONYMOUS)
        current->function->name = copyString("<anon_fn>", 10);
    else if (type == TYPE_TRY)
        current->function->name = copyString("<try>", 5);
    else if (type == TYPE_CATCH)
        current->function->name = copyString("<catch>", 7);
    else if (type != TYPE_SCRIPT)
        current->function->name = copyString(parser.prev.start, parser.prev.len);
    else
        current->function->name = copyString(file, strlen(file));

    Local *local = &current->locals[current->localCount++];
    local->depth = 0;
    local->name.start = "";
    local->name.len = 0;
    local->isCaptured = false;
    local->isConst = false;
    if (type != TYPE_FUNCTION && type != TYPE_TRY && type != TYPE_CATCH && type != TYPE_ANONYMOUS && type != TYPE_SCRIPT)
    {
        local->name.start = "this";
        local->name.len = 4;
    }
}

static ObjFunction *endCompiler()
{
    emitReturn();
    ObjFunction *func = current->function;

    if (!parser.hadError && gCompilerOptimizationLevel > 0)
        optimizeFunction(func, gCompilerOptimizationLevel);

    if (!parser.hadError)
    {
        char errBuf[256];
        if (!optimizerValidateFunction(func, errBuf, sizeof(errBuf)))
        {
            parser.hadError = true;
            const char *fname = (func->name == NULL) ? "<script>" : func->name->chars;
            fprintf(stderr, "%s:0:0 Error: optimizer produced invalid bytecode for %s: %s\n",
                    parser.file,
                    fname,
                    errBuf[0] == '\0' ? "unknown validation error" : errBuf);
        }
    }

    // #if DEBUG_PRINT_CODE
    if (unlikely(current->printBytecode))
        if (!parser.hadError)
        {
            const char *fname = func->name != NULL ? func->name->chars : "<script>";
            if (func->arity > 0)
            {
                printf("-- %s(", fname);
                for (int i = 0; i < func->paramCount; i++)
                {
                    if (i > 0)
                        printf(", ");
                    Value nameVal = func->chunk.constants.values[func->paramNameConsts[i]];
                    if (IS_STR(nameVal))
                        printf("%s", AS_STR(nameVal)->chars);
                    if (func->isVariadic && i == func->paramCount - 1)
                        printf("*");
                    else if (func->defaultStart >= 0 && i >= func->defaultStart)
                        printf("=...");
                }
                printf(") [minArity=%d] --\n", func->minArity);
            }
            disassembleChunk(currentChunk(), fname);
        }
    // #endif
    current = current->enclosing;
    return func;
}

static void list(bool canAssign)
{
    // Comprehension form:
    //   {for item from source => expr}
    //   {for item from source if cond => expr}
    // This keeps existing list literals untouched while offering a Python-like builder.
    if (match(TOKEN_FOR))
    {
        beginScope();

        emitBytes(OP_BUILD_LIST, (uint8_t)0);
        addLocal(synthToken("$comp_result"), false);
        markInitialized();
        int resultSlot = current->localCount - 1;

        consume(TOKEN_IDENTIFIER, "Expect loop variable name after 'for' in list comprehension.");
        Token itemName = parser.prev;

        consume(TOKEN_FROM, "Expect 'from' after loop variable in list comprehension.");
        expression();
        addLocal(synthToken("$comp_source"), true);
        markInitialized();
        int sourceSlot = current->localCount - 1;

        emitConst(NUM_VAL(0));
        addLocal(synthToken("$comp_index"), false);
        markInitialized();
        int indexSlot = current->localCount - 1;

        int loopStart = currentChunk()->size;

        emitBytes(OP_GET_LOCAL, (uint16_t)indexSlot);
        namedVariable(synthToken("len"), false);
        emitBytes(OP_GET_LOCAL, (uint16_t)sourceSlot);
        emitBytes(OP_CALL, (uint16_t)1);
        emitByte(OP_LESS);

        int exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);

        beginScope();
        emitBytes(OP_GET_LOCAL, (uint16_t)sourceSlot);
        emitBytes(OP_GET_LOCAL, (uint16_t)indexSlot);
        emitByte(OP_INDEX_SUBSCR);
        addLocal(itemName, false);
        markInitialized();

        int skipAppendJump = -1;
        int afterAppendJump = -1;
        if (match(TOKEN_IF))
        {
            expression();
            skipAppendJump = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP);
        }

        emitBytes(OP_GET_LOCAL, (uint16_t)resultSlot);
        consume(TOKEN_ARROW, "Expect '=>' before list comprehension element expression.");
        expression();
        emitBytes(OP_INVOKE, identifierConst(&(Token){.start = "append", .len = 6}));
        emitByte((uint8_t)1);
        emitByte(OP_POP);

        if (skipAppendJump != -1)
        {
            afterAppendJump = emitJump(OP_JUMP);
            patchJump(skipAppendJump);
            emitByte(OP_POP);
            patchJump(afterAppendJump);
        }

        endScope();

        emitBytes(OP_GET_LOCAL, (uint16_t)indexSlot);
        emitConst(NUM_VAL(1));
        emitByte(OP_ADD);
        emitBytes(OP_SET_LOCAL, (uint16_t)indexSlot);
        emitByte(OP_POP);

        emitLoop(loopStart);
        patchJump(exitJump);
        emitByte(OP_POP);

        consume(TOKEN_RIGHT_BRACE, "Expect '}' after list comprehension.");

        emitBytes(OP_GET_LOCAL, (uint16_t)resultSlot);
        endScope();
        return;
    }

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
        parsePrecedence(PREC_OR);
    else
        emitConst(NUM_VAL(0)); // This allows for empty lists to be created with either {} or [].

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
    else if (canAssign && match(TOKEN_PLUS_EQUAL))
    {
        expression();
        emitByte(OP_STORE_SUBSCR_ADD);
    }
    else if (canAssign && match(TOKEN_MINUS_EQUAL))
    {
        expression();
        emitByte(OP_STORE_SUBSCR_SUB);
    }
    else if (canAssign && match(TOKEN_STAR_EQUAL))
    {
        expression();
        emitByte(OP_STORE_SUBSCR_MULT);
    }
    else if (canAssign && match(TOKEN_SLASH_EQUAL))
    {
        expression();
        emitByte(OP_STORE_SUBSCR_DIV);
    }
    else if (canAssign && match(TOKEN_SLASH_SLASH_EQUAL))
    {
        expression();
        emitByte(OP_STORE_SUBSCR_INT_DIV);
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
    case TOKEN_CARET:
        emitByte(OP_BIN_XOR);
        break;
    case TOKEN_LESS_LESS:
        emitByte(OP_BIN_SHIFT_LEFT);
        break;
    case TOKEN_GREATER_GREATER:
        emitByte(OP_BIN_SHIFT_RIGHT);
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

typedef struct
{
    uint8_t positional;
    uint8_t keyword;
} ArgInfo;

static void addFunctionParamName(ObjFunction *function, Token *paramName)
{
    uint16_t constant = makeConstant(OBJ_VAL(copyString(paramName->start, paramName->len)));
    int oldCount = function->paramCount;
    function->paramNameConsts = GROW_ARRAY(uint16_t, function->paramNameConsts, oldCount, oldCount + 1);
    function->paramNameConsts[oldCount] = constant;
    function->paramCount = oldCount + 1;
}

static void addFunctionDefaultConst(ObjFunction *function, uint16_t constIndex)
{
    int oldCount = function->defaultCount;
    function->defaultConsts = GROW_ARRAY(uint16_t, function->defaultConsts, oldCount, oldCount + 1);
    function->defaultConsts[oldCount] = constIndex;
    function->defaultCount = oldCount + 1;
}

static void setFunctionLocalName(ObjFunction *function, int slotIndex, Token *localName)
{
    if (slotIndex < 0)
        return;

    if (function->localNameCount <= slotIndex)
    {
        int oldCount = function->localNameCount;
        int newCount = slotIndex + 1;
        function->localNameConsts = GROW_ARRAY(uint16_t, function->localNameConsts, oldCount, newCount);
        for (int i = oldCount; i < newCount; i++)
            function->localNameConsts[i] = UINT16_MAX;
        function->localNameCount = newCount;
    }

    if (localName == NULL || localName->start == NULL || localName->len <= 0)
    {
        function->localNameConsts[slotIndex] = UINT16_MAX;
        return;
    }

    uint16_t constant = makeConstant(OBJ_VAL(copyString(localName->start, localName->len)));
    function->localNameConsts[slotIndex] = constant;
}

static ArgInfo argList()
{
    ArgInfo info = {.positional = 0, .keyword = 0};
    bool hasKeywordArgs = false;
    if (!check(TOKEN_RIGHT_PAREN))
    {
        do
        {
            if (match(TOKEN_AT))
            {
                hasKeywordArgs = true;
                consume(TOKEN_IDENTIFIER, "Expect keyword argument name after '@'.");
                Token keywordName = parser.prev;
                if (!match(TOKEN_EQUAL) && !match(TOKEN_DOUBLE_COLON))
                    error("Expect '=' or '::' after keyword argument name.");

                emitConst(OBJ_VAL(copyString(keywordName.start, keywordName.len)));
                expression();

                if (info.keyword == 255)
                    error("Can't have more than 255 keyword arguments");
                info.keyword++;
                continue;
            }

            if (hasKeywordArgs)
                error("Positional arguments must come before keyword arguments.");

            expression();
            if (info.positional == 255)
                error("Can't have more than 255 arguments");
            info.positional++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments");
    return info;
}

static void call(bool canAssign)
{
    ArgInfo args = argList();
    if (args.keyword > 0)
    {
        emitByte(OP_CALL_KW);
        emitByte(args.positional);
        emitByte(args.keyword);
    }
    else
    {
        emitBytes(OP_CALL, args.positional);
    }
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
    else if (canAssign && match(TOKEN_LEFT_PAREN))
    {
        ArgInfo args = argList();

        emitBytes(args.keyword > 0 ? OP_INVOKE_KW : OP_INVOKE, name);
        emitByte(args.positional);
        if (args.keyword > 0)
            emitByte(args.keyword);
    }
    else if (canAssign && match(TOKEN_PLUS_EQUAL))
    {
        emitByte(OP_DUP);
        emitBytes(OP_GET_PROPERTY, name);
        expression();
        emitByte(OP_ADD);
        emitBytes(OP_SET_PROPERTY, name);
    }
    else if (canAssign && match(TOKEN_MINUS_EQUAL))
    {
        emitByte(OP_DUP);
        emitBytes(OP_GET_PROPERTY, name);
        expression();
        emitByte(OP_SUB);
        emitBytes(OP_SET_PROPERTY, name);
    }
    else if (canAssign && match(TOKEN_STAR_EQUAL))
    {
        emitByte(OP_DUP);
        emitBytes(OP_GET_PROPERTY, name);
        expression();
        emitByte(OP_MULT);
        emitBytes(OP_SET_PROPERTY, name);
    }
    else if (canAssign && match(TOKEN_SLASH_EQUAL))
    {
        emitByte(OP_DUP);
        emitBytes(OP_GET_PROPERTY, name);
        expression();
        emitByte(OP_DIV);
        emitBytes(OP_SET_PROPERTY, name);
    }
    else if (canAssign && match(TOKEN_SLASH_SLASH_EQUAL))
    {
        emitByte(OP_DUP);
        emitBytes(OP_GET_PROPERTY, name);
        expression();
        emitByte(OP_INT_DIV);
        emitBytes(OP_SET_PROPERTY, name);
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

static void returnStatement()
{
    if (current->type == TYPE_SCRIPT)
    {
        error("Can't return from top-level code");
        return;
    }

    if (match(TOKEN_SEMICOLON))
    {
        emitReturn();
    }
    else
    {
        if (current->type == TYPE_INITIALIZER)
            error("Can't return a value from a class's constructor.");

        if (current->hasYield)
            error("Can't return a value from a generator function.");

        expression();
        match(TOKEN_SEMICOLON);
        emitByte(OP_RETURN);
    }
}

static bool isImplicitYieldTerminator(TokenType type)
{
    switch (type)
    {
    case TOKEN_RIGHT_BRACE:
    case TOKEN_EOF:
    case TOKEN_PRINT:
    case TOKEN_FOR:
    case TOKEN_IF:
    case TOKEN_SWITCH:
    case TOKEN_WHILE:
    case TOKEN_RETURN:
    case TOKEN_CONTINUE:
    case TOKEN_BREAK:
    case TOKEN_TRY:
    case TOKEN_CATCH:
    case TOKEN_VAR:
    case TOKEN_CONST:
    case TOKEN_CLASS:
    case TOKEN_FUN:
    case TOKEN_IMPORT:
    case TOKEN_FROM:
    case TOKEN_MODULE:
    case TOKEN_EXPORT:
    case TOKEN_CASE:
    case TOKEN_DEFAULT:
    case TOKEN_YIELD:
        return true;
    default:
        return false;
    }
}

static void yieldStatement()
{
    if (current->type == TYPE_SCRIPT)
    {
        error("Can't yield from top-level code");
        return;
    }

    current->hasYield = true;
    if (match(TOKEN_SEMICOLON) || isImplicitYieldTerminator(parser.current.type))
        emitByte(OP_NIL);
    else
    {
        expression();
        match(TOKEN_SEMICOLON);
    }

    emitByte(OP_YIELD);
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
    initCompiler(&compiler, type, parser.file, current->isRepl, current->printBytecode);
    beginScope();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    bool hasVariadicParam = false;
    bool hasDefaults = false;
    if (!check(TOKEN_RIGHT_PAREN))
    {
        do
        {
            bool isVariadicParam = match(TOKEN_STAR);
            if (isVariadicParam)
            {
                if (hasVariadicParam)
                    error("Function can only have one variadic parameter.");
                hasVariadicParam = true;
            }
            else if (hasVariadicParam)
            {
                error("Variadic parameter must be the last parameter.");
            }

            consume(TOKEN_IDENTIFIER, "Expect parameter name.");
            Token parameter = parser.prev;

            current->function->arity++;
            current->function->isVariadic = hasVariadicParam;

            if (current->function->arity >= UINT16_MAX)
                errorAtCurrent("Can't have more than 65535 parameters");

            declareVar(false);
            addFunctionParamName(current->function, &parameter);
            defineVar(0);

            // Check for default value: param = <expression>
            if (!isVariadicParam && match(TOKEN_EQUAL))
            {
                hasDefaults = true;
                if (current->function->defaultStart < 0)
                    current->function->defaultStart = current->function->arity - 1;
                current->function->defaultCount++;

                // Get the local slot for this parameter
                uint8_t slot = (uint8_t)(current->localCount - 1);
                // Emit: OP_DEFAULT_LOCAL slot offset_placeholder
                // If the local is not UNDEF, jump past the default expression
                emitByte(OP_DEFAULT_LOCAL);
                emitByte(slot);
                int patchOffset = currentChunk()->size;
                emitByte(0xFF);
                emitByte(0xFF);

                // Compile the default expression
                expression();

                // Store result in the parameter's local slot
                emitByte(OP_SET_LOCAL);
                emitByte(slot);
                emitByte(OP_POP);

                // Patch the jump offset to skip past the default expr
                patchJump(patchOffset);
            }
            else if (hasDefaults && !isVariadicParam)
            {
                error("Non-default parameter follows default parameter.");
            }
            else
            {
                // Non-default, non-variadic param: count towards minArity
                if (!hasVariadicParam)
                    current->function->minArity = current->function->arity;
                else if (!isVariadicParam)
                    current->function->minArity = current->function->arity - 1;
            }

            if (hasVariadicParam && check(TOKEN_COMMA))
            {
                error("Variadic parameter must be last.");
            }
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    if (match(TOKEN_ARROW))
        returnStatement();
    else
    {
        consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
        block();
    }
    // optimizeFunction(current->function);
    ObjFunction *function = endCompiler();
    function->isGenerator = compiler.hasYield;
    uint16_t constant = makeConstant(OBJ_VAL(function));
    emitBytes(OP_CLOSURE, constant);
    if (check(TOKEN_LEFT_PAREN))
    {
        consume(TOKEN_LEFT_PAREN, "Expect ')' after parameters.");
        ArgInfo args = argList();
        if (args.keyword > 0)
        {
            emitByte(OP_CALL_KW);
            emitByte(args.positional);
            emitByte(args.keyword);
        }
        else
        {
            emitBytes(OP_CALL, args.positional);
        }
    }

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

        match(TOKEN_SEMICOLON);
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
        ArgInfo args = argList();
        namedVariable(synthToken("super"), false);
        emitBytes(args.keyword > 0 ? OP_SUPER_INVOKE_KW : OP_SUPER_INVOKE, name);
        emitByte(args.positional);
        if (args.keyword > 0)
            emitByte(args.keyword);
    }
    else
    {
        namedVariable(synthToken("super"), false);
        emitBytes(OP_GET_SUPER, name);
    }
}

static void createBuiltinClass()
{
    Token className = synthToken("String");
    uint16_t nameConst = identifierConst(&className);
    declareVar(false);
    emitBytes(OP_CLASS, nameConst);
    defineVar(nameConst);
    ClassCompiler classCompiler;
    classCompiler.enclosing = currentClass;
    classCompiler.hasSuperClass = false;
}

static void classDeclaration()
{
    consume(TOKEN_IDENTIFIER, "Expect class name");
    Token className = parser.prev;
    uint16_t nameConst = identifierConst(&parser.prev);
    declareVar(false);

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
        addLocal(synthToken("super"), false);
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
    if (check(TOKEN_LEFT_PAREN))
    {
        function(TYPE_ANONYMOUS);
        return;
    }
    uint16_t global = parseVariable("Expect function name", false);
    markInitialized();
    function(TYPE_FUNCTION);
    defineVar(global);
}

static void varDeclaration()
{
    uint16_t global = parseVariable("Expect variable name", false);

    if (match(TOKEN_EQUAL))
    {
        expression();
    }
    else
    {
        emitByte(OP_NIL);
    }
    // consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
    match(TOKEN_SEMICOLON);

    defineVar(global);
}

static void constDeclaration()
{
    uint16_t global = parseVariable("Expect constant name", true);
    consume(TOKEN_EQUAL, "Expect '=' after constant name");
    expression();
    // consume(TOKEN_SEMICOLON, "Expect ';' after constant declaration.");
    match(TOKEN_SEMICOLON);
    if (current->scopeDepth > 0)
        defineVar(global);
    else
        emitBytes(OP_DEF_CONST_GLOBAL, global);
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
        // consume(TOKEN_SEMICOLON, "Expect ';' after expression");
        match(TOKEN_SEMICOLON);
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
        patchJump(previousCaseSkip);

    for (int i = 0; i < caseCount; i++)
    {
        patchJump(caseEnds[i]);
    }
    emitByte(OP_POP);
}

static void forStatement()
{
    beginScope();

    if (!check(TOKEN_LEFT_PAREN))
    {
        bool loopVarIsConst = false;
        if (match(TOKEN_CONST))
            loopVarIsConst = true;
        else
            match(TOKEN_VAR);

        consume(TOKEN_IDENTIFIER, "Expect loop variable name after 'for'.");
        Token loopVarName = parser.prev;
        consume(TOKEN_IN, "Expect 'in' after loop variable in for-in loop.");

        expression();
        addLocal(synthToken("$for_iterable"), true);
        markInitialized();
        int iterableSlot = current->localCount - 1;

        emitBytes(OP_GET_LOCAL, (uint16_t)iterableSlot);
        emitBytes(OP_INVOKE, identifierConst(&(Token){.start = "_iter_", .len = 6}));
        emitByte((uint8_t)0);
        addLocal(synthToken("$for_iterator"), true);
        markInitialized();
        int iteratorSlot = current->localCount - 1;

        emitByte(OP_NIL);
        addLocal(synthToken("$for_next"), false);
        markInitialized();
        int nextSlot = current->localCount - 1;

        int surroundingLoopStart = innermostLoopStart;
        int surroundingLoopScopeDepth = innermostLoopScopeDepth;
        int numberOfBreaksPrior = numberOfBreaks;
        innermostLoopStart = currentChunk()->size;
        innermostLoopScopeDepth = current->scopeDepth;

        emitBytes(OP_GET_LOCAL, (uint16_t)iteratorSlot);
        emitBytes(OP_INVOKE, identifierConst(&(Token){.start = "_next_", .len = 6}));
        emitByte((uint8_t)0);
        emitBytes(OP_SET_LOCAL, (uint16_t)nextSlot);
        emitByte(OP_POP);

        emitBytes(OP_GET_LOCAL, (uint16_t)nextSlot);
        emitByte(OP_NIL);
        emitByte(OP_EQUAL);
        int continueJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
        int exitJump = emitJump(OP_JUMP);
        patchJump(continueJump);
        emitByte(OP_POP);

        beginScope();
        emitBytes(OP_GET_LOCAL, (uint16_t)nextSlot);
        addLocal(loopVarName, loopVarIsConst);
        markInitialized();

        statement();

        endScope();
        emitLoop(innermostLoopStart);
        patchJump(exitJump);

        if (numberOfBreaksPrior < numberOfBreaks)
        {
            for (int i = numberOfBreaksPrior; i < numberOfBreaks; i++)
            {
                patchJump(breakAddresses[i]);
            }
        }

        innermostLoopStart = surroundingLoopStart;
        innermostLoopScopeDepth = surroundingLoopScopeDepth;
        numberOfBreaks = numberOfBreaksPrior;

        endScope();
        return;
    }

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
    int numberOfBreaksPrior = numberOfBreaks;
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

    // int innerVar = -1;
    // if (loopVar != -1)
    // {
    //     beginScope();
    //     emitBytes(OP_GET_LOCAL, (uint8_t)loopVar);
    //     addLocal(loopVarName);
    //     markInitialized();

    //     innerVar = current->localCount - 1;
    // }

    statement();

    // if (loopVar != -1)
    // {
    //     emitBytes(OP_GET_LOCAL, (uint8_t)innerVar);
    //     emitBytes(OP_SET_LOCAL, (uint8_t)loopVar);
    //     emitByte(OP_POP);
    //     endScope();
    // }

    emitLoop(innermostLoopStart);

    if (exitJump != -1)
    {
        patchJump(exitJump);
        emitByte(OP_POP); // pop the condition off the stack
    }

    if (numberOfBreaksPrior < numberOfBreaks)
    {
        for (int i = numberOfBreaksPrior; i < numberOfBreaks; i++)
        {
            patchJump(breakAddresses[i]);
        }
    }

    innermostLoopStart = surroundingLoopStart;
    innermostLoopScopeDepth = surroundingLoopScopeDepth;
    numberOfBreaks = numberOfBreaksPrior;

    endScope();
}

static void ifStatement()
{
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression");
    int jumpToElseBlock = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();

    int getPastElse = emitJump(OP_JUMP);

    patchJump(jumpToElseBlock);
    emitByte(OP_POP);

    if (match(TOKEN_ELSE))
        statement();
    patchJump(getPastElse);
}

static void printStatement()
{
    expression();
    // consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    match(TOKEN_SEMICOLON);
    emitByte(OP_PRINT);
}

static void whileStatement()
{
    // beginScope();

    int surroundingLoopStart = innermostLoopStart;
    int numberOfBreaksPrior = numberOfBreaks;
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
    if (numberOfBreaksPrior < numberOfBreaks)
    {
        for (int i = numberOfBreaksPrior; i < numberOfBreaks; i++)
        {
            patchJump(breakAddresses[i]);
        }
    }
    numberOfBreaks = numberOfBreaksPrior;
    innermostLoopStart = surroundingLoopStart;
    innermostLoopScopeDepth = surroundingLoopScopeDepth;
    innermostLoopEnd = surroundingLoopEnd;
    // endScope();
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
        case TOKEN_CONST:
        case TOKEN_MODULE:
        case TOKEN_EXPORT:
        case TOKEN_FROM:
        case TOKEN_IMPORT:
        case TOKEN_FOR:
        case TOKEN_IF:
        case TOKEN_WHILE:
        case TOKEN_PRINT:
        case TOKEN_RETURN:
        case TOKEN_YIELD:
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
    else if (match(TOKEN_MODULE))
        moduleDeclaration();
    else if (match(TOKEN_EXPORT))
        exportStatement();
    else if (match(TOKEN_FROM))
        fromImportStatement();
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
        error("Can't use 'continue' outside of a loop.\n\tThis includes trying to break in a try-catch block surrounded by a loop.\n\tHINT: You can use 'return;' to escape try-catch blocks. Or you can wrap the loop in the try-catch block.");
        return;
    }

    // consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
    match(TOKEN_SEMICOLON);

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
        error("Can't use 'break' outside of a loop.\n\tThis includes trying to break in a try-catch block surrounded by a loop.\n\tHINT: You can use 'return;' to escape try-catch blocks. Or you can wrap the loop in the try-catch block.");
        return;
    }

    // consume(TOKEN_SEMICOLON, "Expect ';' after 'break'.");
    match(TOKEN_SEMICOLON);
    // if (!current->isInTryCatch)
    // {
    for (int i = current->localCount - 1;
         i >= 0 && current->locals[i].depth > innermostLoopScopeDepth;
         i--)
    {
        emitByte(OP_POP);
    }
    // }

    breakAddresses[numberOfBreaks++] = emitJump(OP_JUMP);
}

static void enterTryCatch()
{
    // This is a bit of a hack. We're going to compile the try block as a function, then store it in the closure of the try-catch block.

    // And because of this, we need to save the state of the current compiler, and then restore it after we're done compiling the try block.
    int surroundingLoopStart = innermostLoopStart;
    int surroundingBreakAddrCount = numberOfBreaks;
    int surroundingLoopScopeDepth = innermostLoopScopeDepth;
    int surroundingLoopEnd = innermostLoopEnd;
    innermostLoopStart = -1;
    numberOfBreaks = 0;
    innermostLoopScopeDepth = 0;
    innermostLoopEnd = -1;
    Compiler compiler;
    initCompiler(&compiler, TYPE_TRY, parser.file, current->isRepl, current->printBytecode);
    beginScope();
    statement();
    endScope();
    ObjFunction *function = endCompiler();

    uint16_t constant = makeConstant(OBJ_VAL(function));
    emitBytes(OP_CLOSURE, constant);
    for (int i = 0; i < function->upValueCount; i++)
    {
        emitByte(compiler.upValues[i].isLocal ? 1 : 0);
        emitByte(compiler.upValues[i].index);
    }
    ////
    emitByte(OP_TRY);
    int jumpToElseBlock = emitJump(OP_JUMP_IF_FALSE);

    emitByte(OP_POP); // pop the successful try() condition (true)

    int getPastElse = emitJump(OP_JUMP);

    patchJump(jumpToElseBlock);
    emitByte(OP_POP);

    //// Now we compile the catch block
    consume(TOKEN_CATCH, "Expect 'catch' after try block");
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'catch'");

    innermostLoopStart = -1;
    numberOfBreaks = 0;
    innermostLoopScopeDepth = 0;
    innermostLoopEnd = -1;
    initCompiler(&compiler, TYPE_CATCH, parser.file, current->isRepl, current->printBytecode);
    beginScope();
    uint16_t catchVar = parseVariable("Expect variable name", false);
    defineVar(catchVar);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after catch variable");
    statement();
    endScope();
    function = endCompiler();
    function->arity++;
    constant = makeConstant(OBJ_VAL(function));
    emitBytes(OP_CLOSURE, constant);
    for (int i = 0; i < function->upValueCount; i++)
    {
        emitByte(compiler.upValues[i].isLocal ? 1 : 0);
        emitByte(compiler.upValues[i].index);
    }

    emitByte(OP_CATCH);
    emitByte(OP_POP); // discard catch block return value
    // statement();

    patchJump(getPastElse);
    innermostLoopStart = surroundingLoopStart;
    numberOfBreaks = surroundingBreakAddrCount;
    innermostLoopScopeDepth = surroundingLoopScopeDepth;
    innermostLoopEnd = surroundingLoopEnd;
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
    else if (match(TOKEN_YIELD))
        yieldStatement();
    else if (match(TOKEN_CONTINUE))
        continueStatement();
    else if (match(TOKEN_BREAK))
        breakStatement();
    else if (match(TOKEN_TRY))
        enterTryCatch();
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
    size_t len = parser.prev.len;
    char *raw = ALLOCATE(char, len + 1);
    memcpy(raw, parser.prev.start, len);
    raw[len] = '\0';

    double value = 0;

    if (len > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X'))
    {
        uint64_t acc = 0;
        bool hasDigit = false;
        for (int i = 2; i < len; i++)
        {
            char c = raw[i];
            int d = -1;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F')
                d = 10 + (c - 'A');
            else
            {
                error("Invalid hex literal.");
                FREE_ARRAY(char, raw, len + 1);
                emitConst(NUM_VAL(0));
                return;
            }
            hasDigit = true;
            acc = (acc << 4) | (uint64_t)d;
        }
        if (!hasDigit)
            error("Hex literal must contain at least one digit.");
        value = (double)acc;
    }
    else if (len > 2 && raw[0] == '0' && (raw[1] == 'b' || raw[1] == 'B'))
    {
        uint64_t acc = 0;
        bool hasDigit = false;
        for (int i = 2; i < len; i++)
        {
            char c = raw[i];
            if (c != '0' && c != '1')
            {
                error("Invalid binary literal.");
                FREE_ARRAY(char, raw, len + 1);
                emitConst(NUM_VAL(0));
                return;
            }
            hasDigit = true;
            acc = (acc << 1) | (uint64_t)(c - '0');
        }
        if (!hasDigit)
            error("Binary literal must contain at least one digit.");
        value = (double)acc;
    }
    else if (len > 2 && raw[0] == '0' && (raw[1] == 'y' || raw[1] == 'Y'))
    {
        int acc = 0;
        bool hasDigit = false;
        for (int i = 2; i < len; i++)
        {
            char c = raw[i];
            if (c < '0' || c > '9')
            {
                error("Invalid byte literal.");
                FREE_ARRAY(char, raw, len + 1);
                emitConst(NUM_VAL(0));
                return;
            }
            hasDigit = true;
            acc = (acc * 10) + (c - '0');
        }
        if (!hasDigit)
            error("Byte literal must contain at least one digit.");
        if (acc < 0 || acc > 255)
            error("Byte literal out of range. Expected value in [0, 255].");
        value = (double)acc;
    }
    else
    {
        value = strtod(raw, NULL);
    }

    FREE_ARRAY(char, raw, len + 1);
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

static void interpolateString(bool canAssign)
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
            case '0':
            {
                if (i + 2 < tokenLen && parser.prev.start[i + 1] == '3' && parser.prev.start[i + 2] == '3')
                {
                    new[realLen++] = '\033';
                    i += 2;
                }
                else
                    new[realLen++] = '\0';
                // new[realLen++] = '\0';
                break;
            }
            case 'e':
                new[realLen++] = '\x1b';
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
            case '{':
                new[realLen++] = '{';
                break;
            case '}':
                new[realLen++] = '}';
                break;
            default:
                break;
            }
            continue;
        }

        new[realLen++] = c;
    }
    emitConst(OBJ_VAL(copyString(new, realLen)));
    FREE_ARRAY(char, new, tokenLen);
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
            case '0':
            {
                if (i + 2 < tokenLen && parser.prev.start[i + 1] == '3' && parser.prev.start[i + 2] == '3')
                {
                    new[realLen++] = '\033';
                    i += 2;
                }
                else
                    new[realLen++] = '\0';
                // new[realLen++] = '\0';
                break;
            }
            case 'e':
                new[realLen++] = '\x1b';
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
                new[realLen++] = c;
                new[realLen++] = next;
                break;
            }
            continue;
        }

        new[realLen++] = c;
    }
    emitConst(OBJ_VAL(copyString(new, realLen)));
    FREE_ARRAY(char, new, tokenLen);
}

static void interpolation(bool canAssign)
{
    int count = 0;
    do
    {
        bool concat = false;
        if (parser.prev.len > 2)
        {
            concat = true;
            interpolateString(canAssign);
            if (count > 0)
                emitByte(OP_ADD);
        }
        expression();
        if (concat || (count > 0 && !concat))
            emitByte(OP_ADD);
        count++;
    } while (match(TOKEN_INTERPOLATION));

    consume(TOKEN_RAW_STRING, "Expect string after interpolation");
    if (parser.prev.len > 2)
    {
        interpolateString(canAssign);
        emitByte(OP_ADD);
    }
}

static void basicString(bool canAssign)
{
    templateString(canAssign);
}

static void byteString(bool canAssign)
{
    templateString(canAssign);
}

static int hexDigitVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static bool isIgnoredByteSep(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '_';
}

static void hexString(bool canAssign)
{
    int inLen = parser.prev.len - 2;
    const char *src = parser.prev.start + 1;
    char *out = ALLOCATE(char, inLen + 1);
    int outLen = 0;
    int hi = -1;

    for (int i = 0; i < inLen; i++)
    {
        char c = src[i];
        if (isIgnoredByteSep(c))
            continue;

        int d = hexDigitVal(c);
        if (d < 0)
        {
            FREE_ARRAY(char, out, inLen + 1);
            error("Invalid hex string literal. Use only [0-9a-fA-F], spaces, or '_' separators.");
            emitConst(OBJ_VAL(copyString("", 0)));
            return;
        }

        if (hi < 0)
            hi = d;
        else
        {
            out[outLen++] = (char)((hi << 4) | d);
            hi = -1;
        }
    }

    if (hi >= 0)
    {
        FREE_ARRAY(char, out, inLen + 1);
        error("Hex string literal must contain an even number of hex digits.");
        emitConst(OBJ_VAL(copyString("", 0)));
        return;
    }

    emitConst(OBJ_VAL(copyString(out, outLen)));
    FREE_ARRAY(char, out, inLen + 1);
}

static void binaryString(bool canAssign)
{
    int inLen = parser.prev.len - 2;
    const char *src = parser.prev.start + 1;
    char *out = ALLOCATE(char, inLen + 1);
    int outLen = 0;
    int bits = 0;
    uint8_t acc = 0;

    for (int i = 0; i < inLen; i++)
    {
        char c = src[i];
        if (isIgnoredByteSep(c))
            continue;

        if (c != '0' && c != '1')
        {
            FREE_ARRAY(char, out, inLen + 1);
            error("Invalid binary string literal. Use only 0/1 with optional spaces or '_' separators.");
            emitConst(OBJ_VAL(copyString("", 0)));
            return;
        }

        acc = (uint8_t)((acc << 1) | (uint8_t)(c - '0'));
        bits++;
        if (bits == 8)
        {
            out[outLen++] = (char)acc;
            bits = 0;
            acc = 0;
        }
    }

    if (bits != 0)
    {
        FREE_ARRAY(char, out, inLen + 1);
        error("Binary string literal must contain full bytes (groups of 8 bits).");
        emitConst(OBJ_VAL(copyString("", 0)));
        return;
    }

    emitConst(OBJ_VAL(copyString(out, outLen)));
    FREE_ARRAY(char, out, inLen + 1);
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
    bool isConst = false;
    bool isUnresolved = false;

    if (arg != -1)
    {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
        isConst = current->locals[arg].isConst;
    }
    else if ((arg = resolveUpValue(current, &name)) != -1)
    {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
        isConst = current->upValues[arg].isConst;
    }
    else
    {
        arg = identifierConst(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
        isUnresolved = true;
    }

    if (canAssign && match(TOKEN_EQUAL))
    {
        if (isConst)
        {
            error("Can't assign to a constant.");
            expression();
            return;
        }

        expression();

        emitBytes(setOp, (uint16_t)arg);
    }
    else if (canAssign && match(TOKEN_PLUS_EQUAL))
    {
        if (isConst)
        {
            error("Can't assign to a constant.");
            expression();
        }
        else
            SHORT_HAND_ASSIGN(OP_ADD);
    }
    else if (canAssign && match(TOKEN_MINUS_EQUAL))
    {
        if (isConst)
        {
            error("Can't assign to a constant.");
            expression();
        }
        else
            SHORT_HAND_ASSIGN(OP_SUB);
    }
    else if (canAssign && match(TOKEN_SLASH_EQUAL))
    {
        if (isConst)
        {
            error("Can't assign to a constant.");
            expression();
        }
        else
            SHORT_HAND_ASSIGN(OP_DIV);
    }
    else if (canAssign && match(TOKEN_SLASH_SLASH_EQUAL))
    {
        if (isConst)
        {
            error("Can't assign to a constant.");
            expression();
        }
        else
            SHORT_HAND_ASSIGN(OP_INT_DIV);
    }
    else if (canAssign && match(TOKEN_STAR_EQUAL))
    {
        if (isConst)
        {
            error("Can't assign to a constant.");
            expression();
        }
        else
            SHORT_HAND_ASSIGN(OP_MULT);
    }
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

static void sliceOrTernary(bool canAssign)
{
    if (inTernary)
    {
        int oldTern = ternaryElse;
        ternaryElse = emitJump(OP_JUMP);
        patchJump(ternaryThen);
        emitByte(OP_POP);
        expression();
        patchJump(ternaryElse);
        ternaryElse = oldTern;
        inTernary = false;
        return;
    }
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
    function(TYPE_ANONYMOUS);
}

static void trinary(bool canAssign)
{
    inTernary = true;
    int oldTer = ternaryThen;
    ternaryThen = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // pop the condition

    expression();
    ternaryThen = oldTer;
}

static void map(bool canAssign)
{

    uint16_t itemCount = 0;
    if (!check(TOKEN_RIGHT_BRACE))
    {
        do
        {
            if (check(TOKEN_RIGHT_BRACE))
                break;
            expression();
            consume(TOKEN_DOUBLE_COLON, "Expect '::' after key in map literal");
            expression();

            if (itemCount == UINT16_COUNT)
                error("Cannot have more than 256 items in a list literal");
            itemCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after map literal");
    emitBytes(OP_BUILD_MAP, itemCount);
}

ParseRule rules[] = {
    [TOKEN_LEFT_BRACKET] = {defaultSizeList, subscript, PREC_SUBSCRIPT},
    [TOKEN_DOT_BRACKET] = {map, NULL, PREC_NONE},
    [TOKEN_COLON] = {sliceNoStart, sliceOrTernary, PREC_OR},
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
    [TOKEN_CARET] = {NULL, binary, PREC_TERM},
    [TOKEN_LESS_LESS] = {NULL, binary, PREC_TERM},
    [TOKEN_GREATER_GREATER] = {NULL, binary, PREC_TERM},
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
    [TOKEN_INTERPOLATION] = {interpolation, NULL, PREC_NONE},
    [TOKEN_BASIC_STRING] = {basicString, NULL, PREC_NONE},
    [TOKEN_BYTE_STRING] = {byteString, NULL, PREC_NONE},
    [TOKEN_HEX_STRING] = {hexString, NULL, PREC_NONE},
    [TOKEN_BINARY_STRING] = {binaryString, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_AND] = {NULL, and_, PREC_AND},
    [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
    [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
    [TOKEN_FUN] = {anonFunction, NULL, PREC_NONE},
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},
    [TOKEN_IN] = {NULL, NULL, PREC_NONE},
    [TOKEN_NIL] = {literal, NULL, PREC_NONE},
    [TOKEN_OR] = {NULL, or_, PREC_OR},
    [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
    [TOKEN_YIELD] = {NULL, NULL, PREC_NONE},
    [TOKEN_SUPER] = {super_, NULL, PREC_NONE},
    [TOKEN_THIS] = {_this, NULL, PREC_NONE},
    [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
    [TOKEN_VAR] = {NULL, NULL, PREC_NONE},
    [TOKEN_CONST] = {NULL, NULL, PREC_NONE},
    [TOKEN_WHILE] = {NULL, NULL, PREC_NONE},
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
    [TOKEN_IMPORT] = {NULL, NULL, PREC_NONE},
    [TOKEN_FROM] = {NULL, NULL, PREC_NONE},
    [TOKEN_AS] = {NULL, NULL, PREC_NONE},
    [TOKEN_MODULE] = {NULL, NULL, PREC_NONE},
    [TOKEN_EXPORT] = {NULL, NULL, PREC_NONE},
    [TOKEN_QUESTION] = {NULL, trinary, PREC_ASSIGNMENT},
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

static int addUpValue(Compiler *compiler, uint8_t index, bool isLocal, bool isConst)
{
    int upValueCount = compiler->function->upValueCount;

    for (int i = 0; i < upValueCount; i++)
    {
        UpValue *upValue = &compiler->upValues[i];
        if (upValue->index == index && upValue->isLocal == isLocal && upValue->isConst == isConst)
            return i;
    }

    if (upValueCount == UINT16_COUNT)
    {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upValues[upValueCount].isLocal = isLocal;
    compiler->upValues[upValueCount].index = index;
    compiler->upValues[upValueCount].isConst = isConst;
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
        return addUpValue(compiler, (uint8_t)local, true, compiler->enclosing->locals[local].isConst);
    }

    int upValue = resolveUpValue(compiler->enclosing, name);
    if (upValue != -1)
        return addUpValue(compiler, (uint8_t)upValue, false, compiler->enclosing->upValues[upValue].isConst);
    return -1;
}

static void addLocal(Token name, bool isConst)
{
    if (current->localCount == LOCALS_MAX)
    {
        error("Too many local variables in function.");
        return;
    }

    Local *local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = -1;
    local->isCaptured = false;
    local->isConst = isConst;

    setFunctionLocalName(current->function, current->localCount - 1, &name);
}

static void declareVar(bool isConst)
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

    addLocal(*name, isConst);
}

static uint16_t parseVariable(const char *errMsg, bool isConst)
{
    consume(TOKEN_IDENTIFIER, errMsg);

    declareVar(isConst);
    if (current->scopeDepth > 0)
        return 0;

    return identifierConst(&parser.prev);
}

static uint16_t declareVariableFromToken(Token name, bool isConst)
{
    Token savedPrev = parser.prev;
    parser.prev = name;
    declareVar(isConst);
    parser.prev = savedPrev;

    if (current->scopeDepth > 0)
        return 0;

    return identifierConst(&name);
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

static void moduleDeclaration()
{
    consume(TOKEN_IDENTIFIER, "Expect module name after 'module'.");

    if (match(TOKEN_LEFT_BRACE))
    {
        while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
            declaration();
        consume(TOKEN_RIGHT_BRACE, "Expect '}' after module body.");
        match(TOKEN_SEMICOLON);
        return;
    }

    match(TOKEN_SEMICOLON);
}

static void appendModulePathSegment(char **buffer, int *len, int *capacity, const char *segmentStart, int segmentLen)
{
    int needed = *len + segmentLen + 1;
    if (needed > *capacity)
    {
        int oldCap = *capacity;
        int newCap = GROW_CAPACITY(oldCap);
        while (newCap < needed)
            newCap = GROW_CAPACITY(newCap);
        *buffer = GROW_ARRAY(char, *buffer, oldCap, newCap);
        *capacity = newCap;
    }

    memcpy(*buffer + *len, segmentStart, segmentLen);
    *len += segmentLen;
    (*buffer)[*len] = '\0';
}

static bool emitModuleSpecifier(Token *implicitAlias)
{
    if (!check(TOKEN_IDENTIFIER))
    {
        expression();
        return false;
    }

    char *modulePath = NULL;
    int pathLen = 0;
    int pathCap = 0;

    consume(TOKEN_IDENTIFIER, "Expect module name.");
    if (implicitAlias != NULL)
        *implicitAlias = parser.prev;
    appendModulePathSegment(&modulePath, &pathLen, &pathCap, parser.prev.start, parser.prev.len);

    while (match(TOKEN_DOT))
    {
        appendModulePathSegment(&modulePath, &pathLen, &pathCap, "/", 1);
        consume(TOKEN_IDENTIFIER, "Expect identifier after '.' in module path.");
        if (implicitAlias != NULL)
            *implicitAlias = parser.prev;
        appendModulePathSegment(&modulePath, &pathLen, &pathCap, parser.prev.start, parser.prev.len);
    }

    emitConst(OBJ_VAL(copyString(modulePath, pathLen)));
    FREE_ARRAY(char, modulePath, pathCap);
    return true;
}

static void exportStatement()
{
    if (match(TOKEN_CONST))
    {
        uint16_t global = parseVariable("Expect constant name", true);
        uint16_t symbol = identifierConst(&parser.prev);
        consume(TOKEN_EQUAL, "Expect '=' after constant name");
        expression();
        match(TOKEN_SEMICOLON);

        if (current->scopeDepth > 0)
            defineVar(global);
        else
            emitBytes(OP_DEF_CONST_GLOBAL, global);

        emitBytes(OP_EXPORT, symbol);
        return;
    }

    do
    {
        consume(TOKEN_IDENTIFIER, "Expect exported symbol name.");
        uint16_t symbol = identifierConst(&parser.prev);
        emitBytes(OP_EXPORT, symbol);
    } while (match(TOKEN_COMMA));

    match(TOKEN_SEMICOLON);
}

static void importStatement()
{
    Token implicitAlias;
    bool hasImplicitAlias = emitModuleSpecifier(&implicitAlias);
    emitByte(OP_IMPORT);

    if (match(TOKEN_AS))
    {
        uint16_t alias = parseVariable("Expect alias name after 'as'.", false);
        defineVar(alias);
    }
    else if (hasImplicitAlias)
    {
        uint16_t alias = declareVariableFromToken(implicitAlias, false);
        defineVar(alias);
    }
    else
    {
        emitByte(OP_IMPORT_ALL);
    }

    match(TOKEN_SEMICOLON);
}

static void fromImportStatement()
{
    emitModuleSpecifier(NULL);
    emitByte(OP_IMPORT);

    consume(TOKEN_IMPORT, "Expect 'import' after module path in from-import statement.");

    if (match(TOKEN_STAR))
    {
        emitByte(OP_IMPORT_ALL);
        match(TOKEN_SEMICOLON);
        return;
    }

    do
    {
        consume(TOKEN_IDENTIFIER, "Expect symbol name to import.");
        Token importedName = parser.prev;
        Token aliasName = importedName;

        if (match(TOKEN_AS))
        {
            consume(TOKEN_IDENTIFIER, "Expect alias name after 'as'.");
            aliasName = parser.prev;
        }

        uint16_t importedNameConst = identifierConst(&importedName);
        uint16_t aliasConst = identifierConst(&aliasName);

        emitBytes(OP_IMPORT_NAME, importedNameConst);
        emitByte(aliasConst);
    } while (match(TOKEN_COMMA));

    emitByte(OP_POP);
    match(TOKEN_SEMICOLON);
}

ObjFunction *compile(const char *source, char *file, bool isRepl, bool printBytecode)
{
    initScanner(source);
    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT, file, isRepl, printBytecode);
    compiler.isRepl = isRepl;
    parser.panicMode = false;
    parser.hadError = false;
    advance();

    while (!match(TOKEN_EOF))
        declaration();

    consume(TOKEN_EOF, "Expect end of expr");
    ObjFunction *function = endCompiler();
    // optimizeChunk(currentChunk());
    return parser.hadError ? NULL : function;
}

void compileSetOptimizationLevel(int level)
{
    if (level < 0)
        level = 0;
    if (level > 2)
        level = 2;
    gCompilerOptimizationLevel = level;
}

int compileGetOptimizationLevel(void)
{
    return gCompilerOptimizationLevel;
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
// TODO WIP
// static void optimizeFunction(ObjFunction *fn)
// {
// if (fn->chunk.size == 0)
//     return;
// uint8_t *code = fn->chunk.code;
// int i = 0;
// while (i < fn->chunk.size)
// {
//     switch (code[i])
//     {
//     case OP_ADD:
//     {
//         // check the last two instructions
//         if (i >= 2 && code[i - 1] == OP_CONSTANT && code[i - 2] == OP_CONSTANT)
//         {
//             // get the two constants
//             uint16_t a = (code[i - 1] << 8) | code[i];
//             uint16_t b = (code[i - 2] << 8) | code[i - 1];
//             // get the values of the constants
//             Value aVal = fn->chunk.constants.values[a];
//             Value bVal = fn->chunk.constants.values[b];
//             // check if they are both numbers
//             if (IS_NUM(aVal) && IS_NUM(bVal))
//             {
//                 // get the numbers
//                 double aNum = AS_NUM(aVal);
//                 double bNum = AS_NUM(bVal);
//                 // create a new constant with the sum
//                 Value sum = NUM_VAL(aNum + bNum);
//                 // add the constant to the chunk
//                 uint16_t sumConst = makeConstant(sum);
//                 // replace the last two instructions with the new constant
//                 code[i - 2] = sumConst >> 8;
//                 code[i - 1] = sumConst & 0xff;
//                 // remove the two instructions
//                 for (int j = i; j < fn->chunk.size - 2; j++)
//                 {
//                     code[j] = code[j + 2];
//                 }
//                 fn->chunk.size -= 2;
//                 i -= 2;
//             }
//         }
//     }
//     }
//     i += 1;
// }
// }