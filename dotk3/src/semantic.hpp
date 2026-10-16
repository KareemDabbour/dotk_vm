#pragma once
#include "syntax.hpp"
#include <functional>

namespace dk3 {
struct SemanticMember { std::string name, type, detail; int kind = 5; };
using ImportTree = std::function<Ast(const Ast &)>;
std::vector<SemanticMember> memberCompletions(const Ast &tree, const std::shared_ptr<const Source> &source,
                                            size_t offset, const ImportTree &imports);
std::string memberHover(const Ast &tree, const std::shared_ptr<const Source> &source,
                        size_t offset, const ImportTree &imports);
}