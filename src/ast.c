#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "include/ast.h"

#define AST_ARENA_BLOCK_BYTES 4096
#define AST_STACK_MAX 1024

typedef struct _AstArenaBlock
{
    struct _AstArenaBlock *next;
    size_t used;
    size_t capacity;
    unsigned char data[];
} AstArenaBlock;

typedef struct _AstArena
{
    AstArenaBlock *head;
} AstArena;

typedef struct
{
    AstScope *items;
    int count;
    int capacity;
} ScopeVec;

typedef struct
{
    AstDecl *items;
    int count;
    int capacity;
} DeclVec;

typedef struct
{
    AstSymbol *items;
    int count;
    int capacity;
} SymbolVec;

typedef struct
{
    int line;
    int col;
    int scopeId;
} ScopeOpen;

typedef struct
{
    ScopeOpen *items;
    int count;
    int capacity;
} ScopeOpenVec;

struct _AstTree
{
    AstArena arena;
    AstNode *root;
    ScopeVec scopes;
    DeclVec decls;
    SymbolVec symbols;
};

static void *arenaAlloc(AstArena *arena, size_t bytes);

static void setErr(char *errBuf, size_t errBufSize, const char *fmt, ...)
{
    if (errBuf == NULL || errBufSize == 0)
        return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(errBuf, errBufSize, fmt, args);
    va_end(args);
}

static void arenaInit(AstArena *arena)
{
    arena->head = NULL;
}

static const char *arenaCopySpan(AstArena *arena, const char *start, int len)
{
    if (len <= 0)
    {
        char *empty = (char *)arenaAlloc(arena, 1);
        if (empty == NULL)
            return NULL;
        empty[0] = '\0';
        return empty;
    }

    char *copy = (char *)arenaAlloc(arena, (size_t)len + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, start, (size_t)len);
    copy[len] = '\0';
    return copy;
}

static void arenaFree(AstArena *arena)
{
    AstArenaBlock *block = arena->head;
    while (block != NULL)
    {
        AstArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}

static void *arenaAlloc(AstArena *arena, size_t bytes)
{
    size_t aligned = (bytes + (sizeof(void *) - 1)) & ~(sizeof(void *) - 1);

    AstArenaBlock *block = arena->head;
    if (block == NULL || block->used + aligned > block->capacity)
    {
        size_t capacity = AST_ARENA_BLOCK_BYTES;
        if (aligned > capacity)
            capacity = aligned;

        AstArenaBlock *newBlock = (AstArenaBlock *)malloc(sizeof(AstArenaBlock) + capacity);
        if (newBlock == NULL)
            return NULL;

        newBlock->next = block;
        newBlock->used = 0;
        newBlock->capacity = capacity;
        arena->head = newBlock;
        block = newBlock;
    }

    void *ptr = block->data + block->used;
    block->used += aligned;
    memset(ptr, 0, aligned);
    return ptr;
}

static AstNode *newNode(AstTree *tree, AstNodeKind kind, Token token)
{
    AstNode *node = (AstNode *)arenaAlloc(&tree->arena, sizeof(AstNode));
    if (node == NULL)
        return NULL;

    const char *copy = arenaCopySpan(&tree->arena, token.start, token.len);
    if (copy == NULL)
        return NULL;

    node->kind = kind;
    node->tokenType = token.type;
    node->scopeId = 0;
    node->lexeme = copy;
    node->lexemeLen = token.len;
    node->line = token.line;
    node->col = token.col;
    return node;
}

static bool ensureCapacity(void **items, int *capacity, int requiredCount, size_t itemSize)
{
    if (requiredCount <= *capacity)
        return true;

    int newCapacity = *capacity < 8 ? 8 : (*capacity * 2);
    while (newCapacity < requiredCount)
        newCapacity *= 2;

    void *resized = realloc(*items, (size_t)newCapacity * itemSize);
    if (resized == NULL)
        return false;

    *items = resized;
    *capacity = newCapacity;
    return true;
}

static bool addScope(AstTree *tree, AstScope scope, int *outId)
{
    int nextCount = tree->scopes.count + 1;
    if (!ensureCapacity((void **)&tree->scopes.items, &tree->scopes.capacity, nextCount, sizeof(AstScope)))
        return false;

    tree->scopes.items[tree->scopes.count] = scope;
    if (outId != NULL)
        *outId = scope.id;
    tree->scopes.count = nextCount;
    return true;
}

static bool addDecl(AstTree *tree, AstDecl decl)
{
    int nextCount = tree->decls.count + 1;
    if (!ensureCapacity((void **)&tree->decls.items, &tree->decls.capacity, nextCount, sizeof(AstDecl)))
        return false;
    tree->decls.items[tree->decls.count] = decl;
    tree->decls.count = nextCount;
    return true;
}

static bool addSymbol(AstTree *tree, AstSymbol symbol)
{
    int nextCount = tree->symbols.count + 1;
    if (!ensureCapacity((void **)&tree->symbols.items, &tree->symbols.capacity, nextCount, sizeof(AstSymbol)))
        return false;
    tree->symbols.items[tree->symbols.count] = symbol;
    tree->symbols.count = nextCount;
    return true;
}

static bool addScopeOpen(ScopeOpenVec *scopeOpens, ScopeOpen scopeOpen)
{
    int nextCount = scopeOpens->count + 1;
    if (!ensureCapacity((void **)&scopeOpens->items, &scopeOpens->capacity, nextCount, sizeof(ScopeOpen)))
        return false;
    scopeOpens->items[scopeOpens->count] = scopeOpen;
    scopeOpens->count = nextCount;
    return true;
}

static int findScopeForBrace(const ScopeOpenVec *scopeOpens, int line, int col, int fallbackScope)
{
    for (int i = 0; i < scopeOpens->count; i++)
    {
        if (scopeOpens->items[i].line == line && scopeOpens->items[i].col == col)
            return scopeOpens->items[i].scopeId;
    }
    return fallbackScope;
}

static void annotateNodeScopesRecursive(AstNode *node, int currentScopeId, const ScopeOpenVec *scopeOpens)
{
    if (node == NULL)
        return;

    node->scopeId = currentScopeId;

    int childScopeId = currentScopeId;
    if (node->kind == AST_NODE_GROUP && node->tokenType == TOKEN_LEFT_BRACE)
        childScopeId = findScopeForBrace(scopeOpens, node->line, node->col, currentScopeId);

    for (AstNode *child = node->firstChild; child != NULL; child = child->nextSibling)
        annotateNodeScopesRecursive(child, childScopeId, scopeOpens);
}

static bool tokenCanNameDecl(TokenType type)
{
    return type == TOKEN_IDENTIFIER ||
           type == TOKEN_BASIC_STRING ||
           type == TOKEN_TEMPLATE_STRING ||
           type == TOKEN_RAW_STRING;
}

static AstDeclKind tokenToDeclKind(TokenType type)
{
    switch (type)
    {
    case TOKEN_VAR:
        return AST_DECL_VAR;
    case TOKEN_CONST:
        return AST_DECL_CONST;
    case TOKEN_FUN:
        return AST_DECL_FUNCTION;
    case TOKEN_CLASS:
        return AST_DECL_CLASS;
    case TOKEN_MODULE:
        return AST_DECL_MODULE;
    case TOKEN_IMPORT:
        return AST_DECL_IMPORT;
    case TOKEN_FROM:
        return AST_DECL_FROM_IMPORT;
    case TOKEN_EXPORT:
        return AST_DECL_EXPORT;
    default:
        return AST_DECL_VAR;
    }
}

static bool maybeAddDecl(AstTree *tree, AstDeclKind kind, Token token, int scopeId)
{
    const char *name = arenaCopySpan(&tree->arena, token.start, token.len);
    if (name == NULL)
        return false;

    AstDecl decl = {
        .kind = kind,
        .name = name,
        .nameLen = token.len,
        .line = token.line,
        .col = token.col,
        .scopeId = scopeId,
    };
    return addDecl(tree, decl);
}

static bool maybeAddSymbol(AstTree *tree, AstSymbolKind kind, Token token, int scopeId, bool isMutable)
{
    const char *name = arenaCopySpan(&tree->arena, token.start, token.len);
    if (name == NULL)
        return false;

    AstSymbol symbol = {
        .kind = kind,
        .name = name,
        .nameLen = token.len,
        .line = token.line,
        .col = token.col,
        .scopeId = scopeId,
        .isMutable = isMutable,
    };
    return addSymbol(tree, symbol);
}

static bool buildSemanticModel(AstTree *tree, const char *source, char *errBuf, size_t errBufSize)
{
    int scopeStack[AST_STACK_MAX] = {0};
    int scopeDepth = 0;
    int nextScopeId = 0;
    ScopeOpenVec scopeOpens = {0};

    AstScope global = {
        .id = nextScopeId++,
        .parentId = -1,
        .depth = 0,
        .startLine = 1,
        .startCol = 1,
        .endLine = 1,
        .endCol = 1,
    };
    if (!addScope(tree, global, NULL))
    {
        setErr(errBuf, errBufSize, "out of memory while creating global scope");
        free(scopeOpens.items);
        return false;
    }
    scopeStack[scopeDepth++] = global.id;

    bool expectDeclName = false;
    AstDeclKind expectedDeclKind = AST_DECL_VAR;
    bool awaitFunctionParams = false;
    bool inFunctionParams = false;
    int parenDepth = 0;
    int functionParamDepth = -1;

    int lastLine = 1;
    int lastCol = 1;

    initScanner(source);
    for (;;)
    {
        Token token = scanToken();
        if (token.type == TOKEN_ERROR)
        {
            setErr(errBuf, errBufSize, "%d:%d %.*s", token.line, token.col, token.len, token.start);
            return false;
        }
        if (token.type == TOKEN_EOF)
            break;

        lastLine = token.line;
        lastCol = token.col;

        if (token.type == TOKEN_LEFT_PAREN)
        {
            parenDepth++;
            if (awaitFunctionParams)
            {
                inFunctionParams = true;
                functionParamDepth = parenDepth;
                awaitFunctionParams = false;

                if (expectDeclName && expectedDeclKind == AST_DECL_FUNCTION)
                    expectDeclName = false;
            }
        }

        if (token.type == TOKEN_LEFT_BRACE)
        {
            if (scopeDepth >= AST_STACK_MAX)
            {
                setErr(errBuf, errBufSize, "%d:%d semantic scope depth exceeds limit (%d)", token.line, token.col, AST_STACK_MAX);
                free(scopeOpens.items);
                return false;
            }

            int parentId = scopeStack[scopeDepth - 1];
            AstScope *parent = &tree->scopes.items[parentId];
            AstScope scope = {
                .id = nextScopeId++,
                .parentId = parentId,
                .depth = parent->depth + 1,
                .startLine = token.line,
                .startCol = token.col,
                .endLine = token.line,
                .endCol = token.col,
            };
            if (!addScope(tree, scope, NULL))
            {
                setErr(errBuf, errBufSize, "out of memory while creating nested scope");
                free(scopeOpens.items);
                return false;
            }
            if (!addScopeOpen(&scopeOpens, (ScopeOpen){.line = token.line, .col = token.col, .scopeId = scope.id}))
            {
                setErr(errBuf, errBufSize, "out of memory while recording scope mapping");
                free(scopeOpens.items);
                return false;
            }
            scopeStack[scopeDepth++] = scope.id;
        }

        if (expectDeclName && tokenCanNameDecl(token.type))
        {
            int currentScopeId = scopeStack[scopeDepth - 1];
            if (!maybeAddDecl(tree, expectedDeclKind, token, currentScopeId))
            {
                setErr(errBuf, errBufSize, "out of memory while adding declaration");
                free(scopeOpens.items);
                return false;
            }

            switch (expectedDeclKind)
            {
            case AST_DECL_VAR:
                if (!maybeAddSymbol(tree, AST_SYMBOL_VAR, token, currentScopeId, true))
                {
                    setErr(errBuf, errBufSize, "out of memory while adding variable symbol");
                    free(scopeOpens.items);
                    return false;
                }
                break;
            case AST_DECL_CONST:
                if (!maybeAddSymbol(tree, AST_SYMBOL_CONST, token, currentScopeId, false))
                {
                    setErr(errBuf, errBufSize, "out of memory while adding const symbol");
                    free(scopeOpens.items);
                    return false;
                }
                break;
            case AST_DECL_FUNCTION:
                if (!maybeAddSymbol(tree, AST_SYMBOL_FUNCTION, token, currentScopeId, false))
                {
                    setErr(errBuf, errBufSize, "out of memory while adding function symbol");
                    free(scopeOpens.items);
                    return false;
                }
                awaitFunctionParams = true;
                break;
            case AST_DECL_CLASS:
                if (!maybeAddSymbol(tree, AST_SYMBOL_CLASS, token, currentScopeId, false))
                {
                    setErr(errBuf, errBufSize, "out of memory while adding class symbol");
                    free(scopeOpens.items);
                    return false;
                }
                break;
            case AST_DECL_MODULE:
                if (!maybeAddSymbol(tree, AST_SYMBOL_MODULE, token, currentScopeId, false))
                {
                    setErr(errBuf, errBufSize, "out of memory while adding module symbol");
                    free(scopeOpens.items);
                    return false;
                }
                break;
            default:
                break;
            }

            expectDeclName = false;
        }
        else if (inFunctionParams && token.type == TOKEN_IDENTIFIER)
        {
            int currentScopeId = scopeStack[scopeDepth - 1];
            if (!maybeAddSymbol(tree, AST_SYMBOL_PARAM, token, currentScopeId, true))
            {
                setErr(errBuf, errBufSize, "out of memory while adding function parameter symbol");
                free(scopeOpens.items);
                return false;
            }
        }

        if (token.type == TOKEN_VAR || token.type == TOKEN_CONST || token.type == TOKEN_FUN ||
            token.type == TOKEN_CLASS || token.type == TOKEN_MODULE || token.type == TOKEN_IMPORT ||
            token.type == TOKEN_FROM || token.type == TOKEN_EXPORT)
        {
            expectDeclName = true;
            expectedDeclKind = tokenToDeclKind(token.type);

            if (token.type == TOKEN_FUN)
                awaitFunctionParams = true;
            continue;
        }

        if (token.type == TOKEN_SEMICOLON || token.type == TOKEN_EQUAL || token.type == TOKEN_LEFT_BRACE)
            expectDeclName = false;

        if (token.type == TOKEN_RIGHT_PAREN)
        {
            if (inFunctionParams && parenDepth == functionParamDepth)
            {
                inFunctionParams = false;
                functionParamDepth = -1;
            }
            if (parenDepth > 0)
                parenDepth--;
        }

        if (token.type == TOKEN_RIGHT_BRACE)
        {
            if (scopeDepth > 1)
            {
                int closedId = scopeStack[scopeDepth - 1];
                AstScope *closed = &tree->scopes.items[closedId];
                closed->endLine = token.line;
                closed->endCol = token.col;
                scopeDepth--;
            }
        }
    }

    for (int i = 0; i < tree->scopes.count; i++)
    {
        if (tree->scopes.items[i].endLine <= tree->scopes.items[i].startLine && tree->scopes.items[i].endCol <= tree->scopes.items[i].startCol)
        {
            tree->scopes.items[i].endLine = lastLine;
            tree->scopes.items[i].endCol = lastCol;
        }
    }

    annotateNodeScopesRecursive(tree->root, global.id, &scopeOpens);
    free(scopeOpens.items);

    return true;
}

static void appendChild(AstNode *parent, AstNode *child)
{
    if (parent->firstChild == NULL)
    {
        parent->firstChild = child;
        parent->lastChild = child;
        return;
    }

    parent->lastChild->nextSibling = child;
    parent->lastChild = child;
}

static bool isOpenGroup(TokenType type)
{
    return type == TOKEN_LEFT_PAREN || type == TOKEN_LEFT_BRACKET || type == TOKEN_LEFT_BRACE || type == TOKEN_DOT_BRACKET;
}

static bool isCloseGroup(TokenType type)
{
    return type == TOKEN_RIGHT_PAREN || type == TOKEN_RIGHT_BRACKET || type == TOKEN_RIGHT_BRACE;
}

static bool groupsMatch(TokenType open, TokenType close)
{
    return (open == TOKEN_LEFT_PAREN && close == TOKEN_RIGHT_PAREN) ||
           (open == TOKEN_LEFT_BRACKET && close == TOKEN_RIGHT_BRACKET) ||
           (open == TOKEN_LEFT_BRACE && close == TOKEN_RIGHT_BRACE) ||
           (open == TOKEN_DOT_BRACKET && close == TOKEN_RIGHT_BRACE);
}

AstTree *astBuildFromSource(const char *source, const char *file, char *errBuf, size_t errBufSize)
{
    (void)file;

    if (errBuf != NULL && errBufSize > 0)
        errBuf[0] = '\0';

    AstTree *tree = (AstTree *)malloc(sizeof(AstTree));
    if (tree == NULL)
    {
        setErr(errBuf, errBufSize, "failed to allocate AstTree");
        return NULL;
    }
    memset(tree, 0, sizeof(AstTree));

    arenaInit(&tree->arena);

    Token synthetic = {
        .type = TOKEN_EOF,
        .start = "",
        .len = 0,
        .line = 1,
        .col = 1,
    };

    tree->root = newNode(tree, AST_NODE_ROOT, synthetic);
    if (tree->root == NULL)
    {
        setErr(errBuf, errBufSize, "failed to allocate AST root");
        astFreeTree(tree);
        return NULL;
    }

    AstNode *stack[AST_STACK_MAX];
    int depth = 0;
    stack[depth++] = tree->root;

    initScanner(source);

    for (;;)
    {
        Token token = scanToken();
        if (token.type == TOKEN_ERROR)
        {
            setErr(errBuf, errBufSize, "%d:%d %.*s", token.line, token.col, token.len, token.start);
            astFreeTree(tree);
            return NULL;
        }

        if (token.type == TOKEN_EOF)
            break;

        AstNode *parent = stack[depth - 1];

        if (isOpenGroup(token.type))
        {
            AstNode *group = newNode(tree, AST_NODE_GROUP, token);
            if (group == NULL)
            {
                setErr(errBuf, errBufSize, "out of memory while creating group node");
                astFreeTree(tree);
                return NULL;
            }

            appendChild(parent, group);
            if (depth >= AST_STACK_MAX)
            {
                setErr(errBuf, errBufSize, "AST nesting exceeds limit (%d)", AST_STACK_MAX);
                astFreeTree(tree);
                return NULL;
            }

            stack[depth++] = group;
            continue;
        }

        if (isCloseGroup(token.type))
        {
            if (depth <= 1)
            {
                setErr(errBuf, errBufSize, "%d:%d unexpected closing token %s", token.line, token.col, TOKEN_NAMES[token.type]);
                astFreeTree(tree);
                return NULL;
            }

            AstNode *open = stack[depth - 1];
            if (!groupsMatch(open->tokenType, token.type))
            {
                setErr(errBuf, errBufSize, "%d:%d mismatched closer %s for opener %s", token.line, token.col, TOKEN_NAMES[token.type], TOKEN_NAMES[open->tokenType]);
                astFreeTree(tree);
                return NULL;
            }

            AstNode *closeNode = newNode(tree, AST_NODE_TOKEN, token);
            if (closeNode == NULL)
            {
                setErr(errBuf, errBufSize, "out of memory while creating close token node");
                astFreeTree(tree);
                return NULL;
            }
            appendChild(open, closeNode);
            depth--;
            continue;
        }

        AstNode *leaf = newNode(tree, AST_NODE_TOKEN, token);
        if (leaf == NULL)
        {
            setErr(errBuf, errBufSize, "out of memory while creating token node");
            astFreeTree(tree);
            return NULL;
        }

        appendChild(parent, leaf);
    }

    if (depth != 1)
    {
        AstNode *open = stack[depth - 1];
        setErr(errBuf, errBufSize, "%d:%d unclosed group %s", open->line, open->col, TOKEN_NAMES[open->tokenType]);
        astFreeTree(tree);
        return NULL;
    }

    if (!buildSemanticModel(tree, source, errBuf, errBufSize))
    {
        astFreeTree(tree);
        return NULL;
    }

    return tree;
}

static const char *declKindName(AstDeclKind kind)
{
    switch (kind)
    {
    case AST_DECL_VAR:
        return "VAR";
    case AST_DECL_CONST:
        return "CONST";
    case AST_DECL_FUNCTION:
        return "FUNCTION";
    case AST_DECL_CLASS:
        return "CLASS";
    case AST_DECL_MODULE:
        return "MODULE";
    case AST_DECL_IMPORT:
        return "IMPORT";
    case AST_DECL_FROM_IMPORT:
        return "FROM_IMPORT";
    case AST_DECL_EXPORT:
        return "EXPORT";
    default:
        return "UNKNOWN";
    }
}

static const char *symbolKindName(AstSymbolKind kind)
{
    switch (kind)
    {
    case AST_SYMBOL_VAR:
        return "VAR";
    case AST_SYMBOL_CONST:
        return "CONST";
    case AST_SYMBOL_FUNCTION:
        return "FUNCTION";
    case AST_SYMBOL_CLASS:
        return "CLASS";
    case AST_SYMBOL_MODULE:
        return "MODULE";
    case AST_SYMBOL_PARAM:
        return "PARAM";
    default:
        return "UNKNOWN";
    }
}

static const char *kindName(AstNodeKind kind)
{
    switch (kind)
    {
    case AST_NODE_ROOT:
        return "ROOT";
    case AST_NODE_GROUP:
        return "GROUP";
    case AST_NODE_TOKEN:
        return "TOKEN";
    default:
        return "UNKNOWN";
    }
}

static void dumpNode(const AstNode *node, FILE *out, int depth)
{
    for (int i = 0; i < depth; i++)
        fputs("  ", out);

    fprintf(out, "%s [scope=%d]", kindName(node->kind), node->scopeId);
    if (node->kind != AST_NODE_ROOT)
    {
        fprintf(out, " %s @ %d:%d", TOKEN_NAMES[node->tokenType], node->line, node->col);
        if (node->lexeme != NULL && node->lexemeLen > 0)
        {
            int toPrint = node->lexemeLen > 40 ? 40 : node->lexemeLen;
            fprintf(out, "  '%.*s'", toPrint, node->lexeme);
            if (toPrint < node->lexemeLen)
                fputs("...", out);
        }
    }
    fputc('\n', out);

    for (const AstNode *child = node->firstChild; child != NULL; child = child->nextSibling)
        dumpNode(child, out, depth + 1);
}

void astDumpTree(const AstTree *tree, FILE *out)
{
    if (tree == NULL || tree->root == NULL || out == NULL)
        return;

    dumpNode(tree->root, out, 0);
}

void astDumpSemanticSummary(const AstTree *tree, FILE *out)
{
    if (tree == NULL || out == NULL)
        return;

    fprintf(out, "Scopes (%d):\n", tree->scopes.count);
    for (int i = 0; i < tree->scopes.count; i++)
    {
        const AstScope *scope = &tree->scopes.items[i];
        fprintf(out,
                "  #%d parent=%d depth=%d span=%d:%d..%d:%d\n",
                scope->id,
                scope->parentId,
                scope->depth,
                scope->startLine,
                scope->startCol,
                scope->endLine,
                scope->endCol);
    }

    fprintf(out, "Declarations (%d):\n", tree->decls.count);
    for (int i = 0; i < tree->decls.count; i++)
    {
        const AstDecl *decl = &tree->decls.items[i];
        fprintf(out,
                "  %-11s %.*s @ %d:%d scope=%d\n",
                declKindName(decl->kind),
                decl->nameLen,
                decl->name,
                decl->line,
                decl->col,
                decl->scopeId);
    }

    fprintf(out, "Symbols (%d):\n", tree->symbols.count);
    for (int i = 0; i < tree->symbols.count; i++)
    {
        const AstSymbol *symbol = &tree->symbols.items[i];
        fprintf(out,
                "  %-8s %.*s @ %d:%d scope=%d mutable=%s\n",
                symbolKindName(symbol->kind),
                symbol->nameLen,
                symbol->name,
                symbol->line,
                symbol->col,
                symbol->scopeId,
                symbol->isMutable ? "true" : "false");
    }
}

static void jsonWriteEscaped(FILE *out, const char *s, int len)
{
    if (s == NULL)
        return;

    for (int i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (c < 0x20)
                fprintf(out, "\\u%04x", (unsigned int)c);
            else
                fputc((int)c, out);
            break;
        }
    }
}

static const char *nodeKindJsonName(AstNodeKind kind)
{
    switch (kind)
    {
    case AST_NODE_ROOT:
        return "root";
    case AST_NODE_GROUP:
        return "group";
    case AST_NODE_TOKEN:
        return "token";
    default:
        return "unknown";
    }
}

static const char *declKindJsonName(AstDeclKind kind)
{
    switch (kind)
    {
    case AST_DECL_VAR:
        return "var";
    case AST_DECL_CONST:
        return "const";
    case AST_DECL_FUNCTION:
        return "function";
    case AST_DECL_CLASS:
        return "class";
    case AST_DECL_MODULE:
        return "module";
    case AST_DECL_IMPORT:
        return "import";
    case AST_DECL_FROM_IMPORT:
        return "from_import";
    case AST_DECL_EXPORT:
        return "export";
    default:
        return "unknown";
    }
}

static const char *symbolKindJsonName(AstSymbolKind kind)
{
    switch (kind)
    {
    case AST_SYMBOL_VAR:
        return "var";
    case AST_SYMBOL_CONST:
        return "const";
    case AST_SYMBOL_FUNCTION:
        return "function";
    case AST_SYMBOL_CLASS:
        return "class";
    case AST_SYMBOL_MODULE:
        return "module";
    case AST_SYMBOL_PARAM:
        return "param";
    default:
        return "unknown";
    }
}

static void dumpNodeJson(const AstNode *node, FILE *out)
{
    fputs("{\"kind\":\"", out);
    fputs(nodeKindJsonName(node->kind), out);
    fputs("\",\"token\":\"", out);
    fputs(TOKEN_NAMES[node->tokenType], out);
    fputs("\",\"scope\":", out);
    fprintf(out, "%d", node->scopeId);
    fputs(",\"line\":", out);
    fprintf(out, "%d", node->line);
    fputs(",\"col\":", out);
    fprintf(out, "%d", node->col);
    fputs(",\"lexeme\":\"", out);
    jsonWriteEscaped(out, node->lexeme, node->lexemeLen);
    fputs("\",\"children\":[", out);

    bool first = true;
    for (const AstNode *child = node->firstChild; child != NULL; child = child->nextSibling)
    {
        if (!first)
            fputc(',', out);
        dumpNodeJson(child, out);
        first = false;
    }
    fputs("]}", out);
}

void astDumpSemanticJson(const AstTree *tree, FILE *out)
{
    if (tree == NULL || out == NULL)
        return;

    fputs("{\"version\":1,\"scopes\":[", out);
    for (int i = 0; i < tree->scopes.count; i++)
    {
        if (i > 0)
            fputc(',', out);
        const AstScope *scope = &tree->scopes.items[i];
        fprintf(out,
                "{\"id\":%d,\"parent\":%d,\"depth\":%d,\"start\":{\"line\":%d,\"col\":%d},\"end\":{\"line\":%d,\"col\":%d}}",
                scope->id,
                scope->parentId,
                scope->depth,
                scope->startLine,
                scope->startCol,
                scope->endLine,
                scope->endCol);
    }

    fputs("],\"declarations\":[", out);
    for (int i = 0; i < tree->decls.count; i++)
    {
        if (i > 0)
            fputc(',', out);
        const AstDecl *decl = &tree->decls.items[i];
        fputs("{\"kind\":\"", out);
        fputs(declKindJsonName(decl->kind), out);
        fputs("\",\"name\":\"", out);
        jsonWriteEscaped(out, decl->name, decl->nameLen);
        fprintf(out, "\",\"line\":%d,\"col\":%d,\"scope\":%d}", decl->line, decl->col, decl->scopeId);
    }

    fputs("],\"symbols\":[", out);
    for (int i = 0; i < tree->symbols.count; i++)
    {
        if (i > 0)
            fputc(',', out);
        const AstSymbol *symbol = &tree->symbols.items[i];
        fputs("{\"kind\":\"", out);
        fputs(symbolKindJsonName(symbol->kind), out);
        fputs("\",\"name\":\"", out);
        jsonWriteEscaped(out, symbol->name, symbol->nameLen);
        fprintf(out,
                "\",\"line\":%d,\"col\":%d,\"scope\":%d,\"mutable\":%s}",
                symbol->line,
                symbol->col,
                symbol->scopeId,
                symbol->isMutable ? "true" : "false");
    }

    fputs("],\"tree\":", out);
    dumpNodeJson(tree->root, out);
    fputs("}\n", out);
}

typedef enum
{
    TYPE_UNKNOWN = 0,
    TYPE_ANY,
    TYPE_NUMBER,
    TYPE_STRING,
    TYPE_BOOL,
    TYPE_NIL,
    TYPE_LIST,
    TYPE_MAP,
    TYPE_FUNCTION,
    TYPE_CLASS_INSTANCE,
    TYPE_CLASS_OBJECT,
    TYPE_UNION,
} AstTypeKind;

#define TYPE_UNION_MAX 8

typedef struct
{
    AstTypeKind kind;
    const char *className;
    int classNameLen;
    AstTypeKind listElemKind;
    const char *listElemClassName;
    int listElemClassNameLen;
    AstTypeKind mapKeyKind;
    const char *mapKeyClassName;
    int mapKeyClassNameLen;
    AstTypeKind mapValueKind;
    const char *mapValueClassName;
    int mapValueClassNameLen;
    AstTypeKind unionKinds[TYPE_UNION_MAX];
    const char *unionClassNames[TYPE_UNION_MAX];
    int unionClassNameLens[TYPE_UNION_MAX];
    AstTypeKind unionListElemKinds[TYPE_UNION_MAX];
    const char *unionListElemClassNames[TYPE_UNION_MAX];
    int unionListElemClassNameLens[TYPE_UNION_MAX];
    AstTypeKind unionMapKeyKinds[TYPE_UNION_MAX];
    const char *unionMapKeyClassNames[TYPE_UNION_MAX];
    int unionMapKeyClassNameLens[TYPE_UNION_MAX];
    AstTypeKind unionMapValueKinds[TYPE_UNION_MAX];
    const char *unionMapValueClassNames[TYPE_UNION_MAX];
    int unionMapValueClassNameLens[TYPE_UNION_MAX];
    int unionCount;
} TypeInfo;

typedef struct
{
    const char *name;
    int nameLen;
    int scopeId;
    bool isConst;
    TypeInfo type;
} TypeBinding;

typedef struct
{
    const char *name;
    int nameLen;
    const char *baseName;
    int baseNameLen;
    TypeInfo baseType;
    const char *typeParamNames[8];
    int typeParamNameLens[8];
    int typeParamCount;
    int scopeId;
} ClassInfo;

typedef struct
{
    ClassInfo *items;
    int count;
    int capacity;
} ClassVec;

typedef struct
{
    const char *name;
    int nameLen;
    TypeInfo hintedType;
} TypeHint;

typedef struct
{
    TypeHint *items;
    int count;
    int capacity;
} TypeHintVec;

typedef struct
{
    const char *fnName;
    int fnNameLen;
    const char *paramName;
    int paramNameLen;
    TypeInfo hintedType;
} ParamHint;

typedef struct
{
    ParamHint *items;
    int count;
    int capacity;
} ParamHintVec;

typedef struct
{
    const char *fnName;
    int fnNameLen;
    TypeInfo hintedType;
} ReturnHint;

typedef struct
{
    ReturnHint *items;
    int count;
    int capacity;
} ReturnHintVec;

typedef struct
{
    const char *fnName;
    int fnNameLen;
    const char *ownerClassName;
    int ownerClassNameLen;
    const char *classTypeParamNames[8];
    int classTypeParamNameLens[8];
    int classTypeParamCount;
    int bodyScopeId;
    const AstNode *params[32];
    TypeInfo paramTypes[32];
    bool paramVariadic[32];
    int paramCount;
    TypeInfo returnType;
} FunctionCtx;

typedef struct
{
    FunctionCtx *items;
    int count;
    int capacity;
} FunctionCtxVec;

typedef struct
{
    TypeBinding *items;
    int count;
    int capacity;
} BindingVec;

typedef struct
{
    const AstNode **items;
    int count;
    int capacity;
} NodeRefVec;

static const char *typeName(AstTypeKind kind)
{
    switch (kind)
    {
    case TYPE_ANY:
        return "any";
    case TYPE_NUMBER:
        return "number";
    case TYPE_STRING:
        return "string";
    case TYPE_BOOL:
        return "bool";
    case TYPE_NIL:
        return "null";
    case TYPE_LIST:
        return "list";
    case TYPE_MAP:
        return "map";
    case TYPE_FUNCTION:
        return "function";
    case TYPE_CLASS_INSTANCE:
        return "class";
    case TYPE_CLASS_OBJECT:
        return "class_object";
    case TYPE_UNION:
        return "union";
    case TYPE_UNKNOWN:
    default:
        return "unknown";
    }
}

static TypeInfo parseTypeName(const char *name, int nameLen);

static int findTopLevelChar(const char *s, int len, char target)
{
    int depth = 0;
    for (int i = 0; i < len; i++)
    {
        char ch = s[i];
        if (ch == target && depth == 0)
            return i;

        if (ch == '<' || ch == '[')
            depth++;
        else if (ch == '>' || ch == ']')
        {
            if (depth > 0)
                depth--;
        }
    }
    return -1;
}

static void classTypeBaseName(const char *name, int nameLen, const char **outBase, int *outBaseLen)
{
    if (name == NULL || nameLen <= 0)
    {
        *outBase = name;
        *outBaseLen = nameLen;
        return;
    }

    int genericStart = findTopLevelChar(name, nameLen, '<');
    char close = '>';
    if (!(genericStart > 0 && name[nameLen - 1] == '>'))
    {
        genericStart = findTopLevelChar(name, nameLen, '[');
        close = ']';
    }

    if (genericStart > 0 && name[nameLen - 1] == close)
    {
        *outBase = name;
        *outBaseLen = genericStart;
        return;
    }

    *outBase = name;
    *outBaseLen = nameLen;
}

static bool parseClassTypeArgs(const char *name, int nameLen, TypeInfo *outArgs, int maxArgs, int *outCount)
{
    if (outCount != NULL)
        *outCount = 0;
    if (name == NULL || nameLen <= 0 || outArgs == NULL || maxArgs <= 0)
        return false;

    int genericStart = findTopLevelChar(name, nameLen, '<');
    char close = '>';
    if (!(genericStart > 0 && name[nameLen - 1] == '>'))
    {
        genericStart = findTopLevelChar(name, nameLen, '[');
        close = ']';
    }
    if (genericStart <= 0 || name[nameLen - 1] != close)
        return false;

    int start = genericStart + 1;
    int end = nameLen - 1;
    int depth = 0;
    int segStart = start;
    int count = 0;
    for (int i = start; i <= end; i++)
    {
        char ch = (i < end) ? name[i] : ',';
        if (ch == '<' || ch == '[')
            depth++;
        else if (ch == '>' || ch == ']')
            depth--;

        if (ch == ',' && depth == 0)
        {
            int segEnd = i;
            while (segStart < segEnd && (name[segStart] == ' ' || name[segStart] == '\t'))
                segStart++;
            while (segEnd > segStart && (name[segEnd - 1] == ' ' || name[segEnd - 1] == '\t'))
                segEnd--;

            if (segEnd > segStart)
            {
                if (count >= maxArgs)
                    return false;
                outArgs[count++] = parseTypeName(name + segStart, segEnd - segStart);
            }
            segStart = i + 1;
        }
    }

    if (outCount != NULL)
        *outCount = count;
    return count > 0;
}

static int findTypeParamIndex(const FunctionCtx *ctx, const char *name, int nameLen)
{
    if (ctx == NULL || name == NULL || nameLen <= 0)
        return -1;
    for (int i = 0; i < ctx->classTypeParamCount; i++)
    {
        if (ctx->classTypeParamNameLens[i] != nameLen)
            continue;
        if (memcmp(ctx->classTypeParamNames[i], name, (size_t)nameLen) == 0)
            return i;
    }
    return -1;
}

static void typeInfoToTypeExpr(TypeInfo t, char *out, size_t outSize)
{
    if (out == NULL || outSize == 0)
        return;

    switch (t.kind)
    {
    case TYPE_ANY:
        snprintf(out, outSize, "any");
        return;
    case TYPE_NUMBER:
        snprintf(out, outSize, "number");
        return;
    case TYPE_STRING:
        snprintf(out, outSize, "str");
        return;
    case TYPE_BOOL:
        snprintf(out, outSize, "bool");
        return;
    case TYPE_NIL:
        snprintf(out, outSize, "null");
        return;
    case TYPE_FUNCTION:
        snprintf(out, outSize, "function");
        return;
    case TYPE_CLASS_INSTANCE:
        if (t.className != NULL && t.classNameLen > 0)
            snprintf(out, outSize, "%.*s", t.classNameLen, t.className);
        else
            snprintf(out, outSize, "?");
        return;
    case TYPE_LIST:
        if (t.listElemKind == TYPE_UNKNOWN)
        {
            snprintf(out, outSize, "list");
            return;
        }
        else
        {
            TypeInfo elem = {.kind = t.listElemKind, .className = t.listElemClassName, .classNameLen = t.listElemClassNameLen};
            char elemBuf[128] = {0};
            typeInfoToTypeExpr(elem, elemBuf, sizeof(elemBuf));
            snprintf(out, outSize, "list[%s]", elemBuf);
            return;
        }
    case TYPE_MAP:
        if (t.mapKeyKind == TYPE_UNKNOWN || t.mapValueKind == TYPE_UNKNOWN)
        {
            snprintf(out, outSize, "map");
            return;
        }
        else
        {
            TypeInfo key = {.kind = t.mapKeyKind, .className = t.mapKeyClassName, .classNameLen = t.mapKeyClassNameLen};
            TypeInfo val = {.kind = t.mapValueKind, .className = t.mapValueClassName, .classNameLen = t.mapValueClassNameLen};
            char keyBuf[128] = {0};
            char valBuf[128] = {0};
            typeInfoToTypeExpr(key, keyBuf, sizeof(keyBuf));
            typeInfoToTypeExpr(val, valBuf, sizeof(valBuf));
            snprintf(out, outSize, "map[%s,%s]", keyBuf, valBuf);
            return;
        }
    case TYPE_UNION:
    {
        size_t at = 0;
        out[0] = '\0';
        for (int i = 0; i < t.unionCount; i++)
        {
            TypeInfo member = {
                .kind = t.unionKinds[i],
                .className = t.unionClassNames[i],
                .classNameLen = t.unionClassNameLens[i],
                .listElemKind = t.unionListElemKinds[i],
                .listElemClassName = t.unionListElemClassNames[i],
                .listElemClassNameLen = t.unionListElemClassNameLens[i],
                .mapKeyKind = t.unionMapKeyKinds[i],
                .mapKeyClassName = t.unionMapKeyClassNames[i],
                .mapKeyClassNameLen = t.unionMapKeyClassNameLens[i],
                .mapValueKind = t.unionMapValueKinds[i],
                .mapValueClassName = t.unionMapValueClassNames[i],
                .mapValueClassNameLen = t.unionMapValueClassNameLens[i],
            };
            char part[128] = {0};
            typeInfoToTypeExpr(member, part, sizeof(part));
            int wrote = snprintf(out + at, outSize > at ? outSize - at : 0, "%s%s", i == 0 ? "" : "|", part);
            if (wrote <= 0)
                break;
            at += (size_t)wrote;
            if (at >= outSize)
                break;
        }
        return;
    }
    case TYPE_UNKNOWN:
    default:
        snprintf(out, outSize, "?");
        return;
    }
}

static TypeInfo substituteClassTypeParams(TypeInfo type, const FunctionCtx *ctx, const TypeInfo *receiverTypeArgs, int receiverTypeArgCount)
{
    if (ctx == NULL)
        return type;

    if (type.kind == TYPE_CLASS_INSTANCE)
    {
        int idx = findTypeParamIndex(ctx, type.className, type.classNameLen);
        if (idx >= 0 && idx < receiverTypeArgCount)
            return receiverTypeArgs[idx];

        TypeInfo typeArgs[8] = {0};
        int typeArgCount = 0;
        if (parseClassTypeArgs(type.className, type.classNameLen, typeArgs, 8, &typeArgCount) && typeArgCount > 0)
        {
            TypeInfo subbedArgs[8] = {0};
            for (int i = 0; i < typeArgCount; i++)
                subbedArgs[i] = substituteClassTypeParams(typeArgs[i], ctx, receiverTypeArgs, receiverTypeArgCount);

            const char *base = NULL;
            int baseLen = 0;
            classTypeBaseName(type.className, type.classNameLen, &base, &baseLen);

            size_t total = (size_t)baseLen + 2;
            char pieces[8][128];
            for (int i = 0; i < typeArgCount; i++)
            {
                memset(pieces[i], 0, sizeof(pieces[i]));
                typeInfoToTypeExpr(subbedArgs[i], pieces[i], sizeof(pieces[i]));
                total += strlen(pieces[i]);
                if (i + 1 < typeArgCount)
                    total += 1;
            }

            char *rebuilt = (char *)malloc(total + 1);
            if (rebuilt != NULL)
            {
                size_t at = 0;
                memcpy(rebuilt + at, base, (size_t)baseLen);
                at += (size_t)baseLen;
                rebuilt[at++] = '[';
                for (int i = 0; i < typeArgCount; i++)
                {
                    size_t n = strlen(pieces[i]);
                    memcpy(rebuilt + at, pieces[i], n);
                    at += n;
                    if (i + 1 < typeArgCount)
                        rebuilt[at++] = ',';
                }
                rebuilt[at++] = ']';
                rebuilt[at] = '\0';

                type.className = rebuilt;
                type.classNameLen = (int)at;
                return type;
            }
        }
    }

    if (type.kind == TYPE_LIST && type.listElemKind != TYPE_UNKNOWN)
    {
        TypeInfo elem = {.kind = type.listElemKind, .className = type.listElemClassName, .classNameLen = type.listElemClassNameLen};
        elem = substituteClassTypeParams(elem, ctx, receiverTypeArgs, receiverTypeArgCount);
        type.listElemKind = elem.kind;
        type.listElemClassName = elem.className;
        type.listElemClassNameLen = elem.classNameLen;
        return type;
    }

    if (type.kind == TYPE_MAP && type.mapKeyKind != TYPE_UNKNOWN && type.mapValueKind != TYPE_UNKNOWN)
    {
        TypeInfo key = {.kind = type.mapKeyKind, .className = type.mapKeyClassName, .classNameLen = type.mapKeyClassNameLen};
        TypeInfo val = {.kind = type.mapValueKind, .className = type.mapValueClassName, .classNameLen = type.mapValueClassNameLen};
        key = substituteClassTypeParams(key, ctx, receiverTypeArgs, receiverTypeArgCount);
        val = substituteClassTypeParams(val, ctx, receiverTypeArgs, receiverTypeArgCount);
        type.mapKeyKind = key.kind;
        type.mapKeyClassName = key.className;
        type.mapKeyClassNameLen = key.classNameLen;
        type.mapValueKind = val.kind;
        type.mapValueClassName = val.className;
        type.mapValueClassNameLen = val.classNameLen;
        return type;
    }

    if (type.kind == TYPE_UNION)
    {
        for (int i = 0; i < type.unionCount; i++)
        {
            TypeInfo member = {.kind = type.unionKinds[i], .className = type.unionClassNames[i], .classNameLen = type.unionClassNameLens[i]};
            member = substituteClassTypeParams(member, ctx, receiverTypeArgs, receiverTypeArgCount);
            type.unionKinds[i] = member.kind;
            type.unionClassNames[i] = member.className;
            type.unionClassNameLens[i] = member.classNameLen;
        }
    }

    return type;
}

static bool hasUnboundClassTypeParam(TypeInfo type, const FunctionCtx *ctx)
{
    if (ctx == NULL)
        return false;

    if (type.kind == TYPE_CLASS_INSTANCE)
    {
        if (findTypeParamIndex(ctx, type.className, type.classNameLen) >= 0)
            return true;
        if (type.className != NULL && type.classNameLen > 0)
        {
            bool allUpper = true;
            for (int i = 0; i < type.classNameLen; i++)
            {
                char ch = type.className[i];
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'))
                {
                    allUpper = false;
                    break;
                }
            }
            if (allUpper && type.classNameLen <= 8)
                return true;
        }
        return false;
    }

    if (type.kind == TYPE_LIST && type.listElemKind != TYPE_UNKNOWN)
    {
        TypeInfo elem = {.kind = type.listElemKind, .className = type.listElemClassName, .classNameLen = type.listElemClassNameLen};
        return hasUnboundClassTypeParam(elem, ctx);
    }

    if (type.kind == TYPE_MAP && type.mapKeyKind != TYPE_UNKNOWN && type.mapValueKind != TYPE_UNKNOWN)
    {
        TypeInfo key = {.kind = type.mapKeyKind, .className = type.mapKeyClassName, .classNameLen = type.mapKeyClassNameLen};
        TypeInfo val = {.kind = type.mapValueKind, .className = type.mapValueClassName, .classNameLen = type.mapValueClassNameLen};
        return hasUnboundClassTypeParam(key, ctx) || hasUnboundClassTypeParam(val, ctx);
    }

    if (type.kind == TYPE_UNION)
    {
        for (int i = 0; i < type.unionCount; i++)
        {
            TypeInfo member = {.kind = type.unionKinds[i], .className = type.unionClassNames[i], .classNameLen = type.unionClassNameLens[i]};
            if (hasUnboundClassTypeParam(member, ctx))
                return true;
        }
    }

    return false;
}

static void collectTypeVarsFromType(TypeInfo type, const char **names, int *lens, int *count, int max)
{
    if (count == NULL || *count >= max)
        return;

    if (type.kind == TYPE_CLASS_INSTANCE && type.className != NULL && type.classNameLen > 0)
    {
        bool allUpper = true;
        for (int i = 0; i < type.classNameLen; i++)
        {
            char ch = type.className[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'))
            {
                allUpper = false;
                break;
            }
        }
        if (allUpper && type.classNameLen <= 8)
        {
            for (int i = 0; i < *count; i++)
            {
                if (lens[i] == type.classNameLen && memcmp(names[i], type.className, (size_t)type.classNameLen) == 0)
                    return;
            }
            names[*count] = type.className;
            lens[*count] = type.classNameLen;
            (*count)++;
        }
        return;
    }

    if (type.kind == TYPE_LIST && type.listElemKind != TYPE_UNKNOWN)
    {
        TypeInfo elem = {.kind = type.listElemKind, .className = type.listElemClassName, .classNameLen = type.listElemClassNameLen};
        collectTypeVarsFromType(elem, names, lens, count, max);
        return;
    }

    if (type.kind == TYPE_MAP && type.mapKeyKind != TYPE_UNKNOWN && type.mapValueKind != TYPE_UNKNOWN)
    {
        TypeInfo key = {.kind = type.mapKeyKind, .className = type.mapKeyClassName, .classNameLen = type.mapKeyClassNameLen};
        TypeInfo val = {.kind = type.mapValueKind, .className = type.mapValueClassName, .classNameLen = type.mapValueClassNameLen};
        collectTypeVarsFromType(key, names, lens, count, max);
        collectTypeVarsFromType(val, names, lens, count, max);
        return;
    }

    if (type.kind == TYPE_UNION)
    {
        for (int i = 0; i < type.unionCount && *count < max; i++)
        {
            TypeInfo member = {.kind = type.unionKinds[i], .className = type.unionClassNames[i], .classNameLen = type.unionClassNameLens[i]};
            collectTypeVarsFromType(member, names, lens, count, max);
        }
    }
}

static void inferFallbackClassTypeParams(const FunctionCtx *ctx, const char **names, int *lens, int *count, int max)
{
    if (ctx == NULL || names == NULL || lens == NULL || count == NULL)
        return;

    *count = 0;
    for (int i = 0; i < ctx->paramCount && *count < max; i++)
        collectTypeVarsFromType(ctx->paramTypes[i], names, lens, count, max);
    if (*count < max)
        collectTypeVarsFromType(ctx->returnType, names, lens, count, max);
}

static int findMethodCtxByOwnerAndName(const FunctionCtxVec *contexts,
                                       const char *ownerClassName,
                                       int ownerClassNameLen,
                                       const AstNode *methodNameNode)
{
    for (int i = contexts->count - 1; i >= 0; i--)
    {
        const FunctionCtx *ctx = &contexts->items[i];
        if (ctx->ownerClassName == NULL || ctx->ownerClassNameLen <= 0)
            continue;
        if (ctx->ownerClassNameLen != ownerClassNameLen)
            continue;
        if (memcmp(ctx->ownerClassName, ownerClassName, (size_t)ownerClassNameLen) != 0)
            continue;
        if (ctx->fnNameLen != methodNameNode->lexemeLen)
            continue;
        if (memcmp(ctx->fnName, methodNameNode->lexeme, (size_t)methodNameNode->lexemeLen) == 0)
            return i;
    }
    return -1;
}

static int findMethodCtxByOwnerAndRawName(const FunctionCtxVec *contexts,
                                          const char *ownerClassName,
                                          int ownerClassNameLen,
                                          const char *methodName,
                                          int methodNameLen)
{
    for (int i = contexts->count - 1; i >= 0; i--)
    {
        const FunctionCtx *ctx = &contexts->items[i];
        if (ctx->ownerClassName == NULL || ctx->ownerClassNameLen <= 0)
            continue;
        if (ctx->ownerClassNameLen != ownerClassNameLen)
            continue;
        if (memcmp(ctx->ownerClassName, ownerClassName, (size_t)ownerClassNameLen) != 0)
            continue;
        if (ctx->fnNameLen != methodNameLen)
            continue;
        if (memcmp(ctx->fnName, methodName, (size_t)methodNameLen) == 0)
            return i;
    }
    return -1;
}

static TypeInfo parseTypeAtom(const char *name, int nameLen)
{
    if (name == NULL || nameLen <= 0)
        return (TypeInfo){.kind = TYPE_UNKNOWN};

    if (nameLen == 3 && strncasecmp(name, "any", 3) == 0)
        return (TypeInfo){.kind = TYPE_ANY};
    if ((nameLen == 6 && strncasecmp(name, "number", 6) == 0) ||
        (nameLen == 3 && strncasecmp(name, "int", 3) == 0) ||
        (nameLen == 5 && strncasecmp(name, "float", 5) == 0))
        return (TypeInfo){.kind = TYPE_NUMBER};
    if ((nameLen == 6 && strncasecmp(name, "string", 6) == 0) ||
        (nameLen == 3 && strncasecmp(name, "str", 3) == 0))
        return (TypeInfo){.kind = TYPE_STRING};
    if ((nameLen == 4 && strncasecmp(name, "bool", 4) == 0) ||
        (nameLen == 7 && strncasecmp(name, "boolean", 7) == 0))
        return (TypeInfo){.kind = TYPE_BOOL};
    if ((nameLen == 3 && strncasecmp(name, "nil", 3) == 0) ||
        (nameLen == 4 && strncasecmp(name, "null", 4) == 0))
        return (TypeInfo){.kind = TYPE_NIL};
    if (nameLen == 4 && strncasecmp(name, "list", 4) == 0)
        return (TypeInfo){.kind = TYPE_LIST};
    if (nameLen > 6 && strncasecmp(name, "list<", 5) == 0 && name[nameLen - 1] == '>')
    {
        TypeInfo elem = parseTypeName(name + 5, nameLen - 6);
        TypeInfo out = {.kind = TYPE_LIST};
        out.listElemKind = elem.kind;
        out.listElemClassName = elem.className;
        out.listElemClassNameLen = elem.classNameLen;
        return out;
    }
    if (nameLen > 6 && strncasecmp(name, "list[", 5) == 0 && name[nameLen - 1] == ']')
    {
        TypeInfo elem = parseTypeName(name + 5, nameLen - 6);
        TypeInfo out = {.kind = TYPE_LIST};
        out.listElemKind = elem.kind;
        out.listElemClassName = elem.className;
        out.listElemClassNameLen = elem.classNameLen;
        return out;
    }
    if (nameLen == 3 && strncasecmp(name, "map", 3) == 0)
        return (TypeInfo){.kind = TYPE_MAP};
    if ((nameLen > 5 && strncasecmp(name, "map<", 4) == 0 && name[nameLen - 1] == '>') ||
        (nameLen > 5 && strncasecmp(name, "map[", 4) == 0 && name[nameLen - 1] == ']'))
    {
        int innerStart = 4;
        int innerLen = nameLen - 5;
        int commaAt = -1;
        int depth = 0;
        for (int i = 0; i < innerLen; i++)
        {
            char ch = name[innerStart + i];
            if (ch == '<')
                depth++;
            else if (ch == '>')
                depth--;
            else if (ch == ',' && depth == 0)
            {
                commaAt = i;
                break;
            }
        }
        if (commaAt > 0)
        {
            TypeInfo key = parseTypeName(name + innerStart, commaAt);
            int valStart = innerStart + commaAt + 1;
            while (valStart < innerStart + innerLen && (name[valStart] == ' ' || name[valStart] == '\t'))
                valStart++;
            TypeInfo value = parseTypeName(name + valStart, (innerStart + innerLen) - valStart);

            TypeInfo out = {.kind = TYPE_MAP};
            out.mapKeyKind = key.kind;
            out.mapKeyClassName = key.className;
            out.mapKeyClassNameLen = key.classNameLen;
            out.mapValueKind = value.kind;
            out.mapValueClassName = value.className;
            out.mapValueClassNameLen = value.classNameLen;
            return out;
        }
    }
    if ((nameLen == 8 && strncasecmp(name, "function", 8) == 0) ||
        (nameLen == 2 && strncasecmp(name, "fn", 2) == 0))
        return (TypeInfo){.kind = TYPE_FUNCTION};

    int genericStart = findTopLevelChar(name, nameLen, '<');
    if (!(genericStart > 0 && name[nameLen - 1] == '>'))
        genericStart = findTopLevelChar(name, nameLen, '[');
    if (genericStart > 0 && (name[nameLen - 1] == '>' || name[nameLen - 1] == ']'))
    {
        return (TypeInfo){
            .kind = TYPE_CLASS_INSTANCE,
            .className = name,
            .classNameLen = nameLen,
        };
    }

    return (TypeInfo){
        .kind = TYPE_CLASS_INSTANCE,
        .className = name,
        .classNameLen = nameLen,
    };
}

static bool addUnionMember(TypeInfo *t, TypeInfo member)
{
    if (t->kind != TYPE_UNION)
        return false;
    if (t->unionCount >= TYPE_UNION_MAX)
        return false;

    t->unionKinds[t->unionCount] = member.kind;
    t->unionClassNames[t->unionCount] = member.className;
    t->unionClassNameLens[t->unionCount] = member.classNameLen;
    t->unionListElemKinds[t->unionCount] = member.listElemKind;
    t->unionListElemClassNames[t->unionCount] = member.listElemClassName;
    t->unionListElemClassNameLens[t->unionCount] = member.listElemClassNameLen;
    t->unionMapKeyKinds[t->unionCount] = member.mapKeyKind;
    t->unionMapKeyClassNames[t->unionCount] = member.mapKeyClassName;
    t->unionMapKeyClassNameLens[t->unionCount] = member.mapKeyClassNameLen;
    t->unionMapValueKinds[t->unionCount] = member.mapValueKind;
    t->unionMapValueClassNames[t->unionCount] = member.mapValueClassName;
    t->unionMapValueClassNameLens[t->unionCount] = member.mapValueClassNameLen;
    t->unionCount++;
    return true;
}

static TypeInfo parseTypeName(const char *name, int nameLen)
{
    if (name == NULL || nameLen <= 0)
        return (TypeInfo){.kind = TYPE_UNKNOWN};

    if (nameLen > 1 && name[nameLen - 1] == '?')
    {
        TypeInfo base = parseTypeName(name, nameLen - 1);
        if (base.kind == TYPE_UNKNOWN)
            return base;
        TypeInfo out = {.kind = TYPE_UNION, .unionCount = 0};
        if (!addUnionMember(&out, base))
            return (TypeInfo){.kind = TYPE_UNKNOWN};
        if (!addUnionMember(&out, (TypeInfo){.kind = TYPE_NIL}))
            return (TypeInfo){.kind = TYPE_UNKNOWN};
        return out;
    }

    bool hasPipe = false;
    int depth = 0;
    for (int i = 0; i < nameLen; i++)
    {
        if (name[i] == '<' || name[i] == '[')
            depth++;
        else if (name[i] == '>' || name[i] == ']')
            depth--;
        else if (name[i] == '|' && depth == 0)
        {
            hasPipe = true;
            break;
        }
    }

    if (!hasPipe)
        return parseTypeAtom(name, nameLen);

    TypeInfo out = {.kind = TYPE_UNION, .unionCount = 0};
    int start = 0;
    depth = 0;
    for (int i = 0; i <= nameLen; i++)
    {
        char ch = i < nameLen ? name[i] : '\0';
        if (ch == '<' || ch == '[')
            depth++;
        else if (ch == '>' || ch == ']')
            depth--;

        if (i == nameLen || (ch == '|' && depth == 0))
        {
            int end = i;
            while (start < end && (name[start] == ' ' || name[start] == '\t'))
                start++;
            while (end > start && (name[end - 1] == ' ' || name[end - 1] == '\t'))
                end--;

            if (end > start)
            {
                TypeInfo member = parseTypeAtom(name + start, end - start);
                if (!addUnionMember(&out, member))
                    return (TypeInfo){.kind = TYPE_UNKNOWN};
            }
            start = i + 1;
        }
    }

    if (out.unionCount == 1)
    {
        TypeInfo one = {
            .kind = out.unionKinds[0],
            .className = out.unionClassNames[0],
            .classNameLen = out.unionClassNameLens[0],
            .listElemKind = out.unionListElemKinds[0],
            .listElemClassName = out.unionListElemClassNames[0],
            .listElemClassNameLen = out.unionListElemClassNameLens[0],
            .mapKeyKind = out.unionMapKeyKinds[0],
            .mapKeyClassName = out.unionMapKeyClassNames[0],
            .mapKeyClassNameLen = out.unionMapKeyClassNameLens[0],
            .mapValueKind = out.unionMapValueKinds[0],
            .mapValueClassName = out.unionMapValueClassNames[0],
            .mapValueClassNameLen = out.unionMapValueClassNameLens[0],
        };
        return one;
    }

    return out;
}

static void formatTypeInfo(char *out, size_t outSize, TypeInfo t)
{
    if (out == NULL || outSize == 0)
        return;

    if (t.kind == TYPE_CLASS_INSTANCE && t.className != NULL && t.classNameLen > 0)
    {
        snprintf(out, outSize, "instance<%.*s>", t.classNameLen, t.className);
        return;
    }

    if (t.kind == TYPE_CLASS_OBJECT && t.className != NULL && t.classNameLen > 0)
    {
        snprintf(out, outSize, "class<%.*s>", t.classNameLen, t.className);
        return;
    }

    if (t.kind == TYPE_LIST && t.listElemKind != TYPE_UNKNOWN)
    {
        TypeInfo elem = {.kind = t.listElemKind, .className = t.listElemClassName, .classNameLen = t.listElemClassNameLen};
        char tmp[64] = {0};
        formatTypeInfo(tmp, sizeof(tmp), elem);
        snprintf(out, outSize, "list<%s>", tmp);
        return;
    }

    if (t.kind == TYPE_LIST && t.listElemKind == TYPE_UNKNOWN)
    {
        snprintf(out, outSize, "list<?>");
        return;
    }

    if (t.kind == TYPE_MAP)
    {
        char keyTmp[64] = {0};
        char valTmp[64] = {0};

        if (t.mapKeyKind == TYPE_UNKNOWN)
            snprintf(keyTmp, sizeof(keyTmp), "?");
        else
        {
            TypeInfo key = {.kind = t.mapKeyKind, .className = t.mapKeyClassName, .classNameLen = t.mapKeyClassNameLen};
            formatTypeInfo(keyTmp, sizeof(keyTmp), key);
        }

        if (t.mapValueKind == TYPE_UNKNOWN)
            snprintf(valTmp, sizeof(valTmp), "?");
        else
        {
            TypeInfo value = {.kind = t.mapValueKind, .className = t.mapValueClassName, .classNameLen = t.mapValueClassNameLen};
            formatTypeInfo(valTmp, sizeof(valTmp), value);
        }

        snprintf(out, outSize, "map<%s,%s>", keyTmp, valTmp);
        return;
    }

    if (t.kind == TYPE_UNION && t.unionCount > 0)
    {
        size_t offset = 0;
        out[0] = '\0';
        for (int i = 0; i < t.unionCount; i++)
        {
            TypeInfo member = {
                .kind = t.unionKinds[i],
                .className = t.unionClassNames[i],
                .classNameLen = t.unionClassNameLens[i],
                .listElemKind = t.unionListElemKinds[i],
                .listElemClassName = t.unionListElemClassNames[i],
                .listElemClassNameLen = t.unionListElemClassNameLens[i],
                .mapKeyKind = t.unionMapKeyKinds[i],
                .mapKeyClassName = t.unionMapKeyClassNames[i],
                .mapKeyClassNameLen = t.unionMapKeyClassNameLens[i],
                .mapValueKind = t.unionMapValueKinds[i],
                .mapValueClassName = t.unionMapValueClassNames[i],
                .mapValueClassNameLen = t.unionMapValueClassNameLens[i],
            };
            char tmp[64] = {0};
            formatTypeInfo(tmp, sizeof(tmp), member);
            int wrote = snprintf(out + offset, outSize > offset ? outSize - offset : 0, "%s%s", i == 0 ? "" : "|", tmp);
            if (wrote <= 0)
                break;
            offset += (size_t)wrote;
            if (offset >= outSize)
                break;
        }
        return;
    }

    snprintf(out, outSize, "%s", typeName(t.kind));
}

static bool scopeVisible(const AstTree *tree, int declaredScopeId, int currentScopeId)
{
    int cursor = currentScopeId;
    while (cursor >= 0)
    {
        if (cursor == declaredScopeId)
            return true;
        cursor = tree->scopes.items[cursor].parentId;
    }
    return false;
}

static int findBindingIndex(const AstTree *tree, const BindingVec *bindings, const AstNode *nameNode)
{
    int best = -1;
    int bestDepth = -1;
    for (int i = 0; i < bindings->count; i++)
    {
        const TypeBinding *binding = &bindings->items[i];
        if (binding->nameLen != nameNode->lexemeLen)
            continue;
        if (memcmp(binding->name, nameNode->lexeme, (size_t)binding->nameLen) != 0)
            continue;
        if (!scopeVisible(tree, binding->scopeId, nameNode->scopeId))
            continue;

        int depth = tree->scopes.items[binding->scopeId].depth;
        if (depth > bestDepth)
        {
            bestDepth = depth;
            best = i;
        }
    }
    return best;
}

static bool addBinding(BindingVec *bindings, TypeBinding binding)
{
    int nextCount = bindings->count + 1;
    if (!ensureCapacity((void **)&bindings->items, &bindings->capacity, nextCount, sizeof(TypeBinding)))
        return false;
    bindings->items[bindings->count] = binding;
    bindings->count = nextCount;
    return true;
}

static bool addClassInfo(ClassVec *classes, ClassInfo classInfo)
{
    int nextCount = classes->count + 1;
    if (!ensureCapacity((void **)&classes->items, &classes->capacity, nextCount, sizeof(ClassInfo)))
        return false;
    classes->items[classes->count] = classInfo;
    classes->count = nextCount;
    return true;
}

static bool addTypeHint(TypeHintVec *hints, TypeHint hint)
{
    int nextCount = hints->count + 1;
    if (!ensureCapacity((void **)&hints->items, &hints->capacity, nextCount, sizeof(TypeHint)))
        return false;
    hints->items[hints->count] = hint;
    hints->count = nextCount;
    return true;
}

static bool addParamHint(ParamHintVec *hints, ParamHint hint)
{
    int nextCount = hints->count + 1;
    if (!ensureCapacity((void **)&hints->items, &hints->capacity, nextCount, sizeof(ParamHint)))
        return false;
    hints->items[hints->count] = hint;
    hints->count = nextCount;
    return true;
}

static bool addReturnHint(ReturnHintVec *hints, ReturnHint hint)
{
    int nextCount = hints->count + 1;
    if (!ensureCapacity((void **)&hints->items, &hints->capacity, nextCount, sizeof(ReturnHint)))
        return false;
    hints->items[hints->count] = hint;
    hints->count = nextCount;
    return true;
}

static bool addFunctionCtx(FunctionCtxVec *contexts, FunctionCtx ctx)
{
    int nextCount = contexts->count + 1;
    if (!ensureCapacity((void **)&contexts->items, &contexts->capacity, nextCount, sizeof(FunctionCtx)))
        return false;
    contexts->items[contexts->count] = ctx;
    contexts->count = nextCount;
    return true;
}

static int findHintIndex(const TypeHintVec *hints, const AstNode *nameNode)
{
    for (int i = hints->count - 1; i >= 0; i--)
    {
        if (hints->items[i].nameLen != nameNode->lexemeLen)
            continue;
        if (memcmp(hints->items[i].name, nameNode->lexeme, (size_t)nameNode->lexemeLen) == 0)
            return i;
    }
    return -1;
}

static int findParamHintIndex(const ParamHintVec *hints,
                              const char *fnName,
                              int fnNameLen,
                              const AstNode *paramNode)
{
    for (int i = hints->count - 1; i >= 0; i--)
    {
        if (hints->items[i].fnNameLen != fnNameLen || hints->items[i].paramNameLen != paramNode->lexemeLen)
            continue;
        if (memcmp(hints->items[i].fnName, fnName, (size_t)fnNameLen) != 0)
            continue;
        if (memcmp(hints->items[i].paramName, paramNode->lexeme, (size_t)paramNode->lexemeLen) != 0)
            continue;
        return i;
    }
    return -1;
}

static int findReturnHintIndex(const ReturnHintVec *hints, const char *fnName, int fnNameLen)
{
    for (int i = hints->count - 1; i >= 0; i--)
    {
        if (hints->items[i].fnNameLen != fnNameLen)
            continue;
        if (memcmp(hints->items[i].fnName, fnName, (size_t)fnNameLen) == 0)
            return i;
    }
    return -1;
}

static int findFunctionCtxByName(const FunctionCtxVec *contexts, const AstNode *nameNode)
{
    for (int i = contexts->count - 1; i >= 0; i--)
    {
        if (contexts->items[i].fnNameLen != nameNode->lexemeLen)
            continue;
        if (memcmp(contexts->items[i].fnName, nameNode->lexeme, (size_t)nameNode->lexemeLen) == 0)
            return i;
    }
    return -1;
}

static bool parseTypeNameFromFlowRange(const NodeRefVec *flow, int startIdx, int endIdx, TypeInfo *outType)
{
    if (flow == NULL || outType == NULL)
        return false;
    if (startIdx < 0 || endIdx < startIdx || endIdx >= flow->count)
        return false;

    size_t total = 0;
    for (int i = startIdx; i <= endIdx; i++)
    {
        const AstNode *n = flow->items[i];
        if (n == NULL || n->lexeme == NULL || n->lexemeLen <= 0)
            return false;
        total += (size_t)n->lexemeLen;
    }

    char *buf = (char *)malloc(total + 1);
    if (buf == NULL)
        return false;

    size_t at = 0;
    for (int i = startIdx; i <= endIdx; i++)
    {
        const AstNode *n = flow->items[i];
        memcpy(buf + at, n->lexeme, (size_t)n->lexemeLen);
        at += (size_t)n->lexemeLen;
    }
    buf[at] = '\0';

    *outType = parseTypeName(buf, (int)at);
    return true;
}

static bool parseInlineTypeFromFlow(const NodeRefVec *flow, int colonIndex, TypeInfo *outType, int *typeTokenIndex);

static bool addFunctionCtxFromSignature(const AstTree *tree,
                                        const NodeRefVec *flow,
                                        int fnNameIndex,
                                        int paramsOpen,
                                        int paramsClose,
                                        int bodyScopeId,
                                        TypeInfo returnType,
                                        const char *ownerClassName,
                                        int ownerClassNameLen,
                                        const char **classTypeParamNames,
                                        const int *classTypeParamNameLens,
                                        int classTypeParamCount,
                                        BindingVec *bindings,
                                        FunctionCtxVec *fnContexts,
                                        const ParamHintVec *paramHints)
{
    if (fnNameIndex < 0 || fnNameIndex >= flow->count || paramsOpen < 0 || paramsClose <= paramsOpen)
        return false;

    const AstNode *fnNameNode = flow->items[fnNameIndex];
    if (fnNameNode == NULL || fnNameNode->tokenType != TOKEN_IDENTIFIER)
        return false;

    FunctionCtx ctx = {
        .fnName = fnNameNode->lexeme,
        .fnNameLen = fnNameNode->lexemeLen,
        .ownerClassName = ownerClassName,
        .ownerClassNameLen = ownerClassNameLen,
        .classTypeParamCount = 0,
        .bodyScopeId = bodyScopeId,
        .paramCount = 0,
        .returnType = returnType,
    };

    if (classTypeParamNames != NULL && classTypeParamNameLens != NULL && classTypeParamCount > 0)
    {
        int capped = classTypeParamCount > 8 ? 8 : classTypeParamCount;
        for (int gi = 0; gi < capped; gi++)
        {
            ctx.classTypeParamNames[gi] = classTypeParamNames[gi];
            ctx.classTypeParamNameLens[gi] = classTypeParamNameLens[gi];
        }
        ctx.classTypeParamCount = capped;
    }

    bool pendingVariadic = false;
    for (int k = paramsOpen + 1; k < paramsClose;)
    {
        const AstNode *paramNode = flow->items[k];
        if (paramNode == NULL)
        {
            k++;
            continue;
        }

        if (paramNode->tokenType == TOKEN_COMMA || paramNode->tokenType == TOKEN_STAR)
        {
            if (paramNode->tokenType == TOKEN_STAR)
                pendingVariadic = true;
            k++;
            continue;
        }

        if (paramNode->tokenType != TOKEN_IDENTIFIER)
        {
            k++;
            continue;
        }

        TypeInfo inlineParamType = {.kind = TYPE_UNKNOWN};
        if (k + 1 < paramsClose)
            parseInlineTypeFromFlow(flow, k + 1, &inlineParamType, NULL);

        if (ctx.paramCount < (int)(sizeof(ctx.params) / sizeof(ctx.params[0])))
        {
            int pos = ctx.paramCount;
            ctx.params[pos] = paramNode;
            ctx.paramTypes[pos] = inlineParamType;
            ctx.paramVariadic[pos] = pendingVariadic;
            ctx.paramCount++;
        }

        TypeInfo hinted = (TypeInfo){.kind = TYPE_UNKNOWN};
        int paramHintIdx = findParamHintIndex(paramHints, ctx.fnName, ctx.fnNameLen, paramNode);
        if (paramHintIdx >= 0)
            hinted = paramHints->items[paramHintIdx].hintedType;
        if (inlineParamType.kind != TYPE_UNKNOWN)
            hinted = inlineParamType;

        TypeBinding binding = {
            .name = paramNode->lexeme,
            .nameLen = paramNode->lexemeLen,
            .scopeId = bodyScopeId,
            .isConst = false,
            .type = hinted,
        };
        if (!addBinding(bindings, binding))
            return false;

        if (k + 2 < paramsClose && flow->items[k + 1]->tokenType == TOKEN_COLON && flow->items[k + 2]->tokenType == TOKEN_IDENTIFIER)
            k += 3;
        else
            k += 1;

        pendingVariadic = false;
    }

    if (!addFunctionCtx(fnContexts, ctx))
        return false;

    (void)tree;
    return true;
}

static bool parseTypeMemberFromFlow(const NodeRefVec *flow, int *cursor, TypeInfo *outMember, bool *outNullable)
{
    if (cursor == NULL || outMember == NULL || outNullable == NULL)
        return false;
    if (*cursor < 0 || *cursor >= flow->count)
        return false;

    const AstNode *typeNode = flow->items[*cursor];
    if (typeNode->tokenType != TOKEN_IDENTIFIER && typeNode->tokenType != TOKEN_NIL)
        return false;

    int typeStart = *cursor;
    TypeInfo member = parseTypeName(typeNode->lexeme, typeNode->lexemeLen);

    if (*cursor + 1 < flow->count &&
        (flow->items[*cursor + 1]->tokenType == TOKEN_LESS || flow->items[*cursor + 1]->tokenType == TOKEN_LEFT_BRACKET))
    {
        TokenType openDelim = flow->items[*cursor + 1]->tokenType;
        TokenType closeDelim = (openDelim == TOKEN_LESS) ? TOKEN_GREATER : TOKEN_RIGHT_BRACKET;
        int idx = *cursor + 2;
        TypeInfo arg1 = {.kind = TYPE_UNKNOWN};
        bool arg1Nullable = false;
        if (!parseTypeMemberFromFlow(flow, &idx, &arg1, &arg1Nullable))
            return false;

        if (arg1Nullable)
        {
            TypeInfo nullable = {.kind = TYPE_UNION, .unionCount = 0};
            if (!addUnionMember(&nullable, arg1) || !addUnionMember(&nullable, (TypeInfo){.kind = TYPE_NIL}))
                return false;
            arg1 = nullable;
        }

        if (member.kind == TYPE_LIST)
        {
            member.listElemKind = arg1.kind;
            member.listElemClassName = arg1.className;
            member.listElemClassNameLen = arg1.classNameLen;

            if (idx + 1 >= flow->count || flow->items[idx + 1]->tokenType != closeDelim)
                return false;
            *cursor = idx + 1;
        }
        else if (member.kind == TYPE_MAP)
        {
            if (idx + 1 >= flow->count || flow->items[idx + 1]->tokenType != TOKEN_COMMA)
                return false;
            idx += 2;

            TypeInfo arg2 = {.kind = TYPE_UNKNOWN};
            bool arg2Nullable = false;
            if (!parseTypeMemberFromFlow(flow, &idx, &arg2, &arg2Nullable))
                return false;
            if (arg2Nullable)
            {
                TypeInfo nullable = {.kind = TYPE_UNION, .unionCount = 0};
                if (!addUnionMember(&nullable, arg2) || !addUnionMember(&nullable, (TypeInfo){.kind = TYPE_NIL}))
                    return false;
                arg2 = nullable;
            }

            member.mapKeyKind = arg1.kind;
            member.mapKeyClassName = arg1.className;
            member.mapKeyClassNameLen = arg1.classNameLen;
            member.mapValueKind = arg2.kind;
            member.mapValueClassName = arg2.className;
            member.mapValueClassNameLen = arg2.classNameLen;

            if (idx + 1 >= flow->count || flow->items[idx + 1]->tokenType != closeDelim)
                return false;
            *cursor = idx + 1;
        }
        else if (member.kind == TYPE_CLASS_INSTANCE)
        {
            int depth = 1;
            while (idx < flow->count)
            {
                TokenType t = flow->items[idx]->tokenType;
                if (t == openDelim)
                    depth++;
                else if (t == closeDelim)
                {
                    depth--;
                    if (depth == 0)
                    {
                        TypeInfo ranged = {.kind = TYPE_UNKNOWN};
                        if (parseTypeNameFromFlowRange(flow, typeStart, idx, &ranged))
                            member = ranged;
                        *cursor = idx;
                        break;
                    }
                }
                idx++;
            }
            if (depth != 0)
                return false;
        }
    }

    bool nullable = false;
    if (*cursor + 1 < flow->count && flow->items[*cursor + 1]->tokenType == TOKEN_QUESTION)
    {
        nullable = true;
        (*cursor)++;
    }

    *outMember = member;
    *outNullable = nullable;
    return true;
}

static bool parseInlineTypeFromFlow(const NodeRefVec *flow, int colonIndex, TypeInfo *outType, int *typeTokenIndex)
{
    if (colonIndex < 0 || colonIndex >= flow->count)
        return false;
    if (flow->items[colonIndex]->tokenType != TOKEN_COLON)
        return false;
    if (colonIndex + 1 >= flow->count)
        return false;

    int cursor = colonIndex + 1;
    if (cursor >= flow->count)
        return false;

    TypeInfo unionType = {.kind = TYPE_UNION, .unionCount = 0};
    for (;;)
    {
        TypeInfo member = {.kind = TYPE_UNKNOWN};
        bool nullable = false;
        if (!parseTypeMemberFromFlow(flow, &cursor, &member, &nullable))
            return false;
        if (!addUnionMember(&unionType, member))
            return false;
        if (nullable)
        {
            if (!addUnionMember(&unionType, (TypeInfo){.kind = TYPE_NIL}))
                return false;
        }

        if (cursor + 2 < flow->count &&
            flow->items[cursor + 1]->tokenType == TOKEN_PIPE &&
            (flow->items[cursor + 2]->tokenType == TOKEN_IDENTIFIER || flow->items[cursor + 2]->tokenType == TOKEN_NIL))
        {
            cursor += 2;
            continue;
        }
        break;
    }

    if (outType != NULL)
    {
        if (unionType.unionCount == 1)
        {
            *outType = (TypeInfo){
                .kind = unionType.unionKinds[0],
                .className = unionType.unionClassNames[0],
                .classNameLen = unionType.unionClassNameLens[0],
                .listElemKind = unionType.unionListElemKinds[0],
                .listElemClassName = unionType.unionListElemClassNames[0],
                .listElemClassNameLen = unionType.unionListElemClassNameLens[0],
                .mapKeyKind = unionType.unionMapKeyKinds[0],
                .mapKeyClassName = unionType.unionMapKeyClassNames[0],
                .mapKeyClassNameLen = unionType.unionMapKeyClassNameLens[0],
                .mapValueKind = unionType.unionMapValueKinds[0],
                .mapValueClassName = unionType.unionMapValueClassNames[0],
                .mapValueClassNameLen = unionType.unionMapValueClassNameLens[0],
            };
        }
        else
        {
            *outType = unionType;
        }
    }
    if (typeTokenIndex != NULL)
        *typeTokenIndex = cursor;
    return true;
}

static bool parseIdentifierToken(const char **p, const char *lineEnd, const char **outStart, int *outLen)
{
    const char *cursor = *p;
    if (cursor >= lineEnd)
        return false;
    if (!((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') || *cursor == '_'))
        return false;

    const char *start = cursor;
    cursor++;
    while (cursor < lineEnd && ((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= '0' && *cursor <= '9') || *cursor == '_'))
        cursor++;

    *outStart = start;
    *outLen = (int)(cursor - start);
    *p = cursor;
    return true;
}

static bool collectTypeHints(const char *source, TypeHintVec *hints, ParamHintVec *paramHints, ReturnHintVec *returnHints)
{
    const char *cursor = source;
    while (*cursor != '\0')
    {
        const char *lineStart = cursor;
        while (*cursor != '\0' && *cursor != '\n')
            cursor++;
        const char *lineEnd = cursor;
        if (*cursor == '\n')
            cursor++;

        const char *hash = lineStart;
        while (hash < lineEnd && *hash != '#')
            hash++;
        if (hash >= lineEnd)
            continue;

        const char *p = hash + 1;
        while (p < lineEnd && (*p == ' ' || *p == '\t'))
            p++;
        bool isType = (p + 5 <= lineEnd && strncmp(p, "@type", 5) == 0);
        bool isParam = (p + 6 <= lineEnd && strncmp(p, "@param", 6) == 0);
        bool isReturn = (p + 7 <= lineEnd && strncmp(p, "@return", 7) == 0);
        if (!isType && !isParam && !isReturn)
            continue;

        p += isType ? 5 : (isParam ? 6 : 7);

        while (p < lineEnd && (*p == ' ' || *p == '\t'))
            p++;

        const char *nameStart = NULL;
        int nameLen = 0;
        if (!parseIdentifierToken(&p, lineEnd, &nameStart, &nameLen))
            continue;

        const char *paramStart = NULL;
        int paramLen = 0;
        if (isParam)
        {
            if (p >= lineEnd || *p != '.')
                continue;
            p++;
            if (!parseIdentifierToken(&p, lineEnd, &paramStart, &paramLen))
                continue;
        }

        while (p < lineEnd && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= lineEnd || *p != ':')
            continue;
        p++;

        while (p < lineEnd && (*p == ' ' || *p == '\t'))
            p++;
        const char *typeStart = p;
        while (p < lineEnd && *p != ' ' && *p != '\t' && *p != '#')
            p++;
        int typeLen = (int)(p - typeStart);
        if (typeLen <= 0)
            continue;

        TypeInfo type = parseTypeName(typeStart, typeLen);
        if (isType)
        {
            TypeHint hint = {
                .name = nameStart,
                .nameLen = nameLen,
                .hintedType = type,
            };
            if (!addTypeHint(hints, hint))
                return false;
        }
        else if (isParam)
        {
            ParamHint hint = {
                .fnName = nameStart,
                .fnNameLen = nameLen,
                .paramName = paramStart,
                .paramNameLen = paramLen,
                .hintedType = type,
            };
            if (!addParamHint(paramHints, hint))
                return false;
        }
        else
        {
            ReturnHint hint = {
                .fnName = nameStart,
                .fnNameLen = nameLen,
                .hintedType = type,
            };
            if (!addReturnHint(returnHints, hint))
                return false;
        }
    }

    return true;
}

static int findClassIndex(const ClassVec *classes, const char *name, int nameLen)
{
    const char *base = NULL;
    int baseLen = 0;
    classTypeBaseName(name, nameLen, &base, &baseLen);

    for (int i = 0; i < classes->count; i++)
    {
        if (classes->items[i].nameLen != baseLen)
            continue;
        if (memcmp(classes->items[i].name, base, (size_t)baseLen) == 0)
            return i;
    }
    return -1;
}

static bool resolveInheritedClassType(const ClassVec *classes, TypeInfo expected, TypeInfo actual, TypeInfo *outAligned)
{
    if (expected.kind != TYPE_CLASS_INSTANCE || actual.kind != TYPE_CLASS_INSTANCE)
        return false;

    const char *expectedBase = NULL;
    int expectedBaseLen = 0;
    classTypeBaseName(expected.className, expected.classNameLen, &expectedBase, &expectedBaseLen);

    TypeInfo current = actual;
    for (int hop = 0; hop < 64; hop++)
    {
        const char *currentBase = NULL;
        int currentBaseLen = 0;
        classTypeBaseName(current.className, current.classNameLen, &currentBase, &currentBaseLen);
        if (currentBaseLen == expectedBaseLen && memcmp(currentBase, expectedBase, (size_t)expectedBaseLen) == 0)
        {
            if (outAligned != NULL)
                *outAligned = current;
            return true;
        }

        int clsIdx = findClassIndex(classes, currentBase, currentBaseLen);
        if (clsIdx < 0)
            return false;

        const ClassInfo *cls = &classes->items[clsIdx];
        if (cls->baseType.kind != TYPE_CLASS_INSTANCE)
            return false;

        TypeInfo currentTypeArgs[8] = {0};
        int currentTypeArgCount = 0;
        parseClassTypeArgs(current.className, current.classNameLen, currentTypeArgs, 8, &currentTypeArgCount);

        FunctionCtx subCtx = {0};
        subCtx.classTypeParamCount = cls->typeParamCount > 8 ? 8 : cls->typeParamCount;
        for (int i = 0; i < subCtx.classTypeParamCount; i++)
        {
            subCtx.classTypeParamNames[i] = cls->typeParamNames[i];
            subCtx.classTypeParamNameLens[i] = cls->typeParamNameLens[i];
        }

        TypeInfo next = substituteClassTypeParams(cls->baseType, &subCtx, currentTypeArgs, currentTypeArgCount);
        if (next.kind != TYPE_CLASS_INSTANCE)
        {
            if (cls->baseName != NULL && cls->baseNameLen > 0)
            {
                next.kind = TYPE_CLASS_INSTANCE;
                next.className = cls->baseName;
                next.classNameLen = cls->baseNameLen;
            }
            else
            {
                return false;
            }
        }

        current = next;
    }

    return false;
}

static bool isClassAssignable(const ClassVec *classes, TypeInfo expected, TypeInfo actual)
{
    if (expected.kind != TYPE_CLASS_INSTANCE || actual.kind != TYPE_CLASS_INSTANCE)
        return false;

    const char *expectedBase = NULL;
    int expectedBaseLen = 0;
    const char *actualBase = NULL;
    int actualBaseLen = 0;
    classTypeBaseName(expected.className, expected.classNameLen, &expectedBase, &expectedBaseLen);
    classTypeBaseName(actual.className, actual.classNameLen, &actualBase, &actualBaseLen);

    if (expectedBaseLen == actualBaseLen && memcmp(expectedBase, actualBase, (size_t)expectedBaseLen) == 0)
        return true;

    int cursor = findClassIndex(classes, actualBase, actualBaseLen);
    while (cursor >= 0)
    {
        ClassInfo *cls = &classes->items[cursor];
        if (cls->baseName == NULL || cls->baseNameLen <= 0)
            return false;
        if (cls->baseNameLen == expectedBaseLen && memcmp(cls->baseName, expectedBase, (size_t)expectedBaseLen) == 0)
            return true;
        cursor = findClassIndex(classes, cls->baseName, cls->baseNameLen);
    }

    return false;
}

static bool typeInfoCompatible(const ClassVec *classes, TypeInfo expected, TypeInfo actual)
{
    if (expected.kind == TYPE_ANY)
        return true;

    if (expected.kind == TYPE_UNION)
    {
        for (int i = 0; i < expected.unionCount; i++)
        {
            TypeInfo member = {
                .kind = expected.unionKinds[i],
                .className = expected.unionClassNames[i],
                .classNameLen = expected.unionClassNameLens[i],
                .listElemKind = expected.unionListElemKinds[i],
                .listElemClassName = expected.unionListElemClassNames[i],
                .listElemClassNameLen = expected.unionListElemClassNameLens[i],
                .mapKeyKind = expected.unionMapKeyKinds[i],
                .mapKeyClassName = expected.unionMapKeyClassNames[i],
                .mapKeyClassNameLen = expected.unionMapKeyClassNameLens[i],
                .mapValueKind = expected.unionMapValueKinds[i],
                .mapValueClassName = expected.unionMapValueClassNames[i],
                .mapValueClassNameLen = expected.unionMapValueClassNameLens[i],
            };
            if (typeInfoCompatible(classes, member, actual))
                return true;
        }
        return false;
    }

    if (actual.kind == TYPE_UNION)
    {
        for (int i = 0; i < actual.unionCount; i++)
        {
            TypeInfo member = {
                .kind = actual.unionKinds[i],
                .className = actual.unionClassNames[i],
                .classNameLen = actual.unionClassNameLens[i],
                .listElemKind = actual.unionListElemKinds[i],
                .listElemClassName = actual.unionListElemClassNames[i],
                .listElemClassNameLen = actual.unionListElemClassNameLens[i],
                .mapKeyKind = actual.unionMapKeyKinds[i],
                .mapKeyClassName = actual.unionMapKeyClassNames[i],
                .mapKeyClassNameLen = actual.unionMapKeyClassNameLens[i],
                .mapValueKind = actual.unionMapValueKinds[i],
                .mapValueClassName = actual.unionMapValueClassNames[i],
                .mapValueClassNameLen = actual.unionMapValueClassNameLens[i],
            };
            if (!typeInfoCompatible(classes, expected, member))
                return false;
        }
        return true;
    }

    if (expected.kind == TYPE_LIST && actual.kind == TYPE_LIST)
    {
        if (expected.listElemKind == TYPE_UNKNOWN)
            return true;
        if (actual.listElemKind == TYPE_UNKNOWN)
            return expected.listElemKind == TYPE_ANY;
        TypeInfo expElem = {.kind = expected.listElemKind, .className = expected.listElemClassName, .classNameLen = expected.listElemClassNameLen};
        TypeInfo actElem = {.kind = actual.listElemKind, .className = actual.listElemClassName, .classNameLen = actual.listElemClassNameLen};
        return typeInfoCompatible(classes, expElem, actElem);
    }

    if (expected.kind == TYPE_MAP && actual.kind == TYPE_MAP)
    {
        if (expected.mapKeyKind == TYPE_UNKNOWN || expected.mapValueKind == TYPE_UNKNOWN)
            return true;
        if (actual.mapKeyKind == TYPE_UNKNOWN)
            return expected.mapKeyKind == TYPE_ANY;
        if (actual.mapValueKind == TYPE_UNKNOWN)
            return expected.mapValueKind == TYPE_ANY;

        TypeInfo expKey = {.kind = expected.mapKeyKind, .className = expected.mapKeyClassName, .classNameLen = expected.mapKeyClassNameLen};
        TypeInfo expVal = {.kind = expected.mapValueKind, .className = expected.mapValueClassName, .classNameLen = expected.mapValueClassNameLen};
        TypeInfo actKey = {.kind = actual.mapKeyKind, .className = actual.mapKeyClassName, .classNameLen = actual.mapKeyClassNameLen};
        TypeInfo actVal = {.kind = actual.mapValueKind, .className = actual.mapValueClassName, .classNameLen = actual.mapValueClassNameLen};
        return typeInfoCompatible(classes, expKey, actKey) && typeInfoCompatible(classes, expVal, actVal);
    }

    if (expected.kind == TYPE_CLASS_INSTANCE || actual.kind == TYPE_CLASS_INSTANCE)
    {
        if (expected.kind != TYPE_CLASS_INSTANCE || actual.kind != TYPE_CLASS_INSTANCE)
            return false;

        TypeInfo alignedActual = actual;
        if (!resolveInheritedClassType(classes, expected, actual, &alignedActual))
            return false;

        if (expected.kind == TYPE_CLASS_INSTANCE && alignedActual.kind == TYPE_CLASS_INSTANCE)
        {
            TypeInfo expectedArgs[8] = {0};
            int expectedArgCount = 0;
            TypeInfo actualArgs[8] = {0};
            int actualArgCount = 0;
            bool expectedHasArgs = parseClassTypeArgs(expected.className, expected.classNameLen, expectedArgs, 8, &expectedArgCount);
            bool actualHasArgs = parseClassTypeArgs(alignedActual.className, alignedActual.classNameLen, actualArgs, 8, &actualArgCount);

            if (expectedHasArgs && actualHasArgs)
            {
                if (expectedArgCount != actualArgCount)
                    return false;
                for (int i = 0; i < expectedArgCount; i++)
                    if (!typeInfoCompatible(classes, expectedArgs[i], actualArgs[i]))
                        return false;
            }
        }

        return true;
    }

    if (expected.kind == TYPE_CLASS_OBJECT || actual.kind == TYPE_CLASS_OBJECT)
    {
        const char *expectedBase = NULL;
        int expectedBaseLen = 0;
        const char *actualBase = NULL;
        int actualBaseLen = 0;
        classTypeBaseName(expected.className, expected.classNameLen, &expectedBase, &expectedBaseLen);
        classTypeBaseName(actual.className, actual.classNameLen, &actualBase, &actualBaseLen);
        return expected.kind == TYPE_CLASS_OBJECT &&
               actual.kind == TYPE_CLASS_OBJECT &&
               expectedBaseLen == actualBaseLen &&
               memcmp(expectedBase, actualBase, (size_t)expectedBaseLen) == 0;
    }

    return expected.kind == actual.kind;
}

static bool addNodeRef(NodeRefVec *nodes, const AstNode *node)
{
    int nextCount = nodes->count + 1;
    if (!ensureCapacity((void **)&nodes->items, &nodes->capacity, nextCount, sizeof(AstNode *)))
        return false;
    nodes->items[nodes->count] = node;
    nodes->count = nextCount;
    return true;
}

static bool flattenTokenFlow(const AstNode *node, NodeRefVec *out)
{
    if (node == NULL)
        return true;

    if (node->kind == AST_NODE_ROOT)
    {
        for (const AstNode *child = node->firstChild; child != NULL; child = child->nextSibling)
            if (!flattenTokenFlow(child, out))
                return false;
        return true;
    }

    if (!addNodeRef(out, node))
        return false;

    for (const AstNode *child = node->firstChild; child != NULL; child = child->nextSibling)
        if (!flattenTokenFlow(child, out))
            return false;

    return true;
}

static const AstNode *nextContainerValueNode(const AstNode *node)
{
    const AstNode *cursor = node;
    while (cursor != NULL)
    {
        if (cursor->tokenType != TOKEN_COMMA && cursor->tokenType != TOKEN_RIGHT_BRACE)
            return cursor;
        cursor = cursor->nextSibling;
    }
    return NULL;
}

static TypeInfo inferNodeTypeFromNode(const AstTree *tree,
                                      const BindingVec *bindings,
                                      const ClassVec *classes,
                                      const AstNode *node);
static bool typeInfoCompatible(const ClassVec *classes, TypeInfo expected, TypeInfo actual);

static void mergeContainerMemberType(const ClassVec *classes, TypeInfo *current, TypeInfo next)
{
    if (current->kind == TYPE_UNKNOWN)
    {
        *current = next;
        return;
    }

    if (!typeInfoCompatible(classes, *current, next))
    {
        *current = (TypeInfo){.kind = TYPE_ANY};
    }
}

static TypeInfo inferNodeTypeFromNode(const AstTree *tree,
                                      const BindingVec *bindings,
                                      const ClassVec *classes,
                                      const AstNode *node)
{
    if (node == NULL)
        return (TypeInfo){.kind = TYPE_UNKNOWN};

    switch (node->tokenType)
    {
    case TOKEN_NUMBER:
        return (TypeInfo){.kind = TYPE_NUMBER};
    case TOKEN_BASIC_STRING:
    case TOKEN_TEMPLATE_STRING:
    case TOKEN_RAW_STRING:
    case TOKEN_BYTE_STRING:
    case TOKEN_HEX_STRING:
    case TOKEN_BINARY_STRING:
        return (TypeInfo){.kind = TYPE_STRING};
    case TOKEN_TRUE:
    case TOKEN_FALSE:
        return (TypeInfo){.kind = TYPE_BOOL};
    case TOKEN_NIL:
        return (TypeInfo){.kind = TYPE_NIL};
    case TOKEN_FUN:
        return (TypeInfo){.kind = TYPE_FUNCTION};
    case TOKEN_IDENTIFIER:
    {
        if (node->nextSibling != NULL && node->nextSibling->tokenType == TOKEN_LEFT_PAREN)
        {
            if (node->lexemeLen == 7 && memcmp(node->lexeme, "HashMap", 7) == 0)
            {
                TypeInfo hm = {.kind = TYPE_MAP};
                hm.mapKeyKind = TYPE_UNKNOWN;
                hm.mapValueKind = TYPE_UNKNOWN;
                return hm;
            }
        }

        int cls = findClassIndex(classes, node->lexeme, node->lexemeLen);
        if (cls >= 0)
        {
            return (TypeInfo){
                .kind = TYPE_CLASS_OBJECT,
                .className = classes->items[cls].name,
                .classNameLen = classes->items[cls].nameLen,
            };
        }
        int idx = findBindingIndex(tree, bindings, node);
        if (idx >= 0)
            return bindings->items[idx].type;
        return (TypeInfo){.kind = TYPE_UNKNOWN};
    }
    case TOKEN_LEFT_BRACE:
    case TOKEN_DOT_BRACKET:
    {
        bool mapLiteral = (node->tokenType == TOKEN_DOT_BRACKET);
        for (const AstNode *c = node->firstChild; c != NULL; c = c->nextSibling)
        {
            if (c->tokenType == TOKEN_DOUBLE_COLON)
            {
                mapLiteral = true;
                break;
            }
        }

        if (mapLiteral)
        {
            TypeInfo keyType = {.kind = TYPE_UNKNOWN};
            TypeInfo valueType = {.kind = TYPE_UNKNOWN};
            for (const AstNode *c = node->firstChild; c != NULL; c = c->nextSibling)
            {
                if (c->tokenType != TOKEN_DOUBLE_COLON)
                    continue;

                const AstNode *keyNode = c->nextSibling == NULL ? NULL : c->nextSibling;
                const AstNode *prev = node->firstChild;
                while (prev != NULL && prev->nextSibling != c)
                    prev = prev->nextSibling;
                keyNode = prev;
                const AstNode *valNode = nextContainerValueNode(c->nextSibling);
                if (keyNode == NULL || valNode == NULL)
                    continue;

                mergeContainerMemberType(classes, &keyType, inferNodeTypeFromNode(tree, bindings, classes, keyNode));
                mergeContainerMemberType(classes, &valueType, inferNodeTypeFromNode(tree, bindings, classes, valNode));
            }

            TypeInfo out = {.kind = TYPE_MAP};
            out.mapKeyKind = keyType.kind;
            out.mapKeyClassName = keyType.className;
            out.mapKeyClassNameLen = keyType.classNameLen;
            out.mapValueKind = valueType.kind;
            out.mapValueClassName = valueType.className;
            out.mapValueClassNameLen = valueType.classNameLen;
            return out;
        }

        TypeInfo elem = {.kind = TYPE_UNKNOWN};
        for (const AstNode *c = node->firstChild; c != NULL; c = c->nextSibling)
        {
            if (c->tokenType == TOKEN_COMMA || c->tokenType == TOKEN_RIGHT_BRACE)
                continue;
            mergeContainerMemberType(classes, &elem, inferNodeTypeFromNode(tree, bindings, classes, c));
        }

        TypeInfo out = {.kind = TYPE_LIST};
        out.listElemKind = elem.kind;
        out.listElemClassName = elem.className;
        out.listElemClassNameLen = elem.classNameLen;
        return out;
    }
    default:
        return (TypeInfo){.kind = TYPE_UNKNOWN};
    }
}

static TypeInfo inferNodeType(const AstTree *tree,
                              const BindingVec *bindings,
                              const ClassVec *classes,
                              const NodeRefVec *flow,
                              int nodeIndex)
{
    const AstNode *node = (nodeIndex >= 0 && nodeIndex < flow->count) ? flow->items[nodeIndex] : NULL;
    if (node == NULL)
        return (TypeInfo){.kind = TYPE_UNKNOWN, .className = NULL, .classNameLen = 0};

    switch (node->tokenType)
    {
    case TOKEN_NUMBER:
        return (TypeInfo){.kind = TYPE_NUMBER};
    case TOKEN_BASIC_STRING:
    case TOKEN_TEMPLATE_STRING:
    case TOKEN_RAW_STRING:
    case TOKEN_BYTE_STRING:
    case TOKEN_HEX_STRING:
    case TOKEN_BINARY_STRING:
        return (TypeInfo){.kind = TYPE_STRING};
    case TOKEN_TRUE:
    case TOKEN_FALSE:
        return (TypeInfo){.kind = TYPE_BOOL};
    case TOKEN_NIL:
        return (TypeInfo){.kind = TYPE_NIL};
    case TOKEN_FUN:
        return (TypeInfo){.kind = TYPE_FUNCTION};
    case TOKEN_LEFT_BRACE:
    case TOKEN_DOT_BRACKET:
        return inferNodeTypeFromNode(tree, bindings, classes, node);
    case TOKEN_IDENTIFIER:
    {
        if (nodeIndex + 1 < flow->count && flow->items[nodeIndex + 1]->tokenType == TOKEN_LEFT_PAREN)
        {
            if (node->lexemeLen == 7 && memcmp(node->lexeme, "HashMap", 7) == 0)
            {
                TypeInfo hm = {.kind = TYPE_MAP};
                hm.mapKeyKind = TYPE_UNKNOWN;
                hm.mapValueKind = TYPE_UNKNOWN;
                return hm;
            }
        }

        if (nodeIndex + 1 < flow->count && flow->items[nodeIndex + 1]->tokenType == TOKEN_LEFT_PAREN)
        {
            int cls = findClassIndex(classes, node->lexeme, node->lexemeLen);
            if (cls >= 0)
            {
                return (TypeInfo){
                    .kind = TYPE_CLASS_INSTANCE,
                    .className = classes->items[cls].name,
                    .classNameLen = classes->items[cls].nameLen,
                };
            }
        }

        int bareCls = findClassIndex(classes, node->lexeme, node->lexemeLen);
        if (bareCls >= 0)
        {
            return (TypeInfo){
                .kind = TYPE_CLASS_OBJECT,
                .className = classes->items[bareCls].name,
                .classNameLen = classes->items[bareCls].nameLen,
            };
        }

        int idx = findBindingIndex(tree, bindings, node);
        if (idx >= 0)
            return bindings->items[idx].type;
        return (TypeInfo){.kind = TYPE_UNKNOWN};
    }
    default:
        return (TypeInfo){.kind = TYPE_UNKNOWN};
    }
}

static bool isNumericOperator(TokenType type)
{
    switch (type)
    {
    case TOKEN_MINUS:
    case TOKEN_STAR:
    case TOKEN_STAR_STAR:
    case TOKEN_SLASH:
    case TOKEN_SLASH_SLASH:
    case TOKEN_PERCENT:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL:
        return true;
    default:
        return false;
    }
}

static bool isInheritanceAngleToken(const NodeRefVec *flow, int idx)
{
    const AstNode *node = flow->items[idx];
    if (node->tokenType != TOKEN_LESS && node->tokenType != TOKEN_GREATER)
        return false;

    for (int j = idx - 1; j >= 0; j--)
    {
        TokenType t = flow->items[j]->tokenType;
        if (t == TOKEN_CLASS)
            return true;
        if (t == TOKEN_LEFT_BRACE || t == TOKEN_RIGHT_BRACE || t == TOKEN_SEMICOLON)
            return false;
    }

    return false;
}

static bool isTypeAnnotationAngleToken(const NodeRefVec *flow, int idx)
{
    const AstNode *node = flow->items[idx];
    if (node->tokenType != TOKEN_LESS && node->tokenType != TOKEN_GREATER)
        return false;

    bool hasColonBefore = false;
    for (int j = idx - 1; j >= 0; j--)
    {
        TokenType t = flow->items[j]->tokenType;
        if (t == TOKEN_COLON)
        {
            hasColonBefore = true;
            break;
        }
        if (t == TOKEN_EQUAL || t == TOKEN_SEMICOLON || t == TOKEN_LEFT_BRACE ||
            t == TOKEN_RIGHT_BRACE || t == TOKEN_ARROW || t == TOKEN_COMMA ||
            t == TOKEN_LEFT_PAREN)
            break;
    }

    if (!hasColonBefore)
        return false;

    for (int k = idx + 1; k < flow->count; k++)
    {
        TokenType t = flow->items[k]->tokenType;
        if (t == TOKEN_EQUAL || t == TOKEN_COMMA || t == TOKEN_RIGHT_PAREN ||
            t == TOKEN_LEFT_BRACE || t == TOKEN_ARROW || t == TOKEN_SEMICOLON)
            return true;
        if (t == TOKEN_COLON)
            return false;
    }

    return true;
}

static int runTypeCheck(const AstTree *tree, const char *source, const char *file, bool strictMode, FILE *out)
{
    NodeRefVec flow = {0};
    BindingVec bindings = {0};
    ClassVec classes = {0};
    TypeHintVec hints = {0};
    ParamHintVec paramHints = {0};
    ReturnHintVec returnHints = {0};
    FunctionCtxVec fnContexts = {0};
    int issues = 0;

    if (!flattenTokenFlow(tree->root, &flow))
    {
        free(flow.items);
        free(bindings.items);
        free(classes.items);
        free(hints.items);
        free(paramHints.items);
        free(returnHints.items);
        free(fnContexts.items);
        return 0;
    }

    if (!collectTypeHints(source == NULL ? "" : source, &hints, &paramHints, &returnHints))
    {
        free(flow.items);
        free(bindings.items);
        free(classes.items);
        free(hints.items);
        free(paramHints.items);
        free(returnHints.items);
        free(fnContexts.items);
        return 0;
    }

    for (int i = 0; i < flow.count; i++)
    {
        const AstNode *node = flow.items[i];
        if (node == NULL || node->tokenType != TOKEN_CLASS)
            continue;
        if (i + 1 >= flow.count || flow.items[i + 1]->tokenType != TOKEN_IDENTIFIER)
            continue;

        const AstNode *nameNode = flow.items[i + 1];
        const char *baseName = NULL;
        int baseNameLen = 0;
        TypeInfo baseType = {.kind = TYPE_UNKNOWN};
        const char *classTypeParamNames[8] = {0};
        int classTypeParamNameLens[8] = {0};
        int classTypeParamCount = 0;

        int header = i + 2;
        if (header < flow.count && flow.items[header]->tokenType == TOKEN_LEFT_BRACKET)
        {
            int depth = 1;
            header++;
            while (header < flow.count && depth > 0)
            {
                TokenType t = flow.items[header]->tokenType;
                if (t == TOKEN_LEFT_BRACKET)
                    depth++;
                else if (t == TOKEN_RIGHT_BRACKET)
                    depth--;

                if (depth == 1 && t == TOKEN_IDENTIFIER && classTypeParamCount < 8)
                {
                    classTypeParamNames[classTypeParamCount] = flow.items[header]->lexeme;
                    classTypeParamNameLens[classTypeParamCount] = flow.items[header]->lexemeLen;
                    classTypeParamCount++;
                }
                header++;
            }
        }

        if (header < flow.count && flow.items[header]->tokenType == TOKEN_LESS)
        {
            int depth = 1;
            int inheritStart = header + 1;
            header++;
            while (header < flow.count && depth > 0)
            {
                TokenType t = flow.items[header]->tokenType;
                if (t == TOKEN_LESS)
                    depth++;
                else if (t == TOKEN_GREATER)
                    depth--;
                header++;
            }

            int inheritEnd = header - 2;
            if (inheritEnd >= inheritStart)
            {
                TypeInfo parsed = {.kind = TYPE_UNKNOWN};
                if (parseTypeNameFromFlowRange(&flow, inheritStart, inheritEnd, &parsed))
                {
                    baseType = parsed;
                    if (baseType.kind == TYPE_CLASS_INSTANCE)
                        classTypeBaseName(baseType.className, baseType.classNameLen, &baseName, &baseNameLen);
                }

                if ((baseName == NULL || baseNameLen <= 0) && flow.items[inheritStart]->tokenType == TOKEN_IDENTIFIER)
                {
                    baseName = flow.items[inheritStart]->lexeme;
                    baseNameLen = flow.items[inheritStart]->lexemeLen;
                }
            }
        }

        ClassInfo cls = {
            .name = nameNode->lexeme,
            .nameLen = nameNode->lexemeLen,
            .baseName = baseName,
            .baseNameLen = baseNameLen,
            .baseType = baseType,
            .typeParamCount = classTypeParamCount,
            .scopeId = nameNode->scopeId,
        };
        for (int gi = 0; gi < classTypeParamCount && gi < 8; gi++)
        {
            cls.typeParamNames[gi] = classTypeParamNames[gi];
            cls.typeParamNameLens[gi] = classTypeParamNameLens[gi];
        }
        if (!addClassInfo(&classes, cls))
            break;
    }

    for (int i = 0; i < flow.count; i++)
    {
        const AstNode *node = flow.items[i];
        if (node == NULL || node->tokenType != TOKEN_FUN)
            continue;
        if (i + 1 >= flow.count || flow.items[i + 1]->tokenType != TOKEN_IDENTIFIER)
            continue;

        const AstNode *fnNameNode = flow.items[i + 1];
        int paramsOpen = i + 2;
        if (paramsOpen >= flow.count || flow.items[paramsOpen]->tokenType != TOKEN_LEFT_PAREN)
            continue;

        int paramDepth = 1;
        int paramsClose = -1;
        for (int k = paramsOpen + 1; k < flow.count; k++)
        {
            if (flow.items[k]->tokenType == TOKEN_LEFT_PAREN)
                paramDepth++;
            else if (flow.items[k]->tokenType == TOKEN_RIGHT_PAREN)
            {
                paramDepth--;
                if (paramDepth == 0)
                {
                    paramsClose = k;
                    break;
                }
            }
        }
        if (paramsClose < 0)
            continue;

        int afterParams = paramsClose + 1;
        TypeInfo inlineReturn = {.kind = TYPE_UNKNOWN};
        int typeTokenIndex = -1;
        if (afterParams < flow.count)
            parseInlineTypeFromFlow(&flow, afterParams, &inlineReturn, &typeTokenIndex);

        int bodyOpen = typeTokenIndex >= 0 ? typeTokenIndex + 1 : afterParams;
        if (bodyOpen >= flow.count || flow.items[bodyOpen]->tokenType != TOKEN_LEFT_BRACE)
            continue;

        int fnBodyScopeId = flow.items[bodyOpen]->scopeId;
        if (flow.items[bodyOpen]->firstChild != NULL)
            fnBodyScopeId = flow.items[bodyOpen]->firstChild->scopeId;

        if (!addFunctionCtxFromSignature(tree,
                                         &flow,
                                         i + 1,
                                         paramsOpen,
                                         paramsClose,
                                         fnBodyScopeId,
                                         inlineReturn,
                                         NULL,
                                         0,
                                         NULL,
                                         NULL,
                                         0,
                                         &bindings,
                                         &fnContexts,
                                         &paramHints))
            break;
    }

    for (int i = 0; i < flow.count; i++)
    {
        const AstNode *node = flow.items[i];
        if (node == NULL || node->tokenType != TOKEN_CLASS)
            continue;
        if (i + 1 >= flow.count || flow.items[i + 1]->tokenType != TOKEN_IDENTIFIER)
            continue;

        int classOpen = i + 2;
        const char *classTypeParamNames[8] = {0};
        int classTypeParamNameLens[8] = {0};
        int classTypeParamCount = 0;

        if (classOpen < flow.count && flow.items[classOpen]->tokenType == TOKEN_LEFT_BRACKET)
        {
            int depth = 1;
            classOpen++;
            while (classOpen < flow.count && depth > 0)
            {
                TokenType t = flow.items[classOpen]->tokenType;
                if (t == TOKEN_LEFT_BRACKET)
                    depth++;
                else if (t == TOKEN_RIGHT_BRACKET)
                    depth--;

                if (depth == 1 && t == TOKEN_IDENTIFIER && classTypeParamCount < 8)
                {
                    classTypeParamNames[classTypeParamCount] = flow.items[classOpen]->lexeme;
                    classTypeParamNameLens[classTypeParamCount] = flow.items[classOpen]->lexemeLen;
                    classTypeParamCount++;
                }
                classOpen++;
            }
        }

        if (classOpen < flow.count && flow.items[classOpen]->tokenType == TOKEN_LESS)
        {
            int depth = 1;
            classOpen++;
            while (classOpen < flow.count && depth > 0)
            {
                TokenType t = flow.items[classOpen]->tokenType;
                if (t == TOKEN_LESS)
                    depth++;
                else if (t == TOKEN_GREATER)
                    depth--;
                classOpen++;
            }
        }

        if (classOpen >= flow.count || flow.items[classOpen]->tokenType != TOKEN_LEFT_BRACE)
            continue;

        int classDepth = 1;
        int classClose = -1;
        for (int k = classOpen + 1; k < flow.count; k++)
        {
            if (flow.items[k]->tokenType == TOKEN_LEFT_BRACE)
                classDepth++;
            else if (flow.items[k]->tokenType == TOKEN_RIGHT_BRACE)
            {
                classDepth--;
                if (classDepth == 0)
                {
                    classClose = k;
                    break;
                }
            }
        }
        if (classClose < 0)
            continue;

        int memberDepth = 1;
        for (int k = classOpen + 1; k < classClose; k++)
        {
            const AstNode *member = flow.items[k];
            if (member == NULL)
                continue;

            if (member->tokenType == TOKEN_LEFT_BRACE)
            {
                memberDepth++;
                continue;
            }
            if (member->tokenType == TOKEN_RIGHT_BRACE)
            {
                memberDepth--;
                continue;
            }
            if (memberDepth != 1)
                continue;

            if (member->tokenType != TOKEN_IDENTIFIER)
                continue;
            if (k + 1 >= classClose || flow.items[k + 1]->tokenType != TOKEN_LEFT_PAREN)
                continue;

            int paramsOpen = k + 1;
            int paramDepth = 1;
            int paramsClose = -1;
            for (int m = paramsOpen + 1; m < classClose; m++)
            {
                if (flow.items[m]->tokenType == TOKEN_LEFT_PAREN)
                    paramDepth++;
                else if (flow.items[m]->tokenType == TOKEN_RIGHT_PAREN)
                {
                    paramDepth--;
                    if (paramDepth == 0)
                    {
                        paramsClose = m;
                        break;
                    }
                }
            }
            if (paramsClose < 0)
                continue;

            int afterParams = paramsClose + 1;
            TypeInfo inlineReturn = {.kind = TYPE_UNKNOWN};
            int typeTokenIndex = -1;
            if (afterParams < classClose)
                parseInlineTypeFromFlow(&flow, afterParams, &inlineReturn, &typeTokenIndex);

            int bodyOpen = typeTokenIndex >= 0 ? typeTokenIndex + 1 : afterParams;
            int methodBodyScopeId = member->scopeId;
            if (bodyOpen < classClose && flow.items[bodyOpen]->tokenType == TOKEN_LEFT_BRACE)
            {
                methodBodyScopeId = flow.items[bodyOpen]->scopeId;
                if (flow.items[bodyOpen]->firstChild != NULL)
                    methodBodyScopeId = flow.items[bodyOpen]->firstChild->scopeId;
            }

            if (!addFunctionCtxFromSignature(tree,
                                             &flow,
                                             k,
                                             paramsOpen,
                                             paramsClose,
                                             methodBodyScopeId,
                                             inlineReturn,
                                             flow.items[i + 1]->lexeme,
                                             flow.items[i + 1]->lexemeLen,
                                             classTypeParamNames,
                                             classTypeParamNameLens,
                                             classTypeParamCount,
                                             &bindings,
                                             &fnContexts,
                                             &paramHints))
                break;

            if (bodyOpen < classClose && flow.items[bodyOpen]->tokenType == TOKEN_LEFT_BRACE)
            {
                int bodyDepth = 1;
                int m = bodyOpen + 1;
                for (; m < classClose; m++)
                {
                    if (flow.items[m]->tokenType == TOKEN_LEFT_BRACE)
                        bodyDepth++;
                    else if (flow.items[m]->tokenType == TOKEN_RIGHT_BRACE)
                    {
                        bodyDepth--;
                        if (bodyDepth == 0)
                            break;
                    }
                }
                k = m;
            }
            else
            {
                k = paramsClose;
            }
        }
    }

    for (int i = 0; i < flow.count; i++)
    {
        const AstNode *node = flow.items[i];
        if (node == NULL)
            continue;

        if (node->tokenType == TOKEN_VAR || node->tokenType == TOKEN_CONST)
        {
            if (i + 1 >= flow.count)
                continue;

            const AstNode *nameNode = flow.items[i + 1];
            if (nameNode->tokenType != TOKEN_IDENTIFIER)
                continue;

            TypeInfo inlineType = {.kind = TYPE_UNKNOWN};
            int eqIndex = i + 2;
            if (i + 3 < flow.count)
            {
                TypeInfo parsed;
                int typeTokenIndex = -1;
                if (parseInlineTypeFromFlow(&flow, i + 2, &parsed, &typeTokenIndex))
                {
                    inlineType = parsed;
                    eqIndex = typeTokenIndex + 1;
                }
            }

            if (i + 3 < flow.count && flow.items[i + 2]->tokenType == TOKEN_COLON)
            {
                int k = i + 3;
                int angleDepth = 0;
                for (; k < flow.count; k++)
                {
                    TokenType t = flow.items[k]->tokenType;
                    if (t == TOKEN_LESS)
                    {
                        angleDepth++;
                        continue;
                    }
                    if (t == TOKEN_GREATER)
                    {
                        if (angleDepth > 0)
                            angleDepth--;
                        continue;
                    }
                    if (angleDepth == 0 && (t == TOKEN_EQUAL || t == TOKEN_SEMICOLON || t == TOKEN_COMMA || t == TOKEN_RIGHT_PAREN || t == TOKEN_LEFT_BRACE))
                        break;
                }

                if (k > i + 3)
                {
                    TypeInfo spanType = {.kind = TYPE_UNKNOWN};
                    if (parseTypeNameFromFlowRange(&flow, i + 3, k - 1, &spanType))
                    {
                        bool allowFallback = (inlineType.kind == TYPE_UNKNOWN);
                        if (!allowFallback && inlineType.kind == TYPE_CLASS_INSTANCE)
                        {
                            int hasGeneric = findTopLevelChar(inlineType.className, inlineType.classNameLen, '<');
                            allowFallback = hasGeneric < 0;
                        }

                        if (allowFallback && spanType.kind != TYPE_UNKNOWN)
                        {
                            inlineType = spanType;
                            eqIndex = k;
                        }
                    }
                }
            }

            TypeInfo inferred = {.kind = TYPE_UNKNOWN};
            if (eqIndex + 1 < flow.count && flow.items[eqIndex]->tokenType == TOKEN_EQUAL)
                inferred = inferNodeType(tree, &bindings, &classes, &flow, eqIndex + 1);

            if (inlineType.kind == TYPE_CLASS_INSTANCE &&
                eqIndex + 2 < flow.count &&
                flow.items[eqIndex]->tokenType == TOKEN_EQUAL &&
                flow.items[eqIndex + 1]->tokenType == TOKEN_IDENTIFIER &&
                flow.items[eqIndex + 2]->tokenType == TOKEN_LEFT_PAREN)
            {
                const AstNode *ctorNameNode = flow.items[eqIndex + 1];
                const char *expectedBase = NULL;
                int expectedBaseLen = 0;
                classTypeBaseName(inlineType.className, inlineType.classNameLen, &expectedBase, &expectedBaseLen);

                if (ctorNameNode->lexemeLen == expectedBaseLen &&
                    memcmp(ctorNameNode->lexeme, expectedBase, (size_t)expectedBaseLen) == 0)
                {
                    TypeInfo expectedTypeArgs[8] = {0};
                    int expectedTypeArgCount = 0;
                    parseClassTypeArgs(inlineType.className, inlineType.classNameLen, expectedTypeArgs, 8, &expectedTypeArgCount);

                    int initCtxIdx = findMethodCtxByOwnerAndRawName(&fnContexts, expectedBase, expectedBaseLen, "init", 4);
                    if (initCtxIdx >= 0)
                    {
                        FunctionCtx *ctx = &fnContexts.items[initCtxIdx];
                        FunctionCtx activeCtx = *ctx;

                        if (activeCtx.classTypeParamCount == 0 && expectedTypeArgCount > 0)
                        {
                            const char *fallbackNames[8] = {0};
                            int fallbackLens[8] = {0};
                            int fallbackCount = 0;
                            inferFallbackClassTypeParams(ctx, fallbackNames, fallbackLens, &fallbackCount, 8);
                            if (fallbackCount > 0)
                            {
                                int capped = fallbackCount > 8 ? 8 : fallbackCount;
                                for (int gi = 0; gi < capped; gi++)
                                {
                                    activeCtx.classTypeParamNames[gi] = fallbackNames[gi];
                                    activeCtx.classTypeParamNameLens[gi] = fallbackLens[gi];
                                }
                                activeCtx.classTypeParamCount = capped;
                            }
                        }

                        int depth = 1;
                        int argPos = 0;
                        int argStart = eqIndex + 3;
                        for (int k = eqIndex + 3; k < flow.count; k++)
                        {
                            TokenType t = flow.items[k]->tokenType;
                            if (t == TOKEN_LEFT_PAREN)
                            {
                                depth++;
                            }
                            else if (t == TOKEN_RIGHT_PAREN)
                            {
                                depth--;
                                if (depth == 0)
                                {
                                    if (argStart < k && ctx->paramCount > 0)
                                    {
                                        int checkIndex = argPos;
                                        int lastIndex = ctx->paramCount - 1;
                                        bool hasVariadic = ctx->paramVariadic[lastIndex];
                                        if (checkIndex >= ctx->paramCount)
                                        {
                                            if (!hasVariadic)
                                                checkIndex = -1;
                                            else
                                                checkIndex = lastIndex;
                                        }

                                        if (checkIndex >= 0)
                                        {
                                            TypeInfo expected = activeCtx.paramTypes[checkIndex];
                                            expected = substituteClassTypeParams(expected, &activeCtx, expectedTypeArgs, expectedTypeArgCount);
                                            if (!hasUnboundClassTypeParam(expected, &activeCtx) && expected.kind != TYPE_UNKNOWN)
                                            {
                                                TypeInfo actual = inferNodeType(tree, &bindings, &classes, &flow, argStart);
                                                bool mismatch = false;
                                                if (actual.kind != TYPE_UNKNOWN)
                                                    mismatch = !typeInfoCompatible(&classes, expected, actual);
                                                if (mismatch)
                                                {
                                                    char expectedType[64] = {0};
                                                    char actualType[64] = {0};
                                                    formatTypeInfo(expectedType, sizeof(expectedType), expected);
                                                    formatTypeInfo(actualType, sizeof(actualType), actual);
                                                    fprintf(out,
                                                            "%s:%d:%d %s: constructor argument %d for '%.*s' expected %s, got %s\n",
                                                            file,
                                                            ctorNameNode->line,
                                                            ctorNameNode->col,
                                                            strictMode ? "TypeError" : "TypeWarning",
                                                            argPos + 1,
                                                            expectedBaseLen,
                                                            expectedBase,
                                                            expectedType,
                                                            actualType);
                                                    issues++;
                                                }
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                            else if (t == TOKEN_COMMA && depth == 1)
                            {
                                if (argStart < k && ctx->paramCount > 0)
                                {
                                    int checkIndex = argPos;
                                    int lastIndex = ctx->paramCount - 1;
                                    bool hasVariadic = ctx->paramVariadic[lastIndex];
                                    if (checkIndex >= ctx->paramCount)
                                    {
                                        if (!hasVariadic)
                                            checkIndex = -1;
                                        else
                                            checkIndex = lastIndex;
                                    }

                                    if (checkIndex >= 0)
                                    {
                                        TypeInfo expected = activeCtx.paramTypes[checkIndex];
                                        expected = substituteClassTypeParams(expected, &activeCtx, expectedTypeArgs, expectedTypeArgCount);
                                        if (!hasUnboundClassTypeParam(expected, &activeCtx) && expected.kind != TYPE_UNKNOWN)
                                        {
                                            TypeInfo actual = inferNodeType(tree, &bindings, &classes, &flow, argStart);
                                            bool mismatch = false;
                                            if (actual.kind != TYPE_UNKNOWN)
                                                mismatch = !typeInfoCompatible(&classes, expected, actual);
                                            if (mismatch)
                                            {
                                                char expectedType[64] = {0};
                                                char actualType[64] = {0};
                                                formatTypeInfo(expectedType, sizeof(expectedType), expected);
                                                formatTypeInfo(actualType, sizeof(actualType), actual);
                                                fprintf(out,
                                                        "%s:%d:%d %s: constructor argument %d for '%.*s' expected %s, got %s\n",
                                                        file,
                                                        ctorNameNode->line,
                                                        ctorNameNode->col,
                                                        strictMode ? "TypeError" : "TypeWarning",
                                                        argPos + 1,
                                                        expectedBaseLen,
                                                        expectedBase,
                                                        expectedType,
                                                        actualType);
                                                issues++;
                                            }
                                        }
                                    }
                                }

                                argPos++;
                                argStart = k + 1;
                            }
                        }
                    }
                }
            }

            TypeBinding binding = {
                .name = nameNode->lexeme,
                .nameLen = nameNode->lexemeLen,
                .scopeId = nameNode->scopeId,
                .isConst = node->tokenType == TOKEN_CONST,
                .type = inlineType.kind != TYPE_UNKNOWN ? inlineType : inferred,
            };

            int hintIdx = findHintIndex(&hints, nameNode);
            if (hintIdx >= 0 && inlineType.kind == TYPE_UNKNOWN)
                binding.type = hints.items[hintIdx].hintedType;

            if (!addBinding(&bindings, binding))
                break;
            continue;
        }

        if (node->tokenType == TOKEN_IDENTIFIER && i + 2 < flow.count && flow.items[i + 1]->tokenType == TOKEN_EQUAL)
        {
            const AstNode *prev = i > 0 ? flow.items[i - 1] : NULL;
            if (prev != NULL && (prev->tokenType == TOKEN_VAR || prev->tokenType == TOKEN_CONST || prev->tokenType == TOKEN_AT || prev->tokenType == TOKEN_DOT))
                continue;

            int bindingIdx = findBindingIndex(tree, &bindings, node);
            if (bindingIdx >= 0)
            {
                TypeBinding *binding = &bindings.items[bindingIdx];
                TypeInfo rhs = inferNodeType(tree, &bindings, &classes, &flow, i + 2);

                if (binding->isConst)
                {
                    fprintf(out,
                            "%s:%d:%d %s: assignment to const '%.*s'\n",
                            file,
                            node->line,
                            node->col,
                            strictMode ? "TypeError" : "TypeWarning",
                            node->lexemeLen,
                            node->lexeme);
                    issues++;
                    continue;
                }

                bool mismatch = false;
                if (binding->type.kind != TYPE_UNKNOWN && rhs.kind != TYPE_UNKNOWN)
                {
                    mismatch = !typeInfoCompatible(&classes, binding->type, rhs);
                }

                if (mismatch)
                {
                    char expectedType[64] = {0};
                    char actualType[64] = {0};
                    formatTypeInfo(expectedType, sizeof(expectedType), binding->type);
                    formatTypeInfo(actualType, sizeof(actualType), rhs);
                    fprintf(out,
                            "%s:%d:%d %s: type mismatch for '%.*s': expected %s, got %s\n",
                            file,
                            node->line,
                            node->col,
                            strictMode ? "TypeError" : "TypeWarning",
                            node->lexemeLen,
                            node->lexeme,
                            expectedType,
                            actualType);
                    issues++;
                }
                else if (binding->type.kind == TYPE_UNKNOWN && rhs.kind != TYPE_UNKNOWN)
                {
                    binding->type = rhs;
                }
            }
            continue;
        }

        if (node->tokenType == TOKEN_IDENTIFIER &&
            i + 3 < flow.count &&
            flow.items[i + 1]->tokenType == TOKEN_DOT &&
            flow.items[i + 2]->tokenType == TOKEN_IDENTIFIER &&
            flow.items[i + 3]->tokenType == TOKEN_LEFT_PAREN)
        {
            TypeInfo receiver = inferNodeType(tree, &bindings, &classes, &flow, i);
            if (receiver.kind == TYPE_CLASS_INSTANCE)
            {
                const char *receiverBase = NULL;
                int receiverBaseLen = 0;
                classTypeBaseName(receiver.className, receiver.classNameLen, &receiverBase, &receiverBaseLen);

                int methodCtxIdx = findMethodCtxByOwnerAndName(&fnContexts,
                                                               receiverBase,
                                                               receiverBaseLen,
                                                               flow.items[i + 2]);
                if (methodCtxIdx < 0)
                    methodCtxIdx = findFunctionCtxByName(&fnContexts, flow.items[i + 2]);
                if (methodCtxIdx >= 0)
                {
                    FunctionCtx *ctx = &fnContexts.items[methodCtxIdx];
                    TypeInfo receiverTypeArgs[8] = {0};
                    int receiverTypeArgCount = 0;
                    parseClassTypeArgs(receiver.className, receiver.classNameLen, receiverTypeArgs, 8, &receiverTypeArgCount);

                    FunctionCtx activeCtx = *ctx;
                    if (activeCtx.classTypeParamCount == 0 && receiverTypeArgCount > 0)
                    {
                        const char *fallbackNames[8] = {0};
                        int fallbackLens[8] = {0};
                        int fallbackCount = 0;
                        inferFallbackClassTypeParams(ctx, fallbackNames, fallbackLens, &fallbackCount, 8);
                        if (fallbackCount > 0)
                        {
                            int capped = fallbackCount > 8 ? 8 : fallbackCount;
                            for (int gi = 0; gi < capped; gi++)
                            {
                                activeCtx.classTypeParamNames[gi] = fallbackNames[gi];
                                activeCtx.classTypeParamNameLens[gi] = fallbackLens[gi];
                            }
                            activeCtx.classTypeParamCount = capped;
                        }
                    }

                    int depth = 1;
                    int argPos = 0;
                    int argStart = i + 4;
                    int closeIdx = -1;
                    for (int k = i + 4; k < flow.count; k++)
                    {
                        TokenType t = flow.items[k]->tokenType;
                        if (t == TOKEN_LEFT_PAREN)
                        {
                            depth++;
                        }
                        else if (t == TOKEN_RIGHT_PAREN)
                        {
                            depth--;
                            if (depth == 0)
                            {
                                closeIdx = k;
                                if (argStart < k && ctx->paramCount > 0)
                                {
                                    int checkIndex = argPos;
                                    int lastIndex = ctx->paramCount - 1;
                                    bool hasVariadic = ctx->paramVariadic[lastIndex];
                                    if (checkIndex >= ctx->paramCount)
                                    {
                                        if (!hasVariadic)
                                            checkIndex = -1;
                                        else
                                            checkIndex = lastIndex;
                                    }

                                    if (checkIndex >= 0)
                                    {
                                        TypeInfo expected = activeCtx.paramTypes[checkIndex];
                                        expected = substituteClassTypeParams(expected, &activeCtx, receiverTypeArgs, receiverTypeArgCount);

                                        if (hasUnboundClassTypeParam(expected, &activeCtx))
                                            continue;

                                        if (expected.kind != TYPE_UNKNOWN)
                                        {
                                            TypeInfo actual = inferNodeType(tree, &bindings, &classes, &flow, argStart);
                                            bool mismatch = false;
                                            if (actual.kind != TYPE_UNKNOWN)
                                                mismatch = !typeInfoCompatible(&classes, expected, actual);
                                            if (mismatch)
                                            {
                                                char expectedType[64] = {0};
                                                char actualType[64] = {0};
                                                formatTypeInfo(expectedType, sizeof(expectedType), expected);
                                                formatTypeInfo(actualType, sizeof(actualType), actual);
                                                fprintf(out,
                                                        "%s:%d:%d %s: argument %d for method '%.*s.%.*s' expected %s, got %s\n",
                                                        file,
                                                        node->line,
                                                        node->col,
                                                        strictMode ? "TypeError" : "TypeWarning",
                                                        argPos + 1,
                                                        receiverBaseLen,
                                                        receiverBase,
                                                        flow.items[i + 2]->lexemeLen,
                                                        flow.items[i + 2]->lexeme,
                                                        expectedType,
                                                        actualType);
                                                issues++;
                                            }
                                        }
                                    }
                                }
                                break;
                            }
                        }
                        else if (t == TOKEN_COMMA && depth == 1)
                        {
                            if (argStart < k && ctx->paramCount > 0)
                            {
                                int checkIndex = argPos;
                                int lastIndex = ctx->paramCount - 1;
                                bool hasVariadic = ctx->paramVariadic[lastIndex];
                                if (checkIndex >= ctx->paramCount)
                                {
                                    if (!hasVariadic)
                                        checkIndex = -1;
                                    else
                                        checkIndex = lastIndex;
                                }

                                if (checkIndex >= 0)
                                {
                                    TypeInfo expected = activeCtx.paramTypes[checkIndex];
                                    expected = substituteClassTypeParams(expected, &activeCtx, receiverTypeArgs, receiverTypeArgCount);
                                    if (hasUnboundClassTypeParam(expected, &activeCtx))
                                        continue;
                                    if (expected.kind != TYPE_UNKNOWN)
                                    {
                                        TypeInfo actual = inferNodeType(tree, &bindings, &classes, &flow, argStart);
                                        bool mismatch = false;
                                        if (actual.kind != TYPE_UNKNOWN)
                                            mismatch = !typeInfoCompatible(&classes, expected, actual);
                                        if (mismatch)
                                        {
                                            char expectedType[64] = {0};
                                            char actualType[64] = {0};
                                            formatTypeInfo(expectedType, sizeof(expectedType), expected);
                                            formatTypeInfo(actualType, sizeof(actualType), actual);
                                            fprintf(out,
                                                    "%s:%d:%d %s: argument %d for method '%.*s.%.*s' expected %s, got %s\n",
                                                    file,
                                                    node->line,
                                                    node->col,
                                                    strictMode ? "TypeError" : "TypeWarning",
                                                    argPos + 1,
                                                    receiverBaseLen,
                                                    receiverBase,
                                                    flow.items[i + 2]->lexemeLen,
                                                    flow.items[i + 2]->lexeme,
                                                    expectedType,
                                                    actualType);
                                            issues++;
                                        }
                                    }
                                }
                            }

                            argPos++;
                            argStart = k + 1;
                        }
                    }

                    if (closeIdx > i)
                        i = closeIdx;
                    continue;
                }
            }
        }

        if (node->tokenType == TOKEN_IDENTIFIER && i + 1 < flow.count && flow.items[i + 1]->tokenType == TOKEN_LEFT_PAREN)
        {
            const AstNode *prev = i > 0 ? flow.items[i - 1] : NULL;
            if (prev != NULL && (prev->tokenType == TOKEN_FUN || prev->tokenType == TOKEN_DOT))
                goto skip_call_check;
            if (findClassIndex(&classes, node->lexeme, node->lexemeLen) >= 0)
                goto skip_call_check;

            int fnIdx = findFunctionCtxByName(&fnContexts, node);
            if (fnIdx >= 0)
            {
                FunctionCtx *ctx = &fnContexts.items[fnIdx];

                int depth = 1;
                int argPos = 0;
                int argStart = i + 2;
                int closeIdx = -1;
                for (int k = i + 2; k < flow.count; k++)
                {
                    TokenType t = flow.items[k]->tokenType;
                    if (t == TOKEN_LEFT_PAREN)
                    {
                        depth++;
                    }
                    else if (t == TOKEN_RIGHT_PAREN)
                    {
                        depth--;
                        if (depth == 0)
                        {
                            closeIdx = k;
                            if (argStart < k && ctx->paramCount > 0)
                            {
                                int checkIndex = argPos;
                                int lastIndex = ctx->paramCount - 1;
                                bool hasVariadic = ctx->paramVariadic[lastIndex];
                                if (checkIndex >= ctx->paramCount)
                                {
                                    if (!hasVariadic)
                                        checkIndex = -1;
                                    else
                                        checkIndex = lastIndex;
                                }

                                if (checkIndex >= 0)
                                {
                                    const AstNode *paramNode = ctx->params[checkIndex];
                                    TypeInfo expected = ctx->paramTypes[checkIndex];
                                    if (expected.kind == TYPE_UNKNOWN)
                                    {
                                        int pHintIdx = findParamHintIndex(&paramHints, ctx->fnName, ctx->fnNameLen, paramNode);
                                        if (pHintIdx >= 0)
                                            expected = paramHints.items[pHintIdx].hintedType;
                                    }

                                    if (expected.kind != TYPE_UNKNOWN)
                                    {
                                        TypeInfo actual = inferNodeType(tree, &bindings, &classes, &flow, argStart);
                                        bool mismatch = false;
                                        if (actual.kind != TYPE_UNKNOWN)
                                            mismatch = !typeInfoCompatible(&classes, expected, actual);
                                        if (mismatch)
                                        {
                                            char expectedType[64] = {0};
                                            char actualType[64] = {0};
                                            formatTypeInfo(expectedType, sizeof(expectedType), expected);
                                            formatTypeInfo(actualType, sizeof(actualType), actual);
                                            fprintf(out,
                                                    "%s:%d:%d %s: argument %d for '%.*s' expected %s, got %s\n",
                                                    file,
                                                    node->line,
                                                    node->col,
                                                    strictMode ? "TypeError" : "TypeWarning",
                                                    argPos + 1,
                                                    ctx->fnNameLen,
                                                    ctx->fnName,
                                                    expectedType,
                                                    actualType);
                                            issues++;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                    }
                    else if (t == TOKEN_COMMA && depth == 1)
                    {
                        if (argStart < k && ctx->paramCount > 0)
                        {
                            int checkIndex = argPos;
                            int lastIndex = ctx->paramCount - 1;
                            bool hasVariadic = ctx->paramVariadic[lastIndex];
                            if (checkIndex >= ctx->paramCount)
                            {
                                if (!hasVariadic)
                                    checkIndex = -1;
                                else
                                    checkIndex = lastIndex;
                            }

                            if (checkIndex >= 0)
                            {
                                const AstNode *paramNode = ctx->params[checkIndex];
                                TypeInfo expected = ctx->paramTypes[checkIndex];
                                if (expected.kind == TYPE_UNKNOWN)
                                {
                                    int pHintIdx = findParamHintIndex(&paramHints, ctx->fnName, ctx->fnNameLen, paramNode);
                                    if (pHintIdx >= 0)
                                        expected = paramHints.items[pHintIdx].hintedType;
                                }
                                if (expected.kind != TYPE_UNKNOWN)
                                {
                                    TypeInfo actual = inferNodeType(tree, &bindings, &classes, &flow, argStart);
                                    bool mismatch = false;
                                    if (actual.kind != TYPE_UNKNOWN)
                                        mismatch = !typeInfoCompatible(&classes, expected, actual);
                                    if (mismatch)
                                    {
                                        char expectedType[64] = {0};
                                        char actualType[64] = {0};
                                        formatTypeInfo(expectedType, sizeof(expectedType), expected);
                                        formatTypeInfo(actualType, sizeof(actualType), actual);
                                        fprintf(out,
                                                "%s:%d:%d %s: argument %d for '%.*s' expected %s, got %s\n",
                                                file,
                                                node->line,
                                                node->col,
                                                strictMode ? "TypeError" : "TypeWarning",
                                                argPos + 1,
                                                ctx->fnNameLen,
                                                ctx->fnName,
                                                expectedType,
                                                actualType);
                                        issues++;
                                    }
                                }
                            }
                        }

                        argPos++;
                        argStart = k + 1;
                    }
                }

                if (closeIdx > i)
                    i = closeIdx;
            }
        }
    skip_call_check:

        if (isNumericOperator(node->tokenType) && i > 0 && i + 1 < flow.count)
        {
            if (isInheritanceAngleToken(&flow, i))
                continue;
            if (isTypeAnnotationAngleToken(&flow, i))
                continue;

            TypeInfo left = inferNodeType(tree, &bindings, &classes, &flow, i - 1);
            TypeInfo right = inferNodeType(tree, &bindings, &classes, &flow, i + 1);

            if ((left.kind != TYPE_UNKNOWN && left.kind != TYPE_NUMBER) || (right.kind != TYPE_UNKNOWN && right.kind != TYPE_NUMBER))
            {
                char leftType[64] = {0};
                char rightType[64] = {0};
                formatTypeInfo(leftType, sizeof(leftType), left);
                formatTypeInfo(rightType, sizeof(rightType), right);
                fprintf(out,
                        "%s:%d:%d %s: numeric operator '%.*s' used with %s and %s\n",
                        file,
                        node->line,
                        node->col,
                        strictMode ? "TypeError" : "TypeWarning",
                        node->lexemeLen,
                        node->lexeme,
                        leftType,
                        rightType);
                issues++;
            }
        }

        if (node->tokenType == TOKEN_RETURN)
        {
            int fnCtxIdx = -1;
            int bestDepth = -1;
            for (int j = 0; j < fnContexts.count; j++)
            {
                if (!scopeVisible(tree, fnContexts.items[j].bodyScopeId, node->scopeId))
                    continue;
                int d = tree->scopes.items[fnContexts.items[j].bodyScopeId].depth;
                if (d > bestDepth)
                {
                    bestDepth = d;
                    fnCtxIdx = j;
                }
            }

            if (fnCtxIdx < 0)
                continue;

            FunctionCtx *ctx = &fnContexts.items[fnCtxIdx];
            TypeInfo expected = ctx->returnType;
            if (expected.kind == TYPE_UNKNOWN)
            {
                int retHintIdx = findReturnHintIndex(&returnHints, ctx->fnName, ctx->fnNameLen);
                if (retHintIdx >= 0)
                    expected = returnHints.items[retHintIdx].hintedType;
            }
            if (expected.kind == TYPE_UNKNOWN)
                continue;

            if (hasUnboundClassTypeParam(expected, ctx))
                continue;

            TypeInfo actual = {.kind = TYPE_NIL};
            if (i + 1 < flow.count)
            {
                TokenType next = flow.items[i + 1]->tokenType;
                if (next != TOKEN_SEMICOLON && next != TOKEN_RIGHT_BRACE)
                    actual = inferNodeType(tree, &bindings, &classes, &flow, i + 1);
            }

            bool mismatch = false;
            if (expected.kind != TYPE_UNKNOWN && actual.kind != TYPE_UNKNOWN)
                mismatch = !typeInfoCompatible(&classes, expected, actual);

            if (mismatch)
            {
                char expectedType[64] = {0};
                char actualType[64] = {0};
                formatTypeInfo(expectedType, sizeof(expectedType), expected);
                formatTypeInfo(actualType, sizeof(actualType), actual);
                fprintf(out,
                        "%s:%d:%d %s: return type mismatch in '%.*s': expected %s, got %s\n",
                        file,
                        node->line,
                        node->col,
                        strictMode ? "TypeError" : "TypeWarning",
                        ctx->fnNameLen,
                        ctx->fnName,
                        expectedType,
                        actualType);
                issues++;
            }
        }
    }

    if (issues > 0)
        fprintf(out, "%s:0:0 %s: type checker found %d issue(s)\n", file, strictMode ? "TypeError" : "TypeWarning", issues);

    free(flow.items);
    free(bindings.items);
    free(classes.items);
    free(hints.items);
    free(paramHints.items);
    free(returnHints.items);
    free(fnContexts.items);
    return issues;
}

void astFreeTree(AstTree *tree)
{
    if (tree == NULL)
        return;
    free(tree->scopes.items);
    free(tree->decls.items);
    free(tree->symbols.items);
    arenaFree(&tree->arena);
    free(tree);
}

bool astDumpFromSource(const char *source, const char *file, FILE *out, char *errBuf, size_t errBufSize)
{
    AstTree *tree = astBuildFromSource(source, file, errBuf, errBufSize);
    if (tree == NULL)
        return false;

    astDumpTree(tree, out);
    astFreeTree(tree);
    return true;
}

bool astDumpSemanticFromSource(const char *source, const char *file, FILE *out, char *errBuf, size_t errBufSize)
{
    AstTree *tree = astBuildFromSource(source, file, errBuf, errBufSize);
    if (tree == NULL)
        return false;

    astDumpSemanticSummary(tree, out);
    astFreeTree(tree);
    return true;
}

bool astDumpSemanticJsonFromSource(const char *source, const char *file, FILE *out, char *errBuf, size_t errBufSize)
{
    AstTree *tree = astBuildFromSource(source, file, errBuf, errBufSize);
    if (tree == NULL)
        return false;

    astDumpSemanticJson(tree, out);
    astFreeTree(tree);
    return true;
}

bool astRunCompilePrepass(const char *source,
                          const char *file,
                          AstTypeCheckMode mode,
                          FILE *diagOut,
                          int *issueCount,
                          char *errBuf,
                          size_t errBufSize)
{
    if (issueCount != NULL)
        *issueCount = 0;

    AstTree *tree = astBuildFromSource(source, file, errBuf, errBufSize);
    if (tree == NULL)
        return false;

    if (mode != AST_TYPECHECK_OFF)
    {
        int issues = runTypeCheck(tree, source, file, mode == AST_TYPECHECK_STRICT, diagOut == NULL ? stderr : diagOut);
        if (issueCount != NULL)
            *issueCount = issues;
    }

    astFreeTree(tree);
    return true;
}