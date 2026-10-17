#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dk3 {

struct Source {
    std::string file;
    std::string text;
};

struct Span {
    std::shared_ptr<const Source> source;
    size_t offset = 0;
    size_t length = 0;
    int line = 1;
    int column = 1;
};

struct ErrorFrame { std::string name; Span span; };

struct Error : std::runtime_error {
    Span span;
    std::string category;
    bool incomplete;
    std::vector<ErrorFrame> trace;
    Error(Span location, std::string message, std::string kind = "syntax", bool needsMore = false)
        : std::runtime_error(std::move(message)), span(std::move(location)), category(std::move(kind)),
          incomplete(needsMore || (category == "syntax" && span.source && span.offset >= span.source->text.size())) {}
};

enum class TokenKind { End, Newline, Name, Integer, Float, String, Bytes, Symbol, FormatString };

struct Token {
    TokenKind kind;
    std::string text;
    Span span;
};

std::vector<Token> lex(std::shared_ptr<const Source> source);
std::vector<size_t> utf8Offsets(const std::string &text, const Span &span);
std::string diagnose(const Error &error, bool color = false);

struct Node;
using Ast = std::shared_ptr<Node>;
enum class Visibility { Public, Protected, Private };
struct TypeData;

enum class TypeKind { Named, Nullable, Union, Intersection, Function, Wildcard };
enum class WildcardBound { None, Extends, Super };
// Named: name + items (type arguments). Nullable: inner. Union/Intersection: items.
// Function: items (parameters) + inner (result). Wildcard: bound + inner (the bound type, if any).
struct TypeExpr;
using TypeAst = std::shared_ptr<const TypeExpr>;
struct TypeExpr {
    TypeKind kind = TypeKind::Named;
    std::string name;
    std::vector<TypeAst> items;
    TypeAst inner;
    WildcardBound bound = WildcardBound::None;
    Span span;
};
struct TypeParam {
    std::string name;
    TypeAst bound;
};

struct Parameter {
    std::string name;
    std::string type;
    Ast defaultValue;
    bool rest = false;
    bool mutableBinding = false;
    Visibility visibility = Visibility::Public;
    TypeAst typeExpr = nullptr;
};
struct Node {
    std::string kind;
    std::string text;
    std::string type;
    std::string documentation;
    Span span;
    TokenKind literalKind = TokenKind::Symbol;
    size_t endOffset = 0;
    bool mutableBinding = false;
    bool exported = false;
    bool staticMember = false;
    bool builtinCollection = false;
    bool constructor = false;
    bool needsScope = true;
    bool safe = false;
    bool widen = false;
    // Set by the typer on numeric operations: 1 = both operands Int, 2 = both operands Float (after widening).
    unsigned char numeric = 0;
    // Qualified name of the core native this call statically resolves to (e.g. "Math.sin").
    std::string target;
    const TypeData *resolved = nullptr;
    mutable signed char yieldCache = -1;  // runtime memo: does this function body contain yield
    Visibility visibility = Visibility::Public;
    std::string baseClass;
    TypeAst typeExpr;
    std::vector<TypeAst> typeArgs;
    std::vector<TypeAst> typeExprs;
    std::vector<TypeParam> typeParams;
    std::vector<Ast> children;
    std::vector<std::string> names;
    std::vector<Parameter> parameters;
};
Ast parse(std::shared_ptr<const Source> source);
Ast parseStub(std::shared_ptr<const Source> source);
std::string typeText(const TypeAst &type);
std::vector<std::string> typeArguments(const std::string &type);
std::vector<std::string> typeIntersections(const std::string &type);

}