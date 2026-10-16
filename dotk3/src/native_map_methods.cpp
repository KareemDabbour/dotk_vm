#include "native_methods.hpp"

namespace dk3 {
template<MapMethods::Update operation>
Value MapMethods::update(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
    if constexpr (operation == Update::PutIfAbsent || operation == Update::Merge) runtime.requireType(arguments[1].value, receiver->elementContract, span);
    if constexpr (operation == Update::Merge) if (std::holds_alternative<std::monostate>(arguments[1].value)) throw Error(span, "merge requires a non-null value", "value");
    Object *callback = nullptr;
    if constexpr (operation != Update::PutIfAbsent) callback = NativeMethods::callback(arguments.back().value, span);
    Value previous;
    for (const auto &entry : receiver->entries) { runtime.tick(span); if (Runtime::equal(entry.first, key)) { previous = entry.second; break; } }
    runtime.roots.push_back(previous); bool absent = std::holds_alternative<std::monostate>(previous);
    if constexpr (operation == Update::IfPresent) { if (absent) return {}; }
    if constexpr (operation == Update::IfAbsent || operation == Update::PutIfAbsent) { if (!absent) return previous; }
    if constexpr (operation == Update::PutIfAbsent) { runtime.setEntry(receiver, key, arguments[1].value, span); return {}; }
    Value computed;
    if constexpr (operation == Update::Merge) {
        if (absent) computed = arguments[1].value;
        else computed = runtime.call(callback, {{"", previous}, {"", arguments[1].value}}, span);
    } else if constexpr (operation == Update::IfAbsent) computed = runtime.call(callback, {{"", key}}, span);
    else computed = runtime.call(callback, {{"", key}, {"", previous}}, span);
    runtime.roots.push_back(computed); for (size_t index = 0; index < receiver->entries.size(); index++) runtime.tick(span);
    if (std::holds_alternative<std::monostate>(computed)) { if constexpr (operation != Update::IfAbsent) runtime.removeEntry(receiver, key, span); }
    else runtime.setEntry(receiver, key, computed, span);
    return computed;
}
void MapMethods::install(Runtime &runtime) {
    NativeMethods::collection(runtime, Kind::Map);
    NativeMethods::install(runtime, Kind::Map, mapMembers(), {
        {"compute", update<Update::Compute>}, {"computeIfAbsent", update<Update::IfAbsent>}, {"computeIfPresent", update<Update::IfPresent>},
        {"putIfAbsent", update<Update::PutIfAbsent>}, {"merge", update<Update::Merge>},
        {"clear", [](Runtime &, Object *receiver, const auto &, const Span &) -> Value { receiver->entries.clear(); return {}; }},
        {"contains", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
            for (const auto &entry : receiver->entries) { runtime.tick(span); if (Runtime::equal(entry.first, key)) return true; } return false;
        }},
        {"get", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
            for (const auto &entry : receiver->entries) { runtime.tick(span); if (Runtime::equal(entry.first, key)) return entry.second; } return {};
        }},
        {"remove", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
            for (auto entry = receiver->entries.begin(); entry != receiver->entries.end(); ++entry) {
                runtime.tick(span); if (!Runtime::equal(entry->first, key)) continue; receiver->entries.erase(entry); return true;
            } return false;
        }},
        {"keys", [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value {
            Object *snapshot = runtime.allocate(Kind::List, span); runtime.roots.push_back(snapshot); snapshot->elementContract = receiver->keyContract;
            for (const auto &entry : receiver->entries) { runtime.tick(span); snapshot->items.push_back(entry.first); } return snapshot;
        }},
        {"values", [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value {
            Object *snapshot = runtime.allocate(Kind::List, span); runtime.roots.push_back(snapshot); snapshot->elementContract = receiver->elementContract;
            for (const auto &entry : receiver->entries) { runtime.tick(span); snapshot->items.push_back(entry.second); } return snapshot;
        }}
    });
}
}