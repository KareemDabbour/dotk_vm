#include "runtime.hpp"
#include <algorithm>
#include <cstdint>
#include <numeric>

namespace dk3 {
namespace {
Object *object(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected a managed value", "type");
    return std::get<Object *>(value);
}
const char *platformName() {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}
}

void Runtime::installLibrary() {
    auto arity = [](const std::vector<Value> &values, size_t minimum, size_t maximum, const char *name, const Span &span) {
        if (values.size() < minimum || values.size() > maximum) throw Error(span, std::string(name) + " received the wrong number of arguments", "call");
    };
    auto hostCall = [](const Span &span, auto function) -> Value {
        try { return function(); }
        catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; }
        catch (const std::exception &error) { throw Error(span, error.what(), "io"); }
    };

    registerNative("System.platform", [this, arity](const auto &arguments, const Span &span) -> Value {
        arity(positional(arguments, span), 0, 0, "platform", span);
        return text(platformName(), false, span);
    });
    registerNative("System.env", [this, arity, hostCall](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "env", span);
        if (typeOf(values[0]) != "String") throw Error(span, "env expects a String name", "type");
        const std::string &name = object(values[0], span)->text;
        if (name.empty() || name.find('\0') != std::string::npos || name.find('=') != std::string::npos) throw Error(span, "Invalid environment variable name", "value");
        if (!config.environment) throw Error(span, "Environment access is not enabled by the host", "capability");
        return hostCall(span, [&]() -> Value {
            auto value = config.environment(name);
            return value ? text(std::move(*value), false, span) : Value{};
        });
    });

    registerNative("GC.collect", [this, arity](const auto &arguments, const Span &span) -> Value {
        arity(positional(arguments, span), 0, 0, "collect", span);
        collectionRequested = true; return {};
    });
    registerNative("GC.stats", [this, arity](const auto &arguments, const Span &span) -> Value {
        arity(positional(arguments, span), 0, 0, "stats", span);
        Object *result = allocate(Kind::Map, span); roots.push_back(result);
        const std::pair<const char *, size_t> entries[] = {{"objects", heap.size()}, {"collections", collections},
            {"nextCollection", nextCollection}, {"objectLimit", config.objectLimit}, {"steps", static_cast<size_t>(steps)}};
        for (const auto &entry : entries) setEntry(result, text(entry.first, false, span), static_cast<int64_t>(entry.second), span);
        return result;
    });

    registerNative("Reflection.has", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 2, "has", span);
        if (typeOf(values[1]) != "String") throw Error(span, "has expects a String member name", "type");
        const std::string &name = object(values[1], span)->text;
        for (const auto &field : inspectMembers(values[0])) if (field.name == name) return true;
        return false;
    });
    registerNative("Reflection.get", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 2, "get", span);
        if (typeOf(values[1]) != "String") throw Error(span, "get expects a String member name", "type");
        return member(values[0], object(values[1], span)->text, span);
    });
    registerNative("Reflection.isInstance", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 2, "isInstance", span);
        Object *type = object(values[1], span);
        if (type->kind != Kind::ClassType && type->kind != Kind::RecordType && type->kind != Kind::TraitType && type->kind != Kind::EnumType)
            throw Error(span, "isInstance expects a type as its second argument", "type");
        if (!std::holds_alternative<Object *>(values[0])) return false;
        if (type == objectType) return true;
        Object *instance = std::get<Object *>(values[0]), *origin = instance->origin;
        if (!origin) { auto found = collectionTypes.find(instance->kind); if (found != collectionTypes.end()) origin = found->second; }
        for (Object *current = origin; current; current = current->baseClass) {
            if (current == type) return true;
            if (std::find(current->traits.begin(), current->traits.end(), type) != current->traits.end()) return true;
        }
        return false;
    });
    registerNative("Reflection.id", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "id", span);
        return static_cast<int64_t>(reinterpret_cast<std::uintptr_t>(object(values[0], span)) >> 3);
    });

    // Copies an iterable into a rooted List so callbacks run while every element stays reachable.
    auto gather = [this](Value source, const Span &span) {
        Object *list = allocate(Kind::List, span); roots.push_back(list);
        Value item; size_t cursor = 0;
        while (next(source, item, cursor, span)) { tick(span); list->items.push_back(item); }
        return list;
    };
    auto less = [this](Value left, Value right, const Span &span) {
        if (typeOf(left) == "String" && typeOf(right) == "String") return object(left, span)->text < object(right, span)->text;
        return boolean(binary("<", left, right, span), span);
    };
    registerNative("Collections.sorted", [this, arity, gather, less](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 2, "sorted", span);
        Object *source = gather(values[0], span);
        bool custom = values.size() == 2 && !std::holds_alternative<std::monostate>(values[1]);
        std::vector<size_t> order(source->items.size()); std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
            tick(span);
            if (custom) return integer(call(values[1], {{"", source->items[left]}, {"", source->items[right]}}, span), span) < 0;
            return less(source->items[left], source->items[right], span);
        });
        Object *result = allocate(Kind::List, span);
        for (size_t position : order) result->items.push_back(source->items[position]);
        return result;
    });
    registerNative("Collections.reversed", [this, arity, gather](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "reversed", span);
        Object *result = gather(values[0], span); std::reverse(result->items.begin(), result->items.end()); return result;
    });
    for (const std::string &name : {"min", "max"}) {
        registerNative("Collections." + name, [this, name, arity, gather, less](const auto &arguments, const Span &span) -> Value {
            auto values = positional(arguments, span); arity(values, 1, 1, name.c_str(), span);
            Object *source = gather(values[0], span);
            if (source->items.empty()) throw Error(span, name + " of an empty sequence", "value");
            Value best = source->items[0];
            for (size_t position = 1; position < source->items.size(); position++) {
                tick(span); Value candidate = source->items[position];
                if (name == "min" ? less(candidate, best, span) : less(best, candidate, span)) best = candidate;
            }
            return best;
        });
    }
    registerNative("Collections.enumerate", [this, arity, gather](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 2, "enumerate", span);
        int64_t start = values.size() == 2 ? integer(values[1], span) : 0;
        Object *source = gather(values[0], span), *result = allocate(Kind::List, span); roots.push_back(result);
        for (size_t position = 0; position < source->items.size(); position++) {
            Object *pair = allocate(Kind::Tuple, span);
            pair->items = {start + static_cast<int64_t>(position), source->items[position]}; result->items.push_back(pair);
        }
        return result;
    });
    registerNative("Collections.zip", [this, arity, gather](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 2, "zip", span);
        Object *left = gather(values[0], span), *right = gather(values[1], span), *result = allocate(Kind::List, span); roots.push_back(result);
        for (size_t position = 0; position < std::min(left->items.size(), right->items.size()); position++) {
            Object *pair = allocate(Kind::Tuple, span); pair->items = {left->items[position], right->items[position]}; result->items.push_back(pair);
        }
        return result;
    });
}
}
