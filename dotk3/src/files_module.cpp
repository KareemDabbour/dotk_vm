#include "runtime.hpp"
#include <algorithm>

namespace dk3 {
namespace {
Object *object(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected a managed value", "type");
    return std::get<Object *>(value);
}
std::shared_ptr<FileHandle> fileOf(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected a File resource", "type");
    Object *resource = std::get<Object *>(value);
    auto handle = resource->kind == Kind::Resource ? std::dynamic_pointer_cast<FileHandle>(resource->external) : nullptr;
    if (!handle) throw Error(span, "Expected a File resource", "type");
    return handle;
}
template<class Function> Value guarded(const Span &span, Function function) {
    try { return function(); }
    catch (const Error &error) { throw Error(span, error.what(), error.category); }
    catch (const std::bad_alloc &) { throw; }
    catch (const std::length_error &error) { throw Error(span, error.what(), "limit"); }
    catch (const std::exception &error) { throw Error(span, error.what(), "io"); }
}
}

void Runtime::installFiles() {
    auto text_ = [](const std::vector<Value> &values, size_t index, const char *name, const Span &span) -> const std::string & {
        if (values.size() <= index || typeOf(values[index]) != "String") throw Error(span, std::string(name) + " must be a String", "type");
        return object(values[index], span)->text;
    };
    auto path = [text_](const std::vector<Value> &values, size_t index, const Span &span) -> const std::string & {
        const std::string &result = text_(values, index, "Path", span);
        if (result.empty() || result.find('\0') != std::string::npos) throw Error(span, "File path must be nonempty and contain no NUL", "value");
        return result;
    };
    auto bytesOf = [this](const std::vector<Value> &values, size_t index, const Span &span) -> const std::string & {
        std::string type = values.size() > index ? typeOf(values[index]) : "";
        if (type != "String" && type != "Bytes") throw Error(span, "Expected String or Bytes", "type");
        const std::string &result = object(values[index], span)->text;
        if (result.size() > config.maxFileBytes) throw Error(span, "Write exceeds configured file limit", "limit");
        return result;
    };
    auto require = [](bool available, const Span &span) { if (!available) throw Error(span, "File system access is not enabled by the host", "capability"); };
    auto arity = [](const std::vector<Value> &values, size_t minimum, size_t maximum, const char *name, const Span &span) {
        if (values.size() < minimum || values.size() > maximum) throw Error(span, std::string(name) + " received the wrong number of arguments", "call");
    };
    registerNative("Files.open", [this, path, text_, arity, require](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 2, "open", span);
        const std::string &name = path(values, 0, span);
        std::string mode = values.size() > 1 ? text_(values, 1, "Mode", span) : "r";
        if (mode != "r" && mode != "w" && mode != "a" && mode != "r+" && mode != "w+" && mode != "a+") throw Error(span, "File mode must be r, w, a, r+, w+ or a+", "value");
        require(static_cast<bool>(config.fileSystem.open), span);
        return guarded(span, [&]() -> Value {
            auto opened = config.fileSystem.open(name, mode);
            if (!opened) throw Error(span, "Host did not provide a File", "io");
            Object *resource = allocate(Kind::Resource, span); resource->external = opened; return resource;
        });
    });
    registerNative("Files.read", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 2, "read", span);
        auto file = fileOf(values[0], span); int64_t maximum = integer(values[1], span);
        if (file->closed) throw Error(span, "File is closed", "state");
        if (maximum < 0 || static_cast<uint64_t>(maximum) > config.maxFileBytes) throw Error(span, "Read size exceeds host limit", "value");
        return guarded(span, [&]() -> Value {
            std::string bytes = file->read(static_cast<size_t>(maximum));
            if (bytes.size() > static_cast<size_t>(maximum)) throw Error(span, "Host read exceeds requested maximum", "limit");
            return text(std::move(bytes), true, span);
        });
    });
    registerNative("Files.readLine", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "readLine", span);
        auto file = fileOf(values[0], span);
        if (file->closed) throw Error(span, "File is closed", "state");
        return guarded(span, [&]() -> Value {
            auto line = file->readLine(config.maxFileBytes);
            if (!line) return {};
            return text(std::move(*line), false, span);
        });
    });
    registerNative("Files.write", [this, arity, bytesOf](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 2, "write", span);
        auto file = fileOf(values[0], span); const std::string &bytes = bytesOf(values, 1, span);
        if (file->closed) throw Error(span, "File is closed", "state");
        return guarded(span, [&]() -> Value { file->write(bytes); return static_cast<int64_t>(bytes.size()); });
    });
    registerNative("Files.seek", [this, arity, text_](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 3, "seek", span);
        auto file = fileOf(values[0], span); int64_t offset = integer(values[1], span);
        std::string origin = values.size() > 2 ? text_(values, 2, "Origin", span) : "start";
        int mode = origin == "start" ? 0 : origin == "current" ? 1 : origin == "end" ? 2 : -1;
        if (mode < 0) throw Error(span, "Seek origin must be start, current or end", "value");
        if (file->closed) throw Error(span, "File is closed", "state");
        return guarded(span, [&]() -> Value { return file->seek(offset, mode); });
    });
    registerNative("Files.tell", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "tell", span);
        auto file = fileOf(values[0], span);
        if (file->closed) throw Error(span, "File is closed", "state");
        return guarded(span, [&]() -> Value { return file->seek(0, 1); });
    });
    registerNative("Files.flush", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "flush", span);
        auto file = fileOf(values[0], span);
        if (file->closed) throw Error(span, "File is closed", "state");
        return guarded(span, [&]() -> Value { file->flush(); return {}; });
    });
    registerNative("Files.close", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "close", span);
        auto file = fileOf(values[0], span);
        return guarded(span, [&]() -> Value { file->close(); return {}; });
    });
    for (const std::string &name : {"writeText", "append"}) {
        registerNative("Files." + name, [this, name, path, bytesOf, arity, require](const auto &arguments, const Span &span) -> Value {
            auto values = positional(arguments, span); arity(values, 2, 2, name.c_str(), span);
            const std::string &target = path(values, 0, span); const std::string &bytes = bytesOf(values, 1, span);
            require(static_cast<bool>(config.fileSystem.open), span);
            return guarded(span, [&]() -> Value {
                auto file = config.fileSystem.open(target, name == "append" ? "a" : "w");
                if (!file) throw Error(span, "Host did not provide a File", "io");
                try { file->write(bytes); } catch (...) { try { file->close(); } catch (...) {} throw; }
                file->close(); return {};
            });
        });
    }
    registerNative("Files.list", [this, path, arity, require](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "list", span);
        const std::string &target = path(values, 0, span);
        require(static_cast<bool>(config.fileSystem.list), span);
        return guarded(span, [&]() -> Value {
            auto names = config.fileSystem.list(target); std::sort(names.begin(), names.end());
            Object *result = allocate(Kind::List, span); roots.push_back(result);
            for (const auto &name : names) { tick(span); result->items.push_back(text(name, false, span)); }
            return result;
        });
    });
    registerNative("Files.isDirectory", [this, path, arity, require](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "isDirectory", span);
        const std::string &target = path(values, 0, span);
        require(static_cast<bool>(config.fileSystem.isDirectory), span);
        return guarded(span, [&]() -> Value { return config.fileSystem.isDirectory(target); });
    });
    registerNative("Files.mkdir", [this, path, arity, require](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "mkdir", span);
        const std::string &target = path(values, 0, span);
        require(static_cast<bool>(config.fileSystem.makeDirectory), span);
        return guarded(span, [&]() -> Value { config.fileSystem.makeDirectory(target); return {}; });
    });
    registerNative("Files.remove", [this, path, arity, require](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "remove", span);
        const std::string &target = path(values, 0, span);
        require(static_cast<bool>(config.fileSystem.remove), span);
        return guarded(span, [&]() -> Value { config.fileSystem.remove(target); return {}; });
    });
    registerNative("Files.rename", [this, path, arity, require](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 2, 2, "rename", span);
        std::string from = path(values, 0, span), to = path(values, 1, span);
        require(static_cast<bool>(config.fileSystem.rename), span);
        return guarded(span, [&]() -> Value { config.fileSystem.rename(from, to); return {}; });
    });
}
}
