#pragma once
#include "runtime.hpp"
#include "core.hpp"
#include <algorithm>

namespace dk3 {
struct NativeMethods {
    static Value length(Runtime &runtime, Object *receiver, const std::vector<Argument> &, const Span &span);
    static Value initialize(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span);
    static Object *object(Value value, const Span &span);
    static Object *callback(Value value, const Span &span);
    static void key(Runtime &runtime, Object *receiver, Value value, const Span &span);
    static int64_t find(Runtime &runtime, Object *receiver, Value value, const Span &span);
    static Object *snapshot(Runtime &runtime, Object *receiver, const Span &span);
    static void collection(Runtime &runtime, Kind kind);
    template<class Member> static void install(Runtime &runtime, Kind kind, const std::vector<Member> &catalogue,
                                             std::initializer_list<std::pair<const char *, NativeMethod::Body>> bodies) {
        if (catalogue.size() != bodies.size()) throw Error({}, "Native method catalogue/implementation count mismatch", "native");
        for (const auto &body : bodies) {
            auto signature = std::find_if(catalogue.begin(), catalogue.end(), [&](const Member &member) { return std::string(member.name) == body.first; });
            if (signature == catalogue.end()) throw Error({}, "Uncatalogued native method '" + std::string(body.first) + "'", "native");
            runtime.registerNativeMethod({kind, signature->name, signature->result, signature->arguments, signature->arguments, body.second});
        }
    }
};
struct ListMethods {
    static void install(Runtime &runtime);
private:
    enum class Transform { Map, Filter, Reduce, Any, All, Find, FindIndex };
    template<Transform operation> static Value transform(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span);
};
struct MapMethods {
    static void install(Runtime &runtime);
private:
    enum class Update { Compute, IfAbsent, IfPresent, PutIfAbsent, Merge };
    template<Update operation> static Value update(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span);
};
struct SetMethods {
    static void install(Runtime &runtime);
private:
    enum class Algebra { Union, Intersection, Difference, Subset };
    template<Algebra operation> static Value combine(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span);
};
struct TextMethods {
    static void install(Runtime &runtime);
private:
    static Object *needle(const std::vector<Argument> &arguments, const Span &span);
    static Value split(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span);
    static Value join(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span);
};
}