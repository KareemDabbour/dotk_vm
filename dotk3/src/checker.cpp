#include "checker.hpp"
#include "core.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <optional>

namespace dk3 {
namespace {
struct Symbol { std::string type; bool mutableBinding; bool initialized; Ast declaration; std::string coreSpace = {}; };
class Checker {
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
    std::unordered_set<std::string> types{"Object", "Int", "Float", "Bool", "String", "Bytes", "Null", "Any", "List", "Tuple", "Map", "Set", "Function", "Generator", "Task", "Resource", "Error"};
    int functionDepth = 0;
    int loopDepth = 0;
    std::string returnType;
    bool initializing = false;
    Ast accessClass;
    std::unordered_map<const Node *, std::vector<Ast>> implementations;
    std::unordered_map<const Node *, std::vector<Ast>> addedMethods;
    std::unordered_map<const Node *, const Node *> defaultOwners;
    std::unordered_map<const Node *, Ast> bases;
    std::vector<Parameter> fields(const Ast &owner) {
        std::vector<Parameter> result;
        if (bases[owner.get()]) result = fields(bases[owner.get()]);
        result.insert(result.end(), owner->parameters.begin(), owner->parameters.end()); return result;
    }
    Ast declaringOwner(const Ast &owner, const std::string &name) {
        for (Ast current = owner; current; current = bases[current.get()]) {
            for (const auto &field : current->parameters) if (field.name == name) return current;
            for (const auto &entry : current->children) if (entry->text == name) return current;
            for (const auto &entry : addedMethods[current.get()]) if (entry->text == name) return current;
        }
        return owner;
    }
    void requireAccess(Visibility visibility, const Ast &owner, const Span &span) {
        if (visibility == Visibility::Public || accessClass == owner) return;
        if (visibility == Visibility::Protected) for (Ast current = accessClass; current; current = bases[current.get()]) if (current == owner) return;
        throw Error(span, visibility == Visibility::Private ? "Private member is not accessible" : "Protected member is not accessible", "access");
    }
    Ast staticEntry(const Ast &owner, const std::string &name) {
        for (const auto &entry : owner->children) if (entry->text == name && entry->staticMember) return entry;
        return bases[owner.get()] ? staticEntry(bases[owner.get()], name) : Ast{};
    }
    Ast method(const Ast &owner, const std::string &name, bool staticMember = false) {
        for (const auto &entry : owner->children) if (entry->text == name && entry->kind != "binding" && entry->staticMember == staticMember) return entry;
        for (const auto &entry : addedMethods[owner.get()]) if (entry->text == name) return entry;
        return bases[owner.get()] ? method(bases[owner.get()], name, staticMember) : Ast{};
    }
    Ast declaration(const std::string &name) {
        if (name == "Error.Kind" || name == "System.Error.Kind") return errorKindDeclaration();
        size_t separator = name.find('.');
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            if (separator != std::string::npos) {
                auto space = scope->find(name.substr(0, separator));
                if (space != scope->end()) return space->second.coreSpace == "Collections" ? (name.substr(separator + 1) == "Sized" ? sizedDeclaration() : collectionDeclaration(name.substr(separator + 1))) : space->second.coreSpace == "System" && name.substr(separator + 1) == "Object" ? collectionDeclaration("Object") : Ast{};
            }
            auto found = scope->find(name);
            if (found != scope->end()) {
                Ast node = found->second.declaration;
                if (!node) return collectionDeclaration(name);
                if (node && node->kind == "from_import" && node->text == "Collections")
                    for (size_t index = 0; index < node->names.size(); index += 2)
                        if (node->names[index + 1] == name) return node->names[index] == "Sized" ? sizedDeclaration() : collectionDeclaration(node->names[index]);
                if (node && node->kind == "from_import" && node->text == "System")
                    for (size_t index = 0; index < node->names.size(); index += 2) if (node->names[index + 1] == name && node->names[index] == "Object") return collectionDeclaration("Object");
                return node && (node->kind == "record" || node->kind == "class" || node->kind == "trait" || node->kind == "enum") ? node : Ast{};
            }
        }
        return collectionDeclaration(name);
    }
    std::string collectionBase(const std::string &type) {
        std::string name = baseType(type); Ast owner = declaration(name);
        for (Ast current = owner; current; current = bases[current.get()]) if (current->builtinCollection) return current->text;
        return name;
    }
    std::string baseType(std::string type) {
        if (!type.empty() && type.back() == '?') type.pop_back();
        std::string name = type.substr(0, type.find('[')); Ast owner = declaration(name);
        return owner && owner->builtinCollection ? owner->text : name;
    }
    Ast variant(const Ast &owner, const std::string &name, const Span &span) {
        for (const Ast &child : owner->children) if (child->text == name) return child;
        throw Error(span, "Unknown variant '" + owner->text + "." + name + "'", "name");
    }
    Symbol &lookup(const std::string &name, const Span &span) {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            auto found = scope->find(name);
            if (found != scope->end()) {
                if (!found->second.initialized && !functionDepth) throw Error(span, "Read before declaration '" + name + "'", "name");
                return found->second;
            }
        }
        throw Error(span, "Undeclared name '" + name + "'", "name");
    }
    void compatible(const std::string &actual, const std::string &expected, const Span &span) {
        if (expected.empty() || actual == "Any" || expected == "Any") return;
        if (baseType(actual) == "Any") return;
        bool nullable = expected.back() == '?';
        if (actual == "Null" && nullable) return;
        if (baseType(expected) == "Object") { if (actual == "Null" || (!actual.empty() && actual.back() == '?' && !nullable)) throw Error(span, "Object requires a non-null value", "type"); return; }
        auto expectedParts = typeIntersections(expected), actualParts = typeIntersections(actual);
        if (!expectedParts.empty()) {
            if (!actual.empty() && actual.back() == '?' && !nullable) throw Error(span, "Nullable value cannot satisfy intersection", "type");
            std::string candidate = actual;
            if (nullable && !candidate.empty() && candidate.back() == '?') candidate.pop_back();
            for (const auto &part : expectedParts) compatible(candidate, part, span);
            return;
        }
        if (baseType(expected) == "Any") return;
        Ast required = declaration(baseType(expected));
        if (collectionBase(actual) == baseType(expected) && declaration(baseType(actual)) && declaration(baseType(actual))->kind == "class") return;
        if (required && required->kind == "class") {
            if (!actual.empty() && actual.back() == '?' && !nullable) throw Error(span, "Nullable value cannot satisfy class", "type");
            for (Ast candidate = declaration(baseType(actual)); candidate; candidate = bases[candidate.get()]) if (candidate == required) return;
        }
        for (Ast candidate = declaration(baseType(actual)); candidate && candidate->kind == "class"; candidate = bases[candidate.get()])
            if (!candidate->baseClass.empty() && candidate->baseClass == baseType(expected)) return;
        if (required && required->kind == "trait") {
            if (!actual.empty() && actual.back() == '?' && !nullable) throw Error(span, "Nullable value cannot satisfy trait", "type");
            for (const auto &part : actualParts) if (declaration(baseType(part)) == required) return;
            if (declaration(baseType(actual)) == required) return;
            std::function<bool(const std::string &)> contains = [&](const std::string &candidate) {
                if (declaration(baseType(candidate)) == required) return true;
                for (const auto &part : typeIntersections(candidate)) if (contains(part)) return true;
                return false;
            };
            if (contains(actual)) return;
            if (required == sizedDeclaration() && std::unordered_set<std::string>{"List", "Tuple", "Map", "String", "Bytes"}.count(baseType(actual))) return;
            if (actual.find('.') != std::string::npos) return;
            Ast owner = declaration(baseType(actual));
            for (Ast candidate = owner; candidate; candidate = bases[candidate.get()])
                for (const auto &trait : implementations[candidate.get()]) if (trait == required) return;
            throw Error(span, "Type '" + actual + "' does not implement trait '" + expected + "'", "type");
        }
        if ((!actual.empty() && actual.back() == '?' && !nullable) || baseType(actual) != baseType(expected))
            throw Error(span, "Expected " + expected + ", inferred " + actual, "type");
    }
    void annotation(const std::string &type, const Span &span) {
        if (type.empty()) return;
        if (type == "Error.Kind" || type == "Error.Kind?" || type == "System.Error.Kind" || type == "System.Error.Kind?") return;
        auto parts = typeIntersections(type);
        if (!parts.empty()) {
            std::unordered_set<std::string> methods;
            for (const auto &part : parts) {
                annotation(part, span); Ast trait = declaration(baseType(part));
                if ((!part.empty() && part.back() == '?') || (trait && trait->kind != "trait") || (!trait && types.count(baseType(part))))
                    throw Error(span, "Intersections require non-null trait components", "type");
                if (trait) for (const auto &entry : trait->children) methods.insert(entry->text);
            }
            for (const auto &name : methods) intersectionMethod(type, name, span);
            return;
        }
        std::string text = type;
        if (text.back() == '?') text.pop_back();
        size_t generic = text.find('['); std::string base = baseType(text);
        if (generic != std::string::npos) {
            if (base != "List" && base != "Tuple" && base != "Map" && base != "Set") throw Error(span, "Unsupported generic annotation '" + type + "'", "type");
            auto arguments = typeArguments(text); size_t expected = base == "Map" ? 2 : 1;
            if (arguments.size() != expected) throw Error(span, base + " requires " + std::to_string(expected) + " type arguments", "type");
            for (const auto &argument : arguments) annotation(argument, span);
            if (base == "Map" && !std::unordered_set<std::string>{"Any", "Int", "Float", "Bool", "Null", "String", "Bytes"}.count(baseType(arguments[0])))
                throw Error(span, "Map key annotations must be primitive or Any", "type");
        }
        size_t separator = base.find('.');
        if (separator != std::string::npos) {
            Symbol &space = lookup(base.substr(0, separator), span);
            if (space.type != "Module" && space.type != "Any") throw Error(span, "Qualified annotation requires a module namespace", "type");
            return;
        }
        if (!types.count(base) && !declaration(base)) {
            Symbol &symbol = lookup(base, span);
            if (!symbol.declaration || symbol.declaration->kind != "from_import") throw Error(span, "Unknown annotation '" + type + "'", "type");
        }
    }
    Ast intersectionMethod(const std::string &type, const std::string &name, const Span &span) {
        Ast result;
        for (const auto &part : typeIntersections(type)) {
            Ast trait = declaration(baseType(part));
            Ast candidate = trait ? method(trait, name) : intersectionMethod(part, name, span);
            if (!candidate) continue;
            if (result) {
                if (result->parameters.size() != candidate->parameters.size() || !signatureType(result->type, candidate->type))
                    throw Error(span, "Conflicting intersection method '" + name + "'", "type");
                for (size_t position = 0; position < result->parameters.size(); position++)
                    if (result->parameters[position].name != candidate->parameters[position].name || !signatureType(result->parameters[position].type, candidate->parameters[position].type))
                        throw Error(span, "Conflicting intersection method '" + name + "'", "type");
            }
            result = candidate;
        }
        return result;
    }
    Ast callbackSignature(const Ast &callback) {
        if (callback->kind == "function") return callback;
        if (callback->kind == "name") {
            const auto &symbol = lookup(callback->text, callback->span);
            if (symbol.declaration && symbol.declaration->kind == "function") return symbol.declaration;
            if (!symbol.mutableBinding && symbol.declaration && symbol.declaration->kind == "binding" &&
                !symbol.declaration->children.empty() && symbol.declaration->children[0]->kind == "function") return symbol.declaration->children[0];
        }
        if (callback->kind == "member") {
            std::string receiver = expression(callback->children[0]);
            if (auto signature = intersectionMethod(receiver, callback->text, callback->span)) return signature;
            Ast owner = declaration(receiver); if (owner) return method(owner, callback->text);
        }
        return {};
    }
    void literalContract(const Ast &node, const std::string &type) {
        auto arguments = typeArguments(type);
        if (arguments.empty()) return;
        std::string base = baseType(type);
        if ((base == "List" || base == "Tuple") && (node->kind == "list" || node->kind == "tuple")) {
            for (const auto &child : node->children) { compatible(expression(child), arguments[0], child->span); literalContract(child, arguments[0]); }
        } else if (base == "Map" && node->kind == "map" && arguments.size() == 2) {
            for (size_t index = 0; index < node->children.size(); index++) {
                const auto &child = node->children[index]; const auto &expected = arguments[index % 2];
                compatible(expression(child), expected, child->span); literalContract(child, expected);
            }
        }
    }
    void arguments(const Ast &call, const Ast &signature, const std::vector<std::string> &actual) {
        if (signature->kind == "variant") {
            if (actual.size() != signature->names.size()) throw Error(call->span, "Enum payload arity mismatch", "call");
            for (size_t index = 0; index < actual.size(); index++) {
                if (!call->names[index].empty()) throw Error(call->children[index + 1]->span, "Enum payloads require positional arguments", "call");
                compatible(actual[index], signature->names[index], call->children[index + 1]->span);
            }
            return;
        }
        const auto &parameters = signature->parameters;
        std::vector<bool> supplied(parameters.size(), false); size_t positional = 0;
        for (size_t index = 0; index < actual.size(); index++) {
            size_t target = parameters.size();
            if (call->names[index].empty()) {
                if (positional < parameters.size()) target = positional++;
                else if (!parameters.empty() && parameters.back().rest) continue;
            } else {
                for (size_t parameter = 0; parameter < parameters.size(); parameter++)
                    if (parameters[parameter].name == call->names[index] && !parameters[parameter].rest) target = parameter;
            }
            if (target == parameters.size()) throw Error(call->children[index + 1]->span,
                call->names[index].empty() ? "Too many positional arguments" : "Unknown named argument '" + call->names[index] + "'", "call");
            if (parameters[target].rest) continue;
            if (supplied[target]) throw Error(call->children[index + 1]->span, "Argument supplied more than once: '" + parameters[target].name + "'", "call");
            supplied[target] = true; compatible(actual[index], parameters[target].type, call->children[index + 1]->span);
            literalContract(call->children[index + 1], parameters[target].type);
        }
        for (size_t parameter = 0; parameter < parameters.size(); parameter++)
            if (!supplied[parameter] && !parameters[parameter].rest && !parameters[parameter].defaultValue)
                throw Error(call->span, "Missing argument '" + parameters[parameter].name + "'", "call");
    }
    std::string merged(const std::string &first, const std::string &second) {
        if (first == second) return first;
        if (first == "Any" || second == "Any") return "Any";
        if (first == "Null") return second.back() == '?' ? second : second + '?';
        if (second == "Null") return first.back() == '?' ? first : first + '?';
        if (baseType(first) == baseType(second)) return baseType(first) + (first.back() == '?' || second.back() == '?' ? "?" : "");
        return "Any";
    }
    std::string randomCall(const Ast &node, const CoreMember &member, const std::vector<std::string> &actual) {
        for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Random functions require positional arguments", "call");
        std::string name = member.name;
        if (name == "seed" || name == "int") for (size_t position = 0; position < actual.size(); position++)
            compatible(actual[position], "Int", node->children[position + 1]->span);
        if (name == "shuffle") compatible(actual[0], "List", node->children[1]->span);
        if (name == "choice" && !std::unordered_set<std::string>{"Any", "List", "Tuple", "String", "Bytes"}.count(baseType(actual[0])))
            throw Error(node->span, "choice requires a sequence", "type");
        return randomResult(member, actual.empty() ? "Any" : actual[0]);
    }
    std::string requestsCall(const Ast &node, const CoreMember &member, const std::vector<std::string> &actual) {
        for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Requests functions require positional arguments", "call");
        std::string name = member.name; size_t strings = name == "request" ? 2 : name == "json" ? 0 : 1;
        for (size_t position = 0; position < actual.size(); position++)
            compatible(actual[position], position < strings ? "String" : "Map?", node->children[position + 1]->span);
        return member.result;
    }
    std::string networkCall(const Ast &node, const CoreMember &member, const std::vector<std::string> &actual) {
        for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Network functions require positional arguments", "call");
        std::string space = member.space, name = member.name;
        std::vector<std::string> expected;
        if (space == "Sockets") {
            if (name == "listen" || name == "connect") expected = {"String", "Int", "Int"};
            else if (name == "receive") expected = {"Resource?", "Int", "Int"};
            else if (name == "send") expected = {"Resource?", "Body", "Int"};
            else expected = {"Resource?", "Int"};
        } else if (name == "writeRequest") expected = {"Resource?", "String", "String", "Body", "Int"};
        else if (name == "writeResponse") expected = {"Resource?", "Int", "Body", "String", "Int"};
        else expected = {"Resource?", "Int"};
        for (size_t position = 0; position < actual.size(); position++) {
            const Span &span = node->children[position + 1]->span;
            if (expected[position] == "Body") {
                if (actual[position] != "Any" && actual[position] != "String" && actual[position] != "Bytes") throw Error(span, "Expected String or Bytes", "type");
            } else {
                if (expected[position] == "Resource?" && actual[position] == "Null") throw Error(span, "Expected non-null Resource", "type");
                compatible(actual[position], expected[position], span);
            }
        }
        return member.result;
    }
    std::optional<std::string> operatorResult(const std::string &operation, const std::string &receiver, const std::vector<std::string> &inputs, const Ast &node) {
        const auto *spec = operatorHook(operation); Ast owner = declaration(baseType(receiver));
        if (!spec || !owner) return {};
        Ast signature = method(owner, spec->name); if (!signature) return {};
        Ast invocation = std::make_shared<Node>(*node); invocation->names.assign(inputs.size(), "");
        invocation->children.resize(inputs.size() + 1, node);
        if (!inputs.empty()) invocation->children[1] = node->children.back();
        arguments(invocation, signature, inputs);
        std::string result = callbackResult(signature);
        if (spec->predicate) { compatible(result, "Bool", node->span); return "Bool"; }
        return result;
    }
    std::string expression(const Ast &node) {
        if (!node) return "Null";
        if (node->kind == "format") { for (const auto &part : node->children) expression(part); return "String"; }
        if (node->kind == "literal") {
            if (node->literalKind == TokenKind::Integer) return "Int";
            if (node->literalKind == TokenKind::Float) return "Float";
            if (node->literalKind == TokenKind::String) return "String";
            if (node->literalKind == TokenKind::Bytes) return "Bytes";
            return node->text == "null" ? "Null" : "Bool";
        }
        if (node->kind == "name") return lookup(node->text, node->span).type;
        if (node->kind == "function") { function(node); return "Function"; }
        if (node->kind == "assignment") {
            Ast target = node->children[0]; std::string expected = expression(target);
            if (target->kind == "name" && !lookup(target->text, target->span).mutableBinding)
                throw Error(target->span, "Cannot reassign let binding", "mutability");
            if (target->kind == "member") {
                if (!typeIntersections(expression(target->children[0])).empty()) throw Error(target->span, "Trait intersection methods are immutable", "mutability");
                if (baseType(expression(target->children[0])) == "Resource")
                    throw Error(target->span, "Resource metadata is immutable", "mutability");
                if (target->children[0]->kind == "name" && !lookup(target->children[0]->text, target->span).coreSpace.empty())
                    throw Error(target->span, "Core namespace members are immutable", "mutability");
                Ast record = declaration(expression(target->children[0]));
                bool classAccess = target->children[0]->kind == "name" && declaration(target->children[0]->text) && declaration(target->children[0]->text)->kind == "class";
                if (classAccess) record = declaration(target->children[0]->text);
                if (record && record->kind == "record") throw Error(target->span, "Record fields are immutable", "mutability");
                if (record && record->kind == "trait") throw Error(target->span, "Trait methods are immutable", "mutability");
                if (record && record->kind == "class") {
                    bool writable = false;
                    if (classAccess) { Ast field = staticEntry(record, target->text); writable = field && field->kind == "binding" && field->mutableBinding; }
                    else for (const auto &field : fields(record)) if (field.name == target->text) writable = field.mutableBinding || (initializing && !field.defaultValue);
                    if (!writable) throw Error(target->span, "Class let fields and methods are immutable", "mutability");
                }
            }
            std::string actual = expression(node->children[1]);
            if (target->kind == "index") {
                Ast owner = declaration(baseType(expression(target->children[0])));
                if (owner) if (Ast setter = method(owner, "__setitem__")) {
                    if (node->text != "=") if (auto overloaded = operatorResult(node->text.substr(0, 1), expected, {actual}, node)) actual = *overloaded;
                    Ast invocation = std::make_shared<Node>(*node); invocation->children = {target->children[0], target->children[1], node->children[1]}; invocation->names = {"", ""};
                    arguments(invocation, setter, {expression(target->children[1]), actual});
                    compatible(callbackResult(setter), "Null", node->span); return actual;
                }
            }
            if (node->text != "=") if (auto overloaded = operatorResult(node->text.substr(0, 1), expected, {actual}, node)) actual = *overloaded;
            compatible(actual, expected, node->span);
            literalContract(node->children[1], expected); return actual;
        }
        if (node->kind == "unary") {
            std::string operand = expression(node->children[0]);
            if (node->text == "!" || node->text == "not") { compatible(operand, "Bool", node->span); return "Bool"; }
            if (auto overloaded = operatorResult("unary" + node->text, operand, {}, node)) return *overloaded;
            if (operand != "Any" && operand != "Int" && operand != "Float") throw Error(node->span, "Unary arithmetic requires numbers", "type");
            return operand;
        }
        if (node->kind == "binary") {
            std::string left = expression(node->children[0]), right = expression(node->children[1]);
            if (node->text == "&&" || node->text == "and" || node->text == "||" || node->text == "or") {
                compatible(left, "Bool", node->span); compatible(right, "Bool", node->span); return "Bool";
            }
            if (auto overloaded = operatorResult(node->text, left, {right}, node)) return *overloaded;
            if (node->text == "==" || node->text == "!=" || node->text == "<" || node->text == ">" || node->text == "<=" || node->text == ">=") return "Bool";
            if (left == "Any" || right == "Any") return "Any";
            if (node->text == "??") {
                if (left == "Null") return right;
                if (left.back() != '?') return left;
                left.pop_back(); return merged(left, right);
            }
            if (node->text == "/") return "Float";
            left = baseType(left); right = baseType(right);
            if (left == right) return left;
            if ((left == "Int" || left == "Float") && (right == "Int" || right == "Float")) return "Float";
            throw Error(node->span, "Incompatible binary operand types", "type");
        }
        if (node->kind == "call") {
            std::string path = expressionPath(node->children[0]);
            for (const std::string &space : {"Sockets", "Http"}) if (path.rfind("IO." + space + ".", 0) == 0) {
                const auto *spec = coreMember(space, path.substr(space.size() + 4)); size_t count = node->children.size() - 1;
                if (!spec || count < static_cast<size_t>(spec->minimum) || (spec->maximum >= 0 && count > static_cast<size_t>(spec->maximum))) throw Error(node->span, "Invalid IO." + space + " call", "call");
                for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Network functions require positional arguments", "call");
                std::vector<std::string> actual;
                for (size_t index = 1; index < node->children.size(); index++) actual.push_back(expression(node->children[index]));
                return networkCall(node, *spec, actual);
            }
            if (path.rfind("IO.Terminal.", 0) == 0) {
                const auto *spec = coreMember("Terminal", path.substr(12)); size_t count = node->children.size() - 1;
                if (!spec || count < static_cast<size_t>(spec->minimum) || (spec->maximum >= 0 && count > static_cast<size_t>(spec->maximum))) throw Error(node->span, "Invalid IO.Terminal call", "call");
                for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Terminal functions require positional arguments", "call");
                if (path == "IO.Terminal.readKey" && count) compatible(expression(node->children[1]), "Int", node->children[1]->span);
                return spec->result;
            }
            if (path.rfind("IO.Files.", 0) == 0) {
                const auto *spec = coreMember("Files", path.substr(9));
                if (!spec || node->children.size() != 2) throw Error(node->span, "Invalid IO.Files call", "call");
                compatible(expression(node->children[1]), "String", node->children[1]->span);
                for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Files functions require positional arguments", "call");
                return spec->result;
            }
            expression(node->children[0]);
            std::vector<std::string> actual;
            for (size_t index = 1; index < node->children.size(); index++) actual.push_back(expression(node->children[index]));
            Ast callee = node->children[0];
            Ast anonymous;
            if (callee->kind == "function") anonymous = callee;
            else if (callee->kind == "name") {
                const auto &symbol = lookup(callee->text, callee->span);
                if (!symbol.mutableBinding && symbol.declaration && symbol.declaration->kind == "binding" && symbol.declaration->children[0]->kind == "function")
                    anonymous = symbol.declaration->children[0];
            }
            if (anonymous) {
                arguments(node, anonymous, actual);
                return yields(anonymous->children[0]) ? "Generator" : anonymous->type.empty() ? "Any" : anonymous->type;
            }
            if (callee->kind == "name") {
                Symbol &symbol = lookup(callee->text, callee->span);
                if (symbol.declaration && symbol.declaration->kind == "from_import") {
                    for (size_t index = 0; index < symbol.declaration->names.size(); index += 2) if (symbol.declaration->names[index + 1] == callee->text) {
                        if (const auto *member = coreMember(symbol.declaration->text, symbol.declaration->names[index])) {
                            if (std::string(member->name) == "Sized") throw Error(callee->span, "Trait is not callable", "type");
                            if (actual.size() < static_cast<size_t>(member->minimum) || (member->maximum >= 0 && actual.size() > static_cast<size_t>(member->maximum)))
                                throw Error(node->span, "Wrong number of imported core arguments", "call");
                            if (symbol.declaration->text == "Files") {
                                compatible(actual[0], "String", node->children[1]->span);
                                for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Files functions require positional arguments", "call");
                            }
                            if (symbol.declaration->text == "Json") {
                                if (std::string(member->name) == "parse") compatible(actual[0], "String", node->children[1]->span);
                                for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Json functions require positional arguments", "call");
                            }
                            if (symbol.declaration->text == "Random") return randomCall(node, *member, actual);
                            if (symbol.declaration->text == "Requests") return requestsCall(node, *member, actual);
                            if (symbol.declaration->text == "Sockets" || symbol.declaration->text == "Http") return networkCall(node, *member, actual);
                            return member->result;
                        }
                    }
                }
                if (symbol.declaration && symbol.declaration->kind == "trait") throw Error(callee->span, "Trait is not callable", "type");
                if (symbol.declaration && symbol.declaration->kind == "enum")
                    throw Error(callee->span, "Enum namespace is not callable; use a qualified variant constructor", "call");
                if (symbol.declaration && (symbol.declaration->kind == "function" || symbol.declaration->kind == "record" || symbol.declaration->kind == "class")) {
                    Ast signature = symbol.declaration;
                    if (signature->kind == "class") {
                        Ast init = method(symbol.declaration, "init"); signature = std::make_shared<Node>(*signature);
                        if (init) requireAccess(init->visibility, declaringOwner(symbol.declaration, "init"), node->span);
                        signature->parameters = init && (init->constructor || !init->parameters.empty()) ? init->parameters : fields(symbol.declaration);
                    }
                    if (symbol.declaration->baseClass.empty() || bases[symbol.declaration.get()]) arguments(node, signature, actual);
                    if (symbol.declaration->kind != "function") return symbol.declaration->text;
                    return symbol.declaration->type.empty() ? "Any" : symbol.declaration->type;
                }
                if (types.count(callee->text)) return callee->text;
            }
            if (callee->kind == "member" && callee->children[0]->kind == "name") {
                Symbol &space = lookup(callee->children[0]->text, callee->span);
                if (!space.coreSpace.empty()) {
                    const auto *member = coreMember(space.coreSpace, callee->text);
                    if (member && std::string(member->name) == "Sized") throw Error(callee->span, "Trait is not callable", "type");
                    if (!member || std::string(member->type) == "Tuple" || std::string(member->type) == "Float") throw Error(callee->span, "Core member is not callable", "call");
                    if (std::string(member->type) != "Type") {
                        if (actual.size() < static_cast<size_t>(member->minimum) || (member->maximum >= 0 && actual.size() > static_cast<size_t>(member->maximum)))
                            throw Error(node->span, "Wrong number of core-function arguments", "call");
                        for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Core native functions require positional arguments", "call");
                        if (space.coreSpace == "Files") compatible(actual[0], "String", node->children[1]->span);
                        if (space.coreSpace == "Terminal" && callee->text == "readKey" && !actual.empty()) compatible(actual[0], "Int", node->children[1]->span);
                        if (space.coreSpace == "Json" && callee->text == "parse") compatible(actual[0], "String", node->children[1]->span);
                        if (space.coreSpace == "Random") return randomCall(node, *member, actual);
                        if (space.coreSpace == "Requests") return requestsCall(node, *member, actual);
                        if (space.coreSpace == "Sockets" || space.coreSpace == "Http") return networkCall(node, *member, actual);
                    }
                    return member->result;
                }
                Ast owner = declaration(callee->children[0]->text);
                if (owner && owner->kind == "enum") { arguments(node, variant(owner, callee->text, callee->span), actual); return owner->text; }
                if (owner && owner->kind == "class") if (Ast signature = method(owner, callee->text, true)) {
                    arguments(node, signature, actual); return signature->type.empty() ? "Any" : signature->type;
                }
            }
            if (callee->kind == "member") {
                if (auto signature = intersectionMethod(expression(callee->children[0]), callee->text, callee->span)) {
                    arguments(node, signature, actual); return signature->type.empty() ? "Any" : signature->type;
                }
                Ast owner = declaration(expression(callee->children[0]));
                if (owner && !owner->builtinCollection && (owner->kind == "class" || owner->kind == "record" || owner->kind == "trait")) {
                    Ast signature = method(owner, callee->text);
                    if (signature) {
                        arguments(node, signature, actual);
                        return signature->type.empty() ? "Any" : signature->type;
                    }
                }
            }
            if (callee->kind == "member" && baseType(expression(callee->children[0])) == "String") {
                if (const auto *method = stringMember(callee->text)) {
                    if (actual.size() != method->arguments) throw Error(node->span, "Wrong number of String-method arguments", "call");
                    for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "String methods require positional arguments", "call");
                    if (callee->text == "join") {
                        std::string source = baseType(actual[0]);
                        if (!std::unordered_set<std::string>{"Any", "List", "Tuple", "String", "Generator"}.count(source))
                            throw Error(node->span, "join requires iterable String values", "type");
                        auto parameters = typeArguments(actual[0]);
                        if (parameters.size() == 1) compatible(parameters[0], "String", node->children[1]->span);
                        literalContract(node->children[1], "List[String]");
                    } else compatible(actual[0], "String", node->children[1]->span);
                    return method->result;
                }
            }
            if (callee->kind == "member" && collectionBase(expression(callee->children[0])) == "Map") {
                if (const auto *method = mapMember(callee->text)) {
                    if (actual.size() != method->arguments) throw Error(node->span, "Wrong number of map-method arguments", "call");
                    for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Map methods require positional arguments", "call");
                    auto parameters = typeArguments(expression(callee->children[0]));
                    if (!actual.empty()) {
                        std::string key = baseType(actual[0]);
                        if (!std::unordered_set<std::string>{"Any", "Int", "Float", "Bool", "Null", "String", "Bytes"}.count(key))
                            throw Error(node->span, "Map keys must be primitive values", "type");
                        if (parameters.size() == 2) compatible(actual[0], parameters[0], node->children[1]->span);
                    }
                    if (callee->text == "putIfAbsent" || callee->text == "merge") {
                        if (parameters.size() == 2) { compatible(actual[1], parameters[1], node->children[2]->span); literalContract(node->children[2], parameters[1]); }
                        if (callee->text == "merge" && actual[1] == "Null") throw Error(node->span, "merge requires a non-null value", "type");
                    }
                    if (callee->text == "compute" || callee->text == "computeIfAbsent" || callee->text == "computeIfPresent" || callee->text == "merge") {
                        compatible(actual.back(), "Function", node->children.back()->span);
                        Ast signature = callbackSignature(node->children.back());
                        if (signature) {
                            std::string key = parameters.size() == 2 ? parameters[0] : "Any";
                            std::string value = parameters.size() == 2 ? parameters[1] : "Any";
                            if (!value.empty() && value.back() == '?') value.pop_back();
                            std::string optional = value == "Any" ? value : value + '?';
                            std::vector<std::string> inputs = callee->text == "computeIfAbsent" ? std::vector<std::string>{key} :
                                callee->text == "merge" ? std::vector<std::string>{value, value} : std::vector<std::string>{key, callee->text == "compute" ? optional : value};
                            Ast invocation = std::make_shared<Node>(*node);
                            invocation->children.assign(inputs.size() + 1, node->children.back()); invocation->names.assign(inputs.size(), "");
                            arguments(invocation, signature, inputs); compatible(callbackResult(signature), optional, node->children.back()->span);
                        }
                    }
                    return mapResult(*method, expression(callee->children[0]));
                }
            }
            if (callee->kind == "member") {
                std::string receiver = expression(callee->children[0]); auto parameters = typeArguments(receiver);
                const auto *method = collectionBase(receiver) == "List" ? listMember(callee->text) : nullptr;
                if (method) {
                    if (actual.size() != method->arguments) throw Error(node->span, "Wrong number of list-method arguments", "call");
                    for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "List methods require positional arguments", "call");
                    if (callee->text == "insert" || callee->text == "removeAt") compatible(actual[0], "Int", node->children[1]->span);
                    if (callee->text == "map" || callee->text == "filter" || callee->text == "reduce" || callee->text == "any" || callee->text == "all" || callee->text == "find" || callee->text == "findIndex") {
                        compatible(actual[0], "Function", node->children[1]->span);
                        Ast callback = node->children[1], signature = callbackSignature(callback);
                        std::string element = parameters.empty() ? "Any" : parameters[0];
                        if (signature) {
                            Ast invocation = std::make_shared<Node>(*node);
                            std::vector<std::string> inputs = callee->text == "reduce" ? std::vector<std::string>{actual[1], element} : std::vector<std::string>{element};
                            invocation->children.assign(inputs.size() + 1, callback); invocation->names.assign(inputs.size(), "");
                            arguments(invocation, signature, inputs);
                            size_t input = callee->text == "reduce" ? 1 : 0;
                            if (signature->parameters.size() > input && !signature->parameters[input].type.empty() && !signature->parameters[input].rest)
                                literalContract(callee->children[0], "List[" + signature->parameters[input].type + "]");
                            std::string result = callbackResult(signature);
                            if (callee->text == "map") return result == "Any" ? "List" : "List[" + result + "]";
                            if (callee->text == "reduce") {
                                std::string accumulator = signature->parameters.empty() ? "" : signature->parameters[0].type;
                                if (!accumulator.empty()) { compatible(result, accumulator, node->span); return accumulator; }
                                return result == "Any" ? "Any" : merged(actual[1], result);
                            }
                            compatible(result, "Bool", callback->span);
                        }
                        if (callee->text == "reduce") return actual[1];
                    } else if (callee->text == "extend") {
                        std::string source = baseType(actual[0]);
                        if (!std::unordered_set<std::string>{"Any", "List", "Tuple", "Set", "String", "Bytes", "Generator"}.count(source))
                            throw Error(node->span, "extend requires an iterable", "type");
                        if (!parameters.empty()) {
                            if (source == "String") compatible("String", parameters[0], node->span);
                            if (source == "Bytes") compatible("Int", parameters[0], node->span);
                            auto elements = typeArguments(actual[0]);
                            if (elements.size() == 1) compatible(elements[0], parameters[0], node->span);
                            literalContract(node->children[1], "List[" + parameters[0] + "]");
                        }
                    } else if (!parameters.empty() && !actual.empty() && callee->text != "removeAt") {
                        compatible(actual.back(), parameters[0], node->children.back()->span);
                        literalContract(node->children.back(), parameters[0]);
                    }
                    return listResult(*method, receiver);
                }
            }
            if (callee->kind == "member" && callee->text == "length") {
                std::string receiver = baseType(expression(callee->children[0]));
                if (receiver == "List" || receiver == "Tuple" || receiver == "Map" || receiver == "String" || receiver == "Bytes") {
                    if (!actual.empty()) throw Error(node->span, "length expects no arguments", "call");
                    return "Int";
                }
            }
            if (callee->kind == "member" && collectionBase(expression(callee->children[0])) == "Set") if (const auto *spec = setMember(callee->text)) {
                if (actual.size() != spec->arguments) throw Error(node->span, "Wrong number of Set arguments", "call");
                for (const auto &name : node->names) if (!name.empty()) throw Error(node->span, "Set methods require positional arguments", "call");
                auto parameters = typeArguments(expression(callee->children[0]));
                if (!actual.empty()) {
                    if (callee->text == "add" || callee->text == "remove" || callee->text == "contains") { if (!parameters.empty()) compatible(actual[0], parameters[0], node->span); }
                    else compatible(actual[0], "Set", node->span);
                }
                return setResult(*spec, expression(callee->children[0]));
            }
            return "Any";
        }
        if (node->kind == "index") {
            std::string source = expression(node->children[0]), key = expression(node->children[1]);
            Ast owner = declaration(baseType(source));
            if (owner) if (Ast getter = method(owner, "__getitem__")) {
                Ast invocation = std::make_shared<Node>(*node); invocation->names = {""};
                arguments(invocation, getter, {key}); return callbackResult(getter);
            }
            auto arguments = typeArguments(source); std::string base = collectionBase(source);
            if (base == "Map" && arguments.size() == 2) { compatible(key, arguments[0], node->children[1]->span); return arguments[1]; }
            if (base == "List" || base == "Tuple" || base == "String" || base == "Bytes") {
                compatible(key, "Int", node->children[1]->span);
                if (base == "String") return "String";
                if (base == "Bytes") return "Int";
                if (arguments.size() == 1) return arguments[0];
            }
            return "Any";
        }
        if (node->kind == "member") {
            std::string path = expressionPath(node);
            for (const std::string &space : {"Sockets", "Http"}) if (path.rfind("IO." + space + ".", 0) == 0) { const auto *spec = coreMember(space, node->text); if (!spec) throw Error(node->span, "Unknown " + space + " member", "name"); return spec->type; }
            if (path.rfind("IO.Terminal.", 0) == 0) { const auto *spec = coreMember("Terminal", node->text); if (!spec) throw Error(node->span, "Unknown Terminal member", "name"); return spec->type; }
            if (path.rfind("IO.Files.", 0) == 0) { const auto *spec = coreMember("Files", node->text); if (!spec) throw Error(node->span, "Unknown Files member", "name"); return spec->type; }
            if (path == "Error.Kind" || path == "System.Error.Kind") return "Type";
            if (path.rfind("Error.Kind.", 0) == 0 || path.rfind("System.Error.Kind.", 0) == 0) {
                for (const auto &kind : errorKinds()) if (node->text == kind.first) return "Error.Kind";
                throw Error(node->span, "Unknown Error.Kind member", "name");
            }
            std::string receiver = expression(node->children[0]);
            if (!typeIntersections(receiver).empty()) {
                if (intersectionMethod(receiver, node->text, node->span)) return "Function";
                for (const auto &part : typeIntersections(receiver)) if (part.find('.') != std::string::npos && !declaration(baseType(part))) return "Any";
                throw Error(node->span, "Unknown intersection member '" + node->text + "'", "name");
            }
            if (baseType(receiver) == "Resource") {
                if (node->text == "closed") return "Bool";
                if (node->text == "type_name") return "String";
                throw Error(node->span, "Unknown Resource member '" + node->text + "'", "name");
            }
            if (node->children[0]->kind == "name") {
                Symbol &space = lookup(node->children[0]->text, node->span);
                if (!space.coreSpace.empty()) {
                    const auto *member = coreMember(space.coreSpace, node->text);
                    if (!member) throw Error(node->span, "Unknown core member '" + space.coreSpace + "." + node->text + "'", "name");
                    return member->type;
                }
                Ast owner = declaration(node->children[0]->text);
                if (owner && owner->kind == "enum") { variant(owner, node->text, node->span); return "Function"; }
                if (owner && owner->kind == "class") {
                    if (Ast entry = staticEntry(owner, node->text)) { requireAccess(entry->visibility, declaringOwner(owner, node->text), node->span); return entry->kind == "binding" ? entry->type : "Function"; }
                    throw Error(node->span, "Unknown static member '" + node->text + "'", "name");
                }
                if (owner && (owner->kind == "record" || owner->kind == "trait")) throw Error(node->span, "Type members require an instance", "type");
            }
            Ast record = declaration(receiver);
            if (baseType(receiver) == "Map" && mapMember(node->text)) return "Function";
            if (record && (record->kind == "record" || record->kind == "class" || record->kind == "trait")) {
                for (const auto &field : fields(record)) if (field.name == node->text) { requireAccess(field.visibility, declaringOwner(record, node->text), node->span); return field.type; }
                if (Ast methodNode = method(record, node->text)) { requireAccess(methodNode->visibility, declaringOwner(record, node->text), node->span); return "Function"; }
                throw Error(node->span, "Unknown field '" + record->text + "." + node->text + "'", "name");
            }
            return "Any";
        }
        if (node->kind == "match") {
            std::string subject = expression(node->children[0]); Ast known = declaration(subject);
            if (known && known->kind != "enum") known.reset();
            std::unordered_set<std::string> seen; bool wildcard = false; std::string result;
            for (size_t index = 1; index < node->children.size(); index++) {
                Ast arm = node->children[index]; Ast payload;
                if (wildcard) throw Error(arm->span, "Unreachable match arm after wildcard", "match");
                if (!seen.insert(arm->text).second) throw Error(arm->span, "Duplicate match arm '" + arm->text + "'", "match");
                if (arm->text == "_") {
                    if (!arm->names.empty()) throw Error(arm->span, "Wildcard cannot bind payloads", "match");
                    wildcard = true;
                } else {
                    size_t separator = arm->text.find_last_of('.');
                    if (separator == std::string::npos) throw Error(arm->span, "Expected qualified enum variant", "match");
                    std::string ownerName = arm->text.substr(0, separator);
                    Ast owner = declaration(ownerName);
                    if (owner && owner->kind == "enum") {
                        if (known && owner != known) throw Error(arm->span, "Pattern belongs to a different enum", "match");
                        payload = variant(owner, arm->text.substr(separator + 1), arm->span);
                        if (payload->names.size() != arm->names.size()) throw Error(arm->span, "Pattern payload arity mismatch", "match");
                    } else {
                        size_t first = ownerName.find('.'); Symbol &symbol = lookup(ownerName.substr(0, first), arm->span);
                        if (symbol.type != "Module" && symbol.type != "Any") throw Error(arm->span, "Expected enum namespace", "match");
                    }
                }
                scopes.emplace_back();
                for (size_t binding = 0; binding < arm->names.size(); binding++) {
                    const std::string &name = arm->names[binding];
                    if (scopes.back().count(name)) throw Error(arm->span, "Duplicate pattern binding '" + name + "'", "name");
                    scopes.back()[name] = {payload ? payload->names[binding] : "Any", false, true, {}};
                }
                std::string armType = expression(arm->children[0]); result = index == 1 ? armType : merged(result, armType);
                scopes.pop_back();
            }
            if (known && !wildcard) {
                std::string missing;
                for (const auto &child : known->children) if (!seen.count(known->text + '.' + child->text)) {
                    if (!missing.empty()) missing += ", "; missing += known->text + '.' + child->text;
                }
                if (!missing.empty()) throw Error(node->span, "Non-exhaustive match; missing " + missing, "match");
            }
            return result.empty() ? "Any" : result;
        }
        for (const Ast &child : node->children) if (child) expression(child);
        if (node->kind == "list") return "List";
        if (node->kind == "tuple") return "Tuple";
        if (node->kind == "map") return "Map";
        return "Any";
    }
    void function(const Ast &node, const std::string &selfType = "", bool initializer = false) {
        bool previousInitializing = initializing; initializing = initializer;
        int oldLoop = loopDepth; std::string previousReturn = returnType;
        loopDepth = 0; functionDepth++; returnType = initializer ? "Null" : node->type; annotation(node->type, node->span);
        scopes.emplace_back();
        if (!selfType.empty()) scopes.back()["self"] = {selfType, false, true, {}};
        if (!selfType.empty()) { Ast owner = declaration(selfType); if (owner && !owner->baseClass.empty()) scopes.back()["super"] = {owner->baseClass, false, true, {}}; }
        for (const auto &parameter : node->parameters) {
            annotation(parameter.type, node->span);
            if (parameter.defaultValue) {
                compatible(expression(parameter.defaultValue), parameter.type, parameter.defaultValue->span);
                literalContract(parameter.defaultValue, parameter.type);
            }
            scopes.back()[parameter.name] = {parameter.rest ? "List" : parameter.type.empty() ? "Any" : parameter.type, false, true, {}};
        }
        block(node->children[0]); scopes.pop_back(); functionDepth--; loopDepth = oldLoop; returnType = previousReturn; initializing = previousInitializing;
    }
    bool yields(const Ast &node) {
        if (!node || node->kind == "function") return false;
        if (node->kind == "yield") return true;
        for (const auto &child : node->children) if (yields(child)) return true;
        return false;
    }
    bool signatureType(const std::string &actual, const std::string &expected) {
        if (actual == expected) return true;
        if (actual.empty() || expected.empty() || (actual.back() == '?') != (expected.back() == '?')) return false;
        if (!typeIntersections(actual).empty() || !typeIntersections(expected).empty()) {
            std::vector<std::string> firstParts, secondParts;
            std::function<void(const std::string &, std::vector<std::string> &)> flatten = [&](const std::string &type, std::vector<std::string> &parts) {
                auto components = typeIntersections(type);
                if (components.empty()) {
                    for (const auto &part : parts) if (signatureType(part, type)) return;
                    parts.push_back(type);
                } else for (const auto &part : components) flatten(part, parts);
            };
            flatten(actual, firstParts); flatten(expected, secondParts);
            if (firstParts.size() != secondParts.size()) return false;
            for (const auto &part : firstParts) {
                bool found = false; for (const auto &candidate : secondParts) if (signatureType(part, candidate)) found = true;
                if (!found) return false;
            }
            return true;
        }
        auto first = typeArguments(actual), second = typeArguments(expected);
        if (first.size() != second.size()) return false;
        for (size_t index = 0; index < first.size(); index++) if (!signatureType(first[index], second[index])) return false;
        std::string firstBase = baseType(actual), secondBase = baseType(expected);
        Ast firstType = declaration(firstBase), secondType = declaration(secondBase);
        if (firstType && secondType) return firstType == secondType;
        if (firstBase == secondBase) return true;
        if (types.count(firstBase) || types.count(secondBase)) return false;
        return firstBase.find('.') != std::string::npos || secondBase.find('.') != std::string::npos;
    }
    void implementation(const Ast &node) {
        lookup(node->text.substr(0, node->text.find('.')), node->span);
        lookup(node->names[0].substr(0, node->names[0].find('.')), node->span);
        annotation(node->text, node->span); annotation(node->names[0], node->span);
        Ast trait = declaration(node->text), target = declaration(node->names[0]);
        if ((trait && trait->kind != "trait") || (!trait && types.count(node->text)) || (target && target->kind != "class" && target->kind != "record"))
            throw Error(node->span, "impl requires a trait and a class or record", "type");
        if (!trait || !target) { for (const auto &entry : node->children) function(entry, node->names[0]); return; }
        auto &registered = implementations[target.get()];
        if (std::find(registered.begin(), registered.end(), trait) != registered.end()) throw Error(node->span, "Trait already implemented for class", "type");
        for (const auto &entry : node->children) {
            if (!method(trait, entry->text)) throw Error(entry->span, "Unknown implementation method", "type");
            if (method(target, entry->text)) throw Error(entry->span, "Implementation cannot replace an existing method", "mutability");
            for (const auto &field : target->parameters) if (field.name == entry->text) throw Error(entry->span, "Implementation conflicts with a field", "type");
        }
        std::vector<Ast> defaults;
        for (const auto &signature : trait->children) {
            Ast candidate = method(target, signature->text);
            for (const auto &entry : node->children) if (entry->text == signature->text) candidate = entry;
            if (!candidate) {
                if (signature->kind != "function") throw Error(node->span, "Missing trait method '" + signature->text + "'", "type");
                for (const auto &field : target->parameters) if (field.name == signature->text)
                    throw Error(node->span, "Default method conflicts with a field", "type");
                candidate = signature; defaults.push_back(signature);
            } else if (signature->kind == "function" && defaultOwners.count(candidate.get()) && defaultOwners.at(candidate.get()) != trait.get())
                throw Error(node->span, "Conflicting trait defaults; define an explicit method", "type");
            if (candidate->visibility != Visibility::Public) throw Error(candidate->span, "Trait methods must be public", "access");
            if (candidate->parameters.size() != signature->parameters.size() || yields(candidate->children[0]) || !signatureType(candidate->type, signature->type))
                throw Error(candidate->span, "Trait method signature mismatch", "type");
            for (size_t index = 0; index < signature->parameters.size(); index++) {
                const auto &actual = candidate->parameters[index]; const auto &expected = signature->parameters[index];
                if (actual.rest || actual.name != expected.name || !signatureType(actual.type, expected.type)) throw Error(candidate->span, "Trait parameter signature mismatch", "type");
            }
        }
        auto &methods = addedMethods[target.get()]; methods.insert(methods.end(), node->children.begin(), node->children.end());
        methods.insert(methods.end(), defaults.begin(), defaults.end());
        for (const auto &entry : defaults) defaultOwners[entry.get()] = trait.get();
        registered.push_back(trait);
        Ast previousAccess = accessClass; accessClass = target;
        for (const auto &entry : node->children) function(entry, node->names[0]);
        accessClass = previousAccess;
    }
    void block(const Ast &node) {
        scopes.emplace_back();
        for (const Ast &statement : node->children) {
            if (statement->kind == "from_import") {
                for (size_t index = 0; index < statement->names.size(); index += 2) {
                    const std::string &alias = statement->names[index + 1];
                    if (scopes.back().count(alias)) throw Error(statement->span, "Duplicate import alias '" + alias + "'", "name");
                    const auto *member = coreMember(statement->text, statement->names[index]);
                    scopes.back()[alias] = {member ? member->type : "Any", false, false, statement};
                }
                continue;
            }
            if (statement->kind != "binding" && statement->kind != "function" && statement->kind != "record" && statement->kind != "class" && statement->kind != "trait" && statement->kind != "enum" && statement->kind != "import") continue;
            std::string name = statement->kind == "import" ? statement->names[0] : statement->text;
            if (scopes.back().count(name)) throw Error(statement->span, "Duplicate declaration '" + name + "'", "name");
            std::string type = statement->kind == "function" ? "Function" : statement->kind == "import" ? "Module" :
                statement->kind == "record" || statement->kind == "class" || statement->kind == "trait" || statement->kind == "enum" ? "Type" : statement->type.empty() ? "Any" : statement->type;
            scopes.back()[name] = {type, statement->mutableBinding, false, statement};
        }
        for (const Ast &statement : node->children) {
            if (statement->kind == "binding") {
                annotation(statement->type, statement->span);
                std::string type = expression(statement->children[0]); compatible(type, statement->type, statement->span);
                literalContract(statement->children[0], statement->type);
                auto &symbol = scopes.back().at(statement->text); symbol.initialized = true;
                if (statement->type.empty()) symbol.type = type;
            } else if (statement->kind == "function") {
                scopes.back().at(statement->text).initialized = true; function(statement);
            } else if (statement->kind == "trait") {
                scopes.back().at(statement->text).initialized = true;
                for (const auto &signature : statement->children) {
                    annotation(signature->type, signature->span);
                    for (const auto &parameter : signature->parameters) annotation(parameter.type, signature->span);
                    if (signature->kind == "function") {
                        if (yields(signature->children[0])) throw Error(signature->span, "Trait defaults must be synchronous", "type");
                        function(signature, statement->text);
                    }
                }
            } else if (statement->kind == "impl") implementation(statement);
            else if (statement->kind == "record" || statement->kind == "class" || statement->kind == "enum") {
                scopes.back().at(statement->text).initialized = true;
                Ast declarationAccess = accessClass; accessClass = statement->kind == "class" ? statement : accessClass;
                if (!statement->baseClass.empty()) {
                    lookup(statement->baseClass.substr(0, statement->baseClass.find('.')), statement->span);
                    annotation(statement->baseClass, statement->span); Ast base = declaration(statement->baseClass);
                    if (base == statement || (base && base->kind != "class") || (types.count(statement->baseClass) && !collectionDeclaration(statement->baseClass))) throw Error(statement->span, "Base must be a class", "type");
                    bases[statement.get()] = base;
                    if (base) {
                        size_t depth = 0; for (Ast parent = base; parent; parent = bases[parent.get()]) if (++depth > 128) throw Error(statement->span, "Class inheritance depth exceeded", "limit");
                        implementations[statement.get()] = implementations[base.get()];
                        for (const auto &field : statement->parameters) {
                            for (const auto &inherited : fields(base)) if (field.name == inherited.name) throw Error(statement->span, "Inherited field cannot be redeclared", "type");
                            if (method(base, field.name) || staticEntry(base, field.name)) throw Error(statement->span, "Field conflicts with inherited member", "type");
                        }
                        for (const auto &entry : statement->children) {
                            for (const auto &inherited : fields(base)) if (entry->text == inherited.name) throw Error(entry->span, "Member conflicts with inherited field", "type");
                            Ast previous = method(base, entry->text, entry->staticMember);
                            if (!previous && (method(base, entry->text, !entry->staticMember) || staticEntry(base, entry->text))) throw Error(entry->span, "Inherited member kind cannot change", "type");
                            if (!previous) continue;
                            if (previous->visibility == Visibility::Private || static_cast<int>(entry->visibility) > static_cast<int>(previous->visibility)) throw Error(entry->span, "Override cannot narrow visibility or replace private member", "access");
                            if (entry->text == "init") continue;
                            if (previous->kind == "signature") { if (entry->parameters.size() != previous->parameters.size()) throw Error(entry->span, "Native override arity mismatch", "type"); continue; }
                            if (entry->kind != "function" || entry->parameters.size() != previous->parameters.size() || !signatureType(entry->type, previous->type) || yields(entry->children[0]) != yields(previous->children[0]))
                                throw Error(entry->span, "Override signature mismatch", "type");
                            for (size_t position = 0; position < entry->parameters.size(); position++) {
                                const auto &actual = entry->parameters[position]; const auto &expected = previous->parameters[position];
                                if (actual.name != expected.name || actual.rest != expected.rest || bool(actual.defaultValue) != bool(expected.defaultValue) || !signatureType(actual.type, expected.type)) throw Error(entry->span, "Override parameter signature mismatch", "type");
                            }
                        }
                    }
                }
                scopes.emplace_back(); functionDepth++;
                if (bases[statement.get()]) for (const auto &field : fields(bases[statement.get()])) scopes.back()[field.name] = {field.type, false, true, {}};
                for (const auto &parameter : statement->parameters) {
                    annotation(parameter.type, statement->span);
                    if (parameter.defaultValue) {
                        compatible(expression(parameter.defaultValue), parameter.type, parameter.defaultValue->span);
                        literalContract(parameter.defaultValue, parameter.type);
                    }
                    scopes.back()[parameter.name] = {parameter.type, false, true, {}};
                }
                functionDepth--; scopes.pop_back();
                if (statement->kind == "class" || statement->kind == "record") {
                    Ast previousAccess = accessClass; accessClass = statement;
                    for (const auto &field : statement->children) if (field->kind == "binding") {
                        annotation(field->type, field->span); compatible(expression(field->children[0]), field->type, field->span); literalContract(field->children[0], field->type);
                    }
                    for (const auto &trait : statement->names) {
                        Ast embedded = std::make_shared<Node>(); embedded->kind = "impl"; embedded->text = trait;
                        embedded->names = {statement->text}; embedded->span = statement->span;
                        implementation(embedded);
                    }
                    for (const auto &method : statement->children) {
                        if (method->kind != "function") continue;
                        const auto *operatorSpec = namedOperatorHook(method->text);
                        if (operatorSpec || method->text == "toString") {
                            size_t count = operatorSpec ? operatorSpec->arguments : 0;
                            if (method->staticMember || method->visibility != Visibility::Public || method->parameters.size() != count || yields(method->children[0]))
                                throw Error(method->span, "Conversion/operator hook must be public, synchronous and have the expected arity", "type");
                            for (const auto &parameter : method->parameters) if (parameter.rest) throw Error(method->span, "Operator hooks cannot use rest parameters", "type");
                            if (method->text == "toString") compatible(method->type.empty() ? "Any" : method->type, "String", method->span);
                            if (operatorSpec && operatorSpec->predicate) compatible(method->type.empty() ? "Any" : method->type, "Bool", method->span);
                            if (method->text == "__setitem__") compatible(method->type.empty() ? "Any" : method->type, "Null", method->span);
                        }
                        bool initializer = statement->kind == "class" && method->text == "init";
                        if (initializer && ((!method->type.empty() && method->type != "Null") || yields(method->children[0])))
                            throw Error(method->span, "init must be synchronous and return Null", "type");
                        function(method, method->staticMember ? "" : statement->text, initializer);
                    }
                    accessClass = previousAccess;
                }
                else for (const auto &variant : statement->children) for (const auto &type : variant->names) annotation(type, variant->span);
                accessClass = declarationAccess;
            } else if (statement->kind == "from_import") {
                bool core = false; for (const auto &member : coreMembers()) if (member.space == statement->text) core = true;
                for (size_t index = 0; index < statement->names.size(); index += 2) {
                    if (core && !coreMember(statement->text, statement->names[index])) throw Error(statement->span, "Unknown imported core member", "name");
                    scopes.back().at(statement->names[index + 1]).initialized = true;
                }
            } else if (statement->kind == "import") {
                auto &symbol = scopes.back().at(statement->names[0]); symbol.initialized = true;
                for (const auto &member : coreMembers()) if (member.space == statement->text) symbol.coreSpace = statement->text;
            }
            else if (statement->kind == "block") block(statement);
            else if (statement->kind == "if" || statement->kind == "while") {
                compatible(expression(statement->children[0]), "Bool", statement->span);
                if (statement->kind == "while") loopDepth++;
                block(statement->children[1]);
                if (statement->children.size() == 3) {
                    Ast wrapper = std::make_shared<Node>(); wrapper->kind = "block"; wrapper->children.push_back(statement->children[2]); block(wrapper);
                }
                if (statement->kind == "while") loopDepth--;
            } else if (statement->kind == "for_c") {
                Ast scope = std::make_shared<Node>(); scope->kind = "block"; scope->span = statement->span;
                if (statement->children[0]) scope->children.push_back(statement->children[0]);
                Ast loop = std::make_shared<Node>(); loop->kind = "while"; loop->span = statement->span;
                Ast body = std::make_shared<Node>(); body->kind = "block"; body->span = statement->span;
                body->children.push_back(statement->children[3]);
                if (statement->children[2]) body->children.push_back(statement->children[2]);
                loop->children = {statement->children[1], body}; scope->children.push_back(loop); block(scope);
            } else if (statement->kind == "for") {
                expression(statement->children[0]); scopes.emplace_back(); scopes.back()[statement->text] = {"Any", false, true, {}};
                loopDepth++; block(statement->children[1]); loopDepth--; scopes.pop_back();
            } else if (statement->kind == "return" || statement->kind == "yield") {
                if (!functionDepth) throw Error(statement->span, statement->kind + " outside function", "syntax");
                if (!statement->children.empty()) {
                    std::string type = expression(statement->children[0]);
                    if (statement->kind == "return") {
                        compatible(type, returnType, statement->span);
                        literalContract(statement->children[0], returnType);
                    }
                }
            } else if (statement->kind == "break" || statement->kind == "continue") {
                if (!loopDepth) throw Error(statement->span, statement->kind + " outside loop", "syntax");
            } else if (statement->kind == "try") {
                block(statement->children[0]); scopes.emplace_back(); scopes.back()[statement->text] = {"Any", false, true, {}};
                block(statement->children[1]); scopes.pop_back(); if (statement->children.size() == 3) block(statement->children[2]);
            } else for (const Ast &child : statement->children) expression(child);
        }
        scopes.pop_back();
    }
public:
    Checker() {
        scopes.emplace_back();
        scopes.back()["List"] = {"Function", false, true, {}};
        scopes.back()["Map"] = {"Function", false, true, {}};
        scopes.back()["Tuple"] = {"Function", false, true, {}};
        scopes.back()["Set"] = {"Function", false, true, {}};
        scopes.back()["Object"] = {"Type", false, true, {}};
        scopes.back()["Error"] = {"Type", false, true, {}};
        for (const auto &member : coreMembers()) scopes.back()[member.space] = {"Module", false, true, {}, member.space};
    }
    void run(const Ast &program) { block(program); }
};
}
void check(const Ast &program) { Checker().run(program); }
}