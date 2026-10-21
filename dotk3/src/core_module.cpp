#include "core.hpp"
#include "runtime.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dk3
{
    void Runtime::configureFiles(std::function<std::string(const std::string &, size_t)> read,
                                 std::function<std::optional<uint64_t>(const std::string &)> info, size_t limit)
    {
        requireRunning();
        config.readFile = std::move(read);
        config.fileSize = std::move(info);
        config.maxFileBytes = limit;
    }
    namespace
    {
        Span coreSpan() { return {std::make_shared<Source>(Source{"<core>", ""}), 0, 0, 1, 1}; }
        Object *object(Value value, const Span &span)
        {
            if (!std::holds_alternative<Object *>(value))
                throw Error(span, "Expected object, got " + Runtime::typeOf(value), "type");
            return std::get<Object *>(value);
        }
    }
    void Runtime::installCore()
    {
        prelude = allocate(Kind::Environment, coreSpan());
        global = allocate(Kind::Environment, coreSpan());
        global->parent = prelude;
        objectType = allocate(Kind::ClassType, coreSpan());
        objectType->text = "Object";
        objectType->parent = prelude;
        objectType->code = std::make_shared<Node>();
        objectType->code->kind = "class";
        objectType->code->text = "Object";
        objectType->code->span = coreSpan();
        define(prelude, "Object", objectType, false, "", coreSpan());
        errorType = allocate(Kind::RecordType, coreSpan());
        errorType->text = "Error";
        errorType->parent = prelude;
        errorType->code = std::make_shared<Node>();
        errorType->code->kind = "record";
        errorType->code->span = coreSpan();
        errorKindType = allocate(Kind::EnumType, coreSpan());
        errorKindType->text = "Error.Kind";
        errorKindType->parent = prelude;
        errorKindType->code = std::make_shared<Node>();
        errorKindType->code->kind = "enum";
        errorKindType->code->text = "Error.Kind";
        errorKindType->code->span = coreSpan();
        for (const auto &kind : errorKinds())
        {
            Object *value = allocate(Kind::Variant, coreSpan());
            value->origin = errorKindType;
            value->text = kind.second;
            value->code = std::make_shared<Node>();
            value->code->kind = "variant";
            value->code->text = kind.first;
            value->code->span = coreSpan();
            errorKindType->code->children.push_back(value->code);
            define(errorKindType, kind.first, value, false, "", coreSpan());
        }
        define(errorType, "Kind", errorKindType, false, "", coreSpan());
        define(prelude, "Error", errorType, false, "", coreSpan());
        auto defaultLiteral = [](const std::string &text, TokenKind kind)
        {
            Ast result = std::make_shared<Node>();
            result->kind = "literal";
            result->text = text;
            result->literalKind = kind;
            result->span = coreSpan();
            return result;
        };
        errorType->code->parameters = {
            {"kind", "Error.Kind", {}, false}, {"message", "String", {}, false}, {"file", "String", defaultLiteral("", TokenKind::String), false}, {"line", "Int", defaultLiteral("0", TokenKind::Integer), false}, {"column", "Int", defaultLiteral("0", TokenKind::Integer), false}};

        // Define primitive type objects for method references like Int::toString
        auto makeClassType = [&](const std::string &name) -> Object *
        {
            Object *classType = allocate(Kind::ClassType, coreSpan());
            classType->text = name;
            classType->parent = prelude;
            classType->code = std::make_shared<Node>();
            classType->code->kind = "class";
            classType->code->text = name;
            classType->code->span = coreSpan();
            define(prelude, name, classType, false, "", coreSpan());
            return classType;
        };
        makeClassType("Int");
        makeClassType("Float");
        makeClassType("Bool");
        makeClassType("String");

        sizedTrait = allocate(Kind::TraitType, coreSpan());
        roots.push_back(sizedTrait);
        sizedTrait->text = "Sized";
        sizedTrait->code = sizedDeclaration();
        sizedTrait->parent = prelude;
        define(coreNamespace("Collections"), "Sized", sizedTrait, false, "", coreSpan());
        Object *arguments = allocate(Kind::Tuple, coreSpan());
        for (const auto &argument : config.arguments)
            arguments->items.push_back(text(argument, false, coreSpan()));
        define(coreNamespace("System"), "args", arguments, false, "Tuple", coreSpan());
        installNetwork();
        installFiles();
        installLibrary();
        installStdlib();
        registerNative("System.print", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        std::string output;
        for (size_t index = 0; index < values.size(); index++) { if (index) output += ' '; output += stringify(values[index], span); }
        if (config.write) config.write(output + '\n');
        return {}; });
        for (const std::string &name : {"readText", "readBytes", "exists", "size"})
        {
            registerNative("Files." + name, [this, name](const auto &arguments, const Span &span) -> Value
                           {
            auto values = positional(arguments, span);
            if (values.size() != 1 || !std::holds_alternative<Object *>(values[0]) || object(values[0], span)->kind != Kind::String)
                throw Error(span, name + " expects one String path", "call");
            const auto &path = object(values[0], span)->text;
            if (path.empty() || path.find('\0') != std::string::npos) throw Error(span, "File path must be nonempty and contain no NUL", "value");
            bool reading = name == "readText" || name == "readBytes";
            if ((reading && !config.readFile) || (!reading && !config.fileSize))
                throw Error(span, "File access is not enabled by the host", "capability");
            try {
                if (reading) {
                    std::string bytes = config.readFile(path, config.maxFileBytes);
                    if (bytes.size() > config.maxFileBytes) throw Error(span, "File exceeds configured read limit", "limit");
                    return text(std::move(bytes), name == "readBytes", span);
                }
                auto size = config.fileSize(path);
                if (name == "exists") return size.has_value();
                if (!size) throw Error(span, "File does not exist", "io");
                if (*size > static_cast<uint64_t>(INT64_MAX)) throw Error(span, "File size exceeds Int range", "overflow");
                return static_cast<int64_t>(*size);
            } catch (const Error &error) { throw Error(span, error.what(), error.category); }
            catch (const std::bad_alloc &) { throw; }
            catch (const std::length_error &error) { throw Error(span, error.what(), "limit"); }
            catch (const std::exception &error) { throw Error(span, error.what(), "io"); } });
        }
        registerNative("Random.seed", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "seed expects one Int", "call");
        random.seed(static_cast<uint64_t>(integer(values[0], span))); return {}; });
        registerNative("Random.float", [this](const auto &arguments, const Span &span) -> Value
                       {
        if (!arguments.empty()) throw Error(span, "float expects no arguments", "call");
        return static_cast<double>(random() >> 11) * 0x1.0p-53; });
        registerNative("Random.int", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.empty() || values.size() > 2) throw Error(span, "int expects stop or start, stop", "call");
        int64_t start = values.size() == 2 ? integer(values[0], span) : 0;
        int64_t stop = integer(values.back(), span);
        if (start >= stop) throw Error(span, "Random interval must be nonempty and increasing", "value");
        return std::uniform_int_distribution<int64_t>(start, stop - 1)(random); });
        registerNative("Random.choice", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "choice expects one sequence", "call");
        Object *source = object(values[0], span);
        if (source->kind != Kind::List && source->kind != Kind::Tuple && source->kind != Kind::String && source->kind != Kind::Bytes)
            throw Error(span, "choice expects List, Tuple, String or Bytes", "type");
        if (source->kind == Kind::String) for (size_t position = 0; position < source->text.size(); position++) tick(span);
        int64_t count = length(source, span);
        if (!count) throw Error(span, "Cannot choose from an empty sequence", "value");
        return index(source, std::uniform_int_distribution<int64_t>(0, count - 1)(random), span); });
        registerNative("Random.shuffle", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "shuffle expects one List", "call");
        Object *list = object(values[0], span);
        if (list->kind != Kind::List) throw Error(span, "shuffle expects a mutable List", "type");
        for (size_t position = 0; position < list->items.size(); position++) tick(span);
        std::shuffle(list->items.begin(), list->items.end(), random); return {}; });
        registerNative("IO.write", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1 || !std::holds_alternative<Object *>(values[0]) || (object(values[0], span)->kind != Kind::String && object(values[0], span)->kind != Kind::Bytes))
            throw Error(span, "write expects one String or Bytes", "type");
        if (!config.write) throw Error(span, "Output is not enabled by the host", "capability");
        config.write(object(values[0], span)->text); return {}; });
        registerNative("IO.readLine", [this](const auto &arguments, const Span &span) -> Value
                       {
        if (!arguments.empty()) throw Error(span, "readLine expects no arguments", "call");
        if (terminalOpen) throw Error(span, "Close the raw terminal session before line input", "state");
        if (!config.readLine) throw Error(span, "Input is not enabled by the host", "capability");
        try {
            auto line = config.readLine(); if (!line) return {};
            if (line->size() > config.maxInputBytes) throw Error(span, "Input line exceeds host limit", "limit");
            return text(*line, false, span);
        } catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; } catch (const std::exception &error) { throw Error(span, error.what(), "io"); } });
        define(coreNamespace("IO"), "Files", coreNamespace("Files"), false, "", coreSpan());
        registerNative("Terminal.open", [this](const auto &arguments, const Span &span) -> Value
                       {
        if (!arguments.empty()) throw Error(span, "open expects no arguments", "call");
        if (!config.terminalMode || !config.readKey) throw Error(span, "Terminal access is not enabled by the host", "capability");
        if (terminalOpen) return {};
        try { config.terminalMode(true); terminalOpen = true; return {}; }
        catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; } catch (const std::exception &error) { throw Error(span, error.what(), "io"); } });
        registerNative("Terminal.close", [this](const auto &arguments, const Span &span) -> Value
                       {
        if (!arguments.empty()) throw Error(span, "close expects no arguments", "call");
        if (!terminalOpen) return {};
        try { config.terminalMode(false); terminalOpen = false; return {}; }
        catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; } catch (const std::exception &error) { throw Error(span, error.what(), "io"); } });
        registerNative("Terminal.isOpen", [this](const auto &arguments, const Span &span) -> Value
                       {
        if (!arguments.empty()) throw Error(span, "isOpen expects no arguments", "call"); return terminalOpen; });
        registerNative("Terminal.readKey", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() > 1) throw Error(span, "readKey expects zero or one timeout", "call");
        int64_t timeout = values.empty() ? 0 : integer(values[0], span);
        if (timeout < 0 || static_cast<uint64_t>(timeout) > config.maxKeyWaitMillis) throw Error(span, "Key timeout exceeds host limit", "value");
        if (!terminalOpen) throw Error(span, "Terminal session is not open", "state");
        try { auto key = config.readKey(static_cast<uint64_t>(timeout)); return key ? Value{static_cast<int64_t>(*key)} : Value{}; }
        catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; } catch (const std::exception &error) { throw Error(span, error.what(), "io"); } });
        define(coreNamespace("IO"), "Terminal", coreNamespace("Terminal"), false, "", coreSpan());
        registerNative("Collections.len", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "len expects one argument", "call");
        if (std::holds_alternative<Object *>(values[0])) {
            Object *value = object(values[0], span);
            if (value->origin && value->origin->kind == Kind::ClassType && !value->origin->builtinCollection)
                return integer(call(member(value, "length", span), {}, span), span);
        }
        return length(values[0], span); });
        auto listFactory = [this](const auto &arguments, const Span &span) -> Value
        {
            auto values = positional(arguments, span);
            if (values.size() > 1)
                throw Error(span, "List expects zero or one iterable", "call");
            Object *result = allocate(Kind::List, span);
            roots.push_back(result);
            if (!values.empty())
            {
                Value item;
                size_t cursor = 0;
                while (next(values[0], item, cursor, span))
                {
                    tick(span);
                    result->items.push_back(item);
                }
            }
            return result;
        };
        auto mapFactory = [this](const auto &arguments, const Span &span) -> Value
        {
            auto values = positional(arguments, span);
            if (values.size() > 1)
                throw Error(span, "Map expects zero or one Map", "call");
            Object *result = allocate(Kind::Map, span);
            if (!values.empty())
            {
                Object *original = object(values[0], span);
                if (original->kind != Kind::Map)
                    throw Error(span, "Map copy construction expects a Map", "type");
                result->entries = original->entries;
            }
            return result;
        };
        auto installCollection = [&](const std::string &name, Kind kind, std::function<Value(const std::vector<Argument> &, const Span &)> factory)
        {
            Object *type = allocate(Kind::ClassType, coreSpan());
            type->text = name;
            type->parent = prelude;
            type->builtinCollection = true;
            type->collectionKind = kind;
            type->native = std::move(factory);
            type->traits.push_back(sizedTrait);
            type->baseClass = objectType;
            type->inheritanceDepth = 1;
            type->code = std::make_shared<Node>();
            type->code->kind = "class";
            type->code->text = name;
            type->code->span = coreSpan();
            collectionTypes[kind] = type;
            define(coreNamespace("Collections"), name, type, false, "", coreSpan());
            define(prelude, name, type, false, "", coreSpan());
        };
        installCollection("List", Kind::List, listFactory);
        installCollection("Map", Kind::Map, mapFactory);
        installCollection("Tuple", Kind::Tuple, [this, listFactory](const auto &arguments, const Span &span) -> Value
                          {
        Value list = listFactory(arguments, span); roots.push_back(list);
        Object *result = allocate(Kind::Tuple, span); result->items = object(list, span)->items; return result; });
        installCollection("Set", Kind::Set, [this, listFactory](const auto &arguments, const Span &span) -> Value
                          {
        Value list = listFactory(arguments, span); roots.push_back(list); Object *source = object(list, span);
        Object *result = allocate(Kind::Set, span); roots.push_back(result);
        for (Value value : source->items) { bool found = false; for (Value item : result->items) { tick(span); if (equal(item, value)) found = true; } if (!found) result->items.push_back(value); }
        auto values = positional(arguments, span);
        return result; });
        registerNative("Reflection.type", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "type expects one argument", "call");
        return text(typeOf(values[0]), false, span); });
        registerNative("Reflection.members", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "members expects one value", "call");
        Object *names = allocate(Kind::List, span); roots.push_back(names);
        for (const auto &field : inspectMembers(values[0])) names->items.push_back(text(field.name, false, span));
        return names; });
        registerNative("Reflection.binding", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1 || typeOf(values[0]) != "String") throw Error(span, "binding expects one String path", "call");
        return getPath(object(values[0], span)->text); });
        registerNative("Reflection.bindings", [this](const auto &arguments, const Span &span) -> Value
                       {
        if (!arguments.empty()) throw Error(span, "bindings expects no arguments", "call");
        Object *names = allocate(Kind::List, span); roots.push_back(names); std::vector<std::string> sorted;
        for (const auto &binding : global->bindings) sorted.push_back(binding.first);
        std::sort(sorted.begin(), sorted.end());
        for (const auto &name : sorted) names->items.push_back(text(name, false, span));
        return names; });
        registerNative("Reflection.doc", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "doc expects one value", "call");
        if (!std::holds_alternative<Object *>(values[0])) return text("", false, span);
        Object *value = object(values[0], span);
        if (value->kind == Kind::Record || value->kind == Kind::Instance || value->kind == Kind::Variant || value->kind == Kind::Constructor) value = value->origin;
        return text(value->code ? value->code->documentation : value->documentation, false, span); });
        registerNative("Text.str", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "str expects one argument", "call");
        return text(stringify(values[0], span), false, span); });
        auto defineFast = [this](const std::string &path, std::function<Value(const Value *, size_t, const Span &)> fast)
        {
            registerNative(path, [this, fast](const auto &arguments, const Span &span) -> Value
                           {
            auto values = positional(arguments, span); return fast(values.data(), values.size(), span); });
            std::get<Object *>(getPath(path))->fast = std::move(fast);
        };
        defineFast("Math.toFloat", [this](const Value *values, size_t count, const Span &span) -> Value
                   {
        if (count != 1) throw Error(span, "Float expects one argument", "call");
        return floating(values[0], span, true); });
        defineFast("Math.toInt", [this](const Value *values, size_t count, const Span &span) -> Value
                   {
        if (count != 1) throw Error(span, "Int expects one argument", "call");
        if (std::holds_alternative<int64_t>(values[0])) return values[0];
        double number = floating(values[0], span, true);
        if (!std::isfinite(number) || number < -9223372036854775808.0 || number >= 9223372036854775808.0)
            throw Error(span, "Float is outside Int range", "overflow");
        return static_cast<int64_t>(number); });
        define(coreNamespace("Math"), "PI", 3.14159265358979323846, false, "Float", coreSpan());
        define(coreNamespace("Math"), "TAU", 6.28318530717958647692, false, "Float", coreSpan());
        define(coreNamespace("Math"), "E", 2.71828182845904523536, false, "Float", coreSpan());
        using FloatFunction = double (*)(double);
        for (const auto &entry : std::vector<std::pair<std::string, FloatFunction>>{{"sin", [](double x)
                                                                                     { return std::sin(x); }},
                                                                                    {"cos", [](double x)
                                                                                     { return std::cos(x); }},
                                                                                    {"tan", [](double x)
                                                                                     { return std::tan(x); }},
                                                                                    {"asin", [](double x)
                                                                                     { return std::asin(x); }},
                                                                                    {"acos", [](double x)
                                                                                     { return std::acos(x); }},
                                                                                    {"atan", [](double x)
                                                                                     { return std::atan(x); }},
                                                                                    {"sinh", [](double x)
                                                                                     { return std::sinh(x); }},
                                                                                    {"cosh", [](double x)
                                                                                     { return std::cosh(x); }},
                                                                                    {"tanh", [](double x)
                                                                                     { return std::tanh(x); }},
                                                                                    {"toRadians", [](double x)
                                                                                     { return x * (3.14159265358979323846 / 180.0); }},
                                                                                    {"toDegrees", [](double x)
                                                                                     { return x * (180.0 / 3.14159265358979323846); }},
                                                                                    {"sqrt", [](double x)
                                                                                     { return std::sqrt(x); }},
                                                                                    {"exp", [](double x)
                                                                                     { return std::exp(x); }},
                                                                                    {"log", [](double x)
                                                                                     { return std::log(x); }},
                                                                                    {"log2", [](double x)
                                                                                     { return std::log2(x); }},
                                                                                    {"log10", [](double x)
                                                                                     { return std::log10(x); }}})
        {
            defineFast("Math." + entry.first, [this, name = entry.first, function = entry.second](const Value *values, size_t count, const Span &span) -> Value
                       {
            if (count != 1) throw Error(span, name + " expects one argument", "call");
            double result = function(floating(values[0], span));
            if (!std::isfinite(result)) throw Error(span, "Math." + name + " is undefined or not finite for this argument", "value");
            return result; });
        }
        using BinaryFunction = double (*)(double, double);
        for (const auto &entry : std::vector<std::pair<std::string, BinaryFunction>>{{"atan2", [](double y, double x)
                                                                                      { return std::atan2(y, x); }},
                                                                                     {"pow", [](double x, double y)
                                                                                      { return std::pow(x, y); }},
                                                                                     {"hypot", [](double x, double y)
                                                                                      { return std::hypot(x, y); }}})
        {
            defineFast("Math." + entry.first, [this, name = entry.first, function = entry.second](const Value *values, size_t count, const Span &span) -> Value
                       {
            if (count != 2) throw Error(span, name + " expects two arguments", "call");
            double result = function(floating(values[0], span), floating(values[1], span));
            if (!std::isfinite(result)) throw Error(span, "Math." + name + " is undefined or not finite for these arguments", "value");
            return result; });
        }
        for (const auto &entry : std::vector<std::pair<std::string, FloatFunction>>{{"floor", [](double x)
                                                                                     { return std::floor(x); }},
                                                                                    {"ceil", [](double x)
                                                                                     { return std::ceil(x); }},
                                                                                    {"round", [](double x)
                                                                                     { return std::round(x); }}})
        {
            defineFast("Math." + entry.first, [this, name = entry.first, function = entry.second](const Value *values, size_t count, const Span &span) -> Value
                       {
            if (count != 1) throw Error(span, name + " expects one argument", "call");
            if (std::holds_alternative<int64_t>(values[0])) return values[0];
            double result = function(floating(values[0], span));
            if (!std::isfinite(result) || result < -9223372036854775808.0 || result >= 9223372036854775808.0)
                throw Error(span, "Float is outside Int range", "overflow");
            return static_cast<int64_t>(result); });
        }
        registerNative("Collections.range", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.empty() || values.size() > 3) throw Error(span, "range expects one to three Int arguments", "call");
        int64_t start = values.size() == 1 ? 0 : integer(values[0], span);
        int64_t stop = integer(values.size() == 1 ? values[0] : values[1], span);
        int64_t step = values.size() == 3 ? integer(values[2], span) : 1;
        if (!step) throw Error(span, "range step cannot be zero", "value");
        Object *result = allocate(Kind::Range, span);
        result->items.push_back(start);
        result->items.push_back(stop);
        result->items.push_back(step);
        return result; });
        registerNative("Text.encode", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1 || typeOf(values[0]) != "String") throw Error(span, "encode expects String", "type");
        return text(object(values[0], span)->text, true, span); });
        registerNative("Text.decode", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1 || typeOf(values[0]) != "Bytes") throw Error(span, "decode expects Bytes", "type");
        return text(object(values[0], span)->text, false, span); });
        registerNative("Tasks.close", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "close expects one Generator", "call");
        closeGenerator(values[0], span); return {}; });
        registerNative("Tasks.spawn", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "spawn expects one fresh Generator", "call");
        return spawnTask(values[0], span); });
        registerNative("Tasks.pump", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "pump expects one nonnegative Int", "call");
        int64_t turns = integer(values[0], span);
        if (turns < 0 || static_cast<uint64_t>(turns) > std::numeric_limits<size_t>::max()) throw Error(span, "Invalid pump turn count", "value");
        return static_cast<int64_t>(pumpTasks(static_cast<size_t>(turns), span)); });
        registerNative("Tasks.cancel", [this](const auto &arguments, const Span &span) -> Value
                       {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "cancel expects one Task", "call");
        cancelTask(values[0], span); return {}; });
        installJson();
        installRequests();
    }

}
