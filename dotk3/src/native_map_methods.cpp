#include "native_methods.hpp"

namespace dk3 {
template<MapMethods::Update operation>
Value MapMethods::update(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
    if constexpr (operation == Update::Merge) if (std::holds_alternative<std::monostate>(arguments[1].value)) throw Error(span, "merge requires a non-null value", "value");
    Object *callback = nullptr;
    if constexpr (operation != Update::PutIfAbsent) callback = NativeMethods::callback(arguments.back().value, span);
    Value previous;
    size_t found = runtime.entryPosition(receiver, key);
    runtime.tickEntries(span, found < receiver->entries.size() ? found + 1 : found);
    if (found < receiver->entries.size()) previous = receiver->entries[found].second;
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
    runtime.roots.push_back(computed); runtime.tickEntries(span, receiver->entries.size());
    if (std::holds_alternative<std::monostate>(computed)) { if constexpr (operation != Update::IfAbsent) runtime.removeEntry(receiver, key, span); }
    else runtime.setEntry(receiver, key, computed, span);
    return computed;
}
void MapMethods::install(Runtime &runtime) {
    NativeMethods::collection(runtime, Kind::Map);
    NativeMethods::install(runtime, Kind::Map, mapMembers(), {
        {"compute", update<Update::Compute>}, {"computeIfAbsent", update<Update::IfAbsent>}, {"computeIfPresent", update<Update::IfPresent>},
        {"putIfAbsent", update<Update::PutIfAbsent>}, {"merge", update<Update::Merge>},
        {"clear", [](Runtime &, Object *receiver, const auto &, const Span &) -> Value { receiver->entries.clear(); receiver->entryIndex.reset(); return {}; }},
        {"contains", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
            size_t found = runtime.entryPosition(receiver, key), size = receiver->entries.size();
            runtime.tickEntries(span, found < size ? found + 1 : size); return found < size;
        }},
        {"get", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
            size_t found = runtime.entryPosition(receiver, key), size = receiver->entries.size();
            runtime.tickEntries(span, found < size ? found + 1 : size); return found < size ? receiver->entries[found].second : Value{};
        }},
        {"remove", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            Value key = arguments[0].value; NativeMethods::key(runtime, receiver, key, span);
            size_t found = runtime.entryPosition(receiver, key), size = receiver->entries.size();
            runtime.tickEntries(span, found < size ? found + 1 : size); if (found == size) return false;
            receiver->entries.erase(receiver->entries.begin() + static_cast<std::ptrdiff_t>(found)); receiver->entryIndex.reset(); return true;
        }},
        {"keys", [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value {
            Object *snapshot = runtime.allocate(Kind::List, span); runtime.roots.push_back(snapshot);
            for (const auto &entry : receiver->entries) { runtime.tick(span); snapshot->items.push_back(entry.first); } return snapshot;
        }},
        {"values", [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value {
            Object *snapshot = runtime.allocate(Kind::List, span); runtime.roots.push_back(snapshot);
            for (const auto &entry : receiver->entries) { runtime.tick(span); snapshot->items.push_back(entry.second); } return snapshot;
        }}
    });
}
}