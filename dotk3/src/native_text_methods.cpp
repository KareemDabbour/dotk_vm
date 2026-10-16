#include "native_methods.hpp"

namespace dk3 {
Object *TextMethods::needle(const std::vector<Argument> &arguments, const Span &span) {
    if (!std::holds_alternative<Object *>(arguments[0].value) || NativeMethods::object(arguments[0].value, span)->kind != Kind::String)
        throw Error(span, "String method expects one String", "call");
    return NativeMethods::object(arguments[0].value, span);
}
Value TextMethods::split(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    const auto &separator = needle(arguments, span)->text;
    if (separator.empty()) throw Error(span, "Split separator cannot be empty", "value");
    for (size_t index = 0; index < receiver->text.size(); index++) runtime.tick(span);
    Object *parts = runtime.allocate(Kind::List, span); runtime.roots.push_back(parts); parts->elementContract = runtime.resolveContract("String", runtime.prelude, span);
    size_t start = 0;
    for (;;) {
        runtime.tick(span); size_t found = receiver->text.find(separator, start);
        parts->items.push_back(runtime.text(receiver->text.substr(start, found == std::string::npos ? found : found - start), false, span));
        if (found == std::string::npos) break;
        start = found + separator.size();
    }
    return parts;
}
Value TextMethods::join(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    std::string joined; Value item; size_t cursor = 0; bool first = true;
    while (runtime.next(arguments[0].value, item, cursor, span)) {
        runtime.roots.push_back(item); runtime.tick(span);
        if (!std::holds_alternative<Object *>(item) || NativeMethods::object(item, span)->kind != Kind::String) throw Error(span, "join elements must be String", "type");
        const auto &part = NativeMethods::object(item, span)->text;
        for (size_t index = 0; index < part.size(); index++) runtime.tick(span);
        if (!first) { for (size_t index = 0; index < receiver->text.size(); index++) runtime.tick(span); joined += receiver->text; }
        first = false; joined += part;
    }
    return runtime.text(std::move(joined), false, span);
}
void TextMethods::install(Runtime &runtime) {
    runtime.registerNativeMethod({Kind::String, "length", "Int", 0, 0, NativeMethods::length});
    runtime.registerNativeMethod({Kind::Bytes, "length", "Int", 0, 0, NativeMethods::length});
    NativeMethods::install(runtime, Kind::String, stringMembers(), {
        {"split", split}, {"join", join},
        {"contains", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &search = needle(arguments, span)->text;
            for (size_t index = 0; index < receiver->text.size(); index++) runtime.tick(span);
            return receiver->text.find(search) != std::string::npos;
        }},
        {"find", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &search = needle(arguments, span)->text;
            for (size_t index = 0; index < receiver->text.size(); index++) runtime.tick(span);
            size_t found = receiver->text.find(search); if (found == std::string::npos) return int64_t{-1};
            auto offsets = utf8Offsets(receiver->text, span); return static_cast<int64_t>(std::lower_bound(offsets.begin(), offsets.end(), found) - offsets.begin());
        }},
        {"startsWith", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &search = needle(arguments, span)->text;
            for (size_t index = 0; index < std::min(receiver->text.size(), search.size()); index++) runtime.tick(span);
            return receiver->text.compare(0, search.size(), search) == 0;
        }},
        {"endsWith", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &search = needle(arguments, span)->text;
            for (size_t index = 0; index < std::min(receiver->text.size(), search.size()); index++) runtime.tick(span);
            return search.size() <= receiver->text.size() && receiver->text.compare(receiver->text.size() - search.size(), search.size(), search) == 0;
        }}
    });
}
}