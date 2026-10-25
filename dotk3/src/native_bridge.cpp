#include "runtime.hpp"

#include <array>
#include <cstring>
#include <type_traits>

// Runtime services for native code. Each helper mirrors the bytecode operation of the same name, reports
// failures through NativeContext instead of unwinding through generated frames, and takes the 1-based site
// whose span locates errors.
namespace dk3
{
    struct NativeBridge
    {
        struct GlobalCache
        {
            Object *environment;
            uint64_t epoch;
            Binding *binding;
            uint64_t unused[2];
        };
        static_assert(sizeof(GlobalCache) == sizeof(ByteSite), "generated code reserves one cache shape");

        static const Span &where(NativeContext *context, uint64_t site) { return (*context->sites)[site - 1].span; }
        template <class Body>
        static void guard(NativeContext *context, Body body)
        {
            try
            {
                body(*context->runtime);
            }
            catch (...)
            {
                context->runtime->nativeFailure = std::current_exception();
                context->failure = nativeException;
            }
        }
        static Object *object(const Value &value, const Span &span)
        {
            if (!std::holds_alternative<Object *>(value))
                throw Error(span, "Expected object, got " + Runtime::typeOf(value), "type");
            return std::get<Object *>(value);
        }
        static Binding *cachedSlot(Runtime &runtime, const ByteSite *site, Object *instance)
        {
            return site->type == instance->origin && site->epoch == runtime.bindingEpoch && site->slot < instance->slots.size() ? instance->slots[site->slot] : nullptr;
        }
        // Only public fields are cached so access checks never depend on the calling class.
        static void rememberSlot(Runtime &runtime, ByteSite *site, Object *instance, const Binding *field)
        {
            if (field->visibility != Visibility::Public)
                return;
            for (size_t index = 0; index < instance->slots.size(); index++)
                if (instance->slots[index] == field)
                {
                    *site = {instance->origin, runtime.bindingEpoch, nullptr, nullptr, index};
                    return;
                }
        }
        static std::vector<Argument> arguments(const Value *values, uint64_t count, const char *const *names)
        {
            std::vector<Argument> result;
            result.reserve(count);
            for (uint64_t index = 0; index < count; index++)
                result.push_back({names && names[index] ? names[index] : std::string(), values[index]});
            return result;
        }
        static Binding &binding(Runtime &runtime, Object *environment, const char *name, GlobalCache *cache, const Span &span)
        {
            if (cache->environment == environment && cache->epoch == runtime.bindingEpoch && cache->binding)
            {
                if (!cache->binding->initialized)
                    throw Error(span, "Field has not been initialized: '" + std::string(name) + "'", "state");
                return *cache->binding;
            }
            Binding &found = runtime.lookup(environment, name, span);
            *cache = {environment, runtime.bindingEpoch, &found, {}};
            return found;
        }

        static void global(NativeContext *context, Object *environment, Value *out, const char *name, GlobalCache *cache, uint64_t site)
        {
            guard(context, [&](Runtime &runtime)
                  { *out = binding(runtime, environment, name, cache, where(context, site)).value; });
        }
        static void storeGlobal(NativeContext *context, Object *environment, const Value *value, const char *name, GlobalCache *cache, uint64_t site)
        {
            guard(context, [&](Runtime &runtime)
                  {
                Binding &target = binding(runtime, environment, name, cache, where(context, site));
                if (!target.mutableBinding)
                    throw Error(where(context, site), "Cannot reassign let binding '" + std::string(name) + "'", "mutability");
                target.value = *value; });
        }
        static void member(NativeContext *context, Value *out, const Value *receiver, const char *name, ByteSite *site, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                Value value = *receiver;
                if (std::holds_alternative<Object *>(value))
                {
                    Object *instance = std::get<Object *>(value);
                    if (instance->kind == Kind::Instance || instance->kind == Kind::Record)
                    {
                        Binding *cached = cachedSlot(runtime, site, instance);
                        if (cached && cached->initialized)
                        {
                            *out = cached->value;
                            return;
                        }
                        auto field = instance->bindings.find(name);
                        if (field != instance->bindings.end() && field->second.initialized && runtime.canAccess(field->second))
                        {
                            rememberSlot(runtime, site, instance, &field->second);
                            *out = field->second.value;
                            return;
                        }
                    }
                }
                *out = runtime.member(value, name, where(context, at)); });
        }
        static void prepareMember(NativeContext *context, Value *container, const Value *receiver, const char *name, uint64_t compound, ByteSite *site, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                Value value = *receiver;
                Object *instance = std::holds_alternative<Object *>(value) ? std::get<Object *>(value) : nullptr;
                if (instance && instance->kind == Kind::Instance)
                {
                    Binding *field = cachedSlot(runtime, site, instance);
                    if (field && field->mutableBinding && (field->initialized || !compound))
                    {
                        *container = value;
                        return;
                    }
                }
                Object *result = runtime.prepareMemberWrite(value, name, compound != 0, where(context, at));
                if (result == instance && result->kind == Kind::Instance)
                {
                    auto found = result->bindings.find(name);
                    if (found != result->bindings.end() && found->second.mutableBinding)
                        rememberSlot(runtime, site, result, &found->second);
                }
                *container = result; });
        }
        static void setMember(NativeContext *context, const Value *containerValue, const char *name, const char *operation, const Value *value, ByteSite *site, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                Object *container = object(*containerValue, span);
                Binding *target = container->kind == Kind::Instance ? cachedSlot(runtime, site, container) : nullptr;
                if (!target)
                {
                    target = runtime.classBinding(container, name);
                    if (target && container->kind == Kind::Instance)
                        rememberSlot(runtime, site, container, target);
                }
                Value result = *value;
                if (operation)
                    result = runtime.binary(operation, target->value, result, span);
                target->value = result;
                target->initialized = true; });
        }
        static void index(NativeContext *context, Value *out, const Value *receiver, const Value *key, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  { *out = runtime.index(*receiver, *key, where(context, at)); });
        }
        static void setIndex(NativeContext *context, const Value *receiver, const Value *key, const Value *value, const char *operation, uint64_t at, uint64_t targetAt)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                Object *container = object(*receiver, where(context, targetAt));
                Value result = *value, custom;
                if (operation)
                    result = runtime.binary(operation, runtime.index(*receiver, *key, where(context, targetAt)), result, span);
                if (runtime.mayOverload(*receiver) && runtime.overloaded("setitem", *receiver, {{"", *key}, {"", result}}, span, custom))
                    return;
                if (container->kind == Kind::List)
                {
                    int64_t position = runtime.integer(*key, span);
                    if (position < 0)
                        position += static_cast<int64_t>(container->items.size());
                    if (position < 0 || static_cast<uint64_t>(position) >= container->items.size())
                        throw Error(span, "Index out of range", "index");
                    container->items[static_cast<size_t>(position)] = result;
                }
                else if (container->kind == Kind::Map)
                    runtime.setEntry(container, *key, result, span);
                else
                    throw Error(span, "Sequence is immutable", "mutability"); });
        }
        static void binary(NativeContext *context, Value *out, const char *operation, const Value *left, const Value *right, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  { *out = runtime.binary(operation, *left, *right, where(context, at)); });
        }
        // Positional calls to bytecode functions take the same fast path as ByteOp::Call.
        static bool callFast(Runtime &runtime, Value *out, const Value &callee, const Value *values, uint64_t count, const char *const *names, Object *receiver, const Span &span)
        {
            if (names || runtime.config.debug || !std::holds_alternative<Object *>(callee))
                return false;
            Object *function = std::get<Object *>(callee);
            if (function->kind != Kind::Function)
                return false;
            if (!function->bytecodeAttempted && !receiver)
            {
                function->bytecode = compileBytecode(function->code, runtime.config.optimizationLevel, false, true);
                function->bytecodeAttempted = true;
            }
            if (!function->bytecode || function->bytecode->hasSelf != (receiver != nullptr))
                return false;
            *out = runtime.callFunctionFast(function, values, count, span, receiver);
            return true;
        }
        static void call(NativeContext *context, Value *out, const Value *callee, const Value *values, uint64_t count, const char *const *names, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                if (!callFast(runtime, out, *callee, values, count, names, nullptr, span))
                    *out = runtime.call(*callee, arguments(values, count, names), span); });
        }
        // ResolveMethod + InvokeMethod, sharing their per-site method cache.
        static void callMethod(NativeContext *context, Value *out, const Value *receiver, const char *name, const Value *values, uint64_t count, const char *const *names, ByteSite *site,
                               uint64_t resolveAt, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                Value receiverValue = *receiver;
                Object *instance = std::holds_alternative<Object *>(receiverValue) ? std::get<Object *>(receiverValue) : nullptr;
                Value function;
                MethodTarget target;
                if (instance && instance->kind == Kind::Instance && site->method && site->type == instance->origin && site->epoch == runtime.bindingEpoch)
                {
                    function = site->method;
                    target = {instance, site->owner};
                }
                else
                {
                    function = runtime.member(receiverValue, name, where(context, resolveAt), false, &target);
                    if (instance && instance->kind == Kind::Instance && target.receiver == instance && std::holds_alternative<Object *>(function))
                    {
                        Object *owner = nullptr;
                        Binding *method = runtime.classBinding(instance->origin, name, &owner);
                        if (method && owner == target.owner && method->visibility == Visibility::Public && !method->staticMember)
                            *site = {instance->origin, runtime.bindingEpoch, std::get<Object *>(function), owner, 0};
                    }
                }
                const Span &span = where(context, at);
                if (!callFast(runtime, out, function, values, count, names, target.receiver, span))
                    *out = runtime.call(function, arguments(values, count, names), span, target.receiver, target.owner); });
        }
        static void text(NativeContext *context, Value *out, const char *bytes, uint64_t length, uint64_t binary, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  { *out = runtime.text(std::string(bytes, length), binary != 0, where(context, at)); });
        }
        // kind: 0 List, 1 Tuple, 2 Map (values alternate key, value).
        static void collection(NativeContext *context, Value *out, uint64_t kind, const Value *values, uint64_t count, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                Object *value = runtime.allocate(kind == 0 ? Kind::List : kind == 1 ? Kind::Tuple : Kind::Map, span);
                size_t scratch = runtime.roots.size();
                runtime.roots.push_back(value);
                struct Release
                {
                    std::vector<Value> &roots;
                    size_t size;
                    ~Release() { roots.resize(size); }
                } release{runtime.roots, scratch};
                if (kind == 2)
                    for (uint64_t index = 0; index + 1 < count; index += 2)
                        runtime.setEntry(value, values[index], values[index + 1], span);
                else
                    value->items.assign(values, values + count);
                *out = value; });
        }
        static void format(NativeContext *context, Value *builder, const Value *part, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                std::string converted = runtime.stringify(*part, span);
                object(*builder, span)->text += converted; });
        }
        static void iterInit(NativeContext *context, Value *iterable, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                if (!std::holds_alternative<Object *>(*iterable) || std::get<Object *>(*iterable)->kind != Kind::Map)
                    return;
                Object *map = std::get<Object *>(*iterable), *keys = runtime.allocate(Kind::List, where(context, at));
                for (const auto &entry : map->entries)
                    keys->items.push_back(entry.first);
                *iterable = keys; });
        }
        static uint64_t iterNext(NativeContext *context, Value *item, const Value *iterable, int64_t *cursor, uint64_t at)
        {
            bool more = false;
            guard(context, [&](Runtime &runtime)
                  {
                size_t position = static_cast<size_t>(*cursor), scratch = runtime.roots.size();
                Value next;
                more = runtime.next(*iterable, next, position, where(context, at));
                runtime.roots.resize(scratch);
                *cursor = static_cast<int64_t>(position);
                if (more)
                    *item = next; });
            return more;
        }
        static void raise(NativeContext *context, const Value *value, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  { runtime.throwValue(*value, where(context, at)); });
        }
        static void safePoint(NativeContext *context)
        {
            guard(context, [&](Runtime &runtime)
                  { runtime.collectionSafePoint(); });
        }
        static uint64_t truthy(NativeContext *context, const Value *value, uint64_t at)
        {
            bool result = false;
            guard(context, [&](Runtime &runtime)
                  { result = runtime.boolean(*value, where(context, at)); });
            return result;
        }
        static uint64_t instanceOf(NativeContext *context, Object *environment, const Value *value, const char *type, uint64_t at)
        {
            bool result = false;
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                result = runtime.valueMatches(*value, runtime.resolveContract(type, environment, span), span); });
            return result;
        }
        static void cast(NativeContext *context, Object *environment, const Value *value, const char *type, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                if (!runtime.valueMatches(*value, runtime.resolveContract(type, environment, span), span))
                    throw Error(span, "Cannot cast " + Runtime::typeOf(*value) + " to " + type, "type"); });
        }
        // ByteOp::CallNative: a checker-resolved core function such as Math.sqrt.
        static void callNative(NativeContext *context, Value *out, const char *path, const Value *values, uint64_t count, Object **cache, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                if (!*cache)
                    *cache = std::get<Object *>(runtime.getPath(path));
                Object *function = *cache;
                if (function->fast && !runtime.config.profiler)
                    *out = function->fast(values, count, span);
                else
                    *out = runtime.call(function, arguments(values, count, nullptr), span); });
        }
        // ByteOp::CallMethod: a checker-resolved method on a builtin receiver. cache[0] holds kind + 1, cache[1] the method.
        static void callBuiltin(NativeContext *context, Value *out, const Value *receiverValue, const char *name, const Value *values, uint64_t count, int64_t *cache, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                Value receiverCopy = *receiverValue;
                Object *receiver = std::holds_alternative<Object *>(receiverCopy) ? std::get<Object *>(receiverCopy) : nullptr;
                size_t poolIndex = runtime.argumentDepth++;
                struct Release
                {
                    size_t &depth;
                    ~Release() { depth--; }
                } release{runtime.argumentDepth};
                if (runtime.argumentPool.size() <= poolIndex)
                    runtime.argumentPool.emplace_back();
                std::vector<Argument> &list = runtime.argumentPool[poolIndex];
                list.clear();
                for (uint64_t index = 0; index < count; index++)
                    list.push_back({std::string(), values[index]});
                if (!receiver)
                {
                    const char *primitive = std::holds_alternative<int64_t>(receiverCopy) ? "Int" : std::holds_alternative<double>(receiverCopy) ? "Float"
                                                                                         : std::holds_alternative<bool>(receiverCopy)     ? "Bool"
                                                                                                                                          : nullptr;
                    if (primitive)
                    {
                        auto methods = runtime.primitiveMethods.find(primitive);
                        if (methods != runtime.primitiveMethods.end())
                        {
                            auto found = methods->second.find(name);
                            if (found != methods->second.end())
                            {
                                *out = runtime.callPrimitiveMethod(receiverCopy, found->second, list, span);
                                return;
                            }
                        }
                        throw Error(span, std::string(primitive) + " has no member '" + name + "'", "name");
                    }
                }
                if (receiver && !receiver->origin)
                {
                    auto methods = runtime.kindMethods.find(receiver->kind);
                    if (methods != runtime.kindMethods.end())
                    {
                        auto found = methods->second.find(name);
                        if (found != methods->second.end())
                        {
                            *out = found->second.body(runtime, receiver, list, span);
                            return;
                        }
                    }
                }
                Object *function = nullptr;
                if (receiver && (!receiver->origin || receiver->origin->builtinCollection) && !runtime.config.profiler)
                {
                    int64_t kind = static_cast<int64_t>(receiver->kind) + 1;
                    if (cache[0] != kind)
                    {
                        cache[0] = kind;
                        cache[1] = 0;
                        auto methods = runtime.nativeMethods.find(receiver->kind);
                        if (methods != runtime.nativeMethods.end())
                        {
                            auto found = methods->second.find(name);
                            if (found != methods->second.end())
                                cache[1] = reinterpret_cast<intptr_t>(found->second);
                        }
                    }
                    function = reinterpret_cast<Object *>(cache[1]);
                }
                if (function && function->nativeMethod)
                {
                    size_t scratch = runtime.roots.size();
                    struct Scratch
                    {
                        std::vector<Value> &roots;
                        size_t start;
                        ~Scratch() { roots.resize(start); }
                    } restore{runtime.roots, scratch};
                    *out = runtime.callNativeMethod(receiver, *function->nativeMethod, list, span);
                    return;
                }
                MethodTarget target;
                Value bound = runtime.member(receiverCopy, name, span, false, &target);
                *out = runtime.call(bound, list, span, target.receiver, target.owner); });
        }
        // A free name inside a compiled lambda: a slot capture of the running closure, else its lexical scope.
        static void capture(NativeContext *context, Value *out, const char *name, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                Object *function = context->function;
                if (const Bytecode *code = function->bytecode.get())
                    for (size_t index = 0; index < code->captures && index < function->items.size(); index++)
                        if (code->locals[code->parameters + index].name == name)
                        {
                            *out = function->items[index];
                            return;
                        }
                *out = runtime.lookup(function->parent, name, where(context, at)).value; });
        }
        // ByteOp::MakeClosure for a lambda in native code; the site records the lambda node.
        static void makeClosure(NativeContext *context, Object *environment, Value *out, const Value *captures, const char *const *names, uint64_t count, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const NativeSite &site = (*context->sites)[at - 1];
                if (!site.closure)
                    throw Error(site.span, "Closure site has no lambda", "internal");
                std::shared_ptr<const Bytecode> &code = runtime.nativeClosureCode[site.closure.get()];
                if (!code)
                {
                    code = compileClosure(site.closure, std::vector<std::string>(names, names + count), runtime.config.optimizationLevel);
                    if (!code)
                        throw Error(site.span, "Cannot compile closure", "internal");
                }
                Object *function = runtime.allocate(Kind::Function, site.span);
                function->code = site.closure;
                function->parent = environment;
                function->text = site.closure->text;
                function->bytecode = code;
                function->bytecodeAttempted = true;
                function->items.assign(captures, captures + count);
                *out = function; });
        }
        static void unwind(NativeContext *context, const char *callee, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  { runtime.nativeUnwind.push_back({callee, where(context, at)}); });
        }
        // Fills an inline collection cache when value is a builtin collection whose kind is in mask (bit 1 << Kind)
        // and whose class defines no operator hooks; generated code then accesses its storage directly.
        static void classify(NativeContext *context, const Value *value, ByteSite *site, uint64_t mask)
        {
            guard(context, [&](Runtime &runtime)
                  {
                if (!std::holds_alternative<Object *>(*value))
                    return;
                Object *object = std::get<Object *>(*value);
                if (object->origin && object->origin->builtinCollection && (mask >> static_cast<unsigned>(object->kind) & 1) && !runtime.mayOverload(*value))
                    *site = {object->origin, runtime.bindingEpoch, nullptr, nullptr, static_cast<size_t>(object->kind)}; });
        }
        // `.append(value)` on a builtin List, else the general builtin-method call.
        static void append(NativeContext *context, Value *out, const Value *list, const Value *value, int64_t *cache, uint64_t at)
        {
            if (std::holds_alternative<Object *>(*list))
            {
                Object *object = std::get<Object *>(*list);
                if (object->kind == Kind::List && (!object->origin || object->origin->builtinCollection))
                {
                    guard(context, [&](Runtime &)
                          { object->items.push_back(*value); *out = Value{}; });
                    return;
                }
            }
            callBuiltin(context, out, list, "append", value, 1, cache, at);
        }
        // String + String without revalidating UTF-8 (joining valid text stays valid); otherwise Runtime::binary.
        static void concat(NativeContext *context, Value *out, const Value *left, const Value *right, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                if (std::holds_alternative<Object *>(*left) && std::holds_alternative<Object *>(*right))
                {
                    Object *first = std::get<Object *>(*left), *second = std::get<Object *>(*right);
                    if (first->kind == Kind::String && second->kind == Kind::String && !runtime.config.profiler)
                    {
                        std::string joined;
                        joined.reserve(first->text.size() + second->text.size());
                        joined.append(first->text).append(second->text);
                        Object *result = runtime.allocate(Kind::String, span);
                        result->text = std::move(joined);
                        *out = result;
                        return;
                    }
                }
                *out = runtime.binary("+", *left, *right, span); });
        }
        // ByteOp::PrepareSlice when out is null, else ByteOp::Slice over values[0][values[1]:values[2]:values[3]];
        // mask bits 1, 2, 4 mark which bounds are present.
        static void slice(NativeContext *context, Value *out, const Value *values, uint64_t mask, uint64_t at)
        {
            guard(context, [&](Runtime &runtime)
                  {
                const Span &span = where(context, at);
                if (!out)
                {
                    Kind kind = object(values[0], span)->kind;
                    if (kind != Kind::List && kind != Kind::Tuple && kind != Kind::String && kind != Kind::Bytes && !runtime.mayOverload(values[0]))
                        throw Error(span, "Value is not sliceable", "type");
                    return;
                }
                auto bound = [&](unsigned index)
                { return mask >> (index - 1) & 1 ? std::optional<Value>{values[index]} : std::nullopt; };
                *out = runtime.sliceValues(values[0], bound(1), bound(2), bound(3), span); });
        }
    };

    const void *const *nativeHelpers()
    {
        static const void *const table[] = {
            reinterpret_cast<const void *>(&NativeBridge::global),
            reinterpret_cast<const void *>(&NativeBridge::storeGlobal),
            reinterpret_cast<const void *>(&NativeBridge::member),
            reinterpret_cast<const void *>(&NativeBridge::prepareMember),
            reinterpret_cast<const void *>(&NativeBridge::setMember),
            reinterpret_cast<const void *>(&NativeBridge::index),
            reinterpret_cast<const void *>(&NativeBridge::setIndex),
            reinterpret_cast<const void *>(&NativeBridge::binary),
            reinterpret_cast<const void *>(&NativeBridge::call),
            reinterpret_cast<const void *>(&NativeBridge::callMethod),
            reinterpret_cast<const void *>(&NativeBridge::text),
            reinterpret_cast<const void *>(&NativeBridge::collection),
            reinterpret_cast<const void *>(&NativeBridge::format),
            reinterpret_cast<const void *>(&NativeBridge::iterInit),
            reinterpret_cast<const void *>(&NativeBridge::iterNext),
            reinterpret_cast<const void *>(&NativeBridge::raise),
            reinterpret_cast<const void *>(&NativeBridge::safePoint),
            reinterpret_cast<const void *>(&NativeBridge::truthy),
            reinterpret_cast<const void *>(&NativeBridge::instanceOf),
            reinterpret_cast<const void *>(&NativeBridge::cast),
            reinterpret_cast<const void *>(&NativeBridge::callNative),
            reinterpret_cast<const void *>(&NativeBridge::callBuiltin),
            reinterpret_cast<const void *>(&NativeBridge::capture),
            reinterpret_cast<const void *>(&NativeBridge::makeClosure),
            reinterpret_cast<const void *>(&NativeBridge::unwind),
            reinterpret_cast<const void *>(&NativeBridge::classify),
            reinterpret_cast<const void *>(&NativeBridge::append),
            reinterpret_cast<const void *>(&NativeBridge::concat),
            reinterpret_cast<const void *>(&NativeBridge::slice),
        };
        static_assert(sizeof(table) / sizeof(*table) == static_cast<size_t>(NativeHelper::Count), "one entry per NativeHelper");
        return table;
    }

    NativeValueLayout nativeValueLayout()
    {
        static_assert(std::is_trivially_copyable_v<Value>, "generated code copies Values bytewise");
        NativeValueLayout layout{false, sizeof(Value), 8, 0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (sizeof(Value) != 16 || alignof(Value) != 8)
            return layout;
        auto bytes = [](const Value &value)
        {
            std::array<unsigned char, sizeof(Value)> result;
            std::memcpy(result.data(), &value, sizeof(Value));
            return result;
        };
        int64_t integer = 0x0102030405060708;
        double floating = 1.5;
        Object *pointer = reinterpret_cast<Object *>(uintptr_t{0x1000});
        const Value samples[] = {Value{}, Value{true}, Value{integer}, Value{floating}, Value{pointer}};
        for (const Value &sample : samples)
            if (bytes(sample)[8] != sample.index())
                return layout;
        if (std::memcmp(bytes(samples[2]).data(), &integer, 8) || std::memcmp(bytes(samples[3]).data(), &floating, 8) ||
            std::memcmp(bytes(samples[4]).data(), &pointer, 8) || bytes(samples[1])[0] != 1)
            return layout;
        // Generated frames are zero-filled, which must read back as Null.
        std::array<unsigned char, sizeof(Value)> zero{};
        Value cleared;
        std::memcpy(&cleared, zero.data(), sizeof(Value));
        Binding binding;
        const char *base = reinterpret_cast<const char *>(&binding);
        layout.bindingMutable = static_cast<unsigned>(reinterpret_cast<const char *>(&binding.mutableBinding) - base);
        layout.bindingInitialized = static_cast<unsigned>(reinterpret_cast<const char *>(&binding.initialized) - base);
        static_assert(sizeof(bool) == 1, "generated code reads Binding flags as bytes");
        Object object;
        Binding *fields[2] = {};
        object.slots.assign(fields, fields + 2);
        const char *start = reinterpret_cast<const char *>(&object);
        layout.objectOrigin = static_cast<unsigned>(reinterpret_cast<const char *>(&object.origin) - start);
        layout.objectSlots = static_cast<unsigned>(reinterpret_cast<const char *>(&object.slots) - start);
        Binding *const *words[2];
        std::memcpy(words, start + layout.objectSlots, sizeof(words));
        bool slotsReadable = words[0] == object.slots.data() && words[1] == object.slots.data() + 2;
        layout.objectKind = static_cast<unsigned>(reinterpret_cast<const char *>(&object.kind) - start);
        layout.objectItems = static_cast<unsigned>(reinterpret_cast<const char *>(&object.items) - start);
        layout.objectEntries = static_cast<unsigned>(reinterpret_cast<const char *>(&object.entries) - start);
        object.items.assign(2, Value{});
        object.entries.assign(3, {Value{}, Value{}});
        const Value *items[2];
        const std::pair<Value, Value> *entries[2];
        std::memcpy(items, start + layout.objectItems, sizeof(items));
        std::memcpy(entries, start + layout.objectEntries, sizeof(entries));
        bool collectionsReadable = items[0] == object.items.data() && items[1] == object.items.data() + 2 && entries[0] == object.entries.data() &&
                                   entries[1] == object.entries.data() + 3 && sizeof(std::pair<Value, Value>) == 32 && sizeof(Kind) == 4;
        layout.kindList = static_cast<unsigned>(Kind::List);
        layout.kindTuple = static_cast<unsigned>(Kind::Tuple);
        layout.kindSet = static_cast<unsigned>(Kind::Set);
        layout.kindMap = static_cast<unsigned>(Kind::Map);
        layout.kindRange = static_cast<unsigned>(Kind::Range);
        layout.usable = std::holds_alternative<std::monostate>(cleared) && reinterpret_cast<const char *>(&binding.value) == base && slotsReadable && collectionsReadable;
        return layout;
    }
}
