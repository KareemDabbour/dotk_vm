#include "runtime.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

#ifdef DK3_STACK_CHECKS
#define DK3_STACK_CHECK(condition)                                         \
    do                                                                     \
    {                                                                      \
        if (!(condition))                                                  \
        {                                                                  \
            std::fputs("dotk3: operand stack bounds violated\n", stderr); \
            std::abort();                                                  \
        }                                                                  \
    } while (0)
#else
#define DK3_STACK_CHECK(condition) ((void)0)
#endif

namespace dk3
{
    namespace
    {
        // Fixed-capacity view over a frame's pooled operand storage; popped slots are cleared so GC roots stay exact.
        struct OperandStack
        {
            Value *base, *top, *limit;
            Value &back()
            {
                DK3_STACK_CHECK(top > base);
                return top[-1];
            }
            void push_back(const Value &value)
            {
                DK3_STACK_CHECK(top < limit);
                *top++ = value;
            }
            template <typename... Arguments>
            void emplace_back(Arguments &&...arguments)
            {
                DK3_STACK_CHECK(top < limit);
                *top++ = Value(std::forward<Arguments>(arguments)...);
            }
            void pop_back()
            {
                DK3_STACK_CHECK(top > base);
                *--top = Value{};
            }
            size_t size() const { return static_cast<size_t>(top - base); }
            Value &operator[](size_t index)
            {
                DK3_STACK_CHECK(base + index < top);
                return base[index];
            }
            Value *data() { return base; }
            Value *begin() { return base; }
            Value *end() { return top; }
            void resize(size_t count)
            {
                DK3_STACK_CHECK(count <= size());
                while (top > base + count)
                    *--top = Value{};
            }
        };
        Object *byteObject(Value value, const Span &span)
        {
            if (!std::holds_alternative<Object *>(value))
                throw Error(span, "Expected object, got " + Runtime::typeOf(value), "type");
            return std::get<Object *>(value);
        }
    }
    Value Runtime::executeBytecode(const Bytecode &code, Object *environment, const std::vector<Argument> &arguments, const Span &span, const std::vector<Value> *captured, Object *self)
    {
        if (config.profiler)
            return executeBytecodeLoop<true>(code, environment, arguments, span, captured, nullptr, 0, self);
        return executeBytecodeLoop<false>(code, environment, arguments, span, captured, nullptr, 0, self);
    }
    Value Runtime::callFunctionFast(Object *function, const Value *arguments, size_t count, const Span &span, Object *receiver)
    {
        static const std::vector<Argument> none;
        if (++callDepth > 256)
        {
            callDepth--;
            throw Error(span, "Call depth exceeded", "limit");
        }
        struct DepthGuard
        {
            size_t &depth;
            ~DepthGuard() { depth--; }
        } depthGuard{callDepth};
        FrameScope frame(*this, *function->code, function->parent, receiver && receiver->origin ? &receiver->origin->text : nullptr);
        try
        {
            if (function->code->native && nativeAllowed())
                if (std::optional<Value> result = callNative(*function->code->native, function, arguments, count, receiver))
                    return *result;
            return executeBytecodeLoop<false>(*function->bytecode, function->parent, none, span, function->bytecode->captures ? &function->items : nullptr, arguments, count, receiver);
        }
        catch (...)
        {
            captureFailure(std::current_exception());
            throw;
        }
    }
    std::optional<Value> Runtime::callNative(const NativeFunction &native, Object *function, const Value *arguments, size_t count, Object *receiver)
    {
        uint64_t raw[16];
        if (count != native.parameters.size() || count > 16 || native.method != (receiver != nullptr))
            return std::nullopt;
        if (!native.ready)
        {
            for (const auto &dependency : native.dependencies)
                if (!global->bindings.count(dependency))
                    return std::nullopt;
            native.ready = true;
        }
        for (size_t index = 0; index < count; index++)
        {
            const Value &argument = arguments[index];
            switch (native.parameters[index])
            {
            case 'i':
                if (!std::holds_alternative<int64_t>(argument))
                    return std::nullopt;
                raw[index] = static_cast<uint64_t>(std::get<int64_t>(argument));
                break;
            case 'f':
                if (!std::holds_alternative<double>(argument))
                    return std::nullopt;
                std::memcpy(&raw[index], &std::get<double>(argument), sizeof(double));
                break;
            case 'v':
                // The caller's operand storage stays rooted for the whole call.
                raw[index] = reinterpret_cast<uintptr_t>(&argument);
                break;
            default:
                if (!std::holds_alternative<bool>(argument))
                    return std::nullopt;
                raw[index] = std::get<bool>(argument);
                break;
            }
        }
        Value self = receiver ? Value{receiver} : Value{};
        Object *previousAccess = accessClass;
        accessClass = nullptr;
        for (Object *scope = function->parent; scope; scope = scope->parent)
            if (scope->accessClass)
            {
                accessClass = scope->accessClass;
                break;
            }
        struct Restore
        {
            Object *&access;
            Object *previous;
            ~Restore() { access = previous; }
        } restore{accessClass, previousAccess};
        return enterNative(native, function->parent, function, raw, &self);
    }
    bool Runtime::runNativeStatement(const NativeFunction &native, Object *environment)
    {
        if (!native.ready)
        {
            for (const auto &dependency : native.dependencies)
                if (!global->bindings.count(dependency))
                    return false;
            native.ready = true;
        }
        enterNative(native, environment, nullptr, nullptr, nullptr);
        return true;
    }
    Value Runtime::enterNative(const NativeFunction &native, Object *environment, Object *function, const uint64_t *raw, const Value *self)
    {
        Value boxed;
        NativeContext context{&callDepth, 256, 0, nullptr, this, nativeHelpers(), native.sites, environment, self, &boxed, function, &bindingEpoch, &collectionRequested};
        nativeContexts.push_back(&context);
        struct Restore
        {
            Runtime &runtime;
            size_t depth;
            ~Restore()
            {
                runtime.nativeContexts.pop_back();
                runtime.callDepth = depth;
            }
        } restore{*this, callDepth};
        size_t unwound = nativeUnwind.size();
        uint64_t result = native.entry(&context, raw);
        if (context.failure == nativeException)
        {
            std::exception_ptr failure = std::move(nativeFailure);
            nativeFailure = nullptr;
            captureNativeFailure(failure, unwound);
            std::rethrow_exception(failure);
        }
        if (context.failure)
        {
            const NativeSite &site = (*native.sites)[context.failure - 1];
            std::exception_ptr failure = std::make_exception_ptr(Error(site.span, site.message, site.category));
            captureNativeFailure(failure, unwound);
            std::rethrow_exception(failure);
        }
        switch (native.result)
        {
        case 'v':
            return boxed;
        case 'i':
            return Value{static_cast<int64_t>(result)};
        case 'f':
        {
            double number;
            std::memcpy(&number, &result, sizeof(double));
            return Value{number};
        }
        case 'b':
            return Value{result != 0};
        default:
            return Value{};
        }
    }
    template <bool profiled>
    Value Runtime::executeBytecodeLoop(const Bytecode &code, Object *environment, const std::vector<Argument> &arguments, const Span &span, const std::vector<Value> *captured, const Value *positionalArguments, size_t positionalCount, Object *self)
    {
        if (byteFrameDepth == byteFrames.size())
            byteFrames.emplace_back();
        ByteFrame &byteFrame = byteFrames[byteFrameDepth++];
        struct ReleaseFrame
        {
            ByteFrame &frame;
            size_t &depth;
            ~ReleaseFrame()
            {
                frame.locals.clear();
                frame.stack.clear();
                frame.initialized.clear();
                frame.methods.clear();
                frame.deferred.clear();
                depth--;
            }
        } releaseFrame{byteFrame, byteFrameDepth};
        byteFrame.locals.assign(code.locals.size(), Value{});
        byteFrame.initialized.assign(code.locals.size(), 0);
        byteFrame.stack.assign(code.maxStack, Value{});
        // Storage stays fixed for this activation, so raw pointers remain valid across nested calls.
        struct
        {
            Value *locals;
            unsigned char *initialized;
            std::vector<MethodTarget> &methods;
            std::vector<Value> &deferred;
            OperandStack stack;
        } frame{byteFrame.locals.data(), byteFrame.initialized.data(), byteFrame.methods, byteFrame.deferred,
                {byteFrame.stack.data(), byteFrame.stack.data(), byteFrame.stack.data() + byteFrame.stack.size()}};
        Object *previousAccess = accessClass;
        accessClass = nullptr;
        for (Object *scope = environment; scope; scope = scope->parent)
            if (scope->accessClass)
            {
                accessClass = scope->accessClass;
                break;
            }
        struct RestoreAccess
        {
            Object *&current;
            Object *previous;
            ~RestoreAccess() { current = previous; }
        } restoreAccess{accessClass, previousAccess};
        size_t positional = 0;
        if (positionalArguments)
        {
            size_t required = code.parameters - code.defaultParameters;
            if (positionalCount < required || positionalCount > code.parameters)
                throw Error(span, positionalCount > code.parameters ? "Too many positional arguments" : "Missing argument '" + code.locals[positionalCount].name + "'", "call");
            for (size_t index = 0; index < positionalCount; index++)
            {
                frame.locals[index] = positionalArguments[index];
                frame.initialized[index] = true;
            }
        }
        else
            for (const auto &argument : arguments)
            {
                size_t slot = code.parameters;
                if (argument.name.empty())
                    slot = positional++;
                else
                    for (size_t index = 0; index < code.parameters; index++)
                        if (code.locals[index].name == argument.name)
                            slot = index;
                if (slot >= code.parameters)
                    throw Error(span, argument.name.empty() ? "Too many positional arguments" : "Unknown named argument '" + argument.name + "'", "call");
                if (frame.initialized[slot])
                    throw Error(span, "Argument supplied more than once: '" + code.locals[slot].name + "'", "call");
                frame.locals[slot] = argument.value;
                frame.initialized[slot] = true;
            }
        if (!positionalArguments)
            for (size_t index = 0; index < code.parameters - code.defaultParameters; index++)
                if (!frame.initialized[index])
                    throw Error(span, "Missing argument '" + code.locals[index].name + "'", "call");
        // Evaluate default values for any uninitialized default parameters.
        if (code.defaultParameters > 0)
        {
            size_t firstDefault = code.parameters - code.defaultParameters;
            for (size_t i = firstDefault; i < code.parameters; i++)
            {
                if (!frame.initialized[i])
                {
                    size_t di = i - firstDefault;
                    if (di < code.defaultBytecodes.size() && code.defaultBytecodes[di])
                        frame.locals[i] = executeBytecode(*code.defaultBytecodes[di], environment, {}, span);
                    frame.initialized[i] = true;
                }
            }
        }
        if (code.hasSelf)
        {
            frame.locals[code.parameters] = self;
            frame.initialized[code.parameters] = true;
        }
        if (captured)
            for (size_t index = 0; index < code.captures && index < captured->size(); index++)
            {
                frame.locals[code.parameters + index] = (*captured)[index];
                frame.initialized[code.parameters + index] = true;
            }
        auto pop = [&]()
        { Value value = frame.stack.back(); frame.stack.pop_back(); return value; };
        auto resolveBinding = [&](size_t index, const Span &location) -> Binding &
        {
            if (code.optimization < 2)
                return lookup(environment, code.names[index], location);
            // Long-lived bytecode-level cache. Invalidated only when environment or GC-epoch changes.
            // define() no longer bumps bindingEpoch, so the cache survives across calls to the same
            // function (e.g. a lambda called thousands of times resolves each name only once).
            if (code.nameEnvironment != environment || code.nameEpoch != bindingEpoch)
            {
                if constexpr (profiled)
                {
                    config.profiler->cacheInvalidations++;
                    if (code.nameEnvironment != environment)
                        config.profiler->cacheEnvMisses++;
                    else
                        config.profiler->cacheEpochMisses++;
                }
                code.nameBindings.assign(code.names.size(), nullptr);
                code.nameEnvironment = environment;
                code.nameEpoch = bindingEpoch;
            }
            Binding *&cached = reinterpret_cast<Binding *&>(code.nameBindings[index]);
            if (!cached)
            {
                if constexpr (profiled)
                    config.profiler->cacheMisses++;
                cached = &lookup(environment, code.names[index], location);
            }
            else
            {
                if constexpr (profiled)
                    config.profiler->cacheHits++;
            }
            if (!cached->initialized)
                throw Error(location, "Field has not been initialized: '" + code.names[index] + "'", "state");
            return *cached;
        };
        // Profiled runs always take resolveBinding so cache hit counters stay exact.
        auto binding = [&](size_t index, const Span &location) -> Binding &
        {
            if constexpr (!profiled)
                if (code.nameEnvironment == environment && code.nameEpoch == bindingEpoch && code.optimization >= 2)
                    if (Binding *cached = static_cast<Binding *>(code.nameBindings[index]); cached && cached->initialized)
                        return *cached;
            return resolveBinding(index, location);
        };
        auto siteOf = [&](const ByteInstruction &at) -> ByteSite *
        {
            size_t index = static_cast<size_t>(at.cachedKind);
            return index < code.sites.size() ? &code.sites[index] : nullptr;
        };
        auto cachedSlot = [&](const ByteSite *site, Object *instance) -> Binding *
        {
            return site && site->type == instance->origin && site->epoch == bindingEpoch && site->slot < instance->slots.size() ? instance->slots[site->slot] : nullptr;
        };
        // Only public fields are cached so access checks never depend on the calling class.
        auto rememberSlot = [&](ByteSite *site, Object *instance, const Binding *field)
        {
            if (!site || field->visibility != Visibility::Public)
                return;
            for (size_t index = 0; index < instance->slots.size(); index++)
                if (instance->slots[index] == field)
                {
                    *site = {instance->origin, bindingEpoch, nullptr, nullptr, index};
                    return;
                }
        };
        auto runDeferred = [&](const Span &loc)
        {
            while (!byteFrame.deferred.empty())
            {
                Value fn = byteFrame.deferred.back();
                byteFrame.deferred.pop_back();
                try
                {
                    call(fn, {}, loc);
                }
                catch (...)
                {
                }
            }
        };
        for (size_t cursor = 0; cursor < code.instructions.size();)
        {
            const auto &instruction = code.instructions[cursor++];
            const Ast &node = instruction.node;
            const Span &location = node->span;
            if constexpr (profiled)
            {
                config.profiler->peakOperandValues = std::max(config.profiler->peakOperandValues, frame.stack.size());
                if (config.profiler->settings().vm == VMProfile::Verbose)
                    config.profiler->opcodes[byteOpName(instruction.op)]++;
            }
            try
            {
                switch (instruction.op)
                {
                case ByteOp::Tick:
                    if constexpr (profiled)
                        tick(location);
                    else
                        tickExecution(location);
                    break;
                case ByteOp::Point:
                    if constexpr (profiled)
                    {
                        tick(location);
                        debugPoint(node, environment);
                    }
                    else
                    {
                        tickExecution(location);
                        if (!debugFrames.empty())
                        {
                            debugFrames.back().location = &location;
                            debugFrames.back().environment = environment;
                        }
                    }
                    break;
                case ByteOp::Null:
                    frame.stack.emplace_back();
                    break;
                case ByteOp::Bool:
                    frame.stack.emplace_back(instruction.operand != 0);
                    break;
                case ByteOp::Int:
                    frame.stack.emplace_back(instruction.integer);
                    break;
                case ByteOp::Float:
                    frame.stack.emplace_back(instruction.floating);
                    break;
                case ByteOp::Text:
                    frame.stack.push_back(text(node->text, node->literalKind == TokenKind::Bytes, location));
                    break;
                case ByteOp::Literal:
                    frame.stack.push_back(literalValue(node, instruction.operand != 0));
                    break;
                case ByteOp::Closure:
                {
                    Object *function = allocate(Kind::Function, location);
                    function->code = node;
                    function->parent = environment;
                    function->text = node->text;
                    frame.stack.push_back(function);
                    break;
                }
                case ByteOp::Format:
                {
                    Value converted = frame.stack.back();
                    std::string value = stringify(converted, location);
                    Object *builder = byteObject(frame.stack[frame.stack.size() - 2], location);
                    builder->text += value;
                    pop();
                    break;
                }
                case ByteOp::Slice:
                {
                    size_t begin = frame.stack.size() - 4;
                    std::optional<Value> first = instruction.operand & 1 ? std::optional<Value>{frame.stack[begin + 1]} : std::nullopt;
                    std::optional<Value> last = instruction.operand & 2 ? std::optional<Value>{frame.stack[begin + 2]} : std::nullopt;
                    std::optional<Value> step = instruction.operand & 4 ? std::optional<Value>{frame.stack[begin + 3]} : std::nullopt;
                    Value value = sliceValues(frame.stack[begin], first, last, step, location);
                    frame.stack.resize(begin);
                    frame.stack.push_back(value);
                    break;
                }
                case ByteOp::PrepareSlice:
                {
                    Object *value = byteObject(frame.stack.back(), location);
                    if (value->kind != Kind::List && value->kind != Kind::Tuple && value->kind != Kind::String && value->kind != Kind::Bytes && !mayOverload(frame.stack.back()))
                        throw Error(location, "Value is not sliceable", "type");
                    break;
                }
                case ByteOp::PrepareMember:
                {
                    Value receiver = frame.stack.back();
                    ByteSite *site = siteOf(instruction);
                    Object *instance = std::holds_alternative<Object *>(receiver) ? std::get<Object *>(receiver) : nullptr;
                    if (instance && instance->kind == Kind::Instance)
                    {
                        Binding *field = cachedSlot(site, instance);
                        if (field && field->mutableBinding && (field->initialized || !instruction.operand))
                            break;
                    }
                    Object *container = prepareMemberWrite(receiver, node->text, instruction.operand != 0, location);
                    if (container == instance && container->kind == Kind::Instance)
                    {
                        auto found = container->bindings.find(node->text);
                        if (found != container->bindings.end() && found->second.mutableBinding)
                            rememberSlot(site, container, &found->second);
                    }
                    frame.stack.back() = container;
                    break;
                }
                case ByteOp::SetMember:
                {
                    Object *container = byteObject(frame.stack[frame.stack.size() - 2], location);
                    ByteSite *site = siteOf(instruction);
                    Binding *target = container->kind == Kind::Instance ? cachedSlot(site, container) : nullptr;
                    if (!target)
                    {
                        target = classBinding(container, node->children[0]->text);
                        if (target && container->kind == Kind::Instance)
                            rememberSlot(site, container, target);
                    }
                    Value value = frame.stack.back();
                    if (node->text != "=")
                        value = binary(compoundOperator(node->text), target->value, value, location);
                    target->value = value;
                    target->initialized = true;
                    pop();
                    frame.stack.back() = value;
                    break;
                }
                case ByteOp::Swap:
                    std::swap(frame.stack[frame.stack.size() - 1], frame.stack[frame.stack.size() - 2]);
                    break;
                case ByteOp::DeclareType:
                case ByteOp::Import:
                    executeDeclaration(node, environment);
                    break;
                case ByteOp::Throw:
                    throwValue(frame.stack.back(), location);
                    break;
                case ByteOp::Match:
                {
                    const auto &typedMatch = code.matches[instruction.operand];
                    if (std::any_of(typedMatch.arms.begin(), typedMatch.arms.end(), [](const auto &arm)
                                    { return !arm.type.empty(); }))
                    {
                        Value subject = frame.stack.back();
                        bool matched = false;
                        for (const auto &arm : typedMatch.arms)
                        {
                            if (!arm.type.empty() && !valueMatches(subject, resolveContract(arm.type, environment, arm.span), arm.span))
                                continue;
                            Object *scope = allocate(Kind::Environment, arm.span);
                            scope->parent = environment;
                            roots.push_back(scope);
                            for (const auto &bound : arm.bindings)
                                define(scope, bound, subject, false, "", arm.span);
                            Value result = executeBytecode(*arm.code, scope, {}, arm.span);
                            frame.stack.back() = result;
                            roots.pop_back();
                            matched = true;
                            break;
                        }
                        if (!matched)
                            throw Error(location, "No matching arm", "match");
                        break;
                    }
                    Object *value = byteObject(frame.stack.back(), location);
                    if (value->kind != Kind::Variant)
                        throw Error(location, "match expects an enum value", "type");
                    const auto &match = code.matches[instruction.operand];
                    std::set<Object *> covered;
                    bool wildcard = false;
                    std::vector<Object *> constructors;
                    for (const auto &arm : match.arms)
                    {
                        if (wildcard)
                            throw Error(arm.span, "Unreachable match arm after wildcard", "match");
                        if (arm.pattern == "_")
                        {
                            wildcard = true;
                            constructors.push_back(nullptr);
                            continue;
                        }
                        if (arm.pattern.find('.') == std::string::npos)
                            throw Error(arm.span, "Expected qualified enum variant", "match");
                        Object *constructor = resolveObjectPath(arm.pattern, environment, arm.span);
                        if ((constructor->kind != Kind::Constructor && constructor->kind != Kind::Variant) || constructor->origin != value->origin)
                            throw Error(location, "Pattern belongs to a different enum", "match");
                        if (!covered.insert(constructor).second)
                            throw Error(location, "Duplicate match arm", "match");
                        if (arm.bindings.size() != (constructor->code ? constructor->code->names.size() : 0))
                            throw Error(arm.span, "Pattern payload arity mismatch", "match");
                        constructors.push_back(constructor);
                    }
                    if (!wildcard)
                        for (const auto &variant : value->origin->bindings)
                            if (!covered.count(byteObject(variant.second.value, location)))
                                throw Error(location, "Non-exhaustive match", "match");
                    bool matched = false;
                    for (size_t index = 0; index < match.arms.size(); index++)
                    {
                        if (constructors[index] && constructors[index]->text != value->text)
                            continue;
                        const auto &arm = match.arms[index];
                        if (arm.pattern != "_" && arm.bindings.size() != value->items.size())
                            throw Error(arm.span, "Pattern payload arity mismatch", "match");
                        Object *scope = allocate(Kind::Environment, arm.span);
                        scope->parent = environment;
                        roots.push_back(scope);
                        for (size_t position = 0; position < arm.bindings.size(); position++)
                            define(scope, arm.bindings[position], value->items[position], false, "", arm.span);
                        Value result = executeBytecode(*arm.code, scope, {}, arm.span);
                        frame.stack.back() = result;
                        roots.pop_back();
                        matched = true;
                        break;
                    }
                    if (!matched)
                        throw Error(location, "No matching arm", "match");
                    break;
                }
                case ByteOp::LoadName:
                    frame.stack.push_back(binding(instruction.operand, location).value);
                    break;
                case ByteOp::LoadLocal:
                    if (!frame.initialized[instruction.operand])
                        throw Error(location, "Field has not been initialized: '" + node->text + "'", "state");
                    frame.stack.push_back(frame.locals[instruction.operand]);
                    break;
                case ByteOp::CheckLocal:
                    if (!code.locals[instruction.operand].mutableBinding)
                        throw Error(node->children[0]->span, "Cannot reassign let binding '" + node->children[0]->text + "'", "mutability");
                    break;
                case ByteOp::CheckName:
                    if (!binding(instruction.operand, node->children[0]->span).mutableBinding)
                        throw Error(node->children[0]->span, "Cannot reassign let binding '" + node->children[0]->text + "'", "mutability");
                    break;
                case ByteOp::CheckObject:
                    byteObject(frame.stack.back(), location);
                    break;
                case ByteOp::StoreLocal:
                    frame.locals[instruction.operand] = frame.stack.back();
                    break;
                case ByteOp::StoreName:
                {
                    Binding &target = binding(instruction.operand, node->children[0]->span);
                    target.value = frame.stack.back();
                    break;
                }
                case ByteOp::DeclareName:
                {
                    Value value = pop();
                    define(environment, node->text, value, node->mutableBinding, node->type, location);
                    break;
                }
                case ByteOp::DeclareLocal:
                {
                    Value value = frame.stack.back();
                    frame.locals[instruction.operand] = value;
                    frame.initialized[instruction.operand] = true;
                    pop();
                    break;
                }
                case ByteOp::ClearLocal:
                    frame.locals[instruction.operand] = {};
                    frame.initialized[instruction.operand] = false;
                    break;
                case ByteOp::DefineFunction:
                {
                    Object *function = allocate(Kind::Function, location);
                    function->code = node;
                    function->parent = environment;
                    function->text = node->text;
                    roots.push_back(function);
                    define(environment, node->text, function, false, "", location);
                    roots.pop_back();
                    break;
                }
                case ByteOp::Member:
                {
                    Value value = frame.stack.back();
                    if (std::holds_alternative<Object *>(value))
                    {
                        Object *instance = std::get<Object *>(value);
                        if (instance->kind == Kind::Instance || instance->kind == Kind::Record)
                        {
                            ByteSite *site = siteOf(instruction);
                            Binding *cached = cachedSlot(site, instance);
                            if (cached && cached->initialized)
                            {
                                frame.stack.back() = cached->value;
                                break;
                            }
                            auto field = instance->bindings.find(node->text);
                            if (field != instance->bindings.end() && field->second.initialized && canAccess(field->second))
                            {
                                rememberSlot(site, instance, &field->second);
                                frame.stack.back() = field->second.value;
                                break;
                            }
                        }
                    }
                    frame.stack.back() = member(value, node->text, location);
                    break;
                }
                case ByteOp::ResolveMethod:
                {
                    Value receiver = frame.stack.back();
                    ByteSite *site = siteOf(instruction);
                    Object *instance = std::holds_alternative<Object *>(receiver) ? std::get<Object *>(receiver) : nullptr;
                    if (instance && instance->kind == Kind::Instance && site && site->method && site->type == instance->origin && site->epoch == bindingEpoch)
                    {
                        frame.stack.back() = site->method;
                        frame.methods.push_back({instance, site->owner});
                        break;
                    }
                    MethodTarget target;
                    Value function = member(receiver, node->text, location, false, &target);
                    if (site && instance && instance->kind == Kind::Instance && target.receiver == instance && std::holds_alternative<Object *>(function))
                    {
                        Object *owner = nullptr;
                        Binding *method = classBinding(instance->origin, node->text, &owner);
                        if (method && owner == target.owner && method->visibility == Visibility::Public && !method->staticMember)
                            *site = {instance->origin, bindingEpoch, std::get<Object *>(function), owner, 0};
                    }
                    frame.stack.back() = function;
                    frame.methods.push_back(target);
                    break;
                }
                case ByteOp::Index:
                {
                    Value key = frame.stack.back(), receiver = frame.stack[frame.stack.size() - 2];
                    Value value = index(receiver, key, location);
                    pop();
                    frame.stack.back() = value;
                    break;
                }
                case ByteOp::SetIndex:
                {
                    size_t begin = frame.stack.size() - 3;
                    Value receiver = frame.stack[begin], key = frame.stack[begin + 1], value = frame.stack[begin + 2], custom;
                    Object *container = byteObject(receiver, location);
                    if (node->text != "=")
                        value = binary(compoundOperator(node->text), index(receiver, key, node->children[0]->span), value, location);
                    if (!mayOverload(receiver) || !overloaded("setitem", receiver, {{"", key}, {"", value}}, location, custom))
                    {
                        if (container->kind == Kind::List)
                        {
                            int64_t position = integer(key, location);
                            if (position < 0)
                                position += static_cast<int64_t>(container->items.size());
                            if (position < 0 || static_cast<uint64_t>(position) >= container->items.size())
                                throw Error(location, "Index out of range", "index");
                            container->items[static_cast<size_t>(position)] = value;
                        }
                        else if (container->kind == Kind::Map)
                            setEntry(container, key, value, location);
                        else
                            throw Error(location, "Sequence is immutable", "mutability");
                    }
                    frame.stack.resize(begin);
                    frame.stack.push_back(value);
                    break;
                }
                case ByteOp::Binary:
                {
                    Value right = frame.stack.back(), left = frame.stack[frame.stack.size() - 2];
                    Value value;
                    using Math = ByteInstruction::Math;
                    bool floatOperation = instruction.math >= Math::AddF && instruction.math <= Math::GreaterEqualF;
                    if (floatOperation && std::holds_alternative<double>(left) && std::holds_alternative<double>(right))
                    {
                        if constexpr (profiled)
                            config.profiler->floatSpecializations++;
                        double first = std::get<double>(left), second = std::get<double>(right);
                        switch (instruction.math)
                        {
                        case Math::AddF:
                            value = first + second;
                            break;
                        case Math::SubtractF:
                            value = first - second;
                            break;
                        case Math::MultiplyF:
                            value = first * second;
                            break;
                        case Math::DivideF:
                            if (second == 0)
                                throw Error(location, "Division by zero", "value");
                            value = first / second;
                            break;
                        case Math::EqualF:
                            value = first == second;
                            break;
                        case Math::NotEqualF:
                            value = first != second;
                            break;
                        case Math::LessF:
                            value = first < second;
                            break;
                        case Math::GreaterF:
                            value = first > second;
                            break;
                        case Math::LessEqualF:
                            value = first <= second;
                            break;
                        case Math::GreaterEqualF:
                            value = first >= second;
                            break;
                        default:
                            break;
                        }
                    }
                    else if (!floatOperation && instruction.math != Math::Generic && std::holds_alternative<int64_t>(left) && std::holds_alternative<int64_t>(right))
                    {
                        if constexpr (profiled)
                            config.profiler->integerSpecializations++;
                        int64_t first = std::get<int64_t>(left), second = std::get<int64_t>(right), result = 0;
                        switch (instruction.math)
                        {
                        case Math::Add:
                            __builtin_add_overflow(first, second, &result);
                            value = result;
                            break;
                        case Math::Subtract:
                            __builtin_sub_overflow(first, second, &result);
                            value = result;
                            break;
                        case Math::Multiply:
                            __builtin_mul_overflow(first, second, &result);
                            value = result;
                            break;
                        case Math::FloorDivide:
                        case Math::Modulo:
                        {
                            if (!second)
                                throw Error(location, "Division by zero", "value");
                            if (first == INT64_MIN && second == -1)
                            {
                                value = instruction.math == Math::FloorDivide ? INT64_MIN : 0;
                                break;
                            }
                            int64_t quotient = first / second, remainder = first % second;
                            if (remainder && ((remainder < 0) != (second < 0)))
                            {
                                quotient--;
                                remainder += second;
                            }
                            value = instruction.math == Math::FloorDivide ? quotient : remainder;
                            break;
                        }
                        case Math::Equal:
                            value = first == second;
                            break;
                        case Math::NotEqual:
                            value = first != second;
                            break;
                        case Math::BitAnd:
                            value = first & second;
                            break;
                        case Math::BitOr:
                            value = first | second;
                            break;
                        case Math::BitXor:
                            value = first ^ second;
                            break;
                        case Math::ShiftLeft:
                        case Math::ShiftRight:
                            if (second < 0)
                                throw Error(location, "Negative shift count", "value");
                            value = shiftInt(first, second, instruction.math == Math::ShiftLeft);
                            break;
                        case Math::Less:
                            value = first < second;
                            break;
                        case Math::Greater:
                            value = first > second;
                            break;
                        case Math::LessEqual:
                            value = first <= second;
                            break;
                        case Math::GreaterEqual:
                            value = first >= second;
                            break;
                        default:
                            break;
                        }
                    }
                    else
                    {
                        if constexpr (profiled)
                            config.profiler->genericBinaryOperations++;
                        value = binary(instruction.operand ? compoundOperator(node->text) : node->text, left, right, location);
                    }
                    pop();
                    frame.stack.back() = value;
                    break;
                }
                case ByteOp::Unary:
                {
                    Value value = frame.stack.back(), custom;
                    if (instruction.operand)
                        frame.stack.back() = boolean(value, location);
                    else if (node->text == "!" || node->text == "not")
                        frame.stack.back() = !boolean(value, location);
                    else if (overloaded("unary" + node->text, value, {}, location, custom))
                        frame.stack.back() = custom;
                    else if (node->text == "~")
                    {
                        if (!std::holds_alternative<int64_t>(value))
                            throw Error(location, "Operator '~' requires an Int", "type");
                        frame.stack.back() = ~std::get<int64_t>(value);
                    }
                    else if (node->text == "+")
                    {
                        if (!std::holds_alternative<int64_t>(value) && !std::holds_alternative<double>(value))
                            throw Error(location, "Unary arithmetic requires a number", "type");
                    }
                    else if (std::holds_alternative<int64_t>(value))
                    {
                        frame.stack.back() = static_cast<int64_t>(0 - static_cast<uint64_t>(std::get<int64_t>(value)));
                    }
                    else
                        frame.stack.back() = -floating(value, location);
                    break;
                }
                case ByteOp::CallNative:
                {
                    size_t begin = frame.stack.size() - instruction.operand;
                    if (!instruction.cache)
                        instruction.cache = std::get<Object *>(getPath(node->target));
                    Object *function = instruction.cache;
                    Value result;
                    if (function->fast && !config.profiler)
                        result = function->fast(frame.stack.data() + begin, instruction.operand, location);
                    else
                    {
                        std::vector<Argument> values;
                        values.reserve(instruction.operand);
                        for (size_t index = 0; index < instruction.operand; index++)
                            values.push_back({std::string(), frame.stack[begin + index]});
                        result = call(function, values, location);
                    }
                    frame.stack.resize(begin);
                    frame.stack.push_back(result);
                    break;
                }
                case ByteOp::CallMethod:
                {
                    size_t begin = frame.stack.size() - instruction.operand - 1;
                    Value receiverValue = frame.stack[begin];
                    const std::string &method = node->children[0]->text;
                    Object *receiver = std::holds_alternative<Object *>(receiverValue) ? std::get<Object *>(receiverValue) : nullptr;
                    size_t poolIndex = argumentDepth++;
                    struct Release
                    {
                        size_t &depth;
                        ~Release() { depth--; }
                    } release{argumentDepth};
                    if (argumentPool.size() <= poolIndex)
                        argumentPool.emplace_back();
                    std::vector<Argument> &values = argumentPool[poolIndex];
                    values.clear();
                    for (size_t index = 0; index < instruction.operand; index++)
                        values.push_back({std::string(), frame.stack[begin + 1 + index]});
                    Object *function = nullptr;
                    // Primitive type fast path (Int, Float, Bool) — zero allocation
                    if (!receiver)
                    {
                        const char *primType = std::holds_alternative<int64_t>(receiverValue)  ? "Int"
                                               : std::holds_alternative<double>(receiverValue) ? "Float"
                                               : std::holds_alternative<bool>(receiverValue)   ? "Bool"
                                                                                               : nullptr;
                        if (primType)
                        {
                            auto pt = primitiveMethods.find(primType);
                            if (pt != primitiveMethods.end())
                            {
                                auto fm = pt->second.find(method);
                                if (fm != pt->second.end())
                                {
                                    Value primitiveResult = callPrimitiveMethod(receiverValue, fm->second, values, location);
                                    frame.stack.resize(begin);
                                    frame.stack.push_back(primitiveResult);
                                    break;
                                }
                            }
                            throw Error(location, std::string(primType) + " has no member '" + method + "'", "name");
                        }
                    }
                    // Kind method fast path (e.g. Range) — zero allocation
                    if (receiver && !receiver->origin)
                    {
                        auto km = kindMethods.find(receiver->kind);
                        if (km != kindMethods.end())
                        {
                            auto fm = km->second.find(method);
                            if (fm != km->second.end())
                            {
                                Value kindResult = fm->second.body(*this, receiver, values, location);
                                frame.stack.resize(begin);
                                frame.stack.push_back(kindResult);
                                break;
                            }
                        }
                    }
                    // Exact builtin collections resolve these names to the same native methods member() would find.
                    if (receiver && (!receiver->origin || receiver->origin->builtinCollection) && !config.profiler)
                    {
                        if (instruction.cachedKind != static_cast<int64_t>(receiver->kind))
                        {
                            instruction.cache = nullptr;
                            instruction.cachedKind = static_cast<int64_t>(receiver->kind);
                            auto methods = nativeMethods.find(receiver->kind);
                            if (methods != nativeMethods.end())
                            {
                                auto found = methods->second.find(method);
                                if (found != methods->second.end())
                                    instruction.cache = found->second;
                            }
                        }
                        function = instruction.cache;
                    }
                    Value result;
                    if (function && function->nativeMethod)
                    {
                        struct Scratch
                        {
                            std::vector<Value> &roots;
                            size_t start;
                            ~Scratch() { roots.resize(start); }
                        } scratch{roots, roots.size()};
                        result = callNativeMethod(receiver, *function->nativeMethod, values, location);
                    }
                    else
                    {
                        MethodTarget target;
                        Value bound = member(receiverValue, method, location, false, &target);
                        result = call(bound, values, location, target.receiver, target.owner);
                    }
                    frame.stack.resize(begin);
                    frame.stack.push_back(result);
                    break;
                }
                case ByteOp::Call:
                case ByteOp::InvokeMethod:
                {
                    size_t begin = frame.stack.size() - instruction.operand - 1;
                    if (instruction.integer && !profiled && !config.debug && std::holds_alternative<Object *>(frame.stack[begin]))
                    {
                        Object *callee = std::get<Object *>(frame.stack[begin]);
                        if (callee->kind == Kind::Function)
                        {
                            Object *receiver = instruction.op == ByteOp::InvokeMethod ? frame.methods.back().receiver : nullptr;
                            // Methods compile on their first (general) call so AOT lookup and method mode stay in one place.
                            if (!callee->bytecodeAttempted && !receiver)
                            {
                                callee->bytecode = compileBytecode(callee->code, config.optimizationLevel, false, true);
                                callee->bytecodeAttempted = true;
                            }
                            if (callee->bytecode && callee->bytecode->hasSelf == (receiver != nullptr))
                            {
                                Value result = callFunctionFast(callee, frame.stack.data() + begin + 1, instruction.operand, location, receiver);
                                if (instruction.op == ByteOp::InvokeMethod)
                                    frame.methods.pop_back();
                                frame.stack.resize(begin);
                                frame.stack.push_back(result);
                                break;
                            }
                        }
                    }
                    std::vector<Argument> values;
                    values.reserve(instruction.operand);
                    for (size_t index = 0; index < instruction.operand; index++)
                        values.push_back({node->names[index], frame.stack[begin + index + 1]});
                    MethodTarget target = instruction.op == ByteOp::InvokeMethod ? frame.methods.back() : MethodTarget{};
                    Value result = call(frame.stack[begin], values, location, target.receiver, target.owner);
                    if (instruction.op == ByteOp::InvokeMethod)
                        frame.methods.pop_back();
                    frame.stack.resize(begin);
                    frame.stack.push_back(result);
                    break;
                }
                case ByteOp::List:
                case ByteOp::Tuple:
                case ByteOp::Map:
                {
                    size_t begin = frame.stack.size() - instruction.operand;
                    Object *value = allocate(instruction.op == ByteOp::List ? Kind::List : instruction.op == ByteOp::Tuple ? Kind::Tuple
                                                                                                                           : Kind::Map,
                                             location);
                    roots.push_back(value);
                    if (instruction.op == ByteOp::Map)
                        for (size_t index = begin; index < frame.stack.size(); index += 2)
                            setEntry(value, frame.stack[index], frame.stack[index + 1], location);
                    else
                        value->items.assign(frame.stack.begin() + static_cast<std::ptrdiff_t>(begin), frame.stack.end());
                    frame.stack.resize(begin);
                    frame.stack.push_back(value);
                    roots.pop_back();
                    break;
                }
                case ByteOp::Pop:
                    pop();
                    break;
                case ByteOp::Dup:
                    frame.stack.push_back(frame.stack.back());
                    break;
                case ByteOp::Jump:
                    cursor = instruction.operand;
                    break;
                case ByteOp::JumpFalse:
                {
                    const Value &top = frame.stack.back();
                    if (!(std::holds_alternative<bool>(top) ? std::get<bool>(top) : boolean(top, location)))
                        cursor = instruction.operand;
                    break;
                }
                case ByteOp::BranchFalse:
                {
                    const Value &top = frame.stack.back();
                    bool condition = std::holds_alternative<bool>(top) ? std::get<bool>(top) : boolean(top, location);
                    frame.stack.pop_back();
                    if (!condition)
                        cursor = instruction.operand;
                    break;
                }
                case ByteOp::BranchCompare:
                {
                    using Math = ByteInstruction::Math;
                    const Value &left = frame.stack[frame.stack.size() - 2], &right = frame.stack.back();
                    bool condition;
                    if (instruction.math >= Math::EqualF && instruction.math <= Math::GreaterEqualF && std::holds_alternative<double>(left) && std::holds_alternative<double>(right))
                    {
                        if constexpr (profiled)
                            config.profiler->floatSpecializations++;
                        double first = std::get<double>(left), second = std::get<double>(right);
                        switch (instruction.math)
                        {
                        case Math::EqualF:
                            condition = first == second;
                            break;
                        case Math::NotEqualF:
                            condition = first != second;
                            break;
                        case Math::LessF:
                            condition = first < second;
                            break;
                        case Math::GreaterF:
                            condition = first > second;
                            break;
                        case Math::LessEqualF:
                            condition = first <= second;
                            break;
                        default:
                            condition = first >= second;
                            break;
                        }
                    }
                    else if (instruction.math < Math::AddF && std::holds_alternative<int64_t>(left) && std::holds_alternative<int64_t>(right))
                    {
                        if constexpr (profiled)
                            config.profiler->integerSpecializations++;
                        int64_t first = std::get<int64_t>(left), second = std::get<int64_t>(right);
                        switch (instruction.math)
                        {
                        case Math::Equal:
                            condition = first == second;
                            break;
                        case Math::NotEqual:
                            condition = first != second;
                            break;
                        case Math::Less:
                            condition = first < second;
                            break;
                        case Math::Greater:
                            condition = first > second;
                            break;
                        case Math::LessEqual:
                            condition = first <= second;
                            break;
                        default:
                            condition = first >= second;
                            break;
                        }
                    }
                    else
                    {
                        if constexpr (profiled)
                            config.profiler->genericBinaryOperations++;
                        Value value = binary(node->text, left, right, location);
                        condition = std::holds_alternative<bool>(value) ? std::get<bool>(value) : boolean(value, location);
                    }
                    frame.stack.resize(frame.stack.size() - 2);
                    if (!condition)
                        cursor = instruction.operand;
                    break;
                }
                case ByteOp::NativeStatement:
                    if constexpr (!profiled)
                        if (node->native && nativeAllowed() && runNativeStatement(*node->native, environment))
                            cursor = instruction.operand;
                    break;
                case ByteOp::AddLocalInt:
                {
                    size_t slot = instruction.operand;
                    if (!frame.initialized[slot])
                        throw Error(location, "Field has not been initialized: '" + code.locals[slot].name + "'", "state");
                    Value &target = frame.locals[slot];
                    if (std::holds_alternative<int64_t>(target))
                    {
                        if constexpr (profiled)
                            config.profiler->integerSpecializations++;
                        int64_t result = 0;
                        if (instruction.math == ByteInstruction::Math::Add)
                            __builtin_add_overflow(std::get<int64_t>(target), instruction.integer, &result);
                        else
                            __builtin_sub_overflow(std::get<int64_t>(target), instruction.integer, &result);
                        target = result;
                    }
                    else
                    {
                        if constexpr (profiled)
                            config.profiler->genericBinaryOperations++;
                        target = binary(compoundOperator(node->text), target, instruction.integer, location);
                    }
                    break;
                }
                case ByteOp::JumpTrue:
                {
                    const Value &top = frame.stack.back();
                    if (std::holds_alternative<bool>(top) ? std::get<bool>(top) : boolean(top, location))
                        cursor = instruction.operand;
                    break;
                }
                case ByteOp::JumpPresent:
                    if (!std::holds_alternative<std::monostate>(frame.stack.back()))
                        cursor = instruction.operand;
                    break;
                case ByteOp::JumpNull:
                    if (std::holds_alternative<std::monostate>(frame.stack.back()))
                        cursor = instruction.operand;
                    break;
                case ByteOp::MakeClosure:
                {
                    const auto &info = code.closures[instruction.operand];
                    Object *function = allocate(Kind::Function, location);
                    function->code = info.node;
                    function->parent = environment;
                    function->text = info.node->text;
                    function->bytecode = info.code;
                    function->bytecodeAttempted = true;
                    for (size_t slot : info.slots)
                    {
                        if (!frame.initialized[slot])
                            throw Error(location, "Captured variable '" + code.locals[slot].name + "' is not initialized", "state");
                        function->items.push_back(frame.locals[slot]);
                    }
                    frame.stack.push_back(function);
                    break;
                }
                case ByteOp::IterInit:
                {
                    Value iterable = pop();
                    size_t slot = static_cast<size_t>(instruction.integer);
                    if (std::holds_alternative<Object *>(iterable) && std::get<Object *>(iterable)->kind == Kind::Map)
                    {
                        Object *map = std::get<Object *>(iterable), *keys = allocate(Kind::List, location);
                        for (const auto &entry : map->entries)
                            keys->items.push_back(entry.first);
                        iterable = keys;
                    }
                    frame.locals[slot] = iterable;
                    frame.locals[slot + 1] = int64_t{0};
                    break;
                }
                case ByteOp::IterNext:
                {
                    size_t slot = static_cast<size_t>(instruction.integer), position = static_cast<size_t>(std::get<int64_t>(frame.locals[slot + 1]));
                    Value item;
                    size_t scratch = roots.size();
                    bool more = next(frame.locals[slot], item, position, location);
                    roots.resize(scratch);
                    frame.locals[slot + 1] = static_cast<int64_t>(position);
                    if (more)
                        frame.stack.push_back(item);
                    else
                        cursor = instruction.operand;
                    break;
                }
                case ByteOp::MatchTest:
                {
                    Value subject = frame.stack.back();
                    if (!node->type.empty())
                    {
                        if (!valueMatches(subject, resolveContract(node->type, environment, location), location))
                            cursor = instruction.operand;
                        break;
                    }
                    if (node->text == "_")
                        break;
                    if (!std::holds_alternative<Object *>(subject) || std::get<Object *>(subject)->kind != Kind::Variant)
                        throw Error(location, "match expects an enum value", "type");
                    const std::string &pattern = node->text;
                    if (pattern != "_")
                    {
                        size_t dot = pattern.rfind('.');
                        size_t start = dot == std::string::npos ? 0 : dot + 1;
                        Object *variant = std::get<Object *>(subject);
                        const std::string &name = variant->code && variant->code->kind == "variant" ? variant->code->text : variant->text;
                        if (name.compare(0, std::string::npos, pattern, start, std::string::npos) != 0)
                            cursor = instruction.operand;
                    }
                    break;
                }
                case ByteOp::MatchBind:
                {
                    size_t first = static_cast<size_t>(instruction.integer);
                    if (!node->type.empty())
                    {
                        frame.locals[first] = frame.stack.back();
                        frame.initialized[first] = true;
                        break;
                    }
                    Object *subject = std::get<Object *>(frame.stack.back());
                    for (size_t index = 0; index < instruction.operand && index < subject->items.size(); index++)
                    {
                        frame.locals[first + index] = subject->items[index];
                        frame.initialized[first + index] = true;
                    }
                    break;
                }
                case ByteOp::MatchFail:
                    throw Error(location, "No matching arm", "match");
                case ByteOp::IntToFloat:
                    if (std::holds_alternative<int64_t>(frame.stack.back()))
                        frame.stack.back() = static_cast<double>(std::get<int64_t>(frame.stack.back()));
                    break;
                case ByteOp::NonNull:
                    if (std::holds_alternative<std::monostate>(frame.stack.back()))
                        throw Error(location, "Non-null assertion failed: value is null", "value");
                    break;
                case ByteOp::InstanceOf:
                    frame.stack.back() = valueMatches(frame.stack.back(), resolveContract(node->type, environment, location), location);
                    break;
                case ByteOp::Cast:
                    if (!valueMatches(frame.stack.back(), resolveContract(node->type, environment, location), location))
                        throw Error(location, "Cannot cast " + typeOf(frame.stack.back()) + " to " + node->type, "type");
                    break;
                case ByteOp::SafePoint:
                    collectionSafePoint();
                    break;
                case ByteOp::Defer:
                    frame.deferred.push_back(pop());
                    break;
                case ByteOp::Return:
                {
                    Value retval = frame.stack.back();
                    // Drain deferred calls LIFO before returning; preserve retval as a root.
                    if (!frame.deferred.empty())
                    {
                        roots.push_back(retval);
                        while (!frame.deferred.empty())
                        {
                            Value fn = frame.deferred.back();
                            frame.deferred.pop_back();
                            call(fn, {}, location);
                        }
                        roots.pop_back();
                    }
                    return retval;
                }
                }
            }
            catch (...)
            {
                runDeferred(location);
                throw;
            }
        }
        return {};
    }
}