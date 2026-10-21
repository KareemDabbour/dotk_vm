#pragma once
#include "bytecode.hpp"
#include "syntax.hpp"
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace dk3
{

    // Key identifying a compiled function: (source_file, line, col, hasSelf).
    struct AotKey
    {
        std::string file;
        uint32_t line = 0, col = 0;
        bool hasSelf = false;
        bool operator<(const AotKey &o) const
        {
            return std::tie(file, line, col, hasSelf) < std::tie(o.file, o.line, o.col, o.hasSelf);
        }
    };

    // Map from function identity → pre-compiled Bytecode.
    using AotBytecodes = std::map<AotKey, std::shared_ptr<const Bytecode>>;

    // Walk the AST and eagerly compile every named function / class method to bytecode.
    AotBytecodes compileAllBytecodes(const Ast &program, unsigned optimization);

    // Serialize (source_file, source_text, pool) to a self-contained .dotbc binary.
    std::vector<uint8_t> serializeAot(const std::string &sourceFile,
                                      const std::string &source,
                                      const AotBytecodes &pool);

    // Deserialize a .dotbc binary.  Returns (source_file, source_text, pool).
    // Throws std::runtime_error on format or version mismatch.
    struct AotFile
    {
        std::string sourceFile, source;
        AotBytecodes pool;
    };
    AotFile deserializeAot(const std::vector<uint8_t> &data);

}
