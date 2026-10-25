#pragma once
#include "native_abi.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dk3
{
    struct NativeReport
    {
        std::vector<std::string> compiled;
        // Functions that were requested (or eligible by signature) but stay interpreted, with the reason.
        std::vector<std::pair<Ast, std::string>> interpreted;
    };

    // Program data embedded next to ahead-of-time code.
    struct EmbeddedSource
    {
        std::string file, source;
        std::vector<std::string> options;
        std::vector<std::pair<std::string, std::string>> modules; // relative name, source text
    };

    // Compiles typed top-level functions to machine code and attaches them to their AST nodes.
    class NativeTier
    {
    public:
        NativeTier();
        ~NativeTier();
        static bool available();
        // `all` compiles every eligible function; otherwise only @jit ones. @nojit functions are never compiled.
        NativeReport compile(const Ast &program, bool all);
        // Writes a host object file with every eligible function plus the tables defining dk3_embedded_program.
        NativeReport emitObject(const Ast &program, const EmbeddedSource &embedded, const std::string &path);
        // Later compiles also write their optimized LLVM IR to `path` (appended across modules).
        void setIRPath(const std::string &path);

    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
