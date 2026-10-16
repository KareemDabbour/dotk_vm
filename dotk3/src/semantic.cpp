#include "semantic.hpp"
#include "core.hpp"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace dk3 {
namespace {
struct View;
struct Info {
    Ast declaration, initializer;
    std::string type, core;
    std::weak_ptr<View> owner;
    std::shared_ptr<View> module;
    bool typeObject = false;
    std::string imported = {};
    std::vector<Info> constraints = {};
};
struct View { std::unordered_map<std::string, Info> symbols; std::unordered_map<std::string, std::vector<Ast>> implementations; std::unordered_map<std::string, std::vector<std::string>> traits; Ast accessClass; };
std::string describe(const Info &info, const std::string &name);
std::string base(std::string type) {
    if (!type.empty() && type.back() == '?') type.pop_back();
    return type.substr(0, type.find('['));
}
size_t blockEnd(const Ast &block) {
    auto tokens = lex(block->span.source); int depth = 0; bool begun = false;
    for (const auto &token : tokens) if (token.span.offset >= block->span.offset) {
        if (token.kind != TokenKind::Symbol) continue;
        if (token.text == "{") { depth++; begun = true; }
        else if (token.text == "}" && begun && --depth == 0) return token.span.offset + token.span.length;
    }
    return block->span.source->text.size();
}
void declarations(const Ast &block, const std::shared_ptr<View> &view, size_t cursor, bool exportedOnly = false) {
    for (const auto &node : block->children) {
        if (node->kind == "impl") {
            auto &methods = view->implementations[node->names[0]];
            methods.insert(methods.end(), node->children.begin(), node->children.end());
            view->traits[node->names[0]].push_back(node->text); continue;
        }
        if (exportedOnly && !node->exported) continue;
        if (node->kind == "record" || node->kind == "class" || node->kind == "trait" || node->kind == "enum" || node->kind == "function")
            view->symbols[node->text] = {node, {}, node->type, {}, view, {}, node->kind != "function"};
        else if (node->kind == "binding" && node->span.offset <= cursor)
            view->symbols[node->text] = {node, node->children[0], node->type, {}, view, {}, false};
        else if (node->kind == "import" && node->span.offset <= cursor)
            view->symbols[node->names[0]] = {node, {}, "Module", {}, view, {}, false};
        else if (node->kind == "from_import" && node->span.offset <= cursor)
            for (size_t index = 0; index < node->names.size(); index += 2)
                view->symbols[node->names[index + 1]] = {node, {}, "Any", {}, view, {}, false, node->names[index]};
    }
}
Ast anonymousAt(const Ast &node, size_t cursor) {
    if (!node) return {};
    if (node->kind == "function" && node->text.empty() && !node->children.empty() && cursor >= node->children[0]->span.offset && cursor < node->endOffset) return node;
    for (const auto &child : node->children) if (Ast found = anonymousAt(child, cursor)) return found;
    return {};
}
void visible(const Ast &block, const std::shared_ptr<View> &view, size_t cursor) {
    declarations(block, view, cursor);
    for (const auto &node : block->children) {
        if (node->kind == "for_c" && cursor >= node->span.offset && cursor < blockEnd(node->children[3])) {
            if (node->children[0] && node->children[0]->kind == "binding") {
                const auto &binding = node->children[0]; view->symbols[binding->text] = {binding, binding->children[0], binding->type, {}, view, {}, false};
            }
            if (cursor >= node->children[3]->span.offset) visible(node->children[3], view, cursor);
            return;
        }
        if (node->kind == "function" && !node->children.empty()) {
            const auto &body = node->children[0];
            if (cursor >= body->span.offset && cursor < (node->endOffset ? node->endOffset : blockEnd(body))) {
                for (const auto &parameter : node->parameters) view->symbols[parameter.name] = {{}, {}, parameter.type, {}, view, {}, false};
                visible(body, view, cursor); return;
            }
        } else if (node->kind == "class" || node->kind == "record" || node->kind == "trait" || node->kind == "impl") {
            for (const auto &method : node->children) {
                if (method->kind != "function" || method->children.empty()) continue;
                const auto &body = method->children[0];
                if (cursor >= body->span.offset && cursor < (method->endOffset ? method->endOffset : blockEnd(body))) {
                    if (node->kind == "class" || node->kind == "record") view->accessClass = node;
                    else if (node->kind == "impl") { auto target = view->symbols.find(node->names[0]); if (target != view->symbols.end()) view->accessClass = target->second.declaration; }
                    if (node->kind != "impl" && !method->staticMember) view->symbols["self"] = {node, {}, node->text, {}, view, {}, false};
                    else if (node->kind == "impl") view->symbols["self"] = {{}, {}, node->names[0], {}, view, {}, false};
                    if (!method->staticMember && !node->baseClass.empty()) view->symbols["super"] = {{}, {}, node->baseClass, {}, view, {}, false};
                    for (const auto &parameter : method->parameters) view->symbols[parameter.name] = {{}, {}, parameter.type, {}, view, {}, false};
                    visible(body, view, cursor); return;
                }
            }
        } else {
            if (Ast function = anonymousAt(node, cursor)) {
                for (const auto &parameter : function->parameters) view->symbols[parameter.name] = {{}, {}, parameter.type, {}, view, {}, false};
                visible(function->children[0], view, cursor); return;
            }
            for (const auto &child : node->children) if (child && child->kind == "block" && cursor >= child->span.offset && cursor < blockEnd(child)) {
                visible(child, view, cursor); return;
            }
            if (node->kind == "block" && cursor >= node->span.offset && cursor < blockEnd(node)) { visible(node, view, cursor); return; }
        }
    }
}
class Resolver {
    ImportTree imports;
    std::vector<std::shared_ptr<View>> ownedViews;
public:
    explicit Resolver(ImportTree loader) : imports(std::move(loader)) {}
    Info declaringOwner(Info receiver, const std::string &name, int depth = 0) {
        if (depth > 24 || !receiver.declaration) return receiver;
        for (const auto &field : receiver.declaration->parameters) if (field.name == name) return receiver;
        for (const auto &entry : receiver.declaration->children) if (entry->text == name) return receiver;
        if (!receiver.declaration->baseClass.empty()) return declaringOwner(typePath(receiver.declaration->baseClass, receiver.owner.lock(), depth + 1), name, depth + 1);
        return receiver;
    }
    bool accessible(Visibility visibility, Info receiver, const std::string &name) {
        if (visibility == Visibility::Public) return true;
        auto view = receiver.owner.lock(); if (!view || !view->accessClass) return false;
        Info owner = declaringOwner(receiver, name);
        Info current{view->accessClass, {}, view->accessClass->text, {}, view, {}, false};
        for (size_t depth = 0; current.declaration && depth < 24; depth++) {
            if (current.declaration == owner.declaration) return true;
            if (visibility == Visibility::Private || current.declaration->baseClass.empty()) break;
            current = typePath(current.declaration->baseClass, current.owner.lock(), 0);
        }
        return false;
    }
    std::vector<Parameter> fields(const Info &receiver, int depth = 0) {
        if (depth > 24 || !receiver.declaration) return {};
        std::vector<Parameter> result;
        if (!receiver.declaration->baseClass.empty()) result = fields(typePath(receiver.declaration->baseClass, receiver.owner.lock(), depth + 1), depth + 1);
        result.insert(result.end(), receiver.declaration->parameters.begin(), receiver.declaration->parameters.end()); return result;
    }
    std::vector<Ast> methods(const Info &receiver, int depth = 0) {
        if (depth > 24 || !receiver.declaration) return {};
        std::vector<Ast> result = receiver.declaration->children;
        auto owner = receiver.owner.lock();
        if (!receiver.declaration->baseClass.empty()) {
            Info base = typePath(receiver.declaration->baseClass, owner, depth + 1);
            for (const auto &entry : methods(base, depth + 1)) {
                bool overridden = false; for (const auto &own : result) if (own->text == entry->text) overridden = true;
                if (!overridden) result.push_back(entry);
            }
        }
        if (owner && (receiver.declaration->kind == "class" || receiver.declaration->kind == "record")) {
            auto found = owner->implementations.find(receiver.declaration->text);
            if (found != owner->implementations.end()) result.insert(result.end(), found->second.begin(), found->second.end());
            auto paths = receiver.declaration->names;
            auto external = owner->traits.find(receiver.declaration->text);
            if (external != owner->traits.end()) paths.insert(paths.end(), external->second.begin(), external->second.end());
            for (const auto &path : paths) {
                Info trait = typePath(path, owner, 0);
                if (!trait.declaration || trait.declaration->kind != "trait") continue;
                for (const auto &entry : trait.declaration->children) if (entry->kind == "function") {
                    bool supplied = false; for (const auto &method : result) if (method->text == entry->text) supplied = true;
                    if (!supplied) result.push_back(entry);
                }
            }
        }
        return result;
    }
    Info resolved(Info info, int depth = 0) {
        if (depth > 24) return {};
        if (!info.constraints.empty()) return info;
        auto parts = typeIntersections(info.type);
        if (!parts.empty()) {
            for (const auto &part : parts) info.constraints.push_back(resolved({{}, {}, part, {}, info.owner, {}, false}, depth + 1));
            return info;
        }
        if (info.declaration && info.declaration->kind == "from_import") {
            if (const auto *member = coreMember(info.declaration->text, info.imported)) {
                if (info.declaration->text == "Collections" && info.imported == "Sized") return {sizedDeclaration(), {}, "Sized", {}, info.owner, {}, true};
                if (info.declaration->text == "Collections") if (Ast type = collectionDeclaration(info.imported)) return {type, {}, type->text, {}, info.owner, {}, true};
                info.type = member->type; return info;
            }
            Ast request = std::make_shared<Node>(*info.declaration); request->kind = "import";
            Ast tree = imports ? imports(request) : Ast{};
            if (!tree) return {};
            auto module = std::make_shared<View>(); ownedViews.push_back(module); declarations(tree, module, tree->span.source->text.size(), true);
            auto found = module->symbols.find(info.imported);
            return found == module->symbols.end() ? Info{} : resolved(found->second, depth + 1);
        }
        if (info.declaration && info.declaration->kind == "import") {
            for (const auto &member : coreMembers()) if (member.space == info.declaration->text) { info.core = member.space; return info; }
            Ast tree = imports ? imports(info.declaration) : Ast{};
            if (tree) { info.module = std::make_shared<View>(); ownedViews.push_back(info.module); declarations(tree, info.module, tree->span.source->text.size(), true); }
            return info;
        }
        if (info.declaration && (info.declaration->kind == "record" || info.declaration->kind == "class" || info.declaration->kind == "trait" || info.declaration->kind == "enum" || info.declaration->kind == "function" || info.declaration->kind == "signature")) return info;
        if (info.initializer && info.initializer->kind == "function" && (info.type.empty() || info.type == "Any" || info.type == "Function"))
            return expression(info.initializer, info.owner.lock(), depth + 1);
        if (!info.type.empty() && info.type != "Any") {
            std::string name = base(info.type);
            if (!info.initializer) if (Ast type = collectionDeclaration(name)) { info.declaration = type; return info; }
            if (name.find('.') != std::string::npos) return typePath(name, info.owner.lock(), depth + 1);
            auto owner = info.owner.lock();
            if (owner) {
                auto found = owner->symbols.find(name);
                if (found != owner->symbols.end() && found->second.declaration) {
                    Info result = resolved(found->second, depth + 1);
                    if (result.typeObject && result.declaration) {
                        result.typeObject = false;
                        if (result.declaration->builtinCollection) result.type = info.type;
                        return result;
                    }
                }
            }
            return info;
        }
        return info.initializer ? expression(info.initializer, info.owner.lock(), depth + 1) : info;
    }
    Info typePath(const std::string &path, const std::shared_ptr<View> &view, int depth) {
        size_t separator = path.find('.');
        Info info = name(path.substr(0, separator), view, depth + 1);
        while (separator != std::string::npos) {
            size_t start = separator + 1; separator = path.find('.', start);
            info = field(info, path.substr(start, separator == std::string::npos ? separator : separator - start), depth + 1);
        }
        info.typeObject = false; return info;
    }
    Info name(const std::string &value, const std::shared_ptr<View> &view, int depth) {
        if (depth > 24 || !view) return {};
        auto found = view->symbols.find(value); return found == view->symbols.end() ? Info{} : resolved(found->second, depth + 1);
    }
    std::string nameHover(const std::string &value, const std::shared_ptr<View> &view) {
        auto found = view->symbols.find(value);
        if (found == view->symbols.end()) return {};
        Info inferred = resolved(found->second);
        if (found->second.declaration && found->second.declaration->kind == "from_import") {
            const auto &declaration = found->second.declaration;
            std::string detail = "from " + declaration->text + " import " + found->second.imported;
            if (value != found->second.imported) detail += " as " + value;
            if (const auto *member = coreMember(declaration->text, found->second.imported)) return detail + "\n" + member->detail;
            return detail + "\n" + describe(inferred, value);
        }
        if (!found->second.declaration || found->second.declaration->kind != "binding") return describe(inferred, value);
        Info binding = found->second;
        if (binding.type.empty()) {
            binding.type = inferred.type;
            if (binding.type.empty() && inferred.declaration && !inferred.typeObject &&
                (inferred.declaration->kind == "class" || inferred.declaration->kind == "record")) binding.type = inferred.declaration->text;
        }
        std::string detail = describe(binding, value);
        if (inferred.declaration && (inferred.declaration->kind == "function" || inferred.declaration->kind == "signature")) detail += "\n" + describe(inferred, value);
        return detail;
    }
    Info field(Info receiver, const std::string &member, int depth) {
        if (depth > 24) return {};
        receiver = resolved(receiver, depth + 1);
        if (receiver.type == "Error" && member == "Kind") return {errorKindDeclaration(), {}, "Error.Kind", {}, receiver.owner, {}, true};
        if (receiver.declaration == errorKindDeclaration() && receiver.typeObject) {
            for (const auto &kind : errorKinds()) if (kind.first == member) return {{}, {}, "Error.Kind", {}, receiver.owner, {}, false};
        }
        for (const auto &part : receiver.constraints) {
            Info candidate = field(part, member, depth + 1);
            if (candidate.declaration || !candidate.type.empty()) return candidate;
        }
        if (base(receiver.type) == "Resource") {
            if (member == "closed") return {{}, {}, "Bool", {}, receiver.owner, {}, false};
            if (member == "type_name") return {{}, {}, "String", {}, receiver.owner, {}, false};
        }
        if (receiver.module) {
            auto found = receiver.module->symbols.find(member);
            return found == receiver.module->symbols.end() ? Info{} : resolved(found->second, depth + 1);
        }
        if (!receiver.core.empty()) {
            const auto *entry = coreMember(receiver.core, member);
            if (receiver.core == "System" && member == "Error") return {{}, {}, "Error", {}, receiver.owner, {}, true};
            if (receiver.core == "IO" && member == "Files") return {{}, {}, "Module", "Files", receiver.owner, {}, false};
            if (receiver.core == "IO" && member == "Terminal") return {{}, {}, "Module", "Terminal", receiver.owner, {}, false};
            if (receiver.core == "IO" && (member == "Sockets" || member == "Http")) return {{}, {}, "Module", member, receiver.owner, {}, false};
            if (receiver.core == "Collections" && member == "Sized") return {sizedDeclaration(), {}, "Sized", {}, receiver.owner, {}, true};
            if (receiver.core == "Collections") if (Ast type = collectionDeclaration(member)) return {type, {}, type->text, {}, receiver.owner, {}, true};
            if (entry) return {{}, {}, entry->type, {}, receiver.owner, {}, false};
        }
        if (receiver.declaration && receiver.declaration->kind == "class" && receiver.typeObject) {
            for (const auto &entry : methods(receiver)) if (entry->staticMember && entry->text == member && accessible(entry->visibility, receiver, member))
                return entry->kind == "binding" ? resolved({{}, {}, entry->type, {}, receiver.owner, {}, false}, depth + 1) : Info{entry, {}, "Function", {}, receiver.owner, {}, false};
        }
        if (receiver.declaration && (receiver.declaration->kind == "record" || receiver.declaration->kind == "class" || receiver.declaration->kind == "trait") && !receiver.typeObject) {
            for (const auto &parameter : fields(receiver)) if (parameter.name == member && accessible(parameter.visibility, receiver, member))
                return resolved({{}, {}, parameter.type, {}, receiver.owner, {}, false}, depth + 1);
            if (receiver.declaration->kind == "class" || receiver.declaration->kind == "record" || receiver.declaration->kind == "trait") for (const auto &method : methods(receiver)) if (method->text == member && !method->staticMember && method->kind != "binding" && accessible(method->visibility, receiver, member))
                return {method, {}, "Function", {}, receiver.owner, {}, false};
        }
        return {};
    }
    Info expression(const Ast &node, const std::shared_ptr<View> &view, int depth = 0) {
        if (!node || depth > 24) return {};
        if (node->kind == "format") return {{}, {}, "String", {}, view, {}, false};
        if (node->kind == "literal" && (node->literalKind == TokenKind::String || node->literalKind == TokenKind::Bytes))
            return {{}, {}, node->literalKind == TokenKind::String ? "String" : "Bytes", {}, view, {}, false};
        if (node->kind == "literal") return {{}, {}, node->literalKind == TokenKind::Integer ? "Int" : node->literalKind == TokenKind::Float ? "Float" : node->text == "null" ? "Null" : "Bool", {}, view, {}, false};
        if (node->kind == "list" || node->kind == "map" || node->kind == "tuple") {
            std::string kind = node->kind == "list" ? "List" : node->kind == "map" ? "Map" : "Tuple";
            if (node->children.empty() || node->children.size() > 64) return {{}, {}, kind, {}, view, {}, false};
            size_t components = node->kind == "map" ? 2 : 1;
            std::vector<std::string> types(components);
            for (size_t position = 0; position < node->children.size(); position++) {
                Info child = expression(node->children[position], view, depth + 1);
                std::string type = child.type.empty() ? "Any" : child.type;
                if (type == "Any" && child.declaration && !child.typeObject && (child.declaration->kind == "class" || child.declaration->kind == "record")) type = child.declaration->text;
                auto &component = types[position % components];
                if (component.empty()) component = type;
                else if (component != type) component = "Any";
            }
            kind += "[" + types[0]; if (components == 2) kind += ", " + types[1]; kind += "]";
            return {{}, {}, kind, {}, view, {}, false};
        }
        if (node->kind == "function") return {node, {}, "Function", {}, view, {}, false};
        if (node->kind == "name") return name(node->text, view, depth + 1);
        if (node->kind == "member") return field(expression(node->children[0], view, depth + 1), node->text, depth + 1);
        if (node->kind == "call") {
            Info called = expression(node->children[0], view, depth + 1);
            if (called.declaration && (called.declaration->kind == "record" || called.declaration->kind == "class")) { called.typeObject = false; return called; }
            if (called.declaration && !called.declaration->builtinCollection && (called.declaration->kind == "function" || called.declaration->kind == "signature")) return resolved({{}, {}, callbackResult(called.declaration), {}, called.owner, {}, false}, depth + 1);
            if (called.declaration && called.declaration->kind == "from_import") {
                if (const auto *member = coreMember(called.declaration->text, called.imported)) {
                    std::string result = member->result;
                    if (called.declaration->text == "Random") result = randomResult(*member, node->children.size() > 1 ? expression(node->children[1], view, depth + 1).type : "Any");
                    return resolved({{}, {}, result, {}, view, {}, false}, depth + 1);
                }
            }
            const auto &callee = node->children[0];
            if (callee->kind == "name" && collectionDeclaration(callee->text)) return {{}, {}, callee->text, {}, view, {}, false};
            if (callee->kind == "member") {
                Info receiver = expression(callee->children[0], view, depth + 1);
                if (base(receiver.type) == "String") if (const auto *method = stringMember(callee->text))
                    return resolved({{}, {}, method->result, {}, receiver.owner, {}, false}, depth + 1);
                if (base(receiver.type) == "List") if (const auto *method = listMember(callee->text)) {
                    std::string result = listResult(*method, receiver.type);
                    if ((callee->text == "map" || callee->text == "reduce") && node->children.size() > 1) {
                        Info callback = expression(node->children[1], view, depth + 1);
                        std::string returned = callbackResult(callback.declaration);
                        if (callee->text == "map" && returned != "Any") result = "List[" + returned + "]";
                        if (callee->text == "reduce") {
                            result = callback.declaration && !callback.declaration->parameters.empty() && !callback.declaration->parameters[0].type.empty() ?
                                callback.declaration->parameters[0].type : "Any";
                        }
                    }
                    return resolved({{}, {}, result, {}, receiver.owner, {}, false}, depth + 1);
                }
                if (base(receiver.type) == "Map") if (const auto *method = mapMember(callee->text))
                    return resolved({{}, {}, mapResult(*method, receiver.type), {}, receiver.owner, {}, false}, depth + 1);
                if (base(receiver.type) == "Set") if (const auto *method = setMember(callee->text))
                    return resolved({{}, {}, setResult(*method, receiver.type), {}, receiver.owner, {}, false}, depth + 1);
                if (!receiver.core.empty()) if (const auto *member = coreMember(receiver.core, callee->text)) {
                    std::string result = receiver.core == "Random" ? randomResult(*member, node->children.size() > 1 ? expression(node->children[1], view, depth + 1).type : "Any") : member->result;
                    return resolved({{}, {}, result, {}, view, {}, false}, depth + 1);
                }
            }
            return {};
        }
        if (node->kind == "binary" || node->kind == "unary") {
            std::string symbol = node->kind == "unary" ? "unary" + node->text : node->text;
            if (const auto *hook = operatorHook(symbol)) {
                Info receiver = expression(node->children[0], view, depth + 1);
                Info method = field(receiver, hook->name, depth + 1);
                if (method.declaration) return resolved({{}, {}, hook->predicate ? "Bool" : callbackResult(method.declaration), {}, method.owner, {}, false}, depth + 1);
            }
            std::string left = expression(node->children[0], view, depth + 1).type;
            std::string right = node->kind == "binary" ? expression(node->children[1], view, depth + 1).type : "";
            std::string result;
            if (node->text == "==" || node->text == "!=" || node->text == "<" || node->text == ">" || node->text == "<=" || node->text == ">=" || node->text == "and" || node->text == "or" || node->text == "&&" || node->text == "||" || node->text == "not" || node->text == "!") result = "Bool";
            else if (node->text == "??") {
                if (left == "Null") result = right;
                else if (!left.empty() && left != "Any") { result = left; if (result.back() == '?') result.pop_back(); }
            } else if (node->kind == "unary" && (left == "Int" || left == "Float")) result = left;
            else if (left == right && !left.empty() && left != "Any") {
                if (left == "Int" || left == "Float") result = node->text == "/" ? "Float" : left;
                else if (node->text == "+" && (left == "String" || left == "Bytes" || base(left) == "List")) result = left;
            }
            if (!result.empty()) return {{}, {}, result, {}, view, {}, false};
        }
        if (node->kind == "index") {
            Info source = expression(node->children[0], view, depth + 1);
            Info getter = field(source, "__getitem__", depth + 1);
            if (getter.declaration) return resolved({{}, {}, callbackResult(getter.declaration), {}, getter.owner, {}, false}, depth + 1);
            auto arguments = typeArguments(source.type);
            if (!arguments.empty()) return resolved({{}, {}, arguments.back(), {}, source.owner, {}, false}, depth + 1);
        }
        return {};
    }
    std::vector<SemanticMember> members(Info receiver) {
        receiver = resolved(receiver);
        std::vector<SemanticMember> result;
        if (receiver.type == "Error" && receiver.typeObject) result.push_back({"Kind", "Type", "Error.Kind enum", 6});
        for (const auto &part : receiver.constraints) {
            auto entries = members(part); result.insert(result.end(), entries.begin(), entries.end());
        }
        if (!receiver.core.empty()) {
            for (const auto &entry : coreMembers()) if (entry.space == receiver.core)
                result.push_back({entry.name, entry.type, entry.detail, std::string(entry.type) == "Function" ? 3 : 6});
        }
        if (receiver.module) for (const auto &entry : receiver.module->symbols)
            result.push_back({entry.first, name(entry.first, receiver.module, 0).type, nameHover(entry.first, receiver.module), 6});
        if (receiver.declaration && receiver.declaration->kind == "class" && receiver.typeObject) {
            for (const auto &entry : methods(receiver)) if (entry->staticMember && accessible(entry->visibility, receiver, entry->text))
                result.push_back({entry->text, entry->kind == "binding" ? entry->type : "Function", "static " + describe({entry, {}, entry->type, {}, receiver.owner, {}, false}, entry->text), entry->kind == "binding" ? 5 : 2});
        }
        if (receiver.declaration && (receiver.declaration->kind == "record" || receiver.declaration->kind == "class" || receiver.declaration->kind == "trait") && !receiver.typeObject) {
            for (const auto &parameter : fields(receiver)) if (accessible(parameter.visibility, receiver, parameter.name))
                result.push_back({parameter.name, parameter.type, (receiver.declaration->kind == "class" ? (parameter.mutableBinding ? "var " : "let ") : "") + receiver.declaration->text + "." + parameter.name + ": " + parameter.type +
                    (receiver.declaration->documentation.empty() ? "" : "\n\n" + receiver.declaration->documentation), 5});
            if (receiver.declaration->kind == "class" || receiver.declaration->kind == "record" || receiver.declaration->kind == "trait") for (const auto &method : methods(receiver)) {
                if (method->staticMember || method->kind == "binding" || (method->builtinCollection && method->text == "init")) continue;
                if (!accessible(method->visibility, receiver, method->text)) continue;
                Info signature{method, {}, "Function", {}, receiver.owner, {}, false};
                result.push_back({method->text, "Function", describe(signature, receiver.declaration->text + "." + method->text), 2});
            }
        }
        if (receiver.declaration && receiver.declaration->kind == "enum" && receiver.typeObject)
            for (const auto &variant : receiver.declaration->children) result.push_back({variant->text, receiver.declaration->text, receiver.declaration->text + "." + variant->text, 20});
        if (base(receiver.type) == "List") for (const auto &method : listMembers())
            result.push_back({method.name, "Function", method.detail, 2});
        if (base(receiver.type) == "Map") for (const auto &method : mapMembers())
            result.push_back({method.name, "Function", method.detail, 2});
        if (base(receiver.type) == "Set") for (const auto &method : setMembers()) result.push_back({method.name, "Function", method.detail, 2});
        if (base(receiver.type) == "String") for (const auto &method : stringMembers())
            result.push_back({method.name, "Function", method.detail, 2});
        std::string kind = base(receiver.type);
        if (kind == "List" || kind == "Map" || kind == "Tuple" || kind == "Set" || kind == "String" || kind == "Bytes")
            result.push_back({"length", "Function", "length() -> Int", 3});
        if (base(receiver.type) == "Task") result = {{"status", "String", "status: String", 5}, {"done", "Bool", "done: Bool", 5},
            {"value", "Any", "value: Any", 5}, {"has_value", "Bool", "has_value: Bool", 5}, {"error", "Any", "error: Any", 5}};
        if (base(receiver.type) == "Resource") result = {{"closed", "Bool", "closed: Bool (readonly)", 5},
            {"type_name", "String", "type_name: String (readonly)", 5}};
        std::sort(result.begin(), result.end(), [](const auto &first, const auto &second) { return first.name < second.name; });
        result.erase(std::unique(result.begin(), result.end(), [](const auto &first, const auto &second) { return first.name == second.name; }), result.end());
        return result;
    }
};
std::shared_ptr<View> context(const Ast &tree, size_t cursor) {
    auto view = std::make_shared<View>();
    for (const auto &member : coreMembers()) view->symbols[member.space] = {{}, {}, "Module", member.space, view, {}, false};
    view->symbols["List"] = {{}, {}, "List", {}, view, {}, true};
    view->symbols["Map"] = {{}, {}, "Map", {}, view, {}, true};
    view->symbols["Tuple"] = {collectionDeclaration("Tuple"), {}, "Tuple", {}, view, {}, true};
    view->symbols["Set"] = {collectionDeclaration("Set"), {}, "Set", {}, view, {}, true};
    view->symbols["Object"] = {collectionDeclaration("Object"), {}, "Object", {}, view, {}, true};
    view->symbols["Error"] = {{}, {}, "Error", {}, view, {}, true};
    visible(tree, view, cursor); return view;
}
struct Access { Ast receiver; std::string field; };
Ast hoveredName(const Ast &node, size_t offset) {
    if (!node) return {};
    for (const auto &child : node->children) if (auto found = hoveredName(child, offset)) return found;
    if (node->kind == "name" && offset >= node->span.offset && offset <= node->span.offset + node->span.length) return node;
    return {};
}
std::string describe(const Info &info, const std::string &name) {
    if (!info.declaration) return info.type.empty() ? "" : name + ": " + info.type;
    const auto &node = info.declaration;
    std::string result = node->kind + " " + node->text;
    if (node->kind == "binding") {
        result = std::string(node->exported ? "export " : "") + (node->mutableBinding ? "var " : "let ") + name + ": " + (info.type.empty() ? "Any" : info.type);
        if (info.initializer && info.initializer->kind == "literal" && info.initializer->span.source && info.initializer->span.length <= 80) {
            std::string initializer = info.initializer->span.source->text.substr(info.initializer->span.offset, info.initializer->span.length);
            if (initializer.find_first_of("\r\n") == std::string::npos) result += " = " + initializer;
        }
        result += !node->type.empty() ? "\nDeclared type" : info.type.empty() || info.type == "Any" ? "\nType unknown (not statically inferred)" : "\nType inferred from initializer";
    }
    if (!node->baseClass.empty()) result += " extends " + node->baseClass;
    if ((node->kind == "class" || node->kind == "record") && !node->names.empty()) {
        result += " implements ";
        for (size_t index = 0; index < node->names.size(); index++) { if (index) result += ", "; result += node->names[index]; }
    }
    if (node->kind == "record" || node->kind == "class") {
        result += " {";
        for (const auto &parameter : node->parameters) result += std::string("\n  ") + (node->kind == "class" ? (parameter.mutableBinding ? "var " : "let ") : "") + parameter.name + ": " + parameter.type;
        for (const auto &method : node->children) result += "\n  fn " + method->text + "(...)";
        result += "\n}";
    }
    if (node->kind == "enum") {
        result += " {"; for (const auto &child : node->children) result += "\n  " + child->text; result += "\n}";
    }
    if (node->kind == "trait") {
        result += " {";
        for (const auto &method : node->children) result += "\n  " + describe({method, {}, "Function", {}, info.owner, {}, false}, method->text);
        result += "\n}";
    }
    if (node->kind == "function" || node->kind == "signature") {
        result = "fn " + name + "(";
        for (const auto &parameter : node->parameters) { if (result.back() != '(') result += ", "; result += parameter.name + (parameter.type.empty() ? "" : ": " + parameter.type); }
        result += ')'; if (!node->type.empty()) result += " -> " + node->type;
    }
    if (!node->documentation.empty()) result += "\n\n" + node->documentation;
    return result;
}
Access access(const Ast &node, size_t position) {
    if (!node) return {};
    for (const auto &child : node->children) {
        Access nested = access(child, position); if (nested.receiver) return nested;
    }
    if (node->kind == "member" && position >= node->span.offset && position <= node->span.offset + node->span.length)
        return {node->children[0], node->text};
    return {};
}
}
std::vector<SemanticMember> memberCompletions(const Ast &tree, const std::shared_ptr<const Source> &source, size_t offset, const ImportTree &imports) {
    offset = std::min(offset, source->text.size()); size_t start = offset;
    while (start && (std::isalnum(static_cast<unsigned char>(source->text[start - 1])) || source->text[start - 1] == '_')) start--;
    if (!start || source->text[start - 1] != '.') return {};
    Ast parsed = tree;
    if (!parsed || start == offset) {
        std::string edited = source->text; edited.insert(start, "__member_probe");
        try { parsed = parse(std::make_shared<Source>(Source{source->file, edited})); } catch (const Error &) { return {}; }
    }
    Access found = access(parsed, start); if (!found.receiver) return {};
    Resolver resolver(imports);
    auto scope = context(parsed, start);
    return resolver.members(resolver.expression(found.receiver, scope));
}
std::string memberHover(const Ast &tree, const std::shared_ptr<const Source> &source, size_t offset, const ImportTree &imports) {
    if (!tree) return {};
    Resolver resolver(imports);
    auto scope = context(tree, offset);
    Access found = access(tree, offset);
    if (!found.receiver) {
        auto name = hoveredName(tree, offset);
        if (name) return resolver.nameHover(name->text, scope);
        for (const auto &token : lex(source)) if (token.kind == TokenKind::Name && offset >= token.span.offset && offset <= token.span.offset + token.span.length)
            return resolver.nameHover(token.text, scope);
        return {};
    }
    for (const auto &member : resolver.members(resolver.expression(found.receiver, scope)))
        if (member.name == found.field) return member.detail;
    return {};
}
}