#include "types.hpp"
#include <algorithm>
#include <set>

namespace dk3 {
namespace {
unsigned nextId = 1;
std::string pointerKey(const void *pointer) { return std::to_string(reinterpret_cast<uintptr_t>(pointer)); }
}

Types::Types() {
    auto primitive = [&](TypeTag tag, const char *name) { TypeData data; data.tag = tag; data.name = name; return intern(std::move(data)); };
    error_ = primitive(TypeTag::Error, "<error>");
    never_ = primitive(TypeTag::Never, "Never");
    null_ = primitive(TypeTag::Null, "Null");
    bool_ = primitive(TypeTag::Bool, "Bool");
    int_ = primitive(TypeTag::Int, "Int");
    float_ = primitive(TypeTag::Float, "Float");
    object_ = primitive(TypeTag::Object, "Object");
}

Ty Types::intern(TypeData data) {
    std::string key = std::to_string(static_cast<int>(data.tag)) + '|' + data.name + '|' + pointerKey(data.info) + '|' + pointerKey(data.result) +
        '|' + std::to_string(static_cast<int>(data.wildcard));
    for (Ty argument : data.args) key += ',' + pointerKey(argument);
    auto found = interned_.find(key);
    if (found != interned_.end()) return found->second.get();
    data.id = nextId++;
    auto stored = std::make_unique<TypeData>(std::move(data)); Ty result = stored.get();
    interned_.emplace(std::move(key), std::move(stored)); return result;
}

ClassInfo *Types::newClass(const std::string &name, ClassKind kind) {
    classes_.push_back(std::make_unique<ClassInfo>()); ClassInfo *info = classes_.back().get();
    info->name = name; info->kind = kind; info->self = named(info); return info;
}

Ty Types::named(ClassInfo *info, std::vector<Ty> args) {
    TypeData data; data.tag = TypeTag::Named; data.name = info->name; data.info = info; data.args = std::move(args);
    return intern(std::move(data));
}

Ty Types::variable(const std::string &name, Ty bound) {
    auto data = std::make_unique<TypeData>(); data->tag = TypeTag::Var; data->name = name; data->result = bound ? bound : object_; data->id = nextId++;
    variables_.push_back(std::move(data)); return variables_.back().get();
}

Ty Types::function(std::vector<Ty> parameters, Ty result) {
    TypeData data; data.tag = TypeTag::Function; data.args = std::move(parameters); data.result = result;
    return intern(std::move(data));
}

Ty Types::wildcard(WildcardBound bound, Ty limit) {
    TypeData data; data.tag = TypeTag::Wildcard; data.wildcard = bound; data.result = limit;
    return intern(std::move(data));
}

Ty Types::unite(std::vector<Ty> members) {
    std::vector<Ty> flat;
    for (Ty member : members) {
        if (member->tag == TypeTag::Error) return error_;
        if (member->tag == TypeTag::Never) continue;
        if (member->tag == TypeTag::Union) flat.insert(flat.end(), member->args.begin(), member->args.end());
        else flat.push_back(member);
    }
    std::sort(flat.begin(), flat.end(), [](Ty first, Ty second) { return first->id < second->id; });
    flat.erase(std::unique(flat.begin(), flat.end()), flat.end());
    if (flat.empty()) return never_;
    if (flat.size() == 1) return flat[0];
    TypeData data; data.tag = TypeTag::Union; data.args = std::move(flat);
    return intern(std::move(data));
}

Ty Types::substitute(Ty type, const std::map<Ty, Ty> &replacements) {
    if (replacements.empty()) return type;
    switch (type->tag) {
    case TypeTag::Var: { auto found = replacements.find(type); return found == replacements.end() ? type : found->second; }
    case TypeTag::Named: {
        if (type->args.empty()) return type;
        std::vector<Ty> arguments; for (Ty argument : type->args) arguments.push_back(substitute(argument, replacements));
        return named(type->info, std::move(arguments));
    }
    case TypeTag::Union: {
        std::vector<Ty> members; for (Ty member : type->args) members.push_back(substitute(member, replacements));
        return unite(std::move(members));
    }
    case TypeTag::Function: {
        std::vector<Ty> parameters; for (Ty parameter : type->args) parameters.push_back(substitute(parameter, replacements));
        return function(std::move(parameters), substitute(type->result, replacements));
    }
    case TypeTag::Wildcard: return type->result ? wildcard(type->wildcard, substitute(type->result, replacements)) : type;
    default: return type;
    }
}

Ty Types::asSuper(Ty type, ClassInfo *target, int depth) {
    if (type->tag != TypeTag::Named || depth > 64) return nullptr;
    if (type->info == target) return type;
    ClassInfo *info = type->info; std::map<Ty, Ty> replacements;
    for (size_t index = 0; index < info->params.size() && index < type->args.size(); index++) replacements[info->params[index]] = type->args[index];
    std::vector<Ty> parents = info->traits; if (info->super) parents.insert(parents.begin(), info->super);
    for (Ty parent : parents) if (Ty found = asSuper(substitute(parent, replacements), target, depth + 1)) return found;
    return nullptr;
}

bool Types::isNullable(Ty type) const {
    if (type->tag == TypeTag::Null) return true;
    if (type->tag == TypeTag::Union) for (Ty member : type->args) if (member->tag == TypeTag::Null) return true;
    return false;
}

Ty Types::stripNull(Ty type) {
    if (type->tag == TypeTag::Null) return never_;
    if (type->tag != TypeTag::Union) return type;
    std::vector<Ty> members; for (Ty member : type->args) if (member->tag != TypeTag::Null) members.push_back(member);
    return unite(std::move(members));
}

bool Types::isSubtype(Ty value, Ty target) {
    if (value == target || value->tag == TypeTag::Error || target->tag == TypeTag::Error || value->tag == TypeTag::Never) return true;
    if (value->tag == TypeTag::Union) {
        for (Ty member : value->args) if (!isSubtype(member, target)) return false;
        return true;
    }
    if (target->tag == TypeTag::Union) {
        for (Ty member : target->args) if (isSubtype(value, member)) return true;
        return false;
    }
    if (value->tag == TypeTag::Var) return value->result && isSubtype(value->result, target);
    switch (target->tag) {
    case TypeTag::Object: return value->tag != TypeTag::Null;
    case TypeTag::Float: return value->tag == TypeTag::Int;
    case TypeTag::Function: {
        if (value->tag != TypeTag::Function || value->args.size() != target->args.size()) return false;
        for (size_t index = 0; index < value->args.size(); index++) if (!isSubtype(target->args[index], value->args[index])) return false;
        return isSubtype(value->result, target->result);
    }
    case TypeTag::Named: {
        Ty up = asSuper(value, target->info); if (!up) return false;
        if (up->args.size() != target->args.size()) return up->args.empty() || target->args.empty();
        for (size_t index = 0; index < up->args.size(); index++) {
            Ty actual = up->args[index], expected = target->args[index];
            if (expected->tag == TypeTag::Wildcard) {
                if (expected->wildcard == WildcardBound::Extends && !isSubtype(actual, expected->result)) return false;
                if (expected->wildcard == WildcardBound::Super && !isSubtype(expected->result, actual)) return false;
            } else if (actual != expected) return false;
        }
        return true;
    }
    default: return false;
    }
}

Ty Types::join(Ty first, Ty second) {
    if (isSubtype(first, second)) return second;
    if (isSubtype(second, first)) return first;
    return unite({first, second});
}

std::string Types::show(Ty type) const {
    switch (type->tag) {
    case TypeTag::Named: {
        std::string result = type->name;
        if (!type->args.empty()) {
            result += '<';
            for (size_t index = 0; index < type->args.size(); index++) result += (index ? ", " : "") + show(type->args[index]);
            result += '>';
        }
        return result;
    }
    case TypeTag::Union: {
        std::vector<std::string> parts; bool hasNull = false;
        for (Ty member : type->args) {
            if (member->tag == TypeTag::Null) { hasNull = true; continue; }
            parts.push_back(member->tag == TypeTag::Function ? '(' + show(member) + ')' : show(member));
        }
        std::sort(parts.begin(), parts.end());
        std::string result;
        for (size_t index = 0; index < parts.size(); index++) result += (index ? " | " : "") + parts[index];
        if (!hasNull) return result;
        return parts.size() > 1 ? '(' + result + ")?" : result + '?';
    }
    case TypeTag::Function: {
        std::string result = "fn(";
        for (size_t index = 0; index < type->args.size(); index++) result += (index ? ", " : "") + show(type->args[index]);
        return result + ") -> " + show(type->result);
    }
    case TypeTag::Wildcard:
        if (type->wildcard == WildcardBound::None) return "?";
        return std::string("? ") + (type->wildcard == WildcardBound::Extends ? "extends " : "super ") + show(type->result);
    default: return type->name;
    }
}

}
