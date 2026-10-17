#pragma once
#include "syntax.hpp"
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dk3 {

enum class TypeTag { Error, Never, Null, Bool, Int, Float, Object, Named, Var, Union, Intersection, Function, Wildcard };

struct ClassInfo;
struct TypeData {
    TypeTag tag = TypeTag::Error;
    std::string name;
    // Named: type arguments. Union: members. Function: parameters.
    std::vector<const TypeData *> args;
    // Function: result. Var: upper bound. Wildcard: bound type (may be null).
    const TypeData *result = nullptr;
    ClassInfo *info = nullptr;
    WildcardBound wildcard = WildcardBound::None;
    unsigned id = 0;
};
using Ty = const TypeData *;

struct Signature {
    std::vector<std::string> names;
    std::vector<bool> defaults;
    bool hasRest = false;
    std::vector<Ty> typeParams;
    Ty type = nullptr;
    Ast declaration;
    // Return type still being inferred from the body (unannotated function with return values).
    bool pendingResult = false;
    bool inferring = false;
};

struct Member {
    Ty type = nullptr;
    std::shared_ptr<Signature> signature;
    bool isMethod = false;
    bool isStatic = false;
    bool mutableBinding = false;
    bool hasDefault = false;
    Visibility visibility = Visibility::Public;
    ClassInfo *owner = nullptr;
};

enum class ClassKind { Class, Record, Trait, Enum };

struct Variant {
    std::string name;
    std::vector<Ty> payload;
};

struct ClassInfo {
    std::string name;
    ClassKind kind = ClassKind::Class;
    std::vector<Ty> params;
    Ty super = nullptr;
    std::vector<Ty> traits;
    std::map<std::string, Member> members;
    std::vector<std::string> fieldOrder;
    std::vector<Variant> variants;
    std::shared_ptr<Signature> constructor;
    Ast declaration;
    Ty self = nullptr;
    bool builtin = false;
};

class Types {
public:
    Types();
    Ty error() const { return error_; }
    Ty never() const { return never_; }
    Ty null() const { return null_; }
    Ty boolean() const { return bool_; }
    Ty integer() const { return int_; }
    Ty floating() const { return float_; }
    Ty object() const { return object_; }
    Ty named(ClassInfo *info, std::vector<Ty> args = {});
    Ty variable(const std::string &name, Ty bound = nullptr);
    void setBound(Ty variable, Ty bound) { const_cast<TypeData *>(variable)->result = bound; }
    Ty unite(std::vector<Ty> members);
    Ty intersect(std::vector<Ty> members);
    Ty nullable(Ty type) { return unite({type, null_}); }
    Ty function(std::vector<Ty> parameters, Ty result);
    Ty wildcard(WildcardBound bound, Ty limit);
    ClassInfo *newClass(const std::string &name, ClassKind kind);

    bool isSubtype(Ty value, Ty target);
    Ty substitute(Ty type, const std::map<Ty, Ty> &replacements);
    Ty asSuper(Ty type, ClassInfo *target, int depth = 0);
    Ty stripNull(Ty type);
    bool isNullable(Ty type) const;
    bool isNumeric(Ty type) const { return type == int_ || type == float_; }
    Ty join(Ty first, Ty second);
    std::string show(Ty type) const;

private:
    Ty intern(TypeData data);
    Ty error_, never_, null_, bool_, int_, float_, object_;
    std::map<std::string, std::unique_ptr<TypeData>> interned_;
    std::vector<std::unique_ptr<TypeData>> variables_;
    std::vector<std::unique_ptr<ClassInfo>> classes_;
};

}
