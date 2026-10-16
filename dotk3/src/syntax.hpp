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
struct Parameter {
    std::string name;
    std::string type;
    Ast defaultValue;
    bool rest = false;
    bool mutableBinding = false;
    Visibility visibility = Visibility::Public;
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
    Visibility visibility = Visibility::Public;
    std::string baseClass;
    std::vector<Ast> children;
    std::vector<std::string> names;
    std::vector<Parameter> parameters;
};
Ast parse(std::shared_ptr<const Source> source);
std::vector<std::string> typeArguments(const std::string &type);
std::vector<std::string> typeIntersections(const std::string &type);

}