#include "native_methods.hpp"

namespace dk3 {
template<SetMethods::Algebra operation>
Value SetMethods::combine(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    Object *other = NativeMethods::object(arguments[0].value, span); if (other->kind != Kind::Set) throw Error(span, "Set operation requires another Set", "type");
    if constexpr (operation == Algebra::Subset) {
        for (Value item : receiver->items) if (NativeMethods::find(runtime, other, item, span) < 0) return false;
        return true;
    }
    Object *result = runtime.allocate(Kind::Set, span); runtime.roots.push_back(result);
    for (Value item : receiver->items) {
        runtime.tick(span);
        if constexpr (operation == Algebra::Union) result->items.push_back(item);
        else {
            bool present = NativeMethods::find(runtime, other, item, span) >= 0;
            if constexpr (operation == Algebra::Intersection) { if (present) result->items.push_back(item); }
            if constexpr (operation == Algebra::Difference) { if (!present) result->items.push_back(item); }
        }
    }
    if constexpr (operation == Algebra::Union) for (Value item : other->items) if (NativeMethods::find(runtime, result, item, span) < 0) result->items.push_back(item);
    if (receiver->elementContract) { auto contract = std::make_shared<TypeContract>(); contract->name = "Set"; contract->element = receiver->elementContract; runtime.requireType(result, contract, span); }
    return result;
}
void SetMethods::install(Runtime &runtime) {
    NativeMethods::collection(runtime, Kind::Set);
    NativeMethods::install(runtime, Kind::Set, setMembers(), {
        {"union", combine<Algebra::Union>}, {"intersection", combine<Algebra::Intersection>}, {"difference", combine<Algebra::Difference>}, {"isSubsetOf", combine<Algebra::Subset>},
        {"clear", [](Runtime &, Object *receiver, const auto &, const Span &) -> Value { receiver->items.clear(); return {}; }},
        {"values", [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value { return NativeMethods::snapshot(runtime, receiver, span); }},
        {"add", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value value = arguments[0].value; runtime.requireType(value, receiver->elementContract, span);
            if (NativeMethods::find(runtime, receiver, value, span) >= 0) return false;
            receiver->items.push_back(value); return true;
        }},
        {"remove", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value value = arguments[0].value; runtime.requireType(value, receiver->elementContract, span);
            int64_t index = NativeMethods::find(runtime, receiver, value, span); if (index < 0) return false;
            receiver->items.erase(receiver->items.begin() + index); return true;
        }},
        {"contains", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            runtime.requireType(arguments[0].value, receiver->elementContract, span); return NativeMethods::find(runtime, receiver, arguments[0].value, span) >= 0;
        }}
    });
}
}