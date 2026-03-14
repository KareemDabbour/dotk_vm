#ifndef dotk_ast_h
#define dotk_ast_h

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "scanner.h"

typedef enum
{
    AST_NODE_ROOT,
    AST_NODE_GROUP,
    AST_NODE_TOKEN
} AstNodeKind;

typedef enum
{
    AST_DECL_VAR,
    AST_DECL_CONST,
    AST_DECL_FUNCTION,
    AST_DECL_CLASS,
    AST_DECL_MODULE,
    AST_DECL_IMPORT,
    AST_DECL_FROM_IMPORT,
    AST_DECL_EXPORT
} AstDeclKind;

typedef enum
{
    AST_SYMBOL_VAR,
    AST_SYMBOL_CONST,
    AST_SYMBOL_FUNCTION,
    AST_SYMBOL_CLASS,
    AST_SYMBOL_MODULE,
    AST_SYMBOL_PARAM
} AstSymbolKind;

typedef struct _AstNode
{
    AstNodeKind kind;
    TokenType tokenType;
    int scopeId;
    const char *lexeme;
    int lexemeLen;
    int line;
    int col;
    struct _AstNode *firstChild;
    struct _AstNode *lastChild;
    struct _AstNode *nextSibling;
} AstNode;

typedef struct
{
    int id;
    int parentId;
    int depth;
    int startLine;
    int startCol;
    int endLine;
    int endCol;
} AstScope;

typedef struct
{
    AstDeclKind kind;
    const char *name;
    int nameLen;
    int line;
    int col;
    int scopeId;
} AstDecl;

typedef struct
{
    AstSymbolKind kind;
    const char *name;
    int nameLen;
    int line;
    int col;
    int scopeId;
    bool isMutable;
} AstSymbol;

typedef struct _AstTree AstTree;

typedef enum
{
    AST_TYPECHECK_OFF = 0,
    AST_TYPECHECK_WARN = 1,
    AST_TYPECHECK_STRICT = 2,
} AstTypeCheckMode;

AstTree *astBuildFromSource(const char *source, const char *file, char *errBuf, size_t errBufSize);
void astDumpTree(const AstTree *tree, FILE *out);
void astDumpSemanticSummary(const AstTree *tree, FILE *out);
void astDumpSemanticJson(const AstTree *tree, FILE *out);
void astFreeTree(AstTree *tree);

bool astDumpFromSource(const char *source, const char *file, FILE *out, char *errBuf, size_t errBufSize);
bool astDumpSemanticFromSource(const char *source, const char *file, FILE *out, char *errBuf, size_t errBufSize);
bool astDumpSemanticJsonFromSource(const char *source, const char *file, FILE *out, char *errBuf, size_t errBufSize);
bool astRunCompilePrepass(const char *source,
                          const char *file,
                          AstTypeCheckMode mode,
                          FILE *diagOut,
                          int *issueCount,
                          char *errBuf,
                          size_t errBufSize);

#endif