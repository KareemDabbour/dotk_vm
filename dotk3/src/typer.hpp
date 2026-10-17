#pragma once
#include "syntax.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dk3 {

struct Diagnostic {
    Span span;
    std::string message;
    bool warning = false;
};

// Type-checks a parsed program against the core library and annotates expression
// nodes with their resolved types. Returns every diagnostic found; empty means well-typed.
// The loader supplies the source of imported user modules (it may throw std::exception).
using ModuleLoader = std::function<std::shared_ptr<const Source>(const std::string &)>;
std::vector<Diagnostic> typeCheck(const Ast &program, const ModuleLoader &loader = nullptr, std::map<std::string, Ast> *modules = nullptr);
// Incremental checking for the REPL: declarations persist between submissions and may shadow earlier ones.
class TypeSession {
public:
    explicit TypeSession(ModuleLoader loader);
    ~TypeSession();
    std::vector<Diagnostic> check(const Ast &program, bool allowRedeclaration = true);
    Ast module(const std::string &name) const;
    // Declares a host native; `signature` is the declaration tail, e.g. "(a: Int) -> Int". `name` may be "namespace.member".
    bool declareModule(const std::string &name, const std::vector<std::pair<std::string, std::string>> &exports, std::string &problem);
    bool declareNative(const std::string &name, const std::string &signature, std::string &problem);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::string formatDiagnostic(const Diagnostic &diagnostic, bool color = false);

}
