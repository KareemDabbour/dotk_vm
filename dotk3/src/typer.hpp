#pragma once
#include "syntax.hpp"
#include <functional>
#include <string>
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
std::vector<Diagnostic> typeCheck(const Ast &program, const ModuleLoader &loader = nullptr);
std::string formatDiagnostic(const Diagnostic &diagnostic, bool color = false);

}
