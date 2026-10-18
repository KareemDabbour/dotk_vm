#include "typer.hpp"
#include "prelude.hpp"
#include "types.hpp"
#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>

namespace dk3 {
namespace {

enum class SymbolKind { Value, Function, Class, Alias, TypeVar, Module };

struct Symbol;
using Exports = std::map<std::string, Symbol>;

struct Symbol {
    std::shared_ptr<Exports> exports;
    std::string native;
    SymbolKind kind = SymbolKind::Value;
    Ty type = nullptr;
    Ty declared = nullptr;
    bool mutableBinding = false;
    bool global = false;
    std::shared_ptr<Signature> signature;
    ClassInfo *info = nullptr;
    Ast declaration;
    Ty alias = nullptr;
    std::vector<Ty> aliasParams;
    bool resolving = false;
};

struct Scope {
    std::unordered_map<std::string, Symbol> symbols;
};

struct FunctionContext {
    Ty declaredResult = nullptr;
    std::vector<Ty> returned;
    size_t scopeBase = 0;
    bool constructor = false;
    bool generator = false;
    Ty yieldType = nullptr;
    std::vector<Ty> yielded;
    ClassInfo *owner = nullptr;
};

struct Refinement {
    std::map<std::string, Ty> whenTrue, whenFalse;
};

void walk(const Ast &node, const std::function<bool(const Ast &)> &visit) {
    if (!node || !visit(node)) return;
    for (const auto &child : node->children) walk(child, visit);
    for (const auto &parameter : node->parameters) walk(parameter.defaultValue, visit);
}

bool hasValueReturn(const Ast &body) {
    bool found = false;
    walk(body, [&](const Ast &node) {
        if (node->kind == "function") return false;
        if (node->kind == "return" && !node->children.empty()) found = true;
        return true;
    });
    return found;
}

bool containsYield(const Ast &body) {
    bool found = false;
    walk(body, [&](const Ast &node) {
        if (node->kind == "function") return false;
        if (node->kind == "yield") found = true;
        return true;
    });
    return found;
}

bool containsBreak(const Ast &body) {
    bool found = false;
    walk(body, [&](const Ast &node) {
        if (node->kind == "function" || node->kind == "while" || node->kind == "for" || node->kind == "for_c") return false;
        if (node->kind == "break") found = true;
        return true;
    });
    return found;
}

bool isTrueLiteral(const Ast &node) { return node && node->kind == "literal" && node->text == "true" && node->literalKind == TokenKind::Name; }

// True when control never reaches the end of the statement.
bool alwaysExits(const Ast &node) {
    if (!node) return false;
    if (node->kind == "return" || node->kind == "throw") return true;
    if (node->kind == "block") { for (const auto &child : node->children) if (alwaysExits(child)) return true; return false; }
    if (node->kind == "if") return node->children.size() > 2 && alwaysExits(node->children[1]) && alwaysExits(node->children[2]);
    if (node->kind == "while") return isTrueLiteral(node->children[0]) && !containsBreak(node->children[1]);
    if (node->kind == "for_c") return (!node->children[1] || isTrueLiteral(node->children[1])) && !containsBreak(node->children[3]);
    if (node->kind == "try") return alwaysExits(node->children[0]) && alwaysExits(node->children[1]);
    return false;
}

// Leaves the current block without falling through.
bool leavesBlock(const Ast &node) {
    if (!node) return false;
    if (alwaysExits(node) || node->kind == "break" || node->kind == "continue") return true;
    if (node->kind == "block") { for (const auto &child : node->children) if (leavesBlock(child)) return true; return false; }
    if (node->kind == "if") return node->children.size() > 2 && leavesBlock(node->children[1]) && leavesBlock(node->children[2]);
    return false;
}

class Typer {
public:
    std::vector<Diagnostic> diagnostics;
    void setLoader(ModuleLoader value) { loader = std::move(value); }
    const std::map<std::string, Ast> &loadedPrograms() const { return programs; }

    Typer() {
        scopes.emplace_back();
        Ast stubs = parseStub(std::make_shared<Source>(Source{"<prelude>", preludeSource()}));
        builtinMode = true;
        blockStatements(stubs->children);
        builtinMode = false;
        stringType = classType("String"); bytesType = classType("Bytes");
        listInfo = classInfo("List"); mapInfo = classInfo("Map"); setInfo = classInfo("Set"); tupleInfo = classInfo("Tuple");
        generatorInfo = classInfo("Generator"); errorType = classType("Error"); iterableInfo = classInfo("Iterable");
        scopes.emplace_back();
    }

    void check(const Ast &program) {
        walk(program, [&](const Ast &node) {
            if (node->kind == "assignment" && node->children[0]->kind == "name") assigned.insert(node->children[0]->text);
            return true;
        });
        blockStatements(program->children);
    }

    // Declares host-provided natives from stub syntax in the core scope; false (with `problem`) leaves nothing declared.
    bool declareHost(const std::string &stubText, std::string &problem) {
        Ast stubs;
        try { stubs = parseStub(std::make_shared<Source>(Source{"<host>", stubText})); }
        catch (const Error &failure) { problem = failure.what(); return false; }
        std::vector<Scope> rest(std::make_move_iterator(scopes.begin() + 1), std::make_move_iterator(scopes.end()));
        scopes.resize(1);
        size_t before = diagnostics.size();
        builtinMode = false; replacingCore = true;
        blockStatements(stubs->children);
        replacingCore = false;
        for (auto &scope : rest) scopes.push_back(std::move(scope));
        if (diagnostics.size() == before) return true;
        problem = diagnostics[before].message; diagnostics.resize(before); return false;
    }


    // ---- editor queries ---------------------------------------------------------------------

    std::string describeFunction(const std::string &name, const Ast &node, Ty type = nullptr) const {
        std::string result = "fn " + name + "(";
        for (const auto &parameter : node->parameters) { if (result.back() != '(') result += ", "; result += parameter.name + (parameter.type.empty() ? "" : ": " + parameter.type); }
        result += ')';
        if (type && type->tag == TypeTag::Function) result += " -> " + ty.show(type->result);
        else if (!node->type.empty()) result += " -> " + node->type;
        return result;
    }

    std::string describeDeclaration(const Ast &node, Ty type) const {
        std::string result = node->kind + " " + node->text;
        if (node->kind == "binding") {
            result = std::string(node->exported ? "export " : "") + (node->mutableBinding ? "var " : "let ") + node->text + ": " + (type ? ty.show(type) : node->type);
            const Ast &initializer = node->children.empty() ? nullptr : node->children[0];
            if (initializer && initializer->kind == "literal" && initializer->span.source && initializer->span.length <= 80) {
                std::string text = initializer->span.source->text.substr(initializer->span.offset, initializer->span.length);
                if (text.find_first_of("\r\n") == std::string::npos) result += " = " + text;
            }
            result += node->typeExpr ? "\nDeclared type" : "\nType inferred from initializer";
            if (initializer && initializer->kind == "function") result += "\n" + describeFunction(node->text, initializer, initializer->resolved);
        }
        if (!node->baseClass.empty()) result += " extends " + node->baseClass;
        if ((node->kind == "class" || node->kind == "record") && !node->names.empty()) {
            result += " implements ";
            for (size_t index = 0; index < node->names.size(); index++) { if (index) result += ", "; result += node->names[index]; }
        }
        if (node->kind == "record" || node->kind == "class") {
            result += " {";
            for (const auto &parameter : node->parameters) result += std::string("\n  ") + (node->kind == "class" ? (parameter.mutableBinding ? "var " : "let ") : "") + parameter.name + ": " + parameter.type;
            for (const auto &method : node->children) if (method->kind == "function") result += "\n  fn " + method->text + "(...)";
            result += "\n}";
        }
        if (node->kind == "enum") { result += " {"; for (const auto &child : node->children) result += "\n  " + child->text; result += "\n}"; }
        if (node->kind == "trait") {
            result += " {";
            for (const auto &method : node->children) result += "\n  " + describeFunction(method->text, method);
            result += "\n}";
        }
        if (node->kind == "function" || node->kind == "signature") result = describeFunction(node->text, node, node->resolved);
        if (!node->documentation.empty()) result += "\n\n" + node->documentation;
        return result;
    }

    std::string describeMember(const Member &member, const std::string &name, Ty self) {
        std::map<Ty, Ty> inherited;
        if (self && member.owner) {
            Ty up = ty.asSuper(self, member.owner);
            for (size_t index = 0; up && index < member.owner->params.size() && index < up->args.size(); index++) inherited[member.owner->params[index]] = up->args[index];
        }
        std::string owner = member.owner ? member.owner->name : "";
        if (member.owner && member.owner->builtin) std::replace(owner.begin(), owner.end(), '_', '.');
        if (member.isMethod && member.signature) {
            Ty type = ty.substitute(member.signature->type, inherited);
            std::string result = owner + "." + name + "(";
            for (size_t index = 0; index < type->args.size(); index++) {
                if (index) result += ", ";
                result += (index < member.signature->names.size() && !member.signature->names[index].empty() ? member.signature->names[index] + ": " : "") + ty.show(type->args[index]);
            }
            return result + ") -> " + ty.show(type->result);
        }
        return owner + "." + name + ": " + (member.type ? ty.show(ty.substitute(member.type, inherited)) : "");
    }

    ClassInfo *enclosingClass(const Ast &root, size_t offset) {
        ClassInfo *found = nullptr;
        walk(root, [&](const Ast &node) {
            if ((node->kind == "class" || node->kind == "record" || node->kind == "trait" || node->kind == "impl") && node->span.offset <= offset && offset <= node->endOffset) {
                if (node->kind == "impl") { Symbol *target = lookup(node->names.empty() ? "" : node->names[0]); if (target && target->kind == SymbolKind::Class) found = target->info; }
                else { auto info = classOf.find(node.get()); if (info != classOf.end()) found = info->second; }
            }
            return true;
        });
        return found;
    }

    bool memberVisible(const Member &member, ClassInfo *context) {
        if (member.visibility == Visibility::Public) return true;
        if (!context) return false;
        if (context == member.owner) return true;
        return member.visibility == Visibility::Protected && ty.asSuper(context->self, member.owner);
    }

    void gatherMembers(ClassInfo *info, Ty self, bool wantStatic, ClassInfo *context, std::map<std::string, CompletionItem> &items, int depth = 0) {
        if (!info || depth > 32) return;
        for (const auto &entry : info->members) {
            const Member &member = entry.second;
            if (member.isStatic != wantStatic || entry.first == "init" || entry.first == "constructor" || !memberVisible(member, context) || items.count(entry.first)) continue;
            if (member.owner && member.owner->builtin && entry.first.compare(0, 2, "__") == 0) continue;
            CompletionItem item; item.label = entry.first; item.kind = member.isMethod ? 2 : 5;
            item.detail = describeMember(member, entry.first, self);
            if (member.isMethod && member.signature && member.signature->declaration) item.documentation = member.signature->declaration->documentation;
            items.emplace(entry.first, std::move(item));
        }
        if (info->super) gatherMembers(info->super->info, self, wantStatic, context, items, depth + 1);
        for (Ty trait : info->traits) gatherMembers(trait->info, self, wantStatic, context, items, depth + 1);
    }

    void gatherType(Ty type, ClassInfo *context, std::map<std::string, CompletionItem> &items, int depth = 0) {
        if (!type || depth > 8) return;
        switch (type->tag) {
        case TypeTag::Var: gatherType(type->result, context, items, depth + 1); break;
        case TypeTag::Union: {
            Ty stripped = ty.stripNull(type);
            if (stripped != type && stripped->tag != TypeTag::Union) gatherType(stripped, context, items, depth + 1);
            break;
        }
        case TypeTag::Intersection: for (Ty part : type->args) gatherType(part, context, items, depth + 1); break;
        case TypeTag::Named: gatherMembers(type->info, type, false, context, items); break;
        default: break;
        }
    }

    std::vector<CompletionItem> completionsFor(const Ast &receiver, const Ast &root, size_t offset) {
        std::map<std::string, CompletionItem> items;
        ClassInfo *context = enclosingClass(root, offset);
        if (receiver->kind == "name") {
            Symbol *symbol = lookup(receiver->text);
            if (symbol && symbol->kind == SymbolKind::Module && symbol->exports) {
                for (const auto &entry : *symbol->exports) {
                    CompletionItem item; item.label = entry.first;
                    const Symbol &exported = entry.second;
                    item.kind = exported.kind == SymbolKind::Function ? 2 : exported.kind == SymbolKind::Class ? 7 : 6;
                    if (exported.declaration) { item.detail = describeDeclaration(exported.declaration, exported.type); item.detail = item.detail.substr(0, item.detail.find("\n")); item.documentation = exported.declaration->documentation; }
                    else item.detail = entry.first;
                    items.emplace(entry.first, std::move(item));
                }
            }
        }
        if (items.empty()) {
            if (ClassInfo *owner = classReference(receiver)) {
                gatherMembers(owner, owner->self, true, context, items);
                for (const auto &variant : owner->variants) { CompletionItem item; item.label = variant.name; item.kind = 20; item.detail = owner->name + "." + variant.name; items.emplace(variant.name, std::move(item)); }
                std::string prefix = owner->name + "_";
                for (const auto &entry : scopes[0].symbols)
                    if (entry.second.kind == SymbolKind::Class && entry.first.compare(0, prefix.size(), prefix) == 0) {
                        CompletionItem item; item.label = entry.first.substr(prefix.size()); item.kind = 7; item.detail = "class " + owner->name + "." + item.label; items.emplace(item.label, std::move(item));
                    }
                if (owner->name == "IO") for (const char *name : {"Files", "Http", "Sockets", "Terminal"}) { CompletionItem item; item.label = name; item.kind = 7; item.detail = std::string("class IO.") + name; items.emplace(item.label, std::move(item)); }
                if (owner->name == "Collections") for (const char *name : {"List", "Map", "Set", "Tuple"}) { CompletionItem item; item.label = name; item.kind = 7; item.detail = std::string("class Collections.") + name; items.emplace(item.label, std::move(item)); }
            } else gatherType(receiver->resolved, context, items);
        }
        std::vector<CompletionItem> result;
        for (auto &entry : items) result.push_back(std::move(entry.second));
        return result;
    }

    // Hover text for the member `name` accessed on `receiver`.
    std::string memberHover(const Ast &receiver, const std::string &name, const Ast &root, size_t offset) {
        for (const auto &item : completionsFor(receiver, root, offset)) if (item.label == name) return item.detail + (item.documentation.empty() ? "" : "\n\n" + item.documentation);
        return {};
    }

    std::string nameHover(const Ast &node) {
        auto found = referenced.find(node.get());
        if (found == referenced.end()) return {};
        const Symbol &symbol = found->second;
        if (!symbol.native.empty()) {
            size_t dot = symbol.native.rfind('.');
            std::string original = symbol.native.substr(dot == std::string::npos ? 0 : dot + 1), module = dot == std::string::npos ? "" : symbol.native.substr(0, dot);
            std::string result = "from " + module + " import " + original + (original == node->text ? "" : " as " + node->text);
            if (symbol.signature) {
                result += "\n" + original + "(";
                Ty type = symbol.signature->type;
                for (size_t index = 0; index < type->args.size(); index++) { if (index) result += ", "; result += (index < symbol.signature->names.size() && !symbol.signature->names[index].empty() ? symbol.signature->names[index] + ": " : "") + ty.show(type->args[index]); }
                result += ") -> " + ty.show(type->result);
            }
            return result;
        }
        if (symbol.declaration) return describeDeclaration(symbol.declaration, symbol.type ? symbol.type : symbol.declared);
        if (symbol.kind == SymbolKind::Class && symbol.info) return "class " + symbol.info->name;
        if (symbol.kind == SymbolKind::Module) return "module " + node->text;
        if (symbol.kind == SymbolKind::Function && symbol.signature) return "fn " + node->text + ": " + ty.show(symbol.signature->type);
        Ty type = symbol.type ? symbol.type : symbol.declared;
        return type ? node->text + ": " + ty.show(type) : std::string();
    }

    std::string declarationHover(const Ast &root, const std::string &name, size_t offset) {
        Ast best; size_t distance = 0;
        walk(root, [&](const Ast &node) {
            static const std::set<std::string> kinds{"binding", "function", "class", "record", "trait", "enum", "type_alias"};
            if (kinds.count(node->kind) && node->text == name && node->span.offset <= offset && offset - node->span.offset <= 24 && (!best || offset - node->span.offset < distance)) { best = node; distance = offset - node->span.offset; }
            return true;
        });
        if (!best) return {};
        Ty type = best->resolved;
        if (best->kind == "function" && classOf.count(best.get())) type = nullptr;
        return describeDeclaration(best, type);
    }

    // One REPL submission: globals persist and may be redeclared; a failed submission leaves no trace.
    std::vector<Diagnostic> checkSubmission(const Ast &program, bool allowRedeclaration) {
        diagnostics.clear();
        Scope saved = scopes[1]; std::set<std::string> savedAssigned = assigned;
        sessionMode = allowRedeclaration; check(program); sessionMode = false;
        bool failed = std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic &diagnostic) { return !diagnostic.warning; });
        if (failed) { scopes.resize(2); scopes[1] = std::move(saved); assigned = std::move(savedAssigned); functions.clear(); }
        return diagnostics;
    }

private:
    Types ty;
    std::vector<Scope> scopes;
    std::vector<FunctionContext> functions;
    std::unordered_map<const Node *, ClassInfo *> classOf;
    std::unordered_map<const Node *, std::shared_ptr<Signature>> signatureOf;
    std::unordered_map<const Node *, ClassInfo *> implTarget;
    std::set<std::string> assigned;
    std::unordered_map<const Node *, Symbol> referenced;  // symbol each name node resolved to, for hover
    ModuleLoader loader;
    std::map<std::string, std::shared_ptr<Exports>> modules;
    std::map<std::string, Ast> programs;
    std::set<std::string> loading;
    bool builtinMode = false;
    bool sessionMode = false, replacingCore = false;
    Ty stringType = nullptr, bytesType = nullptr, errorType = nullptr;
    ClassInfo *listInfo = nullptr, *mapInfo = nullptr, *setInfo = nullptr, *tupleInfo = nullptr, *generatorInfo = nullptr, *iterableInfo = nullptr;

    // ---- diagnostics and scopes -------------------------------------------------------------

    void error(const Span &span, const std::string &message) { diagnostics.push_back({span, message, false}); }
    void warn(const Span &span, const std::string &message) { diagnostics.push_back({span, message, true}); }
    std::string show(Ty type) const { return ty.show(type); }

    ClassInfo *classInfo(const std::string &name) { return scopes[0].symbols.at(name).info; }
    Ty classType(const std::string &name) { return ty.named(classInfo(name)); }

    void pushScope() { scopes.emplace_back(); }
    void popScope() { scopes.pop_back(); }

    Symbol *lookup(const std::string &name, size_t *index = nullptr) {
        for (size_t position = scopes.size(); position-- > 0;) {
            auto found = scopes[position].symbols.find(name);
            if (found != scopes[position].symbols.end()) { if (index) *index = position; return &found->second; }
        }
        return nullptr;
    }

    void declareSymbol(const std::string &name, Symbol symbol, const Span &span) {
        if (scopes.back().symbols.count(name) && !(sessionMode && scopes.size() == 2) && !replacingCore) { error(span, "Duplicate declaration '" + name + "'"); return; }
        scopes.back().symbols.insert_or_assign(name, std::move(symbol));
    }

    void declareValue(const std::string &name, Ty type, bool mutableBinding, const Span &span) {
        Symbol symbol; symbol.type = symbol.declared = type; symbol.mutableBinding = mutableBinding; symbol.global = scopes.size() == 2;
        declareSymbol(name, std::move(symbol), span);
    }

    // ---- type expressions -------------------------------------------------------------------

    std::vector<Ty> declareTypeParams(const std::vector<TypeParam> &params, const Span &span) {
        std::vector<Ty> variables;
        for (const auto &parameter : params) {
            Ty variable = ty.variable(parameter.name); variables.push_back(variable);
            Symbol symbol; symbol.kind = SymbolKind::TypeVar; symbol.type = variable;
            declareSymbol(parameter.name, std::move(symbol), span);
        }
        for (size_t index = 0; index < params.size(); index++) if (params[index].bound) ty.setBound(variables[index], resolveType(params[index].bound));
        return variables;
    }

    void enterTypeParams(const std::vector<Ty> &variables) {
        for (Ty variable : variables) { Symbol symbol; symbol.kind = SymbolKind::TypeVar; symbol.type = variable; scopes.back().symbols[variable->name] = std::move(symbol); }
    }

    void resolveAlias(Symbol &symbol) {
        if (symbol.alias) return;
        if (symbol.resolving) { error(symbol.declaration->span, "Type alias '" + symbol.declaration->text + "' is cyclic"); symbol.alias = ty.error(); return; }
        symbol.resolving = true;
        pushScope(); symbol.aliasParams = declareTypeParams(symbol.declaration->typeParams, symbol.declaration->span);
        symbol.alias = resolveType(symbol.declaration->typeExpr); popScope();
        symbol.resolving = false;
    }

    Ty resolveType(const TypeAst &type) {
        if (!type) return ty.error();
        switch (type->kind) {
        case TypeKind::Named: return resolveNamed(type);
        case TypeKind::Nullable: return ty.nullable(resolveType(type->inner));
        case TypeKind::Union: { std::vector<Ty> members; for (const auto &item : type->items) members.push_back(resolveType(item)); return ty.unite(std::move(members)); }
        case TypeKind::Function: {
            std::vector<Ty> parameters; for (const auto &item : type->items) parameters.push_back(resolveType(item));
            return ty.function(std::move(parameters), resolveType(type->inner));
        }
        case TypeKind::Wildcard: return ty.wildcard(type->bound, type->inner ? resolveType(type->inner) : nullptr);
        case TypeKind::Intersection: {
            std::vector<Ty> members;
            for (const auto &item : type->items) {
                Ty member = resolveType(item);
                if (member->tag != TypeTag::Named && member->tag != TypeTag::Var && member->tag != TypeTag::Error) error(type->span, "Intersection members must be classes or traits");
                members.push_back(member);
            }
            return ty.intersect(std::move(members));
        }
        }
        return ty.error();
    }

    Ty resolveNamed(const TypeAst &type) {
        const std::string name = type->name.rfind("System.", 0) == 0 ? type->name.substr(7) : type->name;
        std::vector<Ty> arguments; for (const auto &item : type->items) arguments.push_back(resolveType(item));
        auto plain = [&](Ty result) { if (!arguments.empty()) error(type->span, name + " takes no type arguments"); return result; };
        if (name == "Int") return plain(ty.integer());
        if (name == "Float") return plain(ty.floating());
        if (name == "Bool") return plain(ty.boolean());
        if (name == "Null") return plain(ty.null());
        if (name == "Object") return plain(ty.object());
        if (name == "Never") return plain(ty.never());
        Symbol *symbol = lookup(name);
        if (!symbol && name.rfind("System.", 0) == 0) symbol = lookup(name.substr(7));
        if (!symbol && name.find('.') != std::string::npos) {
            size_t dot = name.find('.'); Symbol *space = lookup(name.substr(0, dot));
            if (space && space->kind == SymbolKind::Module) {
                auto found = space->exports->find(name.substr(dot + 1));
                if (found != space->exports->end()) symbol = &found->second;
            }
        }
        if (!symbol && name.find('.') != std::string::npos) { std::string nested = name; std::replace(nested.begin(), nested.end(), '.', '_'); symbol = lookup(nested); if (!symbol && (name.rfind("Collections.", 0) == 0 || name.rfind("IO.", 0) == 0)) symbol = lookup(name.substr(name.find('.') + 1)); }
        if (!symbol) { error(type->span, "Unknown type '" + name + "'"); return ty.error(); }
        switch (symbol->kind) {
        case SymbolKind::TypeVar: return plain(symbol->type);
        case SymbolKind::Class: {
            size_t expected = symbol->info->params.size();
            if (arguments.size() != expected) {
                error(type->span, name + " expects " + std::to_string(expected) + " type argument(s), got " + std::to_string(arguments.size()));
                return ty.error();
            }
            return ty.named(symbol->info, std::move(arguments));
        }
        case SymbolKind::Alias: {
            resolveAlias(*symbol);
            if (arguments.size() != symbol->aliasParams.size()) { error(type->span, name + " expects " + std::to_string(symbol->aliasParams.size()) + " type argument(s)"); return ty.error(); }
            std::map<Ty, Ty> replacements; for (size_t index = 0; index < arguments.size(); index++) replacements[symbol->aliasParams[index]] = arguments[index];
            return ty.substitute(symbol->alias, replacements);
        }
        default: error(type->span, "'" + name + "' is not a type"); return ty.error();
        }
    }

    // ---- declarations -----------------------------------------------------------------------

    std::shared_ptr<Signature> makeSignature(const Ast &function, bool lambda) {
        auto signature = std::make_shared<Signature>(); signature->declaration = function;
        pushScope();
        signature->typeParams = declareTypeParams(function->typeParams, function->span);
        std::vector<Ty> parameters;
        for (const auto &parameter : function->parameters) {
            Ty type = ty.error();
            if (parameter.typeExpr) type = resolveType(parameter.typeExpr);
            else if (!lambda) error(function->span, "Parameter '" + parameter.name + "' requires a type annotation");
            parameters.push_back(type); signature->names.push_back(parameter.name); signature->defaults.push_back(static_cast<bool>(parameter.defaultValue));
            if (parameter.rest) signature->hasRest = true;
        }
        Ty result;
        if (function->typeExpr) result = resolveType(function->typeExpr);
        else if (function->constructor) result = ty.null();
        else if (!function->children.empty() && (hasValueReturn(function->children[0]) || containsYield(function->children[0]))) { result = ty.error(); signature->pendingResult = true; }
        else result = ty.null();
        popScope();
        signature->type = ty.function(std::move(parameters), result);
        signatureOf[function.get()] = signature;
        return signature;
    }

    void declareClass(const Ast &node) {
        ClassKind kind = node->kind == "class" ? ClassKind::Class : node->kind == "record" ? ClassKind::Record : node->kind == "trait" ? ClassKind::Trait : ClassKind::Enum;
        ClassInfo *info = ty.newClass(node->text, kind); info->declaration = node; info->builtin = builtinMode; classOf[node.get()] = info;
        Symbol symbol; symbol.kind = SymbolKind::Class; symbol.info = info; symbol.declaration = node;
        declareSymbol(node->text, std::move(symbol), node->span);
    }

    void classParams(const Ast &node) {
        ClassInfo *info = classOf[node.get()];
        pushScope(); info->params = declareTypeParams(node->typeParams, node->span); info->self = ty.named(info, info->params);
        popScope();
    }

    void classHeader(const Ast &node) {
        ClassInfo *info = classOf[node.get()];
        pushScope(); enterTypeParams(info->params);
        if (!node->baseClass.empty()) {
            Ty base = resolveType(node->typeExpr);
            if (base->tag == TypeTag::Object) {}
            else if (base->tag == TypeTag::Named && base->info->kind == ClassKind::Class && !ty.asSuper(base, info)) info->super = base;
            else if (base->tag != TypeTag::Error) error(node->span, "'" + node->baseClass + "' cannot be extended by '" + info->name + "'");
        }
        for (const auto &trait : node->typeExprs) {
            Ty resolved = resolveType(trait);
            if (resolved->tag == TypeTag::Named && resolved->info->kind == ClassKind::Trait) info->traits.push_back(resolved);
            else if (resolved->tag != TypeTag::Error) error(node->span, "'" + typeText(trait) + "' is not a trait");
        }
        popScope();
    }

    void addMember(ClassInfo *info, const std::string &name, Member member, const Span &span) {
        member.owner = info;
        if (!info->members.emplace(name, std::move(member)).second) error(span, "Duplicate member '" + name + "' in " + info->name);
    }

    void classMembers(const Ast &node) {
        ClassInfo *info = classOf[node.get()];
        pushScope(); enterTypeParams(info->params);
        if (info->kind == ClassKind::Enum) {
            for (const auto &child : node->children) {
                if (child->kind != "variant") continue;
                Variant variant; variant.name = child->text;
                for (const auto &payload : child->typeExprs) variant.payload.push_back(resolveType(payload));
                Member member; member.isStatic = true;
                if (variant.payload.empty()) member.type = info->self;
                else {
                    member.type = ty.function(variant.payload, info->self);
                    member.signature = std::make_shared<Signature>(); member.signature->type = member.type; member.signature->typeParams = info->params;
                    member.signature->names.assign(variant.payload.size(), ""); member.signature->defaults.assign(variant.payload.size(), false);
                }
                info->variants.push_back(variant); addMember(info, child->text, std::move(member), child->span);
            }
        } else {
            for (const auto &field : node->parameters) {
                Member member; member.type = field.typeExpr ? resolveType(field.typeExpr) : ty.error();
                member.mutableBinding = info->kind == ClassKind::Class && field.mutableBinding; member.visibility = field.visibility; member.hasDefault = static_cast<bool>(field.defaultValue);
                info->fieldOrder.push_back(field.name); addMember(info, field.name, std::move(member), node->span);
            }
            for (const auto &child : node->children) {
                if (child->kind == "binding") {
                    Member member; member.isStatic = true; member.type = resolveType(child->typeExpr); member.mutableBinding = child->mutableBinding; member.visibility = child->visibility;
                    addMember(info, child->text, std::move(member), child->span);
                } else if (child->kind == "function" || child->kind == "signature") {
                    Member member; member.isMethod = true; member.signature = makeSignature(child, false); member.type = member.signature->type;
                    member.isStatic = child->staticMember; member.visibility = child->visibility;
                    addMember(info, child->text, std::move(member), child->span);
                }
            }
        }
        popScope();
    }

    Signature *constructorOf(ClassInfo *info) {
        if (info->constructor) return info->constructor.get();
        auto signature = std::make_shared<Signature>(); signature->typeParams = info->params; info->constructor = signature;
        std::vector<Ty> parameters;
        auto init = info->members.find("init");
        if (info->kind == ClassKind::Class && init != info->members.end() && init->second.isMethod &&
            (init->second.signature->declaration->constructor || !init->second.signature->declaration->parameters.empty())) {
            const Signature &source = *init->second.signature;
            parameters = source.type->args; signature->names = source.names; signature->defaults = source.defaults; signature->hasRest = source.hasRest;
        } else {
            if (info->super) {
                Signature *inherited = constructorOf(info->super->info); std::map<Ty, Ty> replacements;
                for (size_t index = 0; index < info->super->info->params.size() && index < info->super->args.size(); index++) replacements[info->super->info->params[index]] = info->super->args[index];
                for (Ty parameter : inherited->type->args) parameters.push_back(ty.substitute(parameter, replacements));
                signature->names = inherited->names; signature->defaults = inherited->defaults;
            }
            for (const auto &name : info->fieldOrder) {
                const Member &field = info->members.at(name);
                parameters.push_back(field.type); signature->names.push_back(name); signature->defaults.push_back(field.hasDefault);
            }
        }
        signature->type = ty.function(std::move(parameters), info->self);
        return signature.get();
    }

    const Member *findMember(ClassInfo *info, const std::string &name, int depth = 0) {
        if (depth > 64) return nullptr;
        auto found = info->members.find(name);
        if (found != info->members.end()) return &found->second;
        if (info->super) if (const Member *member = findMember(info->super->info, name, depth + 1)) return member;
        for (Ty trait : info->traits) if (const Member *member = findMember(trait->info, name, depth + 1)) return member;
        return nullptr;
    }

    bool isBodiless(const Member &member) {
        if (!member.isMethod || !member.signature || !member.signature->declaration) return false;
        const Ast &declaration = member.signature->declaration;
        return declaration->kind == "signature" || declaration->children.empty();
    }

    void checkImplements(ClassInfo *info, const Span &span) {
        if (info->kind == ClassKind::Trait || info->kind == ClassKind::Enum || info->builtin) return;
        if (info->super && info->super->tag == TypeTag::Named) {
            for (const auto &entry : info->members) {
                const Member &own = entry.second;
                if (own.owner != info || !own.isMethod || own.isStatic || !own.signature || own.signature->pendingResult || entry.first == "init") continue;
                const Member *base = findMember(info->super->info, entry.first);
                if (!base || !base->isMethod || base->isStatic || !base->signature || base->signature->pendingResult || base->visibility == Visibility::Private) continue;
                Ty up = ty.asSuper(info->self, base->owner); std::map<Ty, Ty> inherited;
                for (size_t index = 0; up && index < base->owner->params.size() && index < up->args.size(); index++) inherited[base->owner->params[index]] = up->args[index];
                Ty expected = ty.substitute(base->signature->type, inherited);
                if (!ty.isSubtype(own.signature->type, expected))
                    error(span, info->name + "." + entry.first + " has type " + show(own.signature->type) + " but overrides " + base->owner->name + "." + entry.first + ": " + show(expected));
            }
        }
        for (Ty trait : info->traits) {
            std::map<Ty, Ty> traitArguments;
            for (size_t index = 0; index < trait->info->params.size() && index < trait->args.size(); index++) traitArguments[trait->info->params[index]] = trait->args[index];
            for (const auto &entry : trait->info->members) {
                const Member &required = entry.second;
                if (!isBodiless(required)) continue;
                const Member *provided = findMember(info, entry.first);
                if (!provided || !provided->isMethod || isBodiless(*provided)) { error(span, info->name + " does not implement " + trait->info->name + "." + entry.first); continue; }
                Ty up = ty.asSuper(info->self, provided->owner); std::map<Ty, Ty> own;
                for (size_t index = 0; up && index < provided->owner->params.size() && index < up->args.size(); index++) own[provided->owner->params[index]] = up->args[index];
                Ty actual = ty.substitute(provided->signature->type, own), expected = ty.substitute(required.signature->type, traitArguments);
                if (!ty.isSubtype(actual, expected)) error(span, info->name + "." + entry.first + " has type " + show(actual) + " but " + trait->info->name + " requires " + show(expected));
            }
        }
    }

    // ---- inference and calls ----------------------------------------------------------------

    bool containsVar(Ty type, const std::vector<Ty> &variables) {
        if (std::find(variables.begin(), variables.end(), type) != variables.end()) return true;
        for (Ty argument : type->args) if (containsVar(argument, variables)) return true;
        return type->result && containsVar(type->result, variables);
    }

    void unify(Ty pattern, Ty actual, std::map<Ty, Ty> &bindings, const std::vector<Ty> &variables) {
        if (actual->tag == TypeTag::Error) return;
        if (pattern->tag == TypeTag::Var && std::find(variables.begin(), variables.end(), pattern) != variables.end()) {
            if (actual->tag == TypeTag::Never) return;
            auto bound = bindings.find(pattern);
            if (bound == bindings.end()) bindings[pattern] = actual;
            else if (!ty.isSubtype(actual, bound->second)) bound->second = ty.join(bound->second, actual);
            return;
        }
        if (pattern->tag == TypeTag::Named && actual->tag == TypeTag::Named) {
            Ty up = ty.asSuper(actual, pattern->info);
            if (up && up->args.size() == pattern->args.size())
                for (size_t index = 0; index < up->args.size(); index++) unify(pattern->args[index]->tag == TypeTag::Wildcard && pattern->args[index]->result ? pattern->args[index]->result : pattern->args[index], up->args[index], bindings, variables);
        } else if (pattern->tag == TypeTag::Function && actual->tag == TypeTag::Function && pattern->args.size() == actual->args.size()) {
            for (size_t index = 0; index < actual->args.size(); index++) unify(pattern->args[index], actual->args[index], bindings, variables);
            unify(pattern->result, actual->result, bindings, variables);
        } else if (pattern->tag == TypeTag::Union) {
            std::vector<Ty> others; for (Ty member : pattern->args) if (member->tag != TypeTag::Null) others.push_back(member);
            if (others.size() == 1 && actual->tag != TypeTag::Null) unify(others[0], ty.stripNull(actual), bindings, variables);
        }
    }

    bool isLambdaNeedingContext(const Ast &node) {
        if (node->kind != "function") return false;
        for (const auto &parameter : node->parameters) if (!parameter.typeExpr) return true;
        return !node->typeExpr;
    }

    bool assignable(Ty actual, Ty expected, const Span &span, const std::string &context = "") {
        if (ty.isSubtype(actual, expected)) return true;
        std::string message = "Expected " + show(expected) + ", got " + show(actual) + context;
        if (ty.isNullable(actual) && ty.isSubtype(ty.stripNull(actual), expected)) message += " (value may be null)";
        error(span, message); return false;
    }

    Ty applyCall(const Ast &node, Ty function, const std::shared_ptr<Signature> &signature, const std::vector<TypeAst> &explicitArguments, Ty expected) {
        const std::vector<Ty> &parameters = function->args; size_t count = node->children.size() - 1;
        std::vector<Ty> variables = signature ? signature->typeParams : std::vector<Ty>{};
        std::map<Ty, Ty> bindings;
        if (signature && signature->pendingResult) {
            error(node->span, signature->inferring ? "Recursive function needs an explicit return type" : "Add a return type to this function; it is called before its return type can be inferred");
            return ty.error();
        }
        if (!explicitArguments.empty()) {
            if (explicitArguments.size() != variables.size()) error(node->span, "Expected " + std::to_string(variables.size()) + " type argument(s), got " + std::to_string(explicitArguments.size()));
            else {
                for (size_t index = 0; index < variables.size(); index++) bindings[variables[index]] = resolveType(explicitArguments[index]);
                variables.clear();
            }
        }
        bool rest = signature && signature->hasRest;
        std::vector<int> target(count, -1); std::vector<bool> supplied(parameters.size(), false);
        size_t positional = 0;
        for (size_t index = 0; index < count; index++) {
            const std::string &name = node->names.size() > index ? node->names[index] : std::string();
            if (name.empty()) {
                if (rest && positional >= parameters.size() - 1) { target[index] = static_cast<int>(parameters.size() - 1); supplied.back() = true; continue; }
                if (positional >= parameters.size()) { error(node->children[index + 1]->span, "Too many arguments (expected " + std::to_string(parameters.size()) + ")"); continue; }
                target[index] = static_cast<int>(positional); supplied[positional++] = true;
            } else {
                auto found = signature ? std::find(signature->names.begin(), signature->names.end(), name) : std::vector<std::string>::iterator{};
                if (!signature || found == signature->names.end()) { error(node->children[index + 1]->span, "Unknown argument '" + name + "'"); continue; }
                size_t slot = static_cast<size_t>(found - signature->names.begin());
                if (supplied[slot]) error(node->children[index + 1]->span, "Argument '" + name + "' supplied more than once");
                target[index] = static_cast<int>(slot); supplied[slot] = true;
            }
        }
        for (size_t slot = 0; slot < parameters.size(); slot++) {
            bool optional = signature && (signature->defaults[slot] || (rest && slot + 1 == parameters.size()));
            if (!supplied[slot] && !optional) error(node->span, signature && !signature->names[slot].empty() ? "Missing argument '" + signature->names[slot] + "'" : "Missing argument " + std::to_string(slot + 1));
        }
        std::vector<Ty> argumentTypes(count, nullptr);
        std::map<Ty, Ty> fromExpected;
        if (expected && !variables.empty()) unify(function->result, expected, fromExpected, variables);
        auto resolvedParameter = [&](int slot) { return ty.substitute(parameters[static_cast<size_t>(slot)], bindings); };
        for (int pass = 0; pass < 2; pass++) {
            for (size_t index = 0; index < count; index++) {
                if (target[index] < 0) continue;
                const Ast &argument = node->children[index + 1];
                bool deferred = isLambdaNeedingContext(argument);
                if (deferred != (pass == 1)) continue;
                Ty parameter = resolvedParameter(target[index]);
                if (deferred && parameter->tag == TypeTag::Function) {
                    std::vector<Ty> hints = parameter->args; Ty resultHint = parameter->result;
                    if (containsVar(resultHint, variables)) resultHint = ty.substitute(resultHint, fromExpected);
                    if (containsVar(resultHint, variables)) resultHint = nullptr;
                    argumentTypes[index] = lambdaExpression(argument, hints, resultHint, variables);
                } else argumentTypes[index] = expr(argument, containsVar(parameter, variables) ? nullptr : parameter);
                unify(parameters[static_cast<size_t>(target[index])], argumentTypes[index], bindings, variables);
            }
        }
        Ty result = function->result;
        for (const auto &entry : fromExpected) bindings.emplace(entry.first, entry.second);
        for (Ty variable : variables) {
            if (bindings.count(variable)) {
                if (variable->result && !ty.isSubtype(bindings[variable], ty.substitute(variable->result, bindings))) error(node->span, "Type " + show(bindings[variable]) + " does not satisfy the bound " + show(variable->result) + " of " + variable->name);
                continue;
            }
            error(node->span, "Cannot infer type argument '" + variable->name + "'; provide it explicitly"); bindings[variable] = ty.error();
        }
        for (size_t index = 0; index < count; index++) {
            if (target[index] < 0 || !argumentTypes[index]) continue;
            Ty parameter = ty.substitute(parameters[static_cast<size_t>(target[index])], bindings);
            assignable(argumentTypes[index], parameter, node->children[index + 1]->span);
        }
        return ty.substitute(result, bindings);
    }

    Ty lambdaExpression(const Ast &node, const std::vector<Ty> &hints, Ty resultHint, const std::vector<Ty> &unresolved = {}) {
        std::vector<Ty> parameters;
        for (size_t index = 0; index < node->parameters.size(); index++) {
            const Parameter &parameter = node->parameters[index];
            if (parameter.typeExpr) parameters.push_back(resolveType(parameter.typeExpr));
            else if (index < hints.size() && !containsVar(hints[index], unresolved)) parameters.push_back(hints[index]);
            else { error(node->span, "Cannot infer the type of parameter '" + parameter.name + "'; annotate it"); parameters.push_back(ty.error()); }
        }
        Ty declared = node->typeExpr ? resolveType(node->typeExpr) : resultHint;
        Ty result = checkFunctionBody(node, parameters, declared, {}, nullptr, false, nullptr);
        return ty.function(std::move(parameters), result);
    }

    Ty checkFunctionBody(const Ast &function, const std::vector<Ty> &parameterTypes, Ty declaredResult, const std::vector<Ty> &typeParams, ClassInfo *owner, bool constructor, Ty self, bool isStatic = false) {
        if (function->children.empty()) return declaredResult ? declaredResult : ty.null();
        bool generator = containsYield(function->children[0]);
        pushScope(); enterTypeParams(typeParams);
        FunctionContext context; context.declaredResult = declaredResult; context.scopeBase = scopes.size() - 1; context.constructor = constructor;
        context.owner = owner ? owner : (functions.empty() ? nullptr : functions.back().owner);
        if (generator) {
            context.generator = true; context.declaredResult = nullptr;
            if (declaredResult && declaredResult->tag == TypeTag::Named && declaredResult->info == generatorInfo) context.yieldType = declaredResult->args[0];
            else if (declaredResult && declaredResult->tag != TypeTag::Error) error(function->span, "A generator must return Generator<T>, not " + show(declaredResult));
        }
        functions.push_back(context);
        if (self && !isStatic) {
            declareValue("self", self, false, function->span);
            if (owner && owner->super) declareValue("super", owner->super, false, function->span);
        }
        for (size_t index = 0; index < function->parameters.size() && index < parameterTypes.size(); index++) {
            const Parameter &parameter = function->parameters[index];
            Ty type = parameterTypes[index];
            if (parameter.rest) type = ty.named(listInfo, {type});
            if (parameter.defaultValue) { Ty value = expr(parameter.defaultValue, parameterTypes[index]); assignable(value, parameterTypes[index], parameter.defaultValue->span); }
            declareValue(parameter.name, type, false, function->span);
        }
        const Ast &body = function->children[0];
        blockStatements(body->children);
        FunctionContext finished = functions.back(); functions.pop_back(); popScope();
        Ty result = declaredResult;
        if (generator) {
            if (!result) {
                Ty element = ty.never();
                for (Ty yielded : finished.yielded) element = element->tag == TypeTag::Never ? yielded : ty.join(element, yielded);
                result = ty.named(generatorInfo, {element});
            }
        } else if (!result) {
            if (finished.returned.empty()) result = ty.null();
            else { result = finished.returned[0]; for (size_t index = 1; index < finished.returned.size(); index++) result = ty.join(result, finished.returned[index]); }
        } else if (result->tag != TypeTag::Null && result->tag != TypeTag::Never && result->tag != TypeTag::Error && !alwaysExits(body))
            error(function->span, "Function may reach its end without returning " + show(result));
        return result;
    }

    // ---- statements -------------------------------------------------------------------------

    void blockStatements(const std::vector<Ast> &statements) {
        for (const auto &statement : statements) if (statement->kind == "import" || statement->kind == "from_import") importStatement(statement);
        for (const auto &statement : statements) {
            if (statement->kind == "class" || statement->kind == "record" || statement->kind == "trait" || statement->kind == "enum") declareClass(statement);
            else if (statement->kind == "type_alias") { Symbol symbol; symbol.kind = SymbolKind::Alias; symbol.declaration = statement; declareSymbol(statement->text, std::move(symbol), statement->span); }
        }
        for (const auto &statement : statements) if (classOf.count(statement.get())) classParams(statement);
        for (const auto &statement : statements) if (classOf.count(statement.get())) classHeader(statement);
        for (const auto &statement : statements) if (classOf.count(statement.get())) classMembers(statement);
        for (const auto &statement : statements) if (statement->kind == "impl") implHeader(statement);
        for (const auto &statement : statements) {
            if (classOf.count(statement.get())) checkImplements(classOf[statement.get()], statement->span);
            else if (statement->kind == "function" && !statement->text.empty()) {
                Symbol symbol; symbol.kind = SymbolKind::Function; symbol.signature = makeSignature(statement, false); symbol.type = symbol.signature->type; symbol.declaration = statement;
                declareSymbol(statement->text, std::move(symbol), statement->span);
            }
        }
        for (const auto &statement : statements) this->statement(statement);
    }

    void block(const Ast &node) { pushScope(); blockStatements(node->children); popScope(); }

    void checkClassBodies(const Ast &node) {
        ClassInfo *info = classOf[node.get()];
        if (info->builtin) return;
        pushScope(); enterTypeParams(info->params);
        pushScope();
        for (const auto &field : node->parameters) {
            const Member &member = info->members.at(field.name);
            if (field.defaultValue) { Ty value = expr(field.defaultValue, member.type); assignable(value, member.type, field.defaultValue->span); }
            declareValue(field.name, member.type, false, node->span);
        }
        popScope();
        for (const auto &child : node->children) {
            if (child->kind == "binding") {
                if (child->children.empty()) continue;
                Ty type = resolveType(child->typeExpr), value = expr(child->children[0], type); assignable(value, type, child->children[0]->span);
            } else if (child->kind == "function" && info->kind != ClassKind::Enum) {
                auto signature = signatureOf[child.get()];
                if (signature->pendingResult) signature->inferring = true;
                Ty declared = signature->pendingResult ? nullptr : signature->type->result;
                bool constructor = child->constructor || child->text == "init";
                Ty result = checkFunctionBody(child, signature->type->args, declared, signature->typeParams, info, constructor, info->self, child->staticMember);
                finishSignature(signature, result);
                if (child->constructor && !child->children.empty()) checkConstructorFields(node, child);
                if (child->text == "toString" && !child->staticMember && signature->type->result != stringType)
                    error(child->span, "Expected String, got " + show(signature->type->result) + " (toString must return String)");
            }
        }
        popScope();
    }

    // Fields without defaults must be assigned on every path through a custom constructor.
    void checkConstructorFields(const Ast &classNode, const Ast &constructor) {
        std::set<std::string> fields;
        for (const auto &field : classNode->parameters) if (!field.defaultValue) fields.insert(field.name);
        if (fields.empty()) return;
        std::set<std::string> assigned;
        bool completes = fieldFlow(constructor->children[0], assigned, fields);
        if (!completes) return;
        for (const auto &field : fields) if (!assigned.count(field)) error(constructor->span, "Constructor may not initialize field '" + field + "'");
    }

    // Returns false when the path never reaches the end of the node.
    bool fieldFlow(const Ast &node, std::set<std::string> &assigned, const std::set<std::string> &fields) {
        if (!node) return true;
        const std::string &kind = node->kind;
        if (kind == "block") { for (const auto &child : node->children) if (!fieldFlow(child, assigned, fields)) return false; return true; }
        if (kind == "throw") return false;
        if (kind == "return") {
            for (const auto &field : fields) if (!assigned.count(field)) error(node->span, "Constructor may return before initializing field '" + field + "'");
            return false;
        }
        if (kind == "expression" && !node->children.empty()) {
            const Ast &value = node->children[0];
            if (value->kind == "assignment" && value->text == "=" && value->children[0]->kind == "member" && value->children[0]->children[0]->kind == "name" &&
                value->children[0]->children[0]->text == "self" && fields.count(value->children[0]->text)) assigned.insert(value->children[0]->text);
            return true;
        }
        if (kind == "if") {
            std::set<std::string> thenState = assigned, elseState = assigned;
            bool thenCompletes = fieldFlow(node->children[1], thenState, fields);
            bool elseCompletes = node->children.size() > 2 ? fieldFlow(node->children[2], elseState, fields) : true;
            if (!thenCompletes && !elseCompletes) return false;
            if (thenCompletes && elseCompletes) {
                std::set<std::string> both;
                for (const auto &field : thenState) if (elseState.count(field)) both.insert(field);
                assigned = std::move(both);
            } else assigned = thenCompletes ? thenState : elseState;
            return true;
        }
        if (kind == "while" || kind == "for" || kind == "for_c") {
            std::set<std::string> copy = assigned; fieldFlow(node->children.back(), copy, fields); return true;
        }
        if (kind == "try") {
            std::set<std::string> bodyState = assigned, catchState = assigned;
            bool bodyCompletes = fieldFlow(node->children[0], bodyState, fields), catchCompletes = fieldFlow(node->children[1], catchState, fields);
            if (!bodyCompletes && !catchCompletes) return false;
            if (bodyCompletes && catchCompletes) {
                std::set<std::string> both;
                for (const auto &field : bodyState) if (catchState.count(field)) both.insert(field);
                assigned = std::move(both);
            } else assigned = bodyCompletes ? bodyState : catchState;
            if (node->children.size() > 2) return fieldFlow(node->children[2], assigned, fields);
            return true;
        }
        return true;
    }

    void finishSignature(const std::shared_ptr<Signature> &signature, Ty result) {
        if (!signature->pendingResult) return;
        signature->pendingResult = false; signature->inferring = false; signature->type = ty.function(signature->type->args, result);
    }

    void yieldStatement(const Ast &node) {
        if (functions.empty() || !functions.back().generator) { error(node->span, "yield outside of a generator"); return; }
        Ty wanted = functions.back().yieldType;
        Ty value = node->children.empty() ? ty.null() : expr(node->children[0], wanted);
        if (wanted) assignable(value, wanted, node->children.empty() ? node->span : node->children[0]->span);
        else functions.back().yielded.push_back(value);
    }

    void importStatement(const Ast &node) {
        auto builtin = scopes[0].symbols.find(node->text);
        if (builtin == scopes[0].symbols.end() || builtin->second.kind != SymbolKind::Class) { importUserModule(node); return; }
        Symbol *module = &builtin->second;
        if (node->kind == "import") { Symbol alias = *module; declareSymbol(node->names[0], std::move(alias), node->span); return; }
        for (size_t index = 0; index + 1 < node->names.size(); index += 2) {
            const Member *member = findMember(module->info, node->names[index]);
            if (!member) {
                Symbol *nested = lookup(module->info->name + "_" + node->names[index]);
                if (!nested && module->info->name == "Collections") nested = lookup(node->names[index]);
                if (nested && nested->kind == SymbolKind::Class) { Symbol alias = *nested; declareSymbol(node->names[index + 1], std::move(alias), node->span); continue; }
            }
            if (!member || !member->isStatic) { error(node->span, node->text + " has no member '" + node->names[index] + "'"); continue; }
            Symbol symbol;
            if (member->signature) { symbol.kind = SymbolKind::Function; symbol.signature = member->signature; symbol.type = member->signature->type; symbol.native = module->info->name + "." + node->names[index]; }
            else { symbol.type = symbol.declared = member->type; symbol.global = true; }
            declareSymbol(node->names[index + 1], std::move(symbol), node->span);
        }
    }

    std::shared_ptr<Exports> loadModule(const std::string &name, const Span &span) {
        auto cached = modules.find(name); if (cached != modules.end()) return cached->second;
        if (loading.count(name)) { error(span, "Import cycle through module '" + name + "'"); return nullptr; }
        if (!loader) { error(span, "Module '" + name + "' was not found"); return nullptr; }
        std::shared_ptr<const Source> source; Ast program;
        try { source = loader(name); if (source) program = parse(source); }
        catch (const Error &failure) { error(failure.span.source ? failure.span : span, failure.what()); return nullptr; }
        catch (const std::exception &failure) { error(span, "Module '" + name + "' could not be loaded: " + failure.what()); return nullptr; }
        if (!program) { error(span, "Module '" + name + "' was not found"); return nullptr; }
        programs[name] = program;
        loading.insert(name);
        Scope prelude = scopes[0];
        auto savedScopes = std::move(scopes); auto savedFunctions = std::move(functions); auto savedAssigned = std::move(assigned);
        scopes.clear(); scopes.push_back(std::move(prelude)); scopes.emplace_back(); functions.clear(); assigned.clear();
        walk(program, [&](const Ast &node) {
            if (node->kind == "assignment" && node->children[0]->kind == "name") assigned.insert(node->children[0]->text);
            return true;
        });
        blockStatements(program->children);
        auto exports = std::make_shared<Exports>();
        for (const auto &statement : program->children) {
            if (!statement->exported) continue;
            auto found = scopes[1].symbols.find(statement->text);
            if (found == scopes[1].symbols.end()) continue;
            if (found->second.kind == SymbolKind::Alias) resolveAlias(found->second);
            exports->emplace(statement->text, found->second);
        }
        scopes = std::move(savedScopes); functions = std::move(savedFunctions); assigned = std::move(savedAssigned);
        loading.erase(name); modules[name] = exports;
        return exports;
    }

    void importUserModule(const Ast &node) {
        auto exports = loadModule(node->text, node->span);
        if (!exports) return;
        if (node->kind == "import") {
            Symbol space; space.kind = SymbolKind::Module; space.exports = exports;
            declareSymbol(node->names[0], std::move(space), node->span); return;
        }
        for (size_t index = 0; index + 1 < node->names.size(); index += 2) {
            auto found = exports->find(node->names[index]);
            if (found == exports->end()) { error(node->span, "Module '" + node->text + "' does not export '" + node->names[index] + "'"); continue; }
            declareSymbol(node->names[index + 1], found->second, node->span);
        }
    }

    // The exported symbol named by `space.name` when `space` is an imported module namespace.
    Symbol *moduleMember(const Ast &node, bool report) {
        if (node->kind != "member" || node->children[0]->kind != "name") return nullptr;
        Symbol *owner = lookup(node->children[0]->text);
        if (!owner || owner->kind != SymbolKind::Module) return nullptr;
        auto found = owner->exports->find(node->text);
        if (found == owner->exports->end()) { if (report) error(node->span, "Module '" + node->children[0]->text + "' does not export '" + node->text + "'"); return nullptr; }
        return &found->second;
    }

    bool isModuleAccess(const Ast &node) {
        if (node->kind != "member" || node->children[0]->kind != "name") return false;
        Symbol *owner = lookup(node->children[0]->text);
        return owner && owner->kind == SymbolKind::Module;
    }

    Ty moduleValue(const Ast &node) {
        Symbol *symbol = moduleMember(node, true);
        if (!symbol) return ty.error();
        if (symbol->kind == SymbolKind::Function) return symbol->signature->type;
        if (symbol->kind != SymbolKind::Value) { error(node->span, "'" + node->text + "' is a type, not a value"); return ty.error(); }
        return symbol->type;
    }

    void implHeader(const Ast &node) {
        auto lookupQualified = [&](const std::string &name) -> Symbol * {
            Symbol *symbol = lookup(name);
            if (!symbol && name.find('.') != std::string::npos) {
                std::string nested = name; std::replace(nested.begin(), nested.end(), '.', '_'); symbol = lookup(nested);
                if (!symbol && (name.rfind("Collections.", 0) == 0 || name.rfind("IO.", 0) == 0)) symbol = lookup(name.substr(name.find('.') + 1));
            }
            return symbol;
        };
        Symbol *traitSymbol = lookupQualified(node->text), *targetSymbol = lookupQualified(node->names[0]);
        if (!traitSymbol || traitSymbol->kind != SymbolKind::Class || traitSymbol->info->kind != ClassKind::Trait) { error(node->span, "'" + node->text + "' is not a trait"); return; }
        if (!targetSymbol || targetSymbol->kind != SymbolKind::Class || (targetSymbol->info->kind != ClassKind::Class && targetSymbol->info->kind != ClassKind::Record)) { error(node->span, "'" + node->names[0] + "' is not a class or record"); return; }
        ClassInfo *target = targetSymbol->info, *trait = traitSymbol->info;
        if (!trait->params.empty()) { error(node->span, "Generic traits cannot be implemented with impl yet"); return; }
        Ty traitType = ty.named(trait);
        if (std::find(target->traits.begin(), target->traits.end(), traitType) != target->traits.end()) { error(node->span, target->name + " already implements " + trait->name); return; }
        target->traits.push_back(traitType); implTarget[node.get()] = target;
        pushScope(); enterTypeParams(target->params);
        for (const auto &child : node->children) {
            if (child->kind != "function") continue;
            Member member; member.isMethod = true; member.signature = makeSignature(child, false); member.type = member.signature->type; member.visibility = child->visibility;
            addMember(target, child->text, std::move(member), child->span);
        }
        popScope();
    }

    void checkImplBodies(const Ast &node) {
        auto found = implTarget.find(node.get());
        if (found == implTarget.end()) return;
        ClassInfo *target = found->second;
        pushScope(); enterTypeParams(target->params);
        for (const auto &child : node->children) {
            if (child->kind != "function") continue;
            auto signature = signatureOf[child.get()];
            if (signature->pendingResult) signature->inferring = true;
            Ty declared = signature->pendingResult ? nullptr : signature->type->result;
            Ty result = checkFunctionBody(child, signature->type->args, declared, signature->typeParams, target, false, target->self);
            finishSignature(signature, result);
        }
        popScope();
    }

    void statement(const Ast &node) {
        const std::string &kind = node->kind;
        if (kind == "binding") bindingStatement(node);
        else if (kind == "expression") expr(node->children[0]);
        else if (kind == "block") block(node);
        else if (kind == "if") ifStatement(node);
        else if (kind == "while") whileStatement(node);
        else if (kind == "for_c") forCStatement(node);
        else if (kind == "for") forInStatement(node);
        else if (kind == "return") returnStatement(node);
        else if (kind == "break" || kind == "continue") return;
        else if (kind == "throw") expr(node->children[0]);
        else if (kind == "defer") expr(node->children[0]);
        else if (kind == "try") tryStatement(node);
        else if (kind == "function") functionStatement(node);
        else if (classOf.count(node.get())) checkClassBodies(node);
        else if (kind == "type_alias") { Symbol *symbol = lookup(node->text); if (symbol && symbol->kind == SymbolKind::Alias) resolveAlias(*symbol); }
        else if (kind == "import" || kind == "from_import") return;
        else if (kind == "impl") checkImplBodies(node);
        else if (kind == "yield") yieldStatement(node);
        else error(node->span, "Unsupported statement '" + kind + "'");
    }

    void functionStatement(const Ast &node) {
        if (node->text.empty() || builtinMode) return;
        Symbol *symbol = lookup(node->text);
        if (!symbol || !symbol->signature || symbol->signature != signatureOf[node.get()]) return;
        std::shared_ptr<Signature> signature = symbol->signature;
        if (signature->pendingResult) signature->inferring = true;
        Ty declared = signature->pendingResult ? nullptr : signature->type->result;
        Ty result = checkFunctionBody(node, signature->type->args, declared, signature->typeParams, nullptr, false, nullptr);
        finishSignature(signature, result);
        if (Symbol *refreshed = lookup(node->text)) refreshed->type = signature->type;
    }

    void bindingStatement(const Ast &node) {
        Ty declared = node->typeExpr ? resolveType(node->typeExpr) : nullptr;
        Ty value = expr(node->children[0], declared);
        Ty type = declared;
        if (!type) {
            type = value;
            if (value->tag == TypeTag::Null && !node->mutableBinding) type = value;
        } else assignable(value, type, node->children[0]->span);
        declareValue(node->text, type, node->mutableBinding, node->span);
        node->resolved = type;
        auto declared_symbol = scopes.back().symbols.find(node->text);
        if (declared_symbol != scopes.back().symbols.end()) declared_symbol->second.declaration = node;
    }

    void returnStatement(const Ast &node) {
        if (functions.empty()) { error(node->span, "return outside of a function"); return; }
        Ty declared = functions.back().declaredResult; bool constructor = functions.back().constructor;
        if (functions.back().generator) {
            if (!node->children.empty()) error(node->span, "A generator cannot return a value; use yield");
            return;
        }
        Ty value = node->children.empty() ? ty.null() : expr(node->children[0], declared);
        if (declared) {
            if (constructor && !node->children.empty()) error(node->span, "A constructor cannot return a value");
            else assignable(value, declared, node->children.empty() ? node->span : node->children[0]->span);
        } else functions.back().returned.push_back(value);
    }

    void applyRefinement(const std::map<std::string, Ty> &narrowed) {
        for (const auto &entry : narrowed) {
            Symbol *symbol = lookup(entry.first);
            if (!symbol || symbol->kind != SymbolKind::Value) continue;
            Symbol copy = *symbol; copy.type = entry.second; scopes.back().symbols[entry.first] = std::move(copy);
        }
    }

    void guarded(const std::map<std::string, Ty> &narrowed, const Ast &body) {
        pushScope(); applyRefinement(narrowed);
        if (body->kind == "block") blockStatements(body->children); else statement(body);
        popScope();
    }

    void ifStatement(const Ast &node) {
        Refinement refinement = condition(node->children[0]);
        guarded(refinement.whenTrue, node->children[1]);
        bool hasElse = node->children.size() > 2;
        if (hasElse) guarded(refinement.whenFalse, node->children[2]);
        if (!leavesBlock(node->children[1])) { if (hasElse && leavesBlock(node->children[2])) applyRefinement(refinement.whenTrue); }
        else if (!hasElse || !leavesBlock(node->children[2])) applyRefinement(refinement.whenFalse);
    }

    void resetAssigned(const Ast &body) {
        walk(body, [&](const Ast &node) {
            if (node->kind == "assignment" && node->children[0]->kind == "name") {
                Symbol *symbol = lookup(node->children[0]->text);
                if (symbol && symbol->kind == SymbolKind::Value && symbol->type != symbol->declared) { Symbol copy = *symbol; copy.type = copy.declared; scopes.back().symbols[node->children[0]->text] = std::move(copy); }
            }
            return true;
        });
    }

    void whileStatement(const Ast &node) {
        pushScope(); resetAssigned(node);
        Refinement refinement = condition(node->children[0]);
        guarded(refinement.whenTrue, node->children[1]);
        popScope();
    }

    void forCStatement(const Ast &node) {
        pushScope();
        if (node->children[0]) statement(node->children[0]);
        resetAssigned(node);
        if (node->children[1]) { Refinement refinement = condition(node->children[1]); guarded(refinement.whenTrue, node->children[3]); }
        else block(node->children[3]);
        if (node->children[2]) statement(node->children[2]);
        popScope();
    }

    Ty elementType(Ty iterable) {
        if (iterable->tag == TypeTag::Var) return elementType(iterable->result);
        if (iterable->tag != TypeTag::Named) return nullptr;
        Ty up = ty.asSuper(iterable, iterableInfo);
        return up && !up->args.empty() ? up->args[0] : nullptr;
    }

    void forInStatement(const Ast &node) {
        Ty iterable = expr(node->children[0]); Ty element = ty.error();
        if (iterable->tag != TypeTag::Error) {
            Ty found = elementType(ty.isNullable(iterable) ? ty.stripNull(iterable) : iterable);
            if (!found) error(node->children[0]->span, show(iterable) + " is not iterable"); else element = found;
        }
        pushScope(); resetAssigned(node);
        declareValue(node->text, element, false, node->span);
        block(node->children[1]);
        popScope();
    }

    void tryStatement(const Ast &node) {
        block(node->children[0]);
        pushScope(); declareValue(node->text, errorType, false, node->span); block(node->children[1]); popScope();
        if (node->children.size() > 2) block(node->children[2]);
    }

    // ---- conditions and narrowing -----------------------------------------------------------

    Ty narrowTo(Ty declared, Ty target) {
        if (declared->tag == TypeTag::Union) {
            std::vector<Ty> kept;
            for (Ty member : declared->args) { if (ty.isSubtype(member, target)) kept.push_back(member); else if (ty.isSubtype(target, member)) kept.push_back(target); }
            return kept.empty() ? target : ty.unite(std::move(kept));
        }
        return ty.isSubtype(declared, target) ? declared : target;
    }

    Ty excludeType(Ty declared, Ty target) {
        if (declared->tag != TypeTag::Union) return ty.isSubtype(declared, target) ? ty.never() : declared;
        std::vector<Ty> kept; for (Ty member : declared->args) if (!ty.isSubtype(member, target)) kept.push_back(member);
        return ty.unite(std::move(kept));
    }

    Symbol *narrowableSymbol(const Ast &node) {
        if (node->kind != "name") return nullptr;
        Symbol *symbol = lookup(node->text);
        return symbol && symbol->kind == SymbolKind::Value ? symbol : nullptr;
    }

    Refinement condition(const Ast &node) {
        Refinement result;
        const std::string &op = node->text;
        if (node->kind == "binary" && (op == "&&" || op == "and")) {
            Refinement left = condition(node->children[0]);
            pushScope(); applyRefinement(left.whenTrue); Refinement right = condition(node->children[1]); popScope();
            result.whenTrue = left.whenTrue; for (const auto &entry : right.whenTrue) result.whenTrue[entry.first] = entry.second;
            node->resolved = ty.boolean(); return result;
        }
        if (node->kind == "binary" && (op == "||" || op == "or")) {
            Refinement left = condition(node->children[0]);
            pushScope(); applyRefinement(left.whenFalse); Refinement right = condition(node->children[1]); popScope();
            result.whenFalse = left.whenFalse; for (const auto &entry : right.whenFalse) result.whenFalse[entry.first] = entry.second;
            node->resolved = ty.boolean(); return result;
        }
        if (node->kind == "unary" && (op == "!" || op == "not")) {
            Refinement inner = condition(node->children[0]);
            result.whenTrue = inner.whenFalse; result.whenFalse = inner.whenTrue; node->resolved = ty.boolean(); return result;
        }
        Ty type = expr(node);
        assignable(type, ty.boolean(), node->span);
        if (node->kind == "binary" && (op == "==" || op == "!=")) {
            const Ast *name = &node->children[0], *other = &node->children[1];
            if ((*name)->kind != "name") std::swap(name, other);
            bool nullCheck = (*other)->kind == "literal" && (*other)->text == "null" && (*other)->literalKind == TokenKind::Name;
            Symbol *symbol = nullCheck ? narrowableSymbol(*name) : nullptr;
            if (symbol) {
                std::map<std::string, Ty> isNull{{(*name)->text, ty.null()}}, notNull{{(*name)->text, ty.stripNull(symbol->type)}};
                if (op == "==") { result.whenTrue = isNull; result.whenFalse = notNull; } else { result.whenTrue = notNull; result.whenFalse = isNull; }
            }
        } else if (node->kind == "is") {
            Symbol *symbol = narrowableSymbol(node->children[0]);
            if (symbol && node->typeExpr) {
                Ty target = resolveType(node->typeExpr);
                result.whenTrue[node->children[0]->text] = narrowTo(symbol->type, target);
                result.whenFalse[node->children[0]->text] = excludeType(symbol->type, target);
            }
        }
        return result;
    }

    // ---- expressions ------------------------------------------------------------------------

    Ty expr(const Ast &node, Ty expected = nullptr) {
        Ty type = exprInner(node, expected);
        node->resolved = type;
        if (expected && type == ty.integer() && wantsFloat(expected)) node->widen = true;
        return type;
    }

    // True when an Int flowing into `expected` must be converted to Float.
    bool wantsFloat(Ty expected) {
        if (expected == ty.floating()) return true;
        if (expected->tag != TypeTag::Union) return false;
        bool hasFloat = false;
        for (Ty member : expected->args) { if (member == ty.integer()) return false; if (member == ty.floating()) hasFloat = true; }
        return hasFloat;
    }

    Ty hint(Ty expected, ClassInfo *info) {
        if (!expected) return nullptr;
        if (expected->tag == TypeTag::Named && expected->info == info) return expected;
        if (expected->tag == TypeTag::Named && expected->info == iterableInfo && expected->args.size() == 1 && (info == listInfo || info == setInfo || info == tupleInfo))
            return ty.named(info, {expected->args[0]});
        if (expected->tag == TypeTag::Union) { Ty stripped = ty.stripNull(expected); if (stripped != expected) return hint(stripped, info); }
        return nullptr;
    }

    Ty exprInner(const Ast &node, Ty expected) {
        const std::string &kind = node->kind;
        if (kind == "literal") return literal(node);
        if (kind == "name") return nameExpression(node);
        if (kind == "binary") return binaryExpression(node, expected);
        if (kind == "unary") return unaryExpression(node);
        if (kind == "call") return callExpression(node, expected);
        if (kind == "member") return isModuleAccess(node) ? moduleValue(node) : memberExpression(node, expected);
        if (kind == "index") return indexExpression(node);
        if (kind == "slice") return sliceExpression(node);
        if (kind == "list" || kind == "tuple") return listExpression(node, expected);
        if (kind == "map") return mapExpression(node, expected);
        if (kind == "format") { for (size_t index = 0; index < node->children.size(); index++) expr(node->children[index]); return stringType; }
        if (kind == "function") {
            Ty hinted = expected && expected->tag == TypeTag::Function ? expected : nullptr;
            return lambdaExpression(node, hinted ? hinted->args : std::vector<Ty>{}, hinted ? hinted->result : nullptr);
        }
        if (kind == "assignment") return assignmentExpression(node);
        if (kind == "is") {
            expr(node->children[0]); Ty target = resolveType(node->typeExpr);
            if (target->tag == TypeTag::Named && !target->args.empty()) for (Ty argument : target->args)
                if (argument->tag != TypeTag::Wildcard || argument->wildcard != WildcardBound::None) { error(node->span, "Type arguments are erased at run time; test against " + target->info->name + "<?> instead"); break; }
            return ty.boolean();
        }
        if (kind == "cast") return castExpression(node);
        if (kind == "nonnull") {
            Ty operand = expr(node->children[0]);
            if (operand->tag != TypeTag::Error && !ty.isNullable(operand)) warn(node->span, "Non-null assertion on a value that is never null");
            return ty.stripNull(operand);
        }
        if (kind == "match") return matchExpression(node, expected);
        error(node->span, "Unsupported expression '" + kind + "'"); return ty.error();
    }

    Ty literal(const Ast &node) {
        switch (node->literalKind) {
        case TokenKind::Integer: return ty.integer();
        case TokenKind::Float: return ty.floating();
        case TokenKind::String: return stringType;
        case TokenKind::Bytes: return bytesType;
        default: return node->text == "null" ? ty.null() : ty.boolean();
        }
    }

    Ty nameExpression(const Ast &node) {
        size_t index = 0; Symbol *symbol = lookup(node->text, &index);
        if (!symbol) { error(node->span, "Undefined name '" + node->text + "'"); return ty.error(); }
        referenced[node.get()] = *symbol;
        switch (symbol->kind) {
        case SymbolKind::Value: {
            bool crossed = !functions.empty() && index < functions.back().scopeBase;
            if (crossed && symbol->mutableBinding && !symbol->global) {
                if (assigned.count(node->text)) error(node->span, "Captured variable '" + node->text + "' must be effectively final");
                return symbol->declared;
            }
            return symbol->type;
        }
        case SymbolKind::Function: return symbol->signature ? symbol->signature->type : symbol->type;
        case SymbolKind::Class: case SymbolKind::Alias: return classType("Type");
        default: error(node->span, "'" + node->text + "' is a type, not a value"); return ty.error();
        }
    }

    // Records the operand kind so the compiler can pick typed opcodes; Int operands of a Float operation are widened.
    void markNumeric(const Ast &node, Ty left, Ty right, bool forceFloat) {
        bool floating = forceFloat || left == ty.floating() || right == ty.floating();
        node->numeric = floating ? 2 : 1;
        if (!floating) return;
        for (const auto &operand : node->children) if (operand && operand->resolved == ty.integer()) operand->widen = true;
    }

    Ty bounded(Ty type) {
        for (int depth = 0; type->tag == TypeTag::Var && type->result && depth < 16; depth++) type = type->result;
        return type;
    }

    Ty unaryExpression(const Ast &node) {
        Ty operand = expr(node->children[0]); const std::string &op = node->text;
        if (operand->tag == TypeTag::Error) return operand;
        if (op == "!" || op == "not") { assignable(operand, ty.boolean(), node->span); return ty.boolean(); }
        operand = bounded(operand);
        if (ty.isNumeric(operand)) return operand;
        if (Ty hooked = hookCall(operand, op == "-" ? "__neg__" : "__pos__", {}, node->span)) return hooked;
        error(node->span, "Operator '" + op + "' cannot be applied to " + show(operand)); return ty.error();
    }

    // Returns the hook result type, or null when the receiver has no such method.
    Ty hookCall(Ty receiver, const char *hook, const std::vector<Ty> &arguments, const Span &span) {
        if (receiver->tag != TypeTag::Named) return nullptr;
        const Member *member = findMember(receiver->info, hook);
        if (!member || !member->isMethod || member->isStatic) return nullptr;
        Ty up = ty.asSuper(receiver, member->owner); std::map<Ty, Ty> replacements;
        for (size_t index = 0; up && index < member->owner->params.size() && index < up->args.size(); index++) replacements[member->owner->params[index]] = up->args[index];
        Ty function = ty.substitute(member->signature->type, replacements);
        if (function->args.size() != arguments.size()) return nullptr;
        for (size_t index = 0; index < arguments.size(); index++) if (!assignable(arguments[index], function->args[index], span)) return ty.error();
        return function->result;
    }

    Ty binaryExpression(const Ast &node, Ty expected) {
        const std::string &op = node->text;
        if (op == "&&" || op == "||" || op == "and" || op == "or") { condition(node); return ty.boolean(); }
        if (op == "??") {
            Ty left = expr(node->children[0]);
            Ty right = expr(node->children[1], expected ? expected : (left->tag == TypeTag::Error ? nullptr : ty.stripNull(left)));
            if (left->tag == TypeTag::Error || right->tag == TypeTag::Error) return ty.error();
            if (!ty.isNullable(left)) { warn(node->span, "Left operand of ?? is never null"); return left; }
            return ty.join(ty.stripNull(left), right);
        }
        Ty left = expr(node->children[0]), right = expr(node->children[1]);
        bool comparison = op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
        if (left->tag == TypeTag::Error || right->tag == TypeTag::Error) return comparison ? ty.boolean() : ty.error();
        if (op == "==" || op == "!=") {
            Ty first = bounded(left), second = bounded(right);
            if (ty.isNumeric(first) && ty.isNumeric(second)) markNumeric(node, first, second, false);
            return ty.boolean();
        }
        left = bounded(left); right = bounded(right);
        if (comparison && ty.isNumeric(left) && ty.isNumeric(right)) markNumeric(node, left, right, false);
        if (!comparison && ty.isNumeric(left) && ty.isNumeric(right)) markNumeric(node, left, right, op == "/");
        if (comparison) {
            if (ty.isNumeric(left) && ty.isNumeric(right)) return ty.boolean();
            if (left == stringType && right == stringType) return ty.boolean();
            const char *hook = op == "<" ? "__lt__" : op == ">" ? "__gt__" : op == "<=" ? "__le__" : "__ge__";
            if (hookCall(left, hook, {right}, node->span)) return ty.boolean();
            error(node->span, "Cannot compare " + show(left) + " with " + show(right)); return ty.boolean();
        }
        if (ty.isNumeric(left) && ty.isNumeric(right)) {
            bool floating = left == ty.floating() || right == ty.floating();
            if (op == "/") return ty.floating();
            return floating ? ty.floating() : ty.integer();
        }
        if (op == "+") {
            if (left == stringType || right == stringType) return stringType;
            if (left == right && left == bytesType) return bytesType;
            if (left->tag == TypeTag::Named && left->info == listInfo && ty.isSubtype(right, left)) return left;
        }
        static const std::map<std::string, const char *> hooks{{"+", "__add__"}, {"-", "__sub__"}, {"*", "__mul__"}, {"/", "__div__"}, {"//", "__floordiv__"}, {"%", "__mod__"}, {"**", "__pow__"}};
        auto hook = hooks.find(op);
        if (hook != hooks.end()) if (Ty hooked = hookCall(left, hook->second, {right}, node->span)) return hooked;
        error(node->span, "Operator '" + op + "' cannot be applied to " + show(left) + " and " + show(right)); return ty.error();
    }

    Ty castExpression(const Ast &node) {
        Ty operand = expr(node->children[0]), target = resolveType(node->typeExpr);
        if (operand->tag == TypeTag::Error || target->tag == TypeTag::Error) return target;
        if (!ty.isSubtype(operand, target) && !ty.isSubtype(target, operand) && !ty.isSubtype(ty.stripNull(target), ty.stripNull(operand)) && ty.stripNull(operand)->tag != TypeTag::Object)
            error(node->span, "Cannot cast " + show(operand) + " to " + show(target));
        else if (target->tag == TypeTag::Named && !target->args.empty() && !ty.isSubtype(operand, target) &&
                 !std::all_of(target->args.begin(), target->args.end(), [&](Ty argument) { return ty.isSubtype(ty.nullable(ty.object()), argument); })) warn(node->span, "Unchecked cast to " + show(target) + "; only the erased type is checked at run time");
        return target;
    }

    Ty assignmentExpression(const Ast &node) {
        const Ast &target = node->children[0]; const std::string &op = node->text;
        Ty targetType = ty.error();
        if (target->kind == "name") {
            size_t index = 0; Symbol *symbol = lookup(target->text, &index);
            if (!symbol) error(target->span, "Undefined name '" + target->text + "'");
            else if (symbol->kind != SymbolKind::Value) error(target->span, "Cannot assign to '" + target->text + "'");
            else {
                if (!symbol->mutableBinding) error(target->span, "Cannot reassign '" + target->text + "'; it is not a var");
                else if (!functions.empty() && index < functions.back().scopeBase && !symbol->global) error(target->span, "Cannot assign to captured variable '" + target->text + "'");
                targetType = symbol->declared; target->resolved = targetType;
                Symbol *current = lookup(target->text); if (current) current->type = current->declared;
            }
        } else if (target->kind == "member") targetType = memberTarget(target);
        else if (target->kind == "index") targetType = indexTarget(target);
        else error(target->span, "Invalid assignment target");
        Ty value = expr(node->children[1], op == "=" ? targetType : nullptr);
        if (targetType->tag == TypeTag::Error || value->tag == TypeTag::Error) return ty.null();
        if (op == "=") { assignable(value, targetType, node->children[1]->span); return ty.null(); }
        Ty combined = compoundResult(op.substr(0, 1), targetType, value, node->span);
        if (ty.isNumeric(bounded(targetType)) && ty.isNumeric(bounded(value))) markNumeric(node, bounded(targetType), bounded(value), op == "/=");
        assignable(combined, targetType, node->span);
        return ty.null();
    }

    Ty compoundResult(const std::string &op, Ty left, Ty right, const Span &span) {
        left = bounded(left); right = bounded(right);
        if (op == "+" && left == bytesType && right == bytesType) return bytesType;
        if (op == "+" && left->tag == TypeTag::Named && left->info == listInfo && ty.isSubtype(right, left)) return left;
        if (ty.isNumeric(left) && ty.isNumeric(right)) return op == "/" ? ty.floating() : (left == ty.floating() || right == ty.floating() ? ty.floating() : ty.integer());
        if (op == "+" && left == stringType) return stringType;
        static const std::map<std::string, const char *> hooks{{"+", "__add__"}, {"-", "__sub__"}, {"*", "__mul__"}, {"/", "__div__"}};
        auto hook = hooks.find(op);
        if (hook != hooks.end()) if (Ty hooked = hookCall(left, hook->second, {right}, span)) return hooked;
        error(span, "Operator '" + op + "=' cannot be applied to " + show(left) + " and " + show(right)); return ty.error();
    }

    struct Resolved {
        Ty type = nullptr;
        std::shared_ptr<Signature> signature;
        bool found = false;
        ClassInfo *genericConstant = nullptr;
    };

    bool visible(const Member &member, const Span &span, const std::string &name) {
        if (member.visibility == Visibility::Public) return true;
        ClassInfo *current = functions.empty() ? nullptr : functions.back().owner;
        bool allowed = current && (current == member.owner || (member.visibility == Visibility::Protected && ty.asSuper(current->self, member.owner)));
        if (!allowed) error(span, "'" + name + "' is " + (member.visibility == Visibility::Private ? "private" : "protected") + " in " + member.owner->name);
        return allowed;
    }

    // The class a name or qualified path (Outer.Nested) refers to, when it denotes a type rather than a value.
    ClassInfo *classReference(const Ast &node) {
        if (node->kind == "name") { Symbol *symbol = lookup(node->text); return symbol && symbol->kind == SymbolKind::Class ? symbol->info : nullptr; }
        if (node->kind != "member") return nullptr;
        if (Symbol *exported = moduleMember(node, false)) return exported->kind == SymbolKind::Class ? exported->info : nullptr;
        ClassInfo *owner = classReference(node->children[0]);
        if (!owner) return nullptr;
        Symbol *nested = lookup(owner->name + "_" + node->text);
        if (!nested && (owner->name == "IO" || owner->name == "Collections")) nested = lookup(node->text);
        return nested && nested->kind == SymbolKind::Class ? nested->info : nullptr;
    }

    Resolved resolveMember(const Ast &node, bool forWrite = false) {
        Resolved result; const Ast &receiverNode = node->children[0];
        if (ClassInfo *owner = classReference(receiverNode)) {
            {
                const Member *member = findMember(owner, node->text);
                if (!member || !member->isStatic) { error(node->span, owner->name + " has no static member '" + node->text + "'"); return result; }
                visible(*member, node->span, node->text);
                result.found = true; result.signature = member->signature; result.type = member->signature ? member->signature->type : member->type;
                if (!member->signature && owner->kind == ClassKind::Enum && !owner->params.empty()) result.genericConstant = owner;
                if (forWrite && !member->mutableBinding) error(node->span, "Cannot assign to '" + node->text + "'");
                return result;
            }
        }
        Ty receiver = expr(receiverNode);
        if (receiver->tag == TypeTag::Error) return result;
        if (ty.isNullable(receiver)) {
            if (!node->safe) error(node->span, "Value of type " + show(receiver) + " may be null; use ?. or check it first");
            receiver = ty.stripNull(receiver);
        }
        while (receiver->tag == TypeTag::Var) receiver = receiver->result;
        if (receiver->tag == TypeTag::Intersection) {
            for (Ty part : receiver->args) { Ty candidate = part; while (candidate->tag == TypeTag::Var) candidate = candidate->result; if (candidate->tag == TypeTag::Named && findMember(candidate->info, node->text)) { receiver = candidate; break; } }
        }
        if (receiver->tag != TypeTag::Named) { error(node->span, show(receiver) + " has no member '" + node->text + "'"); return result; }
        const Member *member = findMember(receiver->info, node->text);
        if (!member || member->isStatic) { error(node->span, show(receiver) + " has no member '" + node->text + "'"); return result; }
        visible(*member, node->span, node->text);
        Ty up = ty.asSuper(receiver, member->owner); std::map<Ty, Ty> replacements;
        for (size_t index = 0; up && index < member->owner->params.size() && index < up->args.size(); index++) replacements[member->owner->params[index]] = up->args[index];
        result.found = true; result.signature = member->signature;
        result.type = ty.substitute(member->signature ? member->signature->type : member->type, replacements);
        if (member->signature && !replacements.empty()) {
            auto substituted = std::make_shared<Signature>(*member->signature); substituted->type = result.type; result.signature = substituted;
        }
        if (forWrite) {
            bool inConstructor = !functions.empty() && functions.back().constructor && functions.back().owner == member->owner && receiverNode->kind == "name" && receiverNode->text == "self";
            if (member->isMethod) error(node->span, "Cannot assign to method '" + node->text + "'");
            else if (!member->mutableBinding && !inConstructor) error(node->span, "Cannot assign to immutable field '" + node->text + "'");
        }
        return result;
    }

    Ty memberTarget(const Ast &node) {
        Resolved resolved = resolveMember(node, true);
        node->resolved = resolved.found ? resolved.type : ty.error();
        return node->resolved;
    }

    Ty memberExpression(const Ast &node, Ty expected) {
        Resolved resolved = resolveMember(node);
        if (!resolved.found) return ty.error();
        if (resolved.genericConstant) {
            Ty wanted = hint(expected, resolved.genericConstant);
            if (wanted) return wanted;
            error(node->span, "Cannot infer the type arguments of " + node->text + "; add a type annotation"); return ty.error();
        }
        if (node->safe) return ty.nullable(resolved.type);
        return resolved.type;
    }

    Ty indexResult(const Ast &node, Ty receiver, Ty index, bool write, Ty value) {
        (void)write; (void)value;
        if (receiver->tag == TypeTag::Error || index->tag == TypeTag::Error) return ty.error();
        if (receiver->tag == TypeTag::Named) {
            ClassInfo *info = receiver->info;
            if (info == listInfo || info == tupleInfo) { assignable(index, ty.integer(), node->children[1]->span); return receiver->args[0]; }
            if (info == mapInfo) { assignable(index, receiver->args[0], node->children[1]->span); return receiver->args[1]; }
            if (receiver == stringType) { assignable(index, ty.integer(), node->children[1]->span); return stringType; }
            if (receiver == bytesType) { assignable(index, ty.integer(), node->children[1]->span); return ty.integer(); }
            if (Ty hooked = hookCall(receiver, "__getitem__", {index}, node->span)) return hooked;
        }
        error(node->span, show(receiver) + " cannot be indexed with " + show(index)); return ty.error();
    }

    Ty indexExpression(const Ast &node) {
        Ty receiver = expr(node->children[0]);
        if (ty.isNullable(receiver)) { error(node->span, "Value of type " + show(receiver) + " may be null"); receiver = ty.stripNull(receiver); }
        return indexResult(node, receiver, expr(node->children[1]), false, nullptr);
    }

    Ty indexTarget(const Ast &node) {
        Ty receiver = expr(node->children[0]);
        if (ty.isNullable(receiver)) { error(node->span, "Value of type " + show(receiver) + " may be null"); receiver = ty.stripNull(receiver); }
        Ty type = indexResult(node, receiver, expr(node->children[1]), true, nullptr);
        if (receiver == stringType || receiver == bytesType || (receiver->tag == TypeTag::Named && receiver->info == tupleInfo)) { error(node->span, show(receiver) + " is immutable"); return ty.error(); }
        node->resolved = type; return type;
    }

    Ty sliceExpression(const Ast &node) {
        Ty receiver = expr(node->children[0]);
        for (size_t index = 1; index < 4; index++) if (node->children[index]) assignable(expr(node->children[index]), ty.integer(), node->children[index]->span);
        if (receiver->tag == TypeTag::Error) return receiver;
        bool sliceable = receiver == stringType || receiver == bytesType || (receiver->tag == TypeTag::Named && (receiver->info == listInfo || receiver->info == tupleInfo));
        if (!sliceable) { error(node->span, show(receiver) + " cannot be sliced"); return ty.error(); }
        return receiver;
    }

    Ty listExpression(const Ast &node, Ty expected) {
        ClassInfo *info = node->kind == "list" ? listInfo : tupleInfo;
        Ty wanted = hint(expected, info); Ty element = wanted && !wanted->args.empty() ? wanted->args[0] : nullptr;
        if (element && element->tag == TypeTag::Wildcard) element = element->result;
        Ty joined = nullptr;
        for (const auto &child : node->children) {
            Ty type = expr(child, element);
            if (type->tag == TypeTag::Error) { joined = ty.error(); continue; }
            if (!element && joined != ty.error()) joined = joined ? ty.join(joined, type) : type;
            if (element) assignable(type, element, child->span);
        }
        if (element) return ty.named(info, {element});
        if (!joined) {
            if (node->kind == "list") error(node->span, "Cannot infer the element type of an empty list; add a type annotation");
            else return ty.named(info, {ty.never()});
            return ty.error();
        }
        return joined->tag == TypeTag::Error ? joined : ty.named(info, {joined});
    }

    Ty mapExpression(const Ast &node, Ty expected) {
        Ty wanted = hint(expected, mapInfo); Ty keyType = wanted ? wanted->args[0] : nullptr, valueType = wanted ? wanted->args[1] : nullptr;
        Ty keys = nullptr, values = nullptr; bool failed = false;
        for (size_t index = 0; index + 1 < node->children.size(); index += 2) {
            Ty key = expr(node->children[index], keyType), value = expr(node->children[index + 1], valueType);
            if (key->tag == TypeTag::Error || value->tag == TypeTag::Error) { failed = true; continue; }
            if (keyType) assignable(key, keyType, node->children[index]->span); else keys = keys ? ty.join(keys, key) : key;
            if (valueType) assignable(value, valueType, node->children[index + 1]->span); else values = values ? ty.join(values, value) : value;
        }
        if (failed) return ty.error();
        if (keyType && valueType) return ty.named(mapInfo, {keyType, valueType});
        if (!keys) { error(node->span, "Cannot infer the types of an empty map; add a type annotation"); return ty.error(); }
        return ty.named(mapInfo, {keys, values});
    }

    Ty callExpression(const Ast &node, Ty expected) {
        const Ast &callee = node->children[0];
        if (callee->kind == "name") {
            Symbol *symbol = lookup(callee->text);
            if (symbol && symbol->kind == SymbolKind::Class) return construct(node, symbol->info, callee->typeArgs, expected);
            if (symbol && symbol->kind == SymbolKind::Function) {
                callee->resolved = symbol->signature->type;
                if (!symbol->native.empty()) node->target = symbol->native;
                return applyCall(node, symbol->signature->type, symbol->signature, callee->typeArgs, expected);
            }
        }
        if (callee->kind == "member" && isModuleAccess(callee)) {
            Symbol *symbol = moduleMember(callee, true);
            if (!symbol) { for (size_t index = 1; index < node->children.size(); index++) expr(node->children[index]); return ty.error(); }
            if (symbol->kind == SymbolKind::Class) return construct(node, symbol->info, callee->typeArgs, expected);
            if (symbol->kind == SymbolKind::Function) { callee->resolved = symbol->signature->type; return applyCall(node, symbol->signature->type, symbol->signature, callee->typeArgs, expected); }
        }
        if (callee->kind == "member" && !isModuleAccess(callee)) {
            Resolved resolved = resolveMember(callee);
            if (!resolved.found) { for (size_t index = 1; index < node->children.size(); index++) expr(node->children[index]); return ty.error(); }
            callee->resolved = resolved.type;
            if (ClassInfo *owner = classReference(callee->children[0])) {
                if (owner->builtin && owner->name.find('_') == std::string::npos && resolved.signature && !callee->safe) node->target = owner->name + "." + callee->text;
            } else if (Ty receiverType = callee->children[0]->resolved; receiverType && receiverType->tag == TypeTag::Named && receiverType->info->builtin && resolved.signature && !callee->safe)
                node->target = "." + callee->text;
            if (resolved.type->tag != TypeTag::Function) { error(node->span, "'" + callee->text + "' is not callable"); return ty.error(); }
            Ty result = applyCall(node, resolved.type, resolved.signature, callee->typeArgs, expected);
            return callee->safe ? ty.nullable(result) : result;
        }
        Ty type = expr(callee);
        if (type->tag == TypeTag::Error) { for (size_t index = 1; index < node->children.size(); index++) expr(node->children[index]); return type; }
        if (ty.isNullable(type)) { error(node->span, "Cannot call a value of type " + show(type) + " that may be null"); type = ty.stripNull(type); }
        if (type->tag != TypeTag::Function) { error(node->span, show(type) + " is not callable"); return ty.error(); }
        return applyCall(node, type, nullptr, {}, expected);
    }

    Ty construct(const Ast &node, ClassInfo *info, const std::vector<TypeAst> &typeArguments, Ty expected) {
        if (info->kind == ClassKind::Trait || info->kind == ClassKind::Enum) { error(node->span, info->name + " cannot be constructed directly"); return ty.error(); }
        Signature *constructor = constructorOf(info);
        std::shared_ptr<Signature> shared = info->constructor;
        (void)constructor;
        return applyCall(node, shared->type, shared, typeArguments, expected);
    }

    static std::string dottedToNested(std::string name) { std::replace(name.begin(), name.end(), '.', '_'); return name; }

    // `match value { n: Int => ..., s: String? => ..., _ => ... }`: each arm tests the run-time type and narrows the bound name.
    Ty typeMatch(const Ast &node, Ty subject, Ty expected) {
        Ty remaining = subject, result = nullptr; bool wildcard = false;
        for (size_t index = 1; index < node->children.size(); index++) {
            const Ast &arm = node->children[index];
            pushScope();
            if (arm->type.empty() && arm->text != "_") error(arm->span, "Type patterns cannot be mixed with enum variant patterns");
            else if (arm->type.empty()) {
                if (wildcard) error(arm->span, "Unreachable match arm after '_'");
                wildcard = true;
            } else {
                Ty target = resolveType(arm->typeExpr);
                if (target->tag == TypeTag::Named && !target->args.empty()) for (Ty argument : target->args)
                    if (argument->tag != TypeTag::Wildcard || argument->wildcard != WildcardBound::None) { error(arm->span, "Type arguments are erased at run time; match against " + target->info->name + "<?> instead"); break; }
                if (wildcard) error(arm->span, "Unreachable match arm after '_'");
                else if (target->tag != TypeTag::Error) {
                    if (!ty.isSubtype(target, subject) && !ty.isSubtype(subject, target) && ty.stripNull(subject)->tag != TypeTag::Object)
                        error(arm->span, show(subject) + " can never be " + show(target));
                    else if (remaining == ty.never()) error(arm->span, "Unreachable match arm; " + show(target) + " is already covered");
                    if (!arm->names.empty()) declareValue(arm->names[0], narrowTo(subject, target), false, arm->span);
                    remaining = excludeType(remaining, target);
                }
            }
            Ty value = expr(arm->children[0], expected);
            popScope();
            if (expected) assignable(value, expected, arm->children[0]->span);
            result = result ? ty.join(result, value) : value;
        }
        if (!wildcard && remaining != ty.never()) error(node->span, "match is not exhaustive; " + show(remaining) + " is not covered (add a '_' arm)");
        return expected && result && ty.isSubtype(result, expected) ? expected : (result ? result : ty.never());
    }

    Ty matchExpression(const Ast &node, Ty expected) {
        Ty subject = expr(node->children[0]);
        if (subject->tag == TypeTag::Error) { for (size_t index = 1; index < node->children.size(); index++) expr(node->children[index]->children[0]); return subject; }
        bool typed = false; for (size_t index = 1; index < node->children.size(); index++) if (!node->children[index]->type.empty()) typed = true;
        if (typed) return typeMatch(node, subject, expected);
        if (subject->tag != TypeTag::Named || subject->info->kind != ClassKind::Enum) { error(node->span, "match requires an enum value, got " + show(subject)); return ty.error(); }
        ClassInfo *info = subject->info; std::map<Ty, Ty> replacements;
        for (size_t index = 0; index < info->params.size() && index < subject->args.size(); index++) replacements[info->params[index]] = subject->args[index];
        std::set<std::string> covered; bool wildcard = false; Ty result = nullptr;
        for (size_t index = 1; index < node->children.size(); index++) {
            const Ast &arm = node->children[index];
            pushScope();
            if (arm->text == "_") wildcard = true;
            else {
                size_t dot = arm->text.rfind('.'); std::string variantName = arm->text.substr(dot == std::string::npos ? 0 : dot + 1);
                std::string owner = dot == std::string::npos ? "" : arm->text.substr(0, dot);
                const Variant *found = nullptr; for (const auto &variant : info->variants) if (variant.name == variantName) found = &variant;
                if (!found || (!owner.empty() && owner.substr(owner.rfind('.') == std::string::npos ? 0 : owner.rfind('.') + 1) != info->name && dottedToNested(owner) != info->name)) error(arm->span, "'" + arm->text + "' is not a variant of " + info->name);
                else {
                    if (!covered.insert(variantName).second) error(arm->span, "Duplicate match arm '" + variantName + "'");
                    if (found->payload.size() != arm->names.size()) error(arm->span, "Pattern binds " + std::to_string(arm->names.size()) + " value(s) but " + variantName + " carries " + std::to_string(found->payload.size()));
                    for (size_t position = 0; position < arm->names.size() && position < found->payload.size(); position++) declareValue(arm->names[position], ty.substitute(found->payload[position], replacements), false, arm->span);
                }
            }
            Ty value = expr(arm->children[0], expected);
            popScope();
            if (expected) assignable(value, expected, arm->children[0]->span);
            result = result ? ty.join(result, value) : value;
        }
        if (!wildcard) for (const auto &variant : info->variants) if (!covered.count(variant.name)) error(node->span, "match is not exhaustive; missing " + info->name + "." + variant.name);
        return expected && result && ty.isSubtype(result, expected) ? expected : (result ? result : ty.never());
    }
};

}

namespace {
void declareHosts(Typer &typer, const std::vector<HostModule> &hosts) {
    for (const auto &host : hosts) {
        std::string stub = "class " + host.name + " {\n", problem;
        for (const auto &entry : host.exports) stub += "static fn " + entry.first + entry.second + "\n";
        typer.declareHost(stub + "}\n", problem);  // an invalid signature only hides that module
    }
}
}

std::vector<Diagnostic> typeCheck(const Ast &program, const ModuleLoader &loader, std::map<std::string, Ast> *modules, const std::vector<HostModule> &hosts) {
    Typer typer; typer.setLoader(loader); declareHosts(typer, hosts); typer.check(program);
    if (modules) *modules = typer.loadedPrograms();
    return typer.diagnostics;
}

struct TypeSession::Impl {
    Typer typer;
    std::map<std::string, std::vector<std::string>> namespaces;  // namespace -> member stubs
};

TypeSession::TypeSession(ModuleLoader loader) : impl(std::make_unique<Impl>()) { impl->typer.setLoader(std::move(loader)); }
TypeSession::~TypeSession() = default;
std::vector<Diagnostic> TypeSession::check(const Ast &program, bool allowRedeclaration) { return impl->typer.checkSubmission(program, allowRedeclaration); }
bool TypeSession::declareNative(const std::string &name, const std::string &signature, std::string &problem) {
    size_t dot = name.find('.');
    if (dot == std::string::npos) return impl->typer.declareHost("fn " + name + signature + "\n", problem);
    if (name.find('.', dot + 1) != std::string::npos) { problem = "Native names support one namespace level"; return false; }
    std::string space = name.substr(0, dot);
    auto members = impl->namespaces[space];
    members.push_back("static fn " + name.substr(dot + 1) + signature + "\n");
    std::string stub = "class " + space + " {\n";
    for (const auto &member : members) stub += member;
    stub += "}\n";
    if (!impl->typer.declareHost(stub, problem)) return false;
    impl->namespaces[space] = std::move(members); return true;
}
bool TypeSession::declareModule(const std::string &name, const std::vector<std::pair<std::string, std::string>> &exports, std::string &problem) {
    std::vector<std::string> members;
    std::string stub = "class " + name + " {\n";
    for (const auto &entry : exports) { members.push_back("static fn " + entry.first + entry.second + "\n"); stub += members.back(); }
    stub += "}\n";
    if (!impl->typer.declareHost(stub, problem)) return false;
    impl->namespaces[name] = std::move(members); return true;
}
Ast TypeSession::module(const std::string &name) const {
    auto found = impl->typer.loadedPrograms().find(name);
    return found == impl->typer.loadedPrograms().end() ? nullptr : found->second;
}

namespace {
Ast memberAt(const Ast &node, size_t position) {
    if (!node) return {};
    for (const auto &child : node->children) if (Ast nested = memberAt(child, position)) return nested;
    if (node->kind == "member" && position >= node->span.offset && position <= node->span.offset + node->span.length) return node;
    return {};
}
}

std::vector<CompletionItem> completeMembers(const std::shared_ptr<const Source> &source, size_t offset, const ModuleLoader &loader, const std::vector<HostModule> &hosts) {
    offset = std::min(offset, source->text.size()); size_t start = offset;
    while (start && (std::isalnum(static_cast<unsigned char>(source->text[start - 1])) || source->text[start - 1] == '_')) start--;
    if (!start || source->text[start - 1] != '.') return {};
    std::string edited = source->text; edited.insert(start, "__member_probe");
    Ast parsed;
    try { parsed = parse(std::make_shared<Source>(Source{source->file, edited})); } catch (const Error &) { return {}; }
    Ast access = memberAt(parsed, start); if (!access) return {};
    Typer typer; typer.setLoader(loader); declareHosts(typer, hosts);
    try { typer.check(parsed); } catch (const Error &) { return {}; }
    return typer.completionsFor(access->children[0], parsed, start);
}

std::string hoverText(const std::shared_ptr<const Source> &source, size_t offset, const ModuleLoader &loader, const std::vector<HostModule> &hosts) {
    Ast parsed; std::vector<Token> tokens;
    try { parsed = parse(source); tokens = lex(source); } catch (const Error &) { return {}; }
    size_t index = 0; bool found = false;
    for (; index < tokens.size(); index++)
        if (tokens[index].kind == TokenKind::Name && offset >= tokens[index].span.offset && offset <= tokens[index].span.offset + tokens[index].span.length) { found = true; break; }
    if (!found) return {};
    const Token &token = tokens[index];
    Typer typer; typer.setLoader(loader); declareHosts(typer, hosts);
    try { typer.check(parsed); } catch (const Error &) { return {}; }
    if (index && tokens[index - 1].text == ".") {
        Ast access = memberAt(parsed, token.span.offset);
        while (access && access->text != token.text) { access = nullptr; }
        if (!access) return {};
        return typer.memberHover(access->children[0], token.text, parsed, token.span.offset);
    }
    static const std::set<std::string> declarationKeywords{"let", "var", "fn", "class", "record", "trait", "enum", "type"};
    if (index && declarationKeywords.count(tokens[index - 1].text)) {
        std::string text = typer.declarationHover(parsed, token.text, token.span.offset);
        if (!text.empty()) return text;
    }
    Ast name;
    walk(parsed, [&](const Ast &node) {
        if (node->kind == "name" && node->text == token.text && offset >= node->span.offset && offset <= node->span.offset + node->span.length) name = node;
        return true;
    });
    if (name) { std::string text = typer.nameHover(name); if (!text.empty()) return text; }
    return typer.declarationHover(parsed, token.text, token.span.offset);
}

std::string formatDiagnostic(const Diagnostic &diagnostic, bool color) {
    return diagnose(Error(diagnostic.span, diagnostic.message, diagnostic.warning ? "warning" : "type"), color);
}

}
