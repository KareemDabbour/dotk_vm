#pragma once
#include "syntax.hpp"
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
std::vector<Diagnostic> typeCheck(const Ast &program);
std::string formatDiagnostic(const Diagnostic &diagnostic, bool color = false);

}
