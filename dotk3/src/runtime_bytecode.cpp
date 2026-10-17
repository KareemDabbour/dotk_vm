#include "runtime.hpp"
#include <algorithm>
#include <limits>
#include <set>

namespace dk3 {
namespace {
Object *byteObject(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected object, got " + Runtime::typeOf(value), "type");
    return std::get<Object *>(value);
}
}
Value Runtime::executeBytecode(const Bytecode &code, Object *environment, const std::vector<Argument> &arguments, const Span &span, const std::vector<Value> *captured) {
    if (config.profiler) return executeBytecodeLoop<true>(code, environment, arguments, span, captured);
    return executeBytecodeLoop<false>(code, environment, arguments, span, captured);
}
template<bool profiled>
Value Runtime::executeBytecodeLoop(const Bytecode &code, Object *environment, const std::vector<Argument> &arguments, const Span &span, const std::vector<Value> *captured) {
    if (byteFrameDepth == byteFrames.size()) byteFrames.emplace_back();
    ByteFrame &frame = byteFrames[byteFrameDepth++];
    struct ReleaseFrame {
        ByteFrame &frame; size_t &depth;
        ~ReleaseFrame() { frame.locals.clear(); frame.stack.clear(); frame.initialized.clear(); frame.contracts.clear(); frame.methods.clear(); depth--; }
    } releaseFrame{frame, byteFrameDepth};
    frame.locals.assign(code.locals.size(), Value{}); frame.initialized.assign(code.locals.size(), false); frame.contracts.resize(code.locals.size());
    frame.names.assign(code.names.size(), nullptr); frame.epoch = bindingEpoch;
    frame.stack.reserve(std::min<size_t>(code.instructions.size(), 64));
    Object *previousAccess = accessClass; accessClass = nullptr;
    for (Object *scope = environment; scope; scope = scope->parent) if (scope->accessClass) { accessClass = scope->accessClass; break; }
    struct RestoreAccess { Object *&current; Object *previous; ~RestoreAccess() { current = previous; } } restoreAccess{accessClass, previousAccess};
    size_t positional = 0;
    for (const auto &argument : arguments) {
        size_t slot = code.parameters;
        if (argument.name.empty()) slot = positional++;
        else for (size_t index = 0; index < code.parameters; index++) if (code.locals[index].name == argument.name) slot = index;
        if (slot >= code.parameters) throw Error(span, argument.name.empty() ? "Too many positional arguments" : "Unknown named argument '" + argument.name + "'", "call");
        if (frame.initialized[slot]) throw Error(span, "Argument supplied more than once: '" + code.locals[slot].name + "'", "call");
        if (config.enforceContracts) { frame.contracts[slot] = resolveContract(code.locals[slot].type, environment, span); requireType(argument.value, frame.contracts[slot], span); }
        frame.locals[slot] = argument.value; frame.initialized[slot] = true;
    }
    for (size_t index = 0; index < code.parameters; index++) if (!frame.initialized[index]) throw Error(span, "Missing argument '" + code.locals[index].name + "'", "call");
    if (captured) for (size_t index = 0; index < code.captures && index < captured->size(); index++) {
        frame.locals[code.parameters + index] = (*captured)[index]; frame.initialized[code.parameters + index] = true;
    }
    auto pop = [&]() { Value value = frame.stack.back(); frame.stack.pop_back(); return value; };
    auto binding = [&](size_t index, const Span &location) -> Binding & {
        if (code.optimization < 2) return lookup(environment, code.names[index], location);
        if (frame.epoch != bindingEpoch) { if constexpr (profiled) config.profiler->cacheInvalidations++; std::fill(frame.names.begin(), frame.names.end(), nullptr); frame.epoch = bindingEpoch; }
        if (!frame.names[index]) { if constexpr (profiled) config.profiler->cacheMisses++; frame.names[index] = &lookup(environment, code.names[index], location); }
        else { if constexpr (profiled) config.profiler->cacheHits++; }
        if (!frame.names[index]->initialized) throw Error(location, "Field has not been initialized: '" + code.names[index] + "'", "state");
        return *frame.names[index];
    };
    for (size_t cursor = 0; cursor < code.instructions.size();) {
        const auto &instruction = code.instructions[cursor++]; const Ast &node = instruction.node; const Span &location = node->span;
        if constexpr (profiled) {
            config.profiler->peakOperandValues = std::max(config.profiler->peakOperandValues, frame.stack.size());
            if (config.profiler->settings().vm == VMProfile::Verbose) config.profiler->opcodes[byteOpName(instruction.op)]++;
        }
        switch (instruction.op) {
        case ByteOp::Tick:
            if constexpr (profiled) tick(location); else tickExecution(location);
            break;
        case ByteOp::Point:
            if constexpr (profiled) { tick(location); debugPoint(node, environment); }
            else {
                tickExecution(location);
                if (!debugFrames.empty()) { debugFrames.back().location = location; debugFrames.back().environment = environment; }
            }
            break;
        case ByteOp::Null: frame.stack.emplace_back(); break;
        case ByteOp::Bool: frame.stack.emplace_back(instruction.operand != 0); break;
        case ByteOp::Int: frame.stack.emplace_back(instruction.integer); break;
        case ByteOp::Float: frame.stack.emplace_back(instruction.floating); break;
        case ByteOp::Text: frame.stack.push_back(text(node->text, node->literalKind == TokenKind::Bytes, location)); break;
        case ByteOp::Literal: frame.stack.push_back(literalValue(node, instruction.operand != 0)); break;
        case ByteOp::Closure: {
            Object *function = allocate(Kind::Function, location); function->code = node; function->parent = environment; function->text = node->text; frame.stack.push_back(function); break;
        }
        case ByteOp::Format: {
            Value converted = frame.stack.back(); std::string value = stringify(converted, location);
            Object *builder = byteObject(frame.stack[frame.stack.size() - 2], location); builder->text += value; pop(); break;
        }
        case ByteOp::Slice: {
            size_t begin = frame.stack.size() - 4;
            std::optional<Value> first = instruction.operand & 1 ? std::optional<Value>{frame.stack[begin + 1]} : std::nullopt;
            std::optional<Value> last = instruction.operand & 2 ? std::optional<Value>{frame.stack[begin + 2]} : std::nullopt;
            std::optional<Value> step = instruction.operand & 4 ? std::optional<Value>{frame.stack[begin + 3]} : std::nullopt;
            Value value = sliceValues(frame.stack[begin], first, last, step, location); frame.stack.resize(begin); frame.stack.push_back(value); break;
        }
        case ByteOp::PrepareSlice: {
            Object *value = byteObject(frame.stack.back(), location);
            if (value->kind != Kind::List && value->kind != Kind::Tuple && value->kind != Kind::String && value->kind != Kind::Bytes) throw Error(location, "Value is not sliceable", "type");
            break;
        }
        case ByteOp::PrepareMember: frame.stack.back() = prepareMemberWrite(frame.stack.back(), node->text, instruction.operand != 0, location); break;
        case ByteOp::SetMember: {
            Object *container = byteObject(frame.stack[frame.stack.size() - 2], location); Binding *target = classBinding(container, node->children[0]->text);
            Value value = frame.stack.back(); if (node->text != "=") value = binary(node->text.substr(0, 1), target->value, value, location);
            requireType(value, target->contract, location); target->value = value; target->initialized = true;
            pop(); frame.stack.back() = value; break;
        }
        case ByteOp::Swap: std::swap(frame.stack[frame.stack.size() - 1], frame.stack[frame.stack.size() - 2]); break;
        case ByteOp::DeclareType: case ByteOp::Import: executeDeclaration(node, environment); break;
        case ByteOp::Throw: throwValue(frame.stack.back(), location); break;
        case ByteOp::Match: {
            Object *value = byteObject(frame.stack.back(), location);
            if (value->kind != Kind::Variant) throw Error(location, "match expects an enum value", "type");
            const auto &match = code.matches[instruction.operand]; std::set<Object *> covered; bool wildcard = false; std::vector<Object *> constructors;
            for (const auto &arm : match.arms) {
                if (wildcard) throw Error(arm.span, "Unreachable match arm after wildcard", "match");
                if (arm.pattern == "_") { wildcard = true; constructors.push_back(nullptr); continue; }
                if (arm.pattern.find('.') == std::string::npos) throw Error(arm.span, "Expected qualified enum variant", "match");
                Object *constructor = resolveObjectPath(arm.pattern, environment, arm.span);
                if ((constructor->kind != Kind::Constructor && constructor->kind != Kind::Variant) || constructor->origin != value->origin) throw Error(location, "Pattern belongs to a different enum", "match");
                if (!covered.insert(constructor).second) throw Error(location, "Duplicate match arm", "match");
                if (arm.bindings.size() != (constructor->code ? constructor->code->names.size() : 0)) throw Error(arm.span, "Pattern payload arity mismatch", "match");
                constructors.push_back(constructor);
            }
            if (!wildcard) for (const auto &variant : value->origin->bindings) if (!covered.count(byteObject(variant.second.value, location))) throw Error(location, "Non-exhaustive match", "match");
            bool matched = false;
            for (size_t index = 0; index < match.arms.size(); index++) {
                if (constructors[index] && constructors[index]->text != value->text) continue;
                const auto &arm = match.arms[index]; if (arm.pattern != "_" && arm.bindings.size() != value->items.size()) throw Error(arm.span, "Pattern payload arity mismatch", "match");
                Object *scope = allocate(Kind::Environment, arm.span); scope->parent = environment; roots.push_back(scope);
                for (size_t position = 0; position < arm.bindings.size(); position++) define(scope, arm.bindings[position], value->items[position], false, "", arm.span);
                Value result = executeBytecode(*arm.code, scope, {}, arm.span); frame.stack.back() = result; roots.pop_back(); matched = true; break;
            }
            if (!matched) throw Error(location, "No matching arm", "match");
            break;
        }
        case ByteOp::LoadName: frame.stack.push_back(binding(instruction.operand, location).value); break;
        case ByteOp::LoadLocal:
            if (!frame.initialized[instruction.operand]) throw Error(location, "Field has not been initialized: '" + node->text + "'", "state");
            frame.stack.push_back(frame.locals[instruction.operand]); break;
        case ByteOp::CheckLocal:
            if (!code.locals[instruction.operand].mutableBinding) throw Error(node->children[0]->span, "Cannot reassign let binding '" + node->children[0]->text + "'", "mutability");
            break;
        case ByteOp::CheckName:
            if (!binding(instruction.operand, node->children[0]->span).mutableBinding) throw Error(node->children[0]->span, "Cannot reassign let binding '" + node->children[0]->text + "'", "mutability");
            break;
        case ByteOp::CheckObject: byteObject(frame.stack.back(), location); break;
        case ByteOp::StoreLocal:
            if (config.enforceContracts) requireType(frame.stack.back(), frame.contracts[instruction.operand], location);
            frame.locals[instruction.operand] = frame.stack.back(); break;
        case ByteOp::StoreName: {
            Binding &target = binding(instruction.operand, node->children[0]->span);
            requireType(frame.stack.back(), target.contract, location); target.value = frame.stack.back(); break;
        }
        case ByteOp::DeclareName: { Value value = pop(); define(environment, node->text, value, node->mutableBinding, node->type, location); break; }
        case ByteOp::DeclareLocal: {
            Value value = frame.stack.back();
            if (config.enforceContracts) { frame.contracts[instruction.operand] = resolveContract(code.locals[instruction.operand].type, environment, location); requireType(value, frame.contracts[instruction.operand], location); }
            frame.locals[instruction.operand] = value; frame.initialized[instruction.operand] = true; pop(); break;
        }
        case ByteOp::ClearLocal: frame.locals[instruction.operand] = {}; frame.initialized[instruction.operand] = false; frame.contracts[instruction.operand].reset(); break;
        case ByteOp::DefineFunction: {
            Object *function = allocate(Kind::Function, location); function->code = node; function->parent = environment; function->text = node->text;
            roots.push_back(function); define(environment, node->text, function, false, "", location); roots.pop_back(); break;
        }
        case ByteOp::Member: { Value value = frame.stack.back(); frame.stack.back() = member(value, node->text, location); break; }
        case ByteOp::ResolveMethod: {
            MethodTarget target;
            Value function = member(frame.stack.back(), node->text, location, false, &target);
            frame.stack.back() = function; frame.methods.push_back(target); break;
        }
        case ByteOp::Index: {
            Value key = frame.stack.back(), receiver = frame.stack[frame.stack.size() - 2]; Value value = index(receiver, key, location);
            pop(); frame.stack.back() = value; break;
        }
        case ByteOp::SetIndex: {
            size_t begin = frame.stack.size() - 3; Value receiver = frame.stack[begin], key = frame.stack[begin + 1], value = frame.stack[begin + 2], custom;
            Object *container = byteObject(receiver, location);
            if (node->text != "=") value = binary(node->text.substr(0, 1), index(receiver, key, node->children[0]->span), value, location);
            if (!overloaded("setitem", receiver, {{"", key}, {"", value}}, location, custom)) {
                if (container->kind == Kind::List) {
                    requireType(value, container->elementContract, location); int64_t position = integer(key, location);
                    if (position < 0) position += static_cast<int64_t>(container->items.size());
                    if (position < 0 || static_cast<uint64_t>(position) >= container->items.size()) throw Error(location, "Index out of range", "index");
                    container->items[static_cast<size_t>(position)] = value;
                } else if (container->kind == Kind::Map) setEntry(container, key, value, location);
                else throw Error(location, "Sequence is immutable", "mutability");
            }
            frame.stack.resize(begin); frame.stack.push_back(value); break;
        }
        case ByteOp::Binary: {
            Value right = frame.stack.back(), left = frame.stack[frame.stack.size() - 2];
            Value value; using Math = ByteInstruction::Math;
            bool floatOperation = instruction.math >= Math::AddF;
            if (floatOperation && std::holds_alternative<double>(left) && std::holds_alternative<double>(right)) {
                if constexpr (profiled) config.profiler->floatSpecializations++;
                double first = std::get<double>(left), second = std::get<double>(right);
                switch (instruction.math) {
                case Math::AddF: value = first + second; break;
                case Math::SubtractF: value = first - second; break;
                case Math::MultiplyF: value = first * second; break;
                case Math::DivideF: if (second == 0) throw Error(location, "Division by zero", "value"); value = first / second; break;
                case Math::EqualF: value = first == second; break;
                case Math::NotEqualF: value = first != second; break;
                case Math::LessF: value = first < second; break;
                case Math::GreaterF: value = first > second; break;
                case Math::LessEqualF: value = first <= second; break;
                case Math::GreaterEqualF: value = first >= second; break;
                default: break;
                }
            } else if (!floatOperation && instruction.math != Math::Generic && std::holds_alternative<int64_t>(left) && std::holds_alternative<int64_t>(right)) {
                if constexpr (profiled) config.profiler->integerSpecializations++;
                int64_t first = std::get<int64_t>(left), second = std::get<int64_t>(right), result = 0; bool overflow = false;
                switch (instruction.math) {
                case Math::Add: overflow = __builtin_add_overflow(first, second, &result); value = result; break;
                case Math::Subtract: overflow = __builtin_sub_overflow(first, second, &result); value = result; break;
                case Math::Multiply: overflow = __builtin_mul_overflow(first, second, &result); value = result; break;
                case Math::FloorDivide: case Math::Modulo: {
                    if (!second) throw Error(location, "Division by zero", "value");
                    if (first == INT64_MIN && second == -1) {
                        if (config.enforceContracts) throw Error(location, "Int overflow", "overflow");
                        value = instruction.math == Math::FloorDivide ? INT64_MIN : 0; break;
                    }
                    int64_t quotient = first / second, remainder = first % second;
                    if (remainder && ((remainder < 0) != (second < 0))) { quotient--; remainder += second; }
                    value = instruction.math == Math::FloorDivide ? quotient : remainder; break;
                }
                case Math::Equal: value = first == second; break;
                case Math::NotEqual: value = first != second; break;
                case Math::Less: value = first < second; break;
                case Math::Greater: value = first > second; break;
                case Math::LessEqual: value = first <= second; break;
                case Math::GreaterEqual: value = first >= second; break;
                default: break;
                }
                if (overflow && config.enforceContracts) throw Error(location, "Int overflow", "overflow");
            } else {
                if constexpr (profiled) config.profiler->genericBinaryOperations++;
                value = binary(instruction.operand ? node->text.substr(0, 1) : node->text, left, right, location);
            }
            pop(); frame.stack.back() = value; break;
        }
        case ByteOp::Unary: {
            Value value = frame.stack.back(), custom;
            if (instruction.operand) frame.stack.back() = boolean(value, location);
            else if (node->text == "!" || node->text == "not") frame.stack.back() = !boolean(value, location);
            else if (overloaded("unary" + node->text, value, {}, location, custom)) frame.stack.back() = custom;
            else if (node->text == "+") {
                if (!std::holds_alternative<int64_t>(value) && !std::holds_alternative<double>(value)) throw Error(location, "Unary arithmetic requires a number", "type");
            } else if (std::holds_alternative<int64_t>(value)) {
                if (std::get<int64_t>(value) == INT64_MIN && config.enforceContracts) throw Error(location, "Int overflow", "overflow");
                frame.stack.back() = static_cast<int64_t>(0 - static_cast<uint64_t>(std::get<int64_t>(value)));
            } else frame.stack.back() = -floating(value, location);
            break;
        }
        case ByteOp::CallNative: {
            size_t begin = frame.stack.size() - instruction.operand;
            if (!instruction.cache) instruction.cache = std::get<Object *>(getPath(node->target));
            Object *function = instruction.cache; Value result;
            if (function->fast && !config.profiler) result = function->fast(frame.stack.data() + begin, instruction.operand, location);
            else {
                std::vector<Argument> values; values.reserve(instruction.operand);
                for (size_t index = 0; index < instruction.operand; index++) values.push_back({std::string(), frame.stack[begin + index]});
                result = call(function, values, location);
            }
            frame.stack.resize(begin); frame.stack.push_back(result); break;
        }
        case ByteOp::CallMethod: {
            size_t begin = frame.stack.size() - instruction.operand - 1;
            Value receiverValue = frame.stack[begin]; const std::string &method = node->children[0]->text;
            Object *receiver = std::holds_alternative<Object *>(receiverValue) ? std::get<Object *>(receiverValue) : nullptr;
            size_t poolIndex = argumentDepth++;
            struct Release { size_t &depth; ~Release() { depth--; } } release{argumentDepth};
            if (argumentPool.size() <= poolIndex) argumentPool.emplace_back();
            std::vector<Argument> &values = argumentPool[poolIndex]; values.clear();
            for (size_t index = 0; index < instruction.operand; index++) values.push_back({std::string(), frame.stack[begin + 1 + index]});
            Object *function = nullptr;
            if (receiver && !receiver->origin && !config.profiler) {
                if (instruction.cachedKind != static_cast<int64_t>(receiver->kind)) {
                    instruction.cache = nullptr; instruction.cachedKind = static_cast<int64_t>(receiver->kind);
                    auto methods = nativeMethods.find(receiver->kind);
                    if (methods != nativeMethods.end()) { auto found = methods->second.find(method); if (found != methods->second.end()) instruction.cache = found->second; }
                }
                function = instruction.cache;
            }
            Value result;
            if (function && function->nativeMethod) result = callNativeMethod(receiver, *function->nativeMethod, values, location);
            else { MethodTarget target; Value bound = member(receiverValue, method, location, false, &target); result = call(bound, values, location, target.receiver, target.owner); }
            frame.stack.resize(begin); frame.stack.push_back(result); break;
        }
        case ByteOp::Call: case ByteOp::InvokeMethod: {
            size_t begin = frame.stack.size() - instruction.operand - 1; std::vector<Argument> values; values.reserve(instruction.operand);
            for (size_t index = 0; index < instruction.operand; index++) values.push_back({node->names[index], frame.stack[begin + index + 1]});
            MethodTarget target = instruction.op == ByteOp::InvokeMethod ? frame.methods.back() : MethodTarget{};
            Value result = call(frame.stack[begin], values, location, target.receiver, target.owner);
            if (instruction.op == ByteOp::InvokeMethod) frame.methods.pop_back();
            frame.stack.resize(begin); frame.stack.push_back(result); break;
        }
        case ByteOp::List: case ByteOp::Tuple: case ByteOp::Map: {
            size_t begin = frame.stack.size() - instruction.operand;
            Object *value = allocate(instruction.op == ByteOp::List ? Kind::List : instruction.op == ByteOp::Tuple ? Kind::Tuple : Kind::Map, location);
            roots.push_back(value);
            if (instruction.op == ByteOp::Map) for (size_t index = begin; index < frame.stack.size(); index += 2) setEntry(value, frame.stack[index], frame.stack[index + 1], location);
            else value->items.assign(frame.stack.begin() + static_cast<std::ptrdiff_t>(begin), frame.stack.end());
            frame.stack.resize(begin); frame.stack.push_back(value); roots.pop_back(); break;
        }
        case ByteOp::Pop: pop(); break;
        case ByteOp::Dup: frame.stack.push_back(frame.stack.back()); break;
        case ByteOp::Jump: cursor = instruction.operand; break;
        case ByteOp::JumpFalse: if (!boolean(frame.stack.back(), location)) cursor = instruction.operand; break;
        case ByteOp::JumpTrue: if (boolean(frame.stack.back(), location)) cursor = instruction.operand; break;
        case ByteOp::JumpPresent: if (!std::holds_alternative<std::monostate>(frame.stack.back())) cursor = instruction.operand; break;
        case ByteOp::JumpNull: if (std::holds_alternative<std::monostate>(frame.stack.back())) cursor = instruction.operand; break;
        case ByteOp::MakeClosure: {
            const auto &info = code.closures[instruction.operand];
            Object *function = allocate(Kind::Function, location); function->code = info.node; function->parent = environment; function->text = info.node->text;
            function->bytecode = info.code; function->bytecodeAttempted = true;
            for (size_t slot : info.slots) {
                if (!frame.initialized[slot]) throw Error(location, "Captured variable '" + code.locals[slot].name + "' is not initialized", "state");
                function->items.push_back(frame.locals[slot]);
            }
            frame.stack.push_back(function); break;
        }
        case ByteOp::IterInit: {
            Value iterable = pop(); size_t slot = static_cast<size_t>(instruction.integer);
            if (std::holds_alternative<Object *>(iterable) && std::get<Object *>(iterable)->kind == Kind::Map) {
                Object *map = std::get<Object *>(iterable), *keys = allocate(Kind::List, location);
                for (const auto &entry : map->entries) keys->items.push_back(entry.first);
                iterable = keys;
            }
            frame.locals[slot] = iterable; frame.locals[slot + 1] = int64_t{0}; break;
        }
        case ByteOp::IterNext: {
            size_t slot = static_cast<size_t>(instruction.integer), position = static_cast<size_t>(std::get<int64_t>(frame.locals[slot + 1]));
            Value item; size_t scratch = roots.size();
            bool more = next(frame.locals[slot], item, position, location);
            roots.resize(scratch);
            frame.locals[slot + 1] = static_cast<int64_t>(position);
            if (more) frame.stack.push_back(item); else cursor = instruction.operand;
            break;
        }
        case ByteOp::MatchTest: {
            Value subject = frame.stack.back();
            if (!std::holds_alternative<Object *>(subject) || std::get<Object *>(subject)->kind != Kind::Variant) throw Error(location, "match expects an enum value", "type");
            const std::string &pattern = node->text;
            if (pattern != "_") {
                size_t dot = pattern.rfind('.'); size_t start = dot == std::string::npos ? 0 : dot + 1;
                if (std::get<Object *>(subject)->text.compare(0, std::string::npos, pattern, start, std::string::npos) != 0) cursor = instruction.operand;
            }
            break;
        }
        case ByteOp::MatchBind: {
            Object *subject = std::get<Object *>(frame.stack.back()); size_t first = static_cast<size_t>(instruction.integer);
            for (size_t index = 0; index < instruction.operand && index < subject->items.size(); index++) { frame.locals[first + index] = subject->items[index]; frame.initialized[first + index] = true; }
            break;
        }
        case ByteOp::MatchFail: throw Error(location, "No matching arm", "match");
        case ByteOp::IntToFloat:
            if (std::holds_alternative<int64_t>(frame.stack.back())) frame.stack.back() = static_cast<double>(std::get<int64_t>(frame.stack.back()));
            break;
        case ByteOp::NonNull:
            if (std::holds_alternative<std::monostate>(frame.stack.back())) throw Error(location, "Non-null assertion failed: value is null", "value");
            break;
        case ByteOp::InstanceOf:
            frame.stack.back() = valueMatches(frame.stack.back(), resolveContract(node->type, environment, location, true, true), location);
            break;
        case ByteOp::Cast:
            if (!valueMatches(frame.stack.back(), resolveContract(node->type, environment, location, true, true), location))
                throw Error(location, "Cannot cast " + typeOf(frame.stack.back()) + " to " + node->type, "type");
            break;
        case ByteOp::SafePoint: collectionSafePoint(); break;
        case ByteOp::Return: return frame.stack.back();
        }
    }
    return {};
}
}