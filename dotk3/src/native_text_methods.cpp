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
    Object *parts = runtime.allocate(Kind::List, span); runtime.roots.push_back(parts);
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
namespace {
int64_t intArgument(const Argument &argument, const char *name, const Span &span) {
    if (!std::holds_alternative<int64_t>(argument.value))
        throw Error(span, std::string(name) + " must be an Int, got " + Runtime::typeOf(argument.value), "type");
    return std::get<int64_t>(argument.value);
}
Object *bytesArgument(const Argument &argument, const char *method, const Span &span) {
    if (!std::holds_alternative<Object *>(argument.value) || std::get<Object *>(argument.value)->kind != Kind::Bytes)
        throw Error(span, std::string(method) + " expects Bytes, got " + Runtime::typeOf(argument.value), "type");
    return std::get<Object *>(argument.value);
}
bool littleEndian(const std::vector<Argument> &arguments, size_t index, const Span &span) {
    if (arguments.size() <= index) return false;
    const Value &order = arguments[index].value;
    const std::string *text = std::holds_alternative<Object *>(order) && std::get<Object *>(order)->kind == Kind::String ? &std::get<Object *>(order)->text : nullptr;
    if (!text || (*text != "big" && *text != "little")) throw Error(span, "Byte order must be \"big\" or \"little\"", "value");
    return *text == "little";
}
// size (at most 8) bytes at offset as an unsigned or two's-complement Int; 8 unsigned bytes keep all 64 bits.
int64_t decodeInt(const std::string &bytes, size_t offset, size_t size, bool little, bool sign) {
    uint64_t value = 0;
    for (size_t index = 0; index < size; index++)
        value = value << 8 | static_cast<unsigned char>(bytes[offset + (little ? size - 1 - index : index)]);
    if (sign && size && size < 8 && (value >> (size * 8 - 1) & 1)) value |= ~uint64_t{0} << (size * 8);
    return static_cast<int64_t>(value);
}
template <bool sign>
Value wholeInt(Runtime &, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    if (receiver->text.size() > 8)
        throw Error(span, "Int conversion needs at most 8 bytes, got " + std::to_string(receiver->text.size()) + "; slice first", "value");
    return decodeInt(receiver->text, 0, receiver->text.size(), littleEndian(arguments, 0, span), sign);
}
template <bool sign>
Value readInt(Runtime &, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    int64_t offset = intArgument(arguments[0], "offset", span), size = intArgument(arguments[1], "size", span);
    if (size < 1 || size > 8) throw Error(span, "Int read size must be 1 to 8 bytes", "value");
    if (offset < 0 || static_cast<uint64_t>(offset) + static_cast<uint64_t>(size) > receiver->text.size())
        throw Error(span, "Read of " + std::to_string(size) + " bytes at " + std::to_string(offset) + " is outside " + std::to_string(receiver->text.size()) + " bytes", "index");
    return decodeInt(receiver->text, static_cast<size_t>(offset), static_cast<size_t>(size), littleEndian(arguments, 2, span), sign);
}
}
void TextMethods::installBytes(Runtime &runtime) {
    std::initializer_list<std::pair<const char *, NativeMethod::Body>> bodies{
        {"toInt", wholeInt<false>}, {"toSignedInt", wholeInt<true>}, {"readInt", readInt<false>}, {"readSignedInt", readInt<true>},
        {"find", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &needle = bytesArgument(arguments[0], "find", span)->text;
            int64_t start = arguments.size() > 1 ? intArgument(arguments[1], "start", span) : 0;
            if (start < 0 || static_cast<uint64_t>(start) > receiver->text.size()) throw Error(span, "find start is outside the Bytes", "index");
            for (size_t index = static_cast<size_t>(start); index < receiver->text.size(); index++) runtime.tick(span);
            size_t found = receiver->text.find(needle, static_cast<size_t>(start));
            return found == std::string::npos ? int64_t{-1} : static_cast<int64_t>(found);
        }},
        {"contains", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &needle = bytesArgument(arguments[0], "contains", span)->text;
            for (size_t index = 0; index < receiver->text.size(); index++) runtime.tick(span);
            return receiver->text.find(needle) != std::string::npos;
        }},
        {"startsWith", [](Runtime &, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &prefix = bytesArgument(arguments[0], "startsWith", span)->text;
            return receiver->text.compare(0, prefix.size(), prefix) == 0;
        }},
        {"endsWith", [](Runtime &, Object *receiver, const auto &arguments, const Span &span) -> Value {
            const auto &suffix = bytesArgument(arguments[0], "endsWith", span)->text;
            return suffix.size() <= receiver->text.size() && receiver->text.compare(receiver->text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }},
        {"join", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value {
            std::string joined; Value item; size_t cursor = 0; bool first = true;
            while (runtime.next(arguments[0].value, item, cursor, span)) {
                runtime.roots.push_back(item);
                const auto &part = bytesArgument({std::string(), item}, "join", span)->text;
                for (size_t index = 0; index < part.size() + (first ? 0 : receiver->text.size()); index++) runtime.tick(span);
                if (!first) joined += receiver->text;
                first = false; joined += part;
            }
            return runtime.text(std::move(joined), true, span);
        }}};
    for (const auto &body : bodies) {
        auto member = std::find_if(bytesMembers().begin(), bytesMembers().end(), [&](const BytesMember &entry) { return std::string(entry.name) == body.first; });
        if (member == bytesMembers().end()) throw Error({}, "Uncatalogued native method '" + std::string(body.first) + "'", "native");
        runtime.registerNativeMethod({Kind::Bytes, member->name, member->result, member->minimum, member->maximum, body.second, false, member->parameters});
    }
    if (bodies.size() != bytesMembers().size()) throw Error({}, "Native method catalogue/implementation count mismatch", "native");
}
void TextMethods::install(Runtime &runtime) {
    runtime.registerNativeMethod({Kind::String, "length", "Int", 0, 0, NativeMethods::length});
    runtime.registerNativeMethod({Kind::Bytes, "length", "Int", 0, 0, NativeMethods::length});
    installBytes(runtime);
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