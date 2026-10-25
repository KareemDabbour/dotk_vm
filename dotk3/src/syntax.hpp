#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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
// ASCII is always valid UTF-8 with one byte per code point.
inline bool asciiText(const std::string &text)
{
    for (unsigned char byte : text)
        if (byte >= 0x80)
            return false;
    return true;
}
std::string diagnose(const Error &error, bool color = false);

struct Node;
using Ast = std::shared_ptr<Node>;
enum class Visibility { Public, Protected, Private };
struct TypeData;
struct NativeFunction;

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

// `@name`, `@a.b` or `@name(args)`; expression is the name/member path, or a call of it when arguments are given.
struct Annotation {
    std::string name;
    Ast expression;
    Span span;
    bool builtin = false;
};
inline bool builtinAnnotation(const std::string &name)
{
    for (const char *directive : {"jit", "nojit", "inline", "override", "deprecated", "test", "pure"})
        if (name == directive)
            return true;
    return false;
}
inline const Annotation *findAnnotation(const std::vector<Annotation> &annotations, const std::string &name)
{
    for (const auto &annotation : annotations)
        if (annotation.name == name)
            return &annotation;
    return nullptr;
}

struct Parameter {
    std::string name;
    std::string type;
    Ast defaultValue;
    bool rest = false;
    bool mutableBinding = false;
    Visibility visibility = Visibility::Public;
    TypeAst typeExpr = nullptr;
    std::vector<Annotation> annotations = {};
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
    // Set by the native tier for compiled top-level functions; owned by that tier.
    mutable const NativeFunction *native = nullptr;
    Visibility visibility = Visibility::Public;
    std::string baseClass;
    TypeAst typeExpr;
    std::vector<TypeAst> typeArgs;
    std::vector<TypeAst> typeExprs;
    std::vector<TypeParam> typeParams;
    std::vector<Ast> children;
    std::vector<std::string> names;
    std::vector<Parameter> parameters;
    std::vector<Annotation> annotations;
};
Ast parse(std::shared_ptr<const Source> source);
Ast parseStub(std::shared_ptr<const Source> source);
std::string typeText(const TypeAst &type);
std::vector<std::string> typeArguments(const std::string &type);
std::vector<std::string> typeIntersections(const std::string &type);

// Operator of a compound assignment: "+=" -> "+", "<<=" -> "<<".
inline std::string compoundOperator(const std::string &assignment) { return assignment.substr(0, assignment.size() - 1); }

// Int shift by a non-negative count; counts of 64 or more shift every bit out (sign-filling for >>).
inline int64_t shiftInt(int64_t value, int64_t count, bool left) {
    if (left)
        return count >= 64 ? 0 : static_cast<int64_t>(static_cast<uint64_t>(value) << count);
    return value >> (count >= 64 ? 63 : count);
}

// Magnitude of an Int literal (decimal, 0x or 0b, with _ separators); nullopt when malformed or above 2^63.
inline std::optional<uint64_t> integerLiteralValue(const std::string &text) {
    std::string digits;
    for (char byte : text)
        if (byte != '_')
            digits += byte;
    unsigned base = 10;
    size_t position = 0;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'b')) {
        base = digits[1] == 'x' ? 16 : 2;
        position = 2;
    }
    if (position == digits.size())
        return std::nullopt;
    uint64_t result = 0;
    for (; position < digits.size(); position++) {
        char byte = digits[position];
        unsigned digit = byte >= '0' && byte <= '9' ? byte - '0' : byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : byte >= 'A' && byte <= 'F' ? byte - 'A' + 10 : 99;
        if (digit >= base || result > ((uint64_t{1} << 63) - digit) / base)
            return std::nullopt;
        result = result * base + digit;
    }
    return result;
}

}