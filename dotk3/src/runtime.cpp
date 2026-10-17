#include "runtime.hpp"
#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>

namespace dk3 {
namespace {
struct Returned { Value value; };
struct Broken {};
struct Continued {};
struct Thrown { Value value; Span span; std::vector<ErrorFrame> trace = {}; };
Value exceptionValue(std::exception_ptr failure) {
    if (!failure) return {};
    try { std::rethrow_exception(failure); }
    catch (const Thrown &thrown) { return thrown.value; }
    catch (const Returned &returned) { return returned.value; }
    catch (...) { return {}; }
}
bool catchable(std::exception_ptr failure) {
    if (!failure) return false;
    try { std::rethrow_exception(failure); }
    catch (const Thrown &) { return true; }
    catch (const Error &error) { return error.category != "limit" && error.category != "cancelled" && error.category != "internal"; }
    catch (...) { return false; }
}
void preferFailure(std::exception_ptr &pending, std::exception_ptr candidate) {
    if (!candidate) return;
    if (!pending) { pending = candidate; return; }
    bool control = false;
    try { std::rethrow_exception(pending); }
    catch (const Returned &) { control = true; }
    catch (const Broken &) { control = true; }
    catch (const Continued &) { control = true; }
    catch (...) {}
    if (control || (catchable(pending) && !catchable(candidate))) pending = candidate;
}
Span hostSpan() { return {std::make_shared<Source>(Source{"<host>", ""}), 0, 0, 1, 1}; }
Object *object(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected object, got " + Runtime::typeOf(value), "type");
    return std::get<Object *>(value);
}
bool hasYield(const Ast &node) {
    if (!node) return false;
    if (node->kind == "yield") return true;
    if (node->kind == "function") return false;
    for (const auto &child : node->children) if (hasYield(child)) return true;
    return false;
}
bool terminal(TaskState state) {
    return state == TaskState::Done || state == TaskState::Cancelled || state == TaskState::Failed;
}
bool taskCancelled(Object *task) {
    for (Object *ancestor = task; ancestor; ancestor = ancestor->taskParent)
        if (ancestor->cancelRequested) return true;
    return false;
}
std::string taskStatus(TaskState state) {
    switch (state) {
    case TaskState::Ready: return "ready";
    case TaskState::Running: return "running";
    case TaskState::Cancelling: return "cancelling";
    case TaskState::Done: return "done";
    case TaskState::Cancelled: return "cancelled";
    case TaskState::Failed: return "failed";
    }
    return "unknown";
}
int64_t parseInteger(std::string text, const Span &span, bool negative = false) {
    text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
    int base = 10; size_t position = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'b')) {
        base = text[1] == 'x' ? 16 : 2; position = 2;
    }
    uint64_t limit = static_cast<uint64_t>(INT64_MAX) + (negative ? 1 : 0), result = 0;
    if (position == text.size()) throw Error(span, "Empty numeric literal", "value");
    for (; position < text.size(); position++) {
        unsigned char byte = static_cast<unsigned char>(text[position]);
        unsigned digit = byte >= '0' && byte <= '9' ? byte - '0' : byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 :
                         byte >= 'A' && byte <= 'F' ? byte - 'A' + 10 : 99;
        if (digit >= static_cast<unsigned>(base)) throw Error(span, "Invalid digit in numeric literal", "value");
        if (result > (limit - digit) / static_cast<unsigned>(base)) throw Error(span, "Int overflow", "overflow");
        result = result * static_cast<unsigned>(base) + digit;
    }
    if (negative && result == static_cast<uint64_t>(INT64_MAX) + 1) return INT64_MIN;
    return negative ? -static_cast<int64_t>(result) : static_cast<int64_t>(result);
}
}

Runtime::Runtime(Config options) : config(std::move(options)) {
    if (config.optimizationLevel > 2) throw Error(hostSpan(), "Optimization level must be 0..2", "value");
    if (config.profiler) config.profiler->optimizationLevel = config.optimizationLevel;
    std::optional<ProfileScope> profile;
    if (config.profiler) profile.emplace(config.profiler.get(), "Initialize", "vm");
    installCore();
    installNativeMethods();
}

Object *Runtime::allocate(Kind kind, const Span &span) {
    requireRunning();
    if (heap.size() >= config.objectLimit) throw Error(span, "Object limit exceeded", "limit");
    auto value = std::make_unique<Object>(); value->kind = kind;
    Object *result = value.get();
    auto type = collectionTypes.find(kind); if (type != collectionTypes.end()) result->origin = type->second;
    heap.push_back(std::move(value));
    if (config.profiler) {
        static const char *names[] = {"Environment", "String", "Bytes", "List", "Tuple", "Map", "Function", "Native", "RecordType", "Record", "ClassType", "Instance", "BoundMethod", "TraitType", "EnumType", "Constructor", "Variant", "Module", "Generator", "Task", "Resource", "Super", "Set"};
        config.profiler->allocation(names[static_cast<size_t>(kind)], span, sizeof(Object), heap.size());
    }
    return result;
}
Value Runtime::text(std::string bytes, bool binary, const Span &span) {
    if (!binary) utf8Offsets(bytes, span);
    Object *result = allocate(binary ? Kind::Bytes : Kind::String, span); result->text = std::move(bytes);
    if (config.profiler) config.profiler->payload(binary ? "Bytes" : "String", span, result->text.size());
    return result;
}
void Runtime::tick(const Span &span) {
    if (config.profiler) config.profiler->checkpoint();
    tickExecution(span);
}
void Runtime::tickExecution(const Span &span) {
    if (cleanupDepth) {
        if (++cleanupSteps > 10000) throw Error(span, "Cleanup execution budget exceeded", "limit");
        return;
    }
    if (taskCancelled(currentTask)) throw Error(span, "Task cancelled", "cancelled");
    if (config.stepLimit && ++steps > config.stepLimit) throw Error(span, "Execution budget exceeded", "limit");
    if (config.interrupted && config.interrupted()) throw Error(span, "Execution interrupted", "cancelled");
}
bool Runtime::boolean(Value value, const Span &span) {
    if (!std::holds_alternative<bool>(value)) throw Error(span, "Condition requires Bool, got " + typeOf(value), "type");
    return std::get<bool>(value);
}
int64_t Runtime::integer(Value value, const Span &span) {
    if (!std::holds_alternative<int64_t>(value)) throw Error(span, "Expected Int, got " + typeOf(value), "type");
    return std::get<int64_t>(value);
}
double Runtime::floating(Value value, const Span &span, bool explicitConversion) {
    if (std::holds_alternative<double>(value)) return std::get<double>(value);
    if (std::holds_alternative<int64_t>(value)) {
        int64_t number = std::get<int64_t>(value);
        if (!explicitConversion && (number < -9007199254740992LL || number > 9007199254740992LL))
            throw Error(span, "Potentially lossy Int/Float mixing; use Float explicitly", "type");
        return static_cast<double>(number);
    }
    throw Error(span, "Expected numeric value", "type");
}
std::string Runtime::typeOf(Value value) {
    if (std::holds_alternative<std::monostate>(value)) return "Null";
    if (std::holds_alternative<bool>(value)) return "Bool";
    if (std::holds_alternative<int64_t>(value)) return "Int";
    if (std::holds_alternative<double>(value)) return "Float";
    Object *instance = std::get<Object *>(value);
    switch (instance->kind) {
    case Kind::String: return "String";
    case Kind::Bytes: return "Bytes";
    case Kind::List: return instance->origin ? instance->origin->text : "List";
    case Kind::Tuple: return instance->origin ? instance->origin->text : "Tuple";
    case Kind::Map: return instance->origin ? instance->origin->text : "Map";
    case Kind::Set: return instance->origin ? instance->origin->text : "Set";
    case Kind::Record: case Kind::Instance: case Kind::Variant: return instance->origin->text;
    case Kind::RecordType: case Kind::ClassType: case Kind::TraitType: case Kind::EnumType: return "Type";
    case Kind::Generator: return "Generator";
    case Kind::Task: return "Task";
    case Kind::Resource: return "Resource";
    case Kind::Super: return "Super";
    case Kind::Module: return "Module";
    case Kind::Function: case Kind::BoundMethod: case Kind::Native: case Kind::Constructor: return "Function";
    case Kind::Environment: return "Environment";
    }
    return "Unknown";
}
namespace {
std::string shortened(const std::string &text, size_t limit = 160) {
    if (text.size() <= limit) return text;
    while (limit && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80) limit--;
    return text.substr(0, limit) + "...";
}
std::string debugValue(Value value, size_t depth = 0) {
    if (!std::holds_alternative<Object *>(value)) return Runtime::display(value);
    Object *instance = std::get<Object *>(value);
    if (instance->kind == Kind::String) {
        std::string result = "\"";
        for (unsigned char byte : shortened(instance->text, 96)) {
            if (byte == '\n') result += "\\n";
            else if (byte == '\r') result += "\\r";
            else if (byte == '\t') result += "\\t";
            else if (byte == '\\' || byte == '"') { result += '\\'; result += static_cast<char>(byte); }
            else if (byte < 32 || byte == 127) {
                const char *hex = "0123456789abcdef"; result += "\\x"; result += hex[byte >> 4]; result += hex[byte & 15];
            } else result += static_cast<char>(byte);
        }
        return result + '"';
    }
    if (instance->kind == Kind::Bytes) return "<Bytes " + std::to_string(instance->text.size()) + ">";
    if (depth < 2 && (instance->kind == Kind::List || instance->kind == Kind::Tuple)) {
        std::string result = instance->kind == Kind::List ? "[" : "(";
        for (size_t index = 0; index < std::min<size_t>(instance->items.size(), 8); index++) {
            if (index) result += ", "; result += debugValue(instance->items[index], depth + 1);
        }
        if (instance->items.size() > 8) result += ", ...";
        result += instance->kind == Kind::List ? "]" : ")"; return shortened(result);
    }
    if (depth < 2 && instance->kind == Kind::Map) {
        std::string result = "{";
        for (size_t index = 0; index < std::min<size_t>(instance->entries.size(), 4); index++) {
            if (index) result += ", ";
            result += debugValue(instance->entries[index].first, depth + 1) + ": " + debugValue(instance->entries[index].second, depth + 1);
        }
        if (instance->entries.size() > 4) result += ", ...";
        return shortened(result + "}");
    }
    return "<" + shortened(Runtime::typeOf(value), 64) + ">";
}
}

Runtime::FrameScope::FrameScope(Runtime &owner, const char *name, const Span &location, Object *environment, Object *generator)
    : runtime(owner), start(owner.debugFrames.size()) {
    uint64_t id = generator ? generator->debugFrameId : ++owner.nextDebugFrame;
    owner.debugFrames.push_back({id, owner.currentTask ? owner.currentTask->debugTaskId : 0,
        name, location, environment, generator});
    if (owner.config.profiler) owner.config.profiler->enter(name, location, owner.currentTask ? owner.currentTask->debugTaskId : 0);
}
Runtime::FrameScope::~FrameScope() { if (runtime.config.profiler) runtime.config.profiler->leave(); runtime.debugFrames.resize(start); }
void Runtime::captureFailure(std::exception_ptr failure) noexcept {
    auto capture = [&](auto &error) {
        if (!error.trace.empty()) return;
        std::vector<ErrorFrame> trace;
        for (auto frame = debugFrames.rbegin(); frame != debugFrames.rend(); ++frame)
            trace.push_back({frame->name, trace.empty() ? error.span : frame->location});
        error.trace = std::move(trace);
    };
    try {
        try { std::rethrow_exception(failure); }
        catch (Error &error) { capture(error); }
        catch (Thrown &thrown) { capture(thrown); }
        catch (...) {}
    } catch (...) {}
}
void Runtime::requireRunning() const {
    if (debugPaused) throw Error(hostSpan(), "VM execution/mutation is unavailable during debugger inspection", "state");
}
std::vector<DebugVariable> Runtime::debugVariables(Object *environment) {
    std::vector<DebugVariable> result; std::set<std::string> seen; Object *first = environment;
    while (environment && result.size() < 64) {
        if ((environment == global || environment == prelude) && environment != first) break;
        for (const auto &entry : environment->bindings) {
            if (result.size() == 64) break;
            if (seen.insert(entry.first).second) result.push_back({shortened(entry.first, 96),
                shortened(typeOf(entry.second.value), 64), debugValue(entry.second.value), entry.second.mutableBinding});
        }
        if (environment == global || environment == prelude) break;
        environment = environment->parent;
    }
    std::sort(result.begin(), result.end(), [](const auto &first, const auto &second) { return first.name < second.name; });
    return result;
}
void Runtime::debugPoint(const Ast &node, Object *environment, const std::string &kind) {
    if (debugFrames.empty()) return;
    debugFrames.back().location = node->span; debugFrames.back().environment = environment;
    if (config.profiler) config.profiler->location(node->span);
    if (!config.debug) return;
    requireRunning();
    DebugSnapshot snapshot; snapshot.sequence = ++debugSequence; snapshot.location = node->span;
    snapshot.kind = kind.empty() ? node->kind : kind;
    for (auto frame = debugFrames.rbegin(); frame != debugFrames.rend(); ++frame)
        snapshot.frames.push_back({frame->id, frame->task, frame->generator != nullptr, shortened(frame->name, 96), frame->location, debugVariables(frame->environment)});
    snapshot.globals = debugVariables(global);
    if (snapshot.globals.size() < 64)
        snapshot.globals.push_back({"System.args", "Tuple", debugValue(coreNamespace("System")->bindings.at("args").value), false});
    std::sort(snapshot.globals.begin(), snapshot.globals.end(), [](const auto &first, const auto &second) { return first.name < second.name; });
    for (const auto &instance : heap) if (instance->kind == Kind::Generator && !instance->finished)
        snapshot.liveGenerators.push_back(instance->debugFrameId);
    for (Object *task : activeTasks) {
        if (snapshot.tasks.size() == 128) { snapshot.tasksTruncated = true; break; }
        Object *generator = task->origin;
        if (!generator) continue;
        Span location = generator->instruction < generator->instructions.size() ?
            generator->instructions[generator->instruction].node->span : generator->code->span;
        for (const auto &frame : debugFrames) if (frame.generator == generator) location = frame.location;
        snapshot.tasks.push_back({task->debugTaskId, task->taskParent ? task->taskParent->debugTaskId : 0,
            generator->debugFrameId, shortened(generator->code->text.empty() ? "<generator>" : generator->code->text, 32),
            taskStatus(task->taskState), location});
    }
    std::sort(snapshot.tasks.begin(), snapshot.tasks.end(), [](const auto &first, const auto &second) { return first.id < second.id; });
    debugPaused = true;
    try { config.debug(snapshot); debugPaused = false; }
    catch (...) { debugPaused = false; throw; }
}
std::string Runtime::display(Value value) {
    std::set<Object *> active;
    std::function<std::string(Value)> render = [&](Value current) -> std::string {
        if (std::holds_alternative<std::monostate>(current)) return "null";
        if (std::holds_alternative<bool>(current)) return std::get<bool>(current) ? "true" : "false";
        if (std::holds_alternative<int64_t>(current)) return std::to_string(std::get<int64_t>(current));
        if (std::holds_alternative<double>(current)) { std::ostringstream result; result << std::setprecision(17) << std::get<double>(current); return result.str(); }
        Object *instance = std::get<Object *>(current);
        if (instance->kind == Kind::Variant && instance->origin && instance->origin->text == "Error.Kind") return instance->text;
        if (instance->kind == Kind::String) return instance->text;
        if (instance->kind == Kind::Bytes) {
            std::ostringstream result; result << "b\"";
            for (unsigned char byte : instance->text) result << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
            return result.str() + '"';
        }
        if (!active.insert(instance).second) return "<cycle>";
        std::string result;
        if (instance->kind == Kind::List || instance->kind == Kind::Tuple || instance->kind == Kind::Variant || instance->kind == Kind::Set) {
            result = instance->kind == Kind::List ? "[" : instance->kind == Kind::Set ? "Set(" : instance->kind == Kind::Tuple ? "(" : instance->origin->text + '.' + instance->text + '(';
            for (size_t index = 0; index < instance->items.size(); index++) { if (index) result += ", "; result += render(instance->items[index]); }
            if (instance->kind == Kind::Tuple && instance->items.size() == 1) result += ',';
            result += instance->kind == Kind::List ? ']' : ')';
        } else if (instance->kind == Kind::Map) {
            result = "{";
            for (size_t index = 0; index < instance->entries.size(); index++) {
                if (index) result += ", "; result += render(instance->entries[index].first) + ": " + render(instance->entries[index].second);
            }
            result += '}';
        } else if (instance->kind == Kind::Record || instance->kind == Kind::Instance) {
            result = instance->origin->text + '(';
            bool first = true;
            for (size_t index = 0; index < instance->origin->code->parameters.size(); index++) {
                const auto &field = instance->origin->code->parameters[index];
                if (field.visibility != Visibility::Public) continue;
                if (!first) result += ", "; first = false; result += field.name + ": " + render(instance->bindings.at(field.name).value);
            }
            result += ')';
        } else result = '<' + typeOf(current) + (instance->text.empty() ? "" : " " + instance->text) + '>';
        active.erase(instance); return result;
    };
    return render(value);
}
bool Runtime::equal(Value left, Value right) {
    if (left.index() != right.index()) {
        if ((std::holds_alternative<int64_t>(left) && std::holds_alternative<double>(right)) ||
            (std::holds_alternative<double>(left) && std::holds_alternative<int64_t>(right))) {
            int64_t number = std::holds_alternative<int64_t>(left) ? std::get<int64_t>(left) : std::get<int64_t>(right);
            double real = std::holds_alternative<double>(left) ? std::get<double>(left) : std::get<double>(right);
            return std::isfinite(real) && real >= -9223372036854775808.0 && real < 9223372036854775808.0 &&
                   std::trunc(real) == real && static_cast<int64_t>(real) == number;
        }
        return false;
    }
    if (!std::holds_alternative<Object *>(left)) return left == right;
    Object *first = std::get<Object *>(left), *second = std::get<Object *>(right);
    if (first == second) return true;
    if (first->kind != second->kind) return false;
    if (first->kind == Kind::String || first->kind == Kind::Bytes) return first->text == second->text;
    if (first->kind == Kind::Tuple || first->kind == Kind::Variant) {
        if (first->origin != second->origin || first->text != second->text || first->items.size() != second->items.size()) return false;
        for (size_t index = 0; index < first->items.size(); index++) if (!equal(first->items[index], second->items[index])) return false;
        return true;
    }
    if (first->kind == Kind::Record && first->origin == second->origin) {
        for (const auto &field : first->bindings) if (!equal(field.second.value, second->bindings.at(field.first).value)) return false;
        return true;
    }
    return false;
}
namespace { bool sameContract(const Contract &first, const Contract &second); }
namespace {
bool wrapsWhole(const std::string &text) {
    if (text.size() < 2 || text.front() != '(' || text.back() != ')') return false;
    int depth = 0;
    for (size_t position = 0; position < text.size(); position++) {
        if (text[position] == '(') depth++; else if (text[position] == ')') depth--;
        if (!depth && position + 1 < text.size()) return false;
    }
    return true;
}
std::vector<std::string> splitUnion(const std::string &text) {
    std::vector<std::string> parts; int depth = 0; size_t start = 0;
    for (size_t position = 0; position < text.size(); position++) {
        char symbol = text[position];
        if (symbol == '[' || symbol == '(') depth++;
        else if (symbol == ']' || symbol == ')') depth--;
        else if (symbol == '|' && !depth) { parts.push_back(text.substr(start, position - start)); start = position + 1; }
    }
    if (parts.empty()) return {};
    parts.push_back(text.substr(start)); return parts;
}
}
Contract Runtime::resolveContract(const std::string &type, Object *environment, const Span &span, bool checkIntersectionMethods, bool forTest) {
    if (type.empty()) return {};
    if (!config.enforceContracts && !forTest && !contractForce) return {};
    struct Force { int &depth; bool active; Force(int &counter, bool enable) : depth(counter), active(enable) { if (active) depth++; } ~Force() { if (active) depth--; } } force(contractForce, forTest);
    static thread_local std::unordered_map<std::string, Contract> builtinContracts;
    if (auto cached = builtinContracts.find(type); cached != builtinContracts.end()) return cached->second;
    auto contract = std::make_shared<TypeContract>(); std::string text = type;
    if (text.back() == '?') { contract->nullable = true; text.pop_back(); }
    std::string inner = text;
    while (wrapsWhole(inner)) inner = inner.substr(1, inner.size() - 2);
    if (inner.rfind("fn(", 0) == 0) { contract->name = "Function"; builtinContracts.emplace(type, contract); return contract; }
    if (auto alternatives = splitUnion(inner); alternatives.size() > 1) {
        contract->name = "Union";
        for (const auto &alternative : alternatives) contract->alternatives.push_back(resolveContract(alternative, environment, span, checkIntersectionMethods));
        return contract;
    }
    auto components = typeIntersections(type);
    if (!components.empty()) {
        contract->name = "Intersection";
        for (const auto &component : components) {
            auto part = resolveContract(component, environment, span, checkIntersectionMethods);
            if (part->nullable || (part->all.empty() && (!part->nominal || part->nominal->kind != Kind::TraitType)))
                throw Error(span, "Intersections require non-null trait components", "type");
            if (part->all.empty()) contract->all.push_back(part);
            else contract->all.insert(contract->all.end(), part->all.begin(), part->all.end());
        }
        std::sort(contract->all.begin(), contract->all.end(), [](const auto &first, const auto &second) { return std::less<Object *>{}(first->nominal, second->nominal); });
        contract->all.erase(std::unique(contract->all.begin(), contract->all.end(), [](const auto &first, const auto &second) { return first->nominal == second->nominal; }), contract->all.end());
        if (contract->all.size() == 1) { auto single = std::make_shared<TypeContract>(*contract->all[0]); single->nullable = contract->nullable; return single; }
        if (checkIntersectionMethods) {
            std::unordered_map<std::string, std::pair<Ast, Object *>> methods;
            for (const auto &part : contract->all) for (const auto &method : part->nominal->code->children) {
                auto inserted = methods.emplace(method->text, std::make_pair(method, part->nominal->parent));
                if (inserted.second) continue;
                auto previous = inserted.first->second;
                if (method->parameters.size() != previous.first->parameters.size() ||
                    !sameContract(resolveContract(method->type, part->nominal->parent, span, false), resolveContract(previous.first->type, previous.second, span, false)))
                    throw Error(span, "Conflicting intersection method '" + method->text + "'", "type");
                for (size_t position = 0; position < method->parameters.size(); position++)
                    if (method->parameters[position].name != previous.first->parameters[position].name ||
                        !sameContract(resolveContract(method->parameters[position].type, part->nominal->parent, span, false), resolveContract(previous.first->parameters[position].type, previous.second, span, false)))
                        throw Error(span, "Conflicting intersection method '" + method->text + "'", "type");
            }
        }
        return contract;
    }
    size_t generic = text.find('['); contract->name = text.substr(0, generic);
    if (contract->name.find('.') != std::string::npos || (generic != std::string::npos && contract->name != "List" && contract->name != "Map" && contract->name != "Tuple" && contract->name != "Set")) {
        Object *type = resolveObjectPath(contract->name, environment, span);
        if (type == objectType) contract->name = "Object";
        if (type->kind == Kind::ClassType && type->builtinCollection) contract->name = type->text;
    }
    if (generic != std::string::npos && contract->name != "List" && contract->name != "Tuple" && contract->name != "Map" && contract->name != "Set")
        throw Error(span, "Unsupported generic annotation '" + type + "'", "type");
    if (generic != std::string::npos) {
        auto arguments = typeArguments(text); size_t expected = contract->name == "Map" ? 2 : 1;
        if (arguments.size() != expected) throw Error(span, contract->name + " requires " + std::to_string(expected) + " type arguments", "type");
        if (contract->name == "Map") {
            contract->key = resolveContract(arguments[0], environment, span, checkIntersectionMethods);
            if (contract->key->nominal || !std::set<std::string>{"Any", "Int", "Float", "Bool", "Null", "String", "Bytes"}.count(contract->key->name))
                throw Error(span, "Map key annotations must be primitive or Any", "type");
        }
        contract->element = resolveContract(arguments.back(), environment, span, checkIntersectionMethods);
    }
    static const std::set<std::string> builtin{"Any", "Object", "Int", "Float", "Bool", "Null", "String", "Bytes", "List", "Tuple", "Map", "Set", "Function", "Generator", "Task", "Resource"};
    if (builtin.count(contract->name)) {
        if (generic == std::string::npos) builtinContracts.emplace(type, contract);
        return contract;
    }
    if (contract->name == "Error") { contract->nominal = errorType; roots.push_back(errorType); return contract; }
    contract->nominal = resolveObjectPath(contract->name, environment, span);
    if (contract->nominal == objectType) { contract->name = "Object"; contract->nominal = nullptr; return contract; }
    if (contract->nominal->kind != Kind::RecordType && contract->nominal->kind != Kind::ClassType && contract->nominal->kind != Kind::TraitType && contract->nominal->kind != Kind::EnumType)
        throw Error(span, "Annotation does not name a record, class, trait or enum type: '" + contract->name + "'", "type");
    roots.push_back(contract->nominal); return contract;
}
Object *Runtime::resolveObjectPath(const std::string &name, Object *environment, const Span &span) {
    size_t separator = name.find('.');
    Value resolved = lookup(environment, name.substr(0, separator), span).value;
    while (separator != std::string::npos) {
        size_t start = separator + 1; separator = name.find('.', start);
        Object *space = object(resolved, span);
        if (space->kind != Kind::Module && space->kind != Kind::EnumType && space != errorType) throw Error(span, "Qualified path requires a module or type namespace", "type");
        auto found = space->bindings.find(name.substr(start, separator == std::string::npos ? separator : separator - start));
        if (found == space->bindings.end()) throw Error(span, "Member is not exported: '" + name + "'", "type");
        resolved = found->second.value;
    }
    return object(resolved, span);
}
namespace {
bool sameContract(const Contract &first, const Contract &second) {
    if (!first || !second) return first == second;
    if (first->all.size() != second->all.size()) return false;
    for (size_t position = 0; position < first->all.size(); position++) if (!sameContract(first->all[position], second->all[position])) return false;
    return first->nullable == second->nullable && first->nominal == second->nominal &&
        (first->nominal || first->name == second->name) && sameContract(first->key, second->key) && sameContract(first->element, second->element);
}
bool primitiveKey(Value value) {
    if (!std::holds_alternative<Object *>(value)) return true;
    Kind kind = std::get<Object *>(value)->kind;
    return kind == Kind::String || kind == Kind::Bytes;
}
}
void Runtime::requireType(Value value, const Contract &contract, const Span &span) {
    if (!contract) return;
    if (!contract->nominal && contract->all.empty() && !contract->element && !std::holds_alternative<Object *>(value)) {
        const std::string &name = contract->name;
        if (name == "Any" || (contract->nullable && std::holds_alternative<std::monostate>(value))) return;
        if ((name == "Int" && std::holds_alternative<int64_t>(value)) || (name == "Float" && std::holds_alternative<double>(value)) ||
            (name == "Bool" && std::holds_alternative<bool>(value)) || (name == "Null" && std::holds_alternative<std::monostate>(value))) return;
    }
    struct Pending { Object *collection; Contract key, element; };
    std::vector<Pending> pending;
    std::unordered_map<Object *, size_t> staged;
    std::function<void(Value, const Contract &)> validate = [&](Value candidate, const Contract &expected) {
        if (!expected || expected->name == "Any") return;
        if (expected->nullable && std::holds_alternative<std::monostate>(candidate)) return;
        if (expected->name == "Object" && !expected->nominal) { if (std::holds_alternative<std::monostate>(candidate)) throw Error(span, "Object does not accept null; use Object?", "type"); return; }
        if (!expected->all.empty()) { for (const auto &part : expected->all) validate(candidate, part); return; }
        if (!expected->alternatives.empty()) {
            for (const auto &alternative : expected->alternatives) {
                try { validate(candidate, alternative); return; }
                catch (const Error &failure) { if (failure.category != "type") throw; }
            }
            throw Error(span, "Expected a union member, got " + typeOf(candidate), "type");
        }
        bool valid = false; Object *instance = std::holds_alternative<Object *>(candidate) ? std::get<Object *>(candidate) : nullptr;
        if (expected->nominal && expected->nominal->kind == Kind::TraitType) {
            if (instance && instance->origin && (instance->origin->kind == Kind::ClassType || instance->origin->kind == Kind::RecordType)) {
                for (Object *type = instance->origin; type; type = type->baseClass)
                    if (std::find(type->traits.begin(), type->traits.end(), expected->nominal) != type->traits.end()) valid = true;
            }
            else if (instance && expected->nominal == sizedTrait)
                valid = instance->kind == Kind::List || instance->kind == Kind::Tuple || instance->kind == Kind::Map || instance->kind == Kind::String || instance->kind == Kind::Bytes;
        } else if (expected->nominal) {
            if (instance && instance->origin) {
                for (Object *type = instance->origin; type; type = type->baseClass) if (type == expected->nominal) valid = true;
            }
        }
        else if (expected->name == "Int") valid = std::holds_alternative<int64_t>(candidate);
        else if (expected->name == "Float") valid = std::holds_alternative<double>(candidate);
        else if (expected->name == "Bool") valid = std::holds_alternative<bool>(candidate);
        else if (expected->name == "Null") valid = std::holds_alternative<std::monostate>(candidate);
        else if (instance && expected->name == "List") valid = instance->kind == Kind::List;
        else if (instance && expected->name == "Tuple") valid = instance->kind == Kind::Tuple;
        else if (instance && expected->name == "Map") valid = instance->kind == Kind::Map;
        else if (instance && expected->name == "Set") valid = instance->kind == Kind::Set;
        else if (instance && instance->kind != Kind::Record && instance->kind != Kind::Instance && instance->kind != Kind::Variant) valid = typeOf(candidate) == expected->name;
        if (!valid) throw Error(span, "Expected " + expected->name + ", got " + typeOf(candidate), "type");
        if (!expected->element || !instance) return;
        if (instance->kind == Kind::Map) {
            for (const auto &entry : instance->entries) {
                if (!primitiveKey(entry.first)) throw Error(span, "Map keys must be primitive values", "type");
                validate(entry.first, expected->key); validate(entry.second, expected->element);
            }
        } else for (Value item : instance->items) validate(item, expected->element);
        if (instance->kind != Kind::List && instance->kind != Kind::Map && instance->kind != Kind::Set) return;
        if ((instance->elementContract && !sameContract(instance->elementContract, expected->element)) ||
            (instance->keyContract && !sameContract(instance->keyContract, expected->key)))
            throw Error(span, "Conflicting collection element annotations", "type");
        auto found = staged.find(instance);
        if (found != staged.end()) {
            const auto &attached = pending[found->second];
            if (!sameContract(attached.key, expected->key) || !sameContract(attached.element, expected->element))
                throw Error(span, "Conflicting collection element annotations", "type");
            return;
        }
        staged.emplace(instance, pending.size());
        pending.push_back({instance, expected->key, expected->element});
    };
    validate(value, contract);
    for (const auto &attached : pending) {
        attached.collection->keyContract = attached.key;
        attached.collection->elementContract = attached.element;
    }
}
bool Runtime::valueMatches(Value value, const Contract &contract, const Span &span) {
    try { requireType(std::move(value), contract, span); return true; }
    catch (const Error &failure) { if (failure.category != "type") throw; return false; }
}
Binding &Runtime::lookup(Object *environment, const std::string &name, const Span &span) {
    for (Object *scope = environment; scope; scope = scope->parent) {
        auto binding = scope->bindings.find(name); if (binding != scope->bindings.end()) {
            if (!binding->second.initialized) throw Error(span, "Field has not been initialized: '" + name + "'", "state");
            return binding->second;
        }
    }
    throw Error(span, "Undeclared name '" + name + "'", "name");
}
void Runtime::define(Object *environment, const std::string &name, Value value, bool mutableBinding, const std::string &type, const Span &span, Contract contract) {
    if (environment->bindings.count(name) && !(shadowGlobals && environment == global)) throw Error(span, "Duplicate declaration '" + name + "'", "name");
    if (!contract) contract = resolveContract(type, environment, span);
    requireType(value, contract, span); environment->bindings[name] = {value, mutableBinding, type, std::move(contract)};
    bindingEpoch++;
}
Value Runtime::native(const std::string &name, std::function<Value(const std::vector<Argument> &, const Span &)> function) {
    Object *result = allocate(Kind::Native, hostSpan()); result->text = name; result->native = std::move(function); return result;
}
Object *Runtime::coreNamespace(const std::string &name) {
    auto found = prelude->bindings.find(name);
    if (found != prelude->bindings.end()) {
        Object *space = object(found->second.value, hostSpan());
        if (space->kind != Kind::Module) throw Error(hostSpan(), "Native namespace conflicts with a binding", "type");
        return space;
    }
    Object *space = allocate(Kind::Module, hostSpan()); space->text = name;
    define(prelude, name, space, false, "", hostSpan()); return space;
}
void Runtime::registerNative(const std::string &name, std::function<Value(const std::vector<Argument> &, const Span &)> function) {
    bool first = true;
    for (unsigned char byte : name) {
        if (byte == '.') { if (first) throw Error(hostSpan(), "Invalid native qualified name", "name"); first = true; continue; }
        if (!(std::isalpha(byte) || byte == '_' || (!first && std::isdigit(byte)))) throw Error(hostSpan(), "Invalid native qualified name", "name");
        first = false;
    }
    if (first) throw Error(hostSpan(), "Invalid native qualified name", "name");
    size_t separator = name.find('.'); Object *space = prelude; size_t start = 0;
    while (separator != std::string::npos) {
        std::string part = name.substr(start, separator - start);
        if (space == prelude) space = coreNamespace(part);
        else {
            auto found = space->bindings.find(part);
            if (found == space->bindings.end()) { Object *child = allocate(Kind::Module, hostSpan()); child->text = part; define(space, part, child, false, "", hostSpan()); space = child; }
            else { space = object(found->second.value, hostSpan()); if (space->kind != Kind::Module) throw Error(hostSpan(), "Native namespace conflicts with a binding", "type"); }
        }
        start = separator + 1; separator = name.find('.', start);
    }
    std::string member = name.substr(start);
    if (member.empty()) throw Error(hostSpan(), "Native member name is empty", "name");
    define(space, member, native(name, std::move(function)), false, "", hostSpan());
}
void Runtime::registerModule(const std::string &name, const std::string &documentation, const std::vector<NativeExport> &exports) {
    requireRunning();
    auto identifier = [](const std::string &candidate) {
        if (candidate.empty()) return false;
        for (size_t position = 0; position < candidate.size(); position++) {
            unsigned char byte = candidate[position];
            if (!(byte == '_' || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (position && byte >= '0' && byte <= '9'))) return false;
        }
        return !std::set<std::string>{"let", "var", "fn", "gen", "class", "record", "trait", "impl", "implements", "enum",
            "import", "from", "as", "export", "if", "else", "while", "for", "in", "return", "yield", "break", "continue",
            "defer", "try", "catch", "finally", "throw", "match", "case", "and", "or", "not", "true", "false", "null", "static", "extends", "super", "constructor", "public", "protected", "private"}.count(candidate);
    };
    if (!identifier(name)) throw Error(hostSpan(), "Invalid native module name", "name");
    if (prelude->bindings.count(name) || global->bindings.count(name) || modules.count(name))
        throw Error(hostSpan(), "Module name is already bound: '" + name + "'", "name");
    std::set<std::string> names;
    utf8Offsets(documentation, hostSpan());
    for (const auto &entry : exports) {
        if (!identifier(entry.name) || !entry.function || !names.insert(entry.name).second)
            throw Error(hostSpan(), "Invalid or duplicate native export", "name");
        utf8Offsets(entry.documentation, hostSpan());
    }
    Object *space = allocate(Kind::Module, hostSpan()); size_t root = retain(space);
    struct Release { Runtime &runtime; size_t root; ~Release() { runtime.release(root); } } release{*this, root};
    space->text = name; space->documentation = documentation;
    for (const auto &entry : exports) {
        Object *function = object(native(name + "." + entry.name, entry.function), hostSpan());
        function->documentation = entry.documentation;
        define(space, entry.name, function, false, "", hostSpan());
    }
    define(prelude, name, space, false, "", hostSpan());
}
std::vector<Value> Runtime::positional(const std::vector<Argument> &arguments, const Span &span) {
    std::vector<Value> result;
    for (const auto &argument : arguments) {
        if (!argument.name.empty()) throw Error(span, "This native expects positional arguments", "call");
        result.push_back(argument.value);
    }
    return result;
}
std::string Runtime::stringify(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) return display(value);
    Object *instance = object(value, span);
    if (!instance->origin || (instance->origin->kind != Kind::ClassType && instance->origin->kind != Kind::RecordType)) return display(value);
    Binding *hook = classBinding(instance->origin, "toString");
    if (!hook) return display(value);
    if (hook->staticMember || hook->visibility != Visibility::Public) throw Error(span, "toString must be a public instance method", "type");
    if (std::find(converting.begin(), converting.end(), instance) != converting.end()) throw Error(span, "Recursive toString conversion", "state");
    converting.push_back(instance); roots.push_back(value);
    struct Finish { std::vector<Object *> &stack; ~Finish() { stack.pop_back(); } } finish{converting};
    Value result = call(member(value, "toString", span), {}, span); roots.push_back(result);
    if (!std::holds_alternative<Object *>(result) || object(result, span)->kind != Kind::String) throw Error(span, "toString must return String", "type");
    return object(result, span)->text;
}
bool Runtime::overloaded(const std::string &operation, Value receiver, const std::vector<Argument> &arguments, const Span &span, Value &result) {
    const auto *hook = operatorHook(operation);
    if (!hook || !std::holds_alternative<Object *>(receiver)) return false;
    Object *instance = object(receiver, span);
    if (!instance->origin || (instance->origin->kind != Kind::ClassType && instance->origin->kind != Kind::RecordType)) return false;
    Binding *binding = classBinding(instance->origin, hook->name); if (!binding) return false;
    if (binding->staticMember || binding->visibility != Visibility::Public) throw Error(span, "Operator hook must be a public instance method", "type");
    tick(span); roots.push_back(receiver);
    result = call(member(receiver, hook->name, span), arguments, span); roots.push_back(result);
    if (hook->predicate) boolean(result, span);
    if (operation == "setitem" && !std::holds_alternative<std::monostate>(result)) throw Error(span, "__setitem__ must return Null", "type");
    return true;
}
Value Runtime::binary(const std::string &operation, Value left, Value right, const Span &span) {
    if (operation == "==") return equal(left, right);
    if (operation == "!=") return !equal(left, right);
    Value custom;
    if (std::holds_alternative<Object *>(left) && overloaded(operation, left, {{"", right}}, span, custom)) return custom;
    if (operation == "+" && std::holds_alternative<Object *>(left) && std::holds_alternative<Object *>(right)) {
        Object *first = object(left, span), *second = object(right, span);
        if (first->kind == second->kind && (first->kind == Kind::String || first->kind == Kind::Bytes))
            return text(first->text + second->text, first->kind == Kind::Bytes, span);
        if (first->kind == Kind::List && second->kind == Kind::List) {
            Object *result = allocate(Kind::List, span); result->items = first->items;
            result->items.insert(result->items.end(), second->items.begin(), second->items.end()); return result;
        }
    }
    if (!config.enforceContracts && operation == "+") {
        auto isText = [&](const Value &value) { return std::holds_alternative<Object *>(value) && std::get<Object *>(value)->kind == Kind::String; };
        if (isText(left) || isText(right)) return text(stringify(left, span) + stringify(right, span), false, span);
    }
    if (std::holds_alternative<int64_t>(left) && std::holds_alternative<int64_t>(right)) {
        int64_t first = std::get<int64_t>(left), second = std::get<int64_t>(right), result = 0;
        bool strict = config.enforceContracts;  // checked programs wrap like Java; contract mode keeps overflow errors
        if (operation == "+") { if (__builtin_add_overflow(first, second, &result) && strict) throw Error(span, "Int overflow", "overflow"); return result; }
        if (operation == "-") { if (__builtin_sub_overflow(first, second, &result) && strict) throw Error(span, "Int overflow", "overflow"); return result; }
        if (operation == "*") { if (__builtin_mul_overflow(first, second, &result) && strict) throw Error(span, "Int overflow", "overflow"); return result; }
        if (operation == "//" || operation == "%") {
            if (!second) throw Error(span, "Division by zero", "value");
            if (first == INT64_MIN && second == -1) {
                if (strict) throw Error(span, "Int overflow", "overflow");
                return operation == "//" ? INT64_MIN : int64_t(0);
            }
            int64_t quotient = first / second, remainder = first % second;
            if (remainder && ((remainder < 0) != (second < 0))) { quotient--; remainder += second; }
            return operation == "//" ? quotient : remainder;
        }
        if (operation == "**") {
            if (second < 0) throw Error(span, "Int exponent must be non-negative; use Math.toFloat for fractional powers", "value");
            int64_t base = first; result = 1; uint64_t exponent = static_cast<uint64_t>(second);
            while (exponent) {
                if ((exponent & 1) && __builtin_mul_overflow(result, base, &result) && strict) throw Error(span, "Int overflow", "overflow");
                exponent >>= 1;
                if (exponent && __builtin_mul_overflow(base, base, &base) && strict) throw Error(span, "Int overflow", "overflow");
            }
            return result;
        }
        if (operation == "<") return first < second;
        if (operation == ">") return first > second;
        if (operation == "<=") return first <= second;
        if (operation == ">=") return first >= second;
    }
    double first = floating(left, span), second = floating(right, span);
    if (operation == "+") return first + second;
    if (operation == "-") return first - second;
    if (operation == "*") return first * second;
    if (operation == "/") { if (second == 0) throw Error(span, "Division by zero", "value"); return first / second; }
    if (operation == "//" || operation == "%") {
        if (second == 0) throw Error(span, "Division by zero", "value");
        double result;
        if (operation == "//") result = std::floor(first / second);
        else { result = std::fmod(first, second); if (result != 0 && ((result < 0) != (second < 0))) result += second; }
        if (!std::isfinite(result)) throw Error(span, "Float result is not finite", "value");
        return result;
    }
    if (operation == "**") {
        double power = std::pow(first, second);
        if (!std::isfinite(power)) throw Error(span, "Float power result is not finite", "value");
        return power;
    }
    if (operation == "<") return first < second;
    if (operation == ">") return first > second;
    if (operation == "<=") return first <= second;
    if (operation == ">=") return first >= second;
    throw Error(span, "Invalid numeric operation '" + operation + "'", "type");
}
Value Runtime::index(Value value, Value key, const Span &span) {
    Value custom; if (overloaded("getitem", value, {{"", key}}, span, custom)) return custom;
    Object *instance = object(value, span);
    if (instance->kind == Kind::Map) {
        if (!primitiveKey(key)) throw Error(span, "Map keys must be primitive values", "type");
        requireType(key, instance->keyContract, span);
        for (const auto &entry : instance->entries) if (equal(entry.first, key)) return entry.second;
        throw Error(span, "Missing map key", "key");
    }
    int64_t position = integer(key, span);
    size_t size = instance->kind == Kind::String ? utf8Offsets(instance->text, span).size() - 1 :
                  instance->kind == Kind::Bytes ? instance->text.size() : instance->items.size();
    if (position < 0) position += static_cast<int64_t>(size);
    if (position < 0 || static_cast<uint64_t>(position) >= size) throw Error(span, "Index out of range", "index");
    if (instance->kind == Kind::String) {
        auto offsets = utf8Offsets(instance->text, span);
        return text(instance->text.substr(offsets[position], offsets[position + 1] - offsets[position]), false, span);
    }
    if (instance->kind == Kind::Bytes) return static_cast<int64_t>(static_cast<unsigned char>(instance->text[position]));
    if (instance->kind == Kind::List || instance->kind == Kind::Tuple) return instance->items[position];
    throw Error(span, "Value is not indexable", "type");
}

Value Runtime::sliceValues(Value receiver, const std::optional<Value> &first, const std::optional<Value> &last, const std::optional<Value> &stride, const Span &span) {
    Object *source = object(receiver, span);
    if (source->kind != Kind::List && source->kind != Kind::Tuple && source->kind != Kind::String && source->kind != Kind::Bytes) throw Error(span, "Value is not sliceable", "type");
    auto offsets = source->kind == Kind::String ? utf8Offsets(source->text, span) : std::vector<size_t>{};
    int64_t size = source->kind == Kind::String ? static_cast<int64_t>(offsets.size() - 1) : source->kind == Kind::Bytes ? static_cast<int64_t>(source->text.size()) : static_cast<int64_t>(source->items.size());
    std::optional<int64_t> start = first ? std::optional<int64_t>{integer(*first, span)} : std::nullopt;
    std::optional<int64_t> stop = last ? std::optional<int64_t>{integer(*last, span)} : std::nullopt;
    int64_t step = stride ? integer(*stride, span) : 1;
    if (!step) throw Error(span, "Slice step cannot be zero", "value");
    auto endpoint = [&](const std::optional<int64_t> &bound, bool begin) {
        if (!bound) return step > 0 ? (begin ? int64_t{0} : size) : (begin ? size - 1 : int64_t{-1});
        int64_t value = *bound; if (value < 0) value += size;
        return step > 0 ? std::clamp(value, int64_t{0}, size) : std::clamp(value, int64_t{-1}, size - 1);
    };
    int64_t begin = endpoint(start, true), end = endpoint(stop, false); Object *result = allocate(source->kind, span);
    for (int64_t position = begin; step > 0 ? position < end : position > end;) {
        tick(span);
        if (source->kind == Kind::String) result->text += source->text.substr(offsets[position], offsets[position + 1] - offsets[position]);
        else if (source->kind == Kind::Bytes) result->text += source->text[position];
        else result->items.push_back(source->items[position]);
        int64_t following; if (__builtin_add_overflow(position, step, &following)) break; position = following;
    }
    return result;
}
Value Runtime::literalValue(const Ast &node, bool negative) {
    if (negative) return parseInteger(node->children[0]->text, node->span, true);
    if (node->literalKind == TokenKind::Integer) return parseInteger(node->text, node->span);
    if (node->literalKind == TokenKind::Float) {
        std::string value = node->text; value.erase(std::remove(value.begin(), value.end(), '_'), value.end());
        try { return std::stod(value); } catch (const std::exception &) { throw Error(node->span, "Float literal out of range", "overflow"); }
    }
    throw Error(node->span, "Invalid literal bytecode", "internal");
}
Object *Runtime::prepareMemberWrite(Value receiver, const std::string &name, bool compound, const Span &span) {
    Object *container = object(receiver, span);
    if (container->kind == Kind::Super) {
        bool inherited = false; for (const auto &field : classFields(container->origin)) if (field.first.name == name) inherited = true;
        if (!inherited) throw Error(span, "Unknown parent field", "type");
        container = container->parent;
    }
    if (container->kind != Kind::Instance && container->kind != Kind::ClassType && container->kind != Kind::List && container->kind != Kind::Map && container->kind != Kind::Set && container->kind != Kind::Tuple) throw Error(span, "Record and module fields are immutable", "mutability");
    Binding *binding = classBinding(container, name);
    if (!binding) throw Error(span, "Unknown or non-field member '" + name + "'", "name");
    requireAccess(*binding, span);
    if (container->kind == Kind::ClassType && !binding->staticMember) throw Error(span, "Instance member requires an instance", "mutability");
    if (!binding->mutableBinding && (binding->initialized || constructing.empty() || constructing.back() != container)) throw Error(span, "Cannot reassign let field '" + name + "'", "mutability");
    if (!binding->initialized && compound) throw Error(span, "Uninitialized field requires plain assignment", "state");
    return container;
}
void Runtime::throwValue(Value value, const Span &span) { throw Thrown{value, span}; }
Binding *Runtime::classBinding(Object *type, const std::string &name, Object **owner) {
    for (Object *current = type; current; current = current->baseClass) {
        auto found = current->bindings.find(name);
        if (found != current->bindings.end()) { if (owner) *owner = current; return &found->second; }
    }
    return nullptr;
}
bool Runtime::canAccess(const Binding &binding) const {
    if (binding.visibility == Visibility::Public) return true;
    if (accessClass == binding.declaringClass) return true;
    if (binding.visibility == Visibility::Protected) for (Object *type = accessClass; type; type = type->baseClass)
        if (type == binding.declaringClass) return true;
    return false;
}
void Runtime::requireAccess(const Binding &binding, const Span &span) const {
    if (canAccess(binding)) return;
    throw Error(span, binding.visibility == Visibility::Private ? "Private member is not accessible" : "Protected member is not accessible", "access");
}
std::vector<std::pair<Parameter, Object *>> Runtime::classFields(Object *type) {
    std::vector<Object *> hierarchy;
    for (Object *current = type; current; current = current->baseClass) hierarchy.push_back(current);
    std::vector<std::pair<Parameter, Object *>> result;
    for (auto current = hierarchy.rbegin(); current != hierarchy.rend(); ++current)
        for (const auto &field : (*current)->code->parameters) result.push_back({field, *current});
    return result;
}
Value Runtime::member(Value value, const std::string &name, const Span &span, bool nativeOnly, MethodTarget *target) {
    Object *instance = object(value, span);
    if (instance == errorType && name == "Kind") return errorKindType;
    if (instance->kind == Kind::Resource && instance->external) {
        if (name == "closed") return instance->external->closed;
        if (name == "type_name") return text(instance->external->name, false, span);
    }
    if (instance->kind == Kind::ClassType) {
        auto field = classBinding(instance, name);
        if (!field || !field->staticMember) throw Error(span, "Unknown static member '" + name + "'", "name");
        requireAccess(*field, span);
        return field->value;
    }
    bool collection = instance->kind == Kind::List || instance->kind == Kind::Map || instance->kind == Kind::Tuple || instance->kind == Kind::Set;
    if (!nativeOnly && (instance->kind == Kind::Instance || instance->kind == Kind::Record || instance->kind == Kind::Super || collection)) {
        Object *receiver = instance->kind == Kind::Super ? instance->parent : instance;
        auto field = instance->bindings.find(name);
        if (field != instance->bindings.end()) {
            requireAccess(field->second, span);
            if (!field->second.initialized) throw Error(span, "Field has not been initialized: '" + name + "'", "state");
            return field->second.value;
        }
        if (instance->kind == Kind::Super) for (const auto &inherited : classFields(instance->origin)) if (inherited.first.name == name) {
            const auto &field = receiver->bindings.at(name);
            requireAccess(field, span);
            if (!field.initialized) throw Error(span, "Field has not been initialized: '" + name + "'", "state");
            return field.value;
        }
        Object *owner = nullptr; auto method = classBinding(instance->origin, name, &owner);
        if (!method || method->staticMember) {
            if (collection) return member(instance, name, span, true, target);
            throw Error(span, "Unknown instance member '" + name + "'", "name");
        }
        requireAccess(*method, span);
        if (target) { target->receiver = receiver; target->owner = owner; return method->value; }
        Object *bound = allocate(Kind::BoundMethod, span);
        bound->parent = receiver; bound->origin = object(method->value, span); bound->methodOwner = owner;
        bound->code = bound->origin->code; bound->text = instance->origin->text + "." + name;
        return bound;
    }
    if (instance->kind == Kind::Task) {
        if (name == "status") return text(taskStatus(instance->taskState), false, span);
        if (name == "done") return terminal(instance->taskState);
        if (name == "value") return instance->taskValue;
        if (name == "has_value") return instance->taskHasValue;
        if (name == "error") return instance->taskFailure ? caughtValue(instance->taskFailure) : Value{};
    }
    if (instance->kind == Kind::Record || instance->kind == Kind::Module || instance->kind == Kind::EnumType) {
        auto field = instance->bindings.find(name);
        if (field == instance->bindings.end()) throw Error(span, "Unknown member '" + name + "'", "name");
        return field->second.value;
    }
    auto methods = nativeMethods.find(instance->kind);
    if (methods != nativeMethods.end()) {
        auto found = methods->second.find(name);
        if (found != methods->second.end()) {
            if (target) { target->receiver = instance; return found->second; }
            Object *bound = allocate(Kind::BoundMethod, span); bound->parent = instance; bound->origin = found->second;
            bound->code = found->second->code; bound->text = found->second->text; return bound;
        }
    }
    throw Error(span, "Unknown member '" + name + "' on " + typeOf(value), "name");
}
Value Runtime::evaluate(const Ast &node, Object *environment) {
    requireRunning();
    Object *previousAccess = accessClass; accessClass = nullptr;
    for (Object *scope = environment; scope; scope = scope->parent) if (scope->accessClass) { accessClass = scope->accessClass; break; }
    struct RestoreAccess { Object *&current; Object *previous; ~RestoreAccess() { current = previous; } } restoreAccess{accessClass, previousAccess};
    if (!node) return {};
    auto code = compileExpression(node, config.optimizationLevel);
    Value result = executeBytecode(*code, environment, {}, node->span); roots.push_back(result); return result;
}

Value Runtime::call(Value callable, const std::vector<Argument> &arguments, const Span &span) {
    return call(callable, arguments, span, nullptr, nullptr);
}
Value Runtime::call(Value callable, const std::vector<Argument> &arguments, const Span &span, Object *receiver, Object *methodOwner) {
    requireRunning();
    Object *function = object(callable, span);
    if (++callDepth > 256) { callDepth--; throw Error(span, "Call depth exceeded", "limit"); }
    struct DepthGuard { size_t &depth; ~DepthGuard() { depth--; } } depthGuard{callDepth};
    size_t rootStart = roots.size();
    struct CallRoots { std::vector<Value> &roots; size_t start; ~CallRoots() { roots.resize(start); } } callRoots{roots, rootStart};
    roots.push_back(callable); for (const auto &argument : arguments) roots.push_back(argument.value);
    if (receiver) roots.push_back(receiver);
    if (methodOwner) roots.push_back(methodOwner);
    else methodOwner = function->methodOwner;
    if (function->kind == Kind::BoundMethod) { receiver = function->parent; function = function->origin; }
    if (function->kind == Kind::Native) {
        std::optional<ProfileScope> profile;
        if (config.profiler) profile.emplace(config.profiler.get(), "native:" + function->text, "native", span, currentTask ? currentTask->debugTaskId : 0, true);
        if (!function->nativeMethod) return function->native(arguments, span);
        if (!receiver) throw Error(span, "Native method requires a receiver", "type");
        return callNativeMethod(receiver, *function->nativeMethod, arguments, span);
    }
    if (function->kind == Kind::Constructor) {
        auto values = positional(arguments, span);
        if (values.size() != function->code->names.size()) throw Error(span, "Enum payload arity mismatch", "call");
        Object *result = allocate(Kind::Variant, span); result->origin = function->origin; result->text = function->text;
        for (size_t index = 0; index < values.size(); index++) requireType(values[index], resolveContract(function->code->names[index], function->origin->parent, span), span);
        result->items = values; return result;
    }
    if (function->kind != Kind::Function && function->kind != Kind::RecordType && function->kind != Kind::ClassType) throw Error(span, "Value is not callable", "type");
    if (function->kind == Kind::Function && !receiver && !config.debug) {
        if (!function->bytecodeAttempted) {
            std::optional<ProfileScope> profile;
            if (config.profiler) profile.emplace(config.profiler.get(), "Compile", "vm", span);
            function->bytecode = compileBytecode(function->code, config.optimizationLevel, false, !config.enforceContracts); function->bytecodeAttempted = true;
        }
        if (function->bytecode) {
            if (config.profiler) config.profiler->bytecodeCalls++;
            FrameScope frame(*this, function->code->text.empty() ? "<anonymous>" : function->code->text.c_str(), function->code->span, function->parent);
            try {
                Value result = executeBytecode(*function->bytecode, function->parent, arguments, span, function->bytecode->captures ? &function->items : nullptr); roots.push_back(result);
                requireType(result, resolveContract(function->code->type, function->parent, span), span); return result;
            } catch (...) { captureFailure(std::current_exception()); throw; }
        }
    }
    if (config.profiler && function->kind == Kind::Function) config.profiler->lexicalCalls++;
    if (function->kind == Kind::ClassType) {
        if (function->builtinCollection) return function->native(arguments, span);
        Binding *initializer = classBinding(function, "init");
        bool custom = initializer && !object(initializer->value, span)->nativeMethod && (object(initializer->value, span)->code->constructor || !object(initializer->value, span)->code->parameters.empty());
        if (custom || function->collectionKind != Kind::Instance) {
            Object *record = allocate(function->collectionKind, span); record->origin = function; roots.push_back(record);
            Object *defaults = allocate(Kind::Environment, span); defaults->parent = function->parent; roots.push_back(defaults);
            for (const auto &entry : classFields(function)) {
                const auto &field = entry.first; Contract contract = resolveContract(field.type, entry.second->parent, span);
                Binding binding{{}, field.mutableBinding, field.type, contract, false, false};
                binding.visibility = field.visibility; binding.declaringClass = entry.second;
                if (field.defaultValue) {
                    Object *scope = allocate(Kind::Environment, span); roots.push_back(scope); scope->parent = entry.second->classScope ? entry.second->classScope : entry.second->parent; scope->bindings = defaults->bindings;
                    binding.value = evaluate(field.defaultValue, scope); requireType(binding.value, contract, span); binding.initialized = true;
                }
                else if (!custom) throw Error(span, "Collection subclass fields require defaults or a custom init", "type");
                record->bindings.emplace(field.name, binding); defaults->bindings.emplace(field.name, binding);
            }
            constructing.push_back(record);
            struct Finish { std::vector<Object *> &stack; ~Finish() { stack.pop_back(); } } finish{constructing};
            Value result = initializer ? call(member(record, "init", span), arguments, span) : Value{};
            if (!std::holds_alternative<std::monostate>(result)) throw Error(span, "init must return Null", "type");
            for (const auto &field : record->bindings) if (!field.second.initialized) throw Error(span, "Constructor did not initialize field '" + field.first + "'", "state");
            return record;
        }
    }
    Object *environment = allocate(Kind::Environment, span); environment->parent = function->parent; roots.push_back(environment);
    if (receiver) define(environment, "self", receiver, false, "", span);
    if (receiver && methodOwner && methodOwner->baseClass) {
        Object *base = allocate(Kind::Super, span); roots.push_back(base);
        base->parent = receiver; base->origin = methodOwner->baseClass;
        define(environment, "super", base, false, "", span);
    }
    Contract returnContract = resolveContract(function->code->type, function->parent, span);
    auto parameters = function->code->parameters;
    std::vector<Object *> parameterOwners(parameters.size(), function);
    if (function->kind == Kind::ClassType) {
        parameters.clear(); parameterOwners.clear();
        for (const auto &field : classFields(function)) { parameters.push_back(field.first); parameterOwners.push_back(field.second); }
    }
    std::vector<std::optional<Value>> bound(parameters.size());
    std::vector<Value> rest;
    size_t positionalIndex = 0;
    auto suppliedArguments = arguments;
    if (function == errorType) for (size_t position = 0; position < suppliedArguments.size(); position++) {
        auto &argument = suppliedArguments[position];
        if ((argument.name == "kind" || (position == 0 && argument.name.empty())) && std::holds_alternative<Object *>(argument.value) && object(argument.value, span)->kind == Kind::String)
            argument.value = errorKind(object(argument.value, span)->text, span);
    }
    for (const auto &argument : suppliedArguments) {
        if (argument.name.empty()) {
            if (positionalIndex < parameters.size() && !parameters[positionalIndex].rest) bound[positionalIndex++] = argument.value;
            else if (!parameters.empty() && parameters.back().rest) rest.push_back(argument.value);
            else throw Error(span, "Too many positional arguments", "call");
        } else {
            auto parameter = std::find_if(parameters.begin(), parameters.end(), [&](const Parameter &item) { return item.name == argument.name; });
            if (parameter == parameters.end() || parameter->rest) throw Error(span, "Unknown named argument '" + argument.name + "'", "call");
            size_t index = static_cast<size_t>(parameter - parameters.begin());
            if (bound[index]) throw Error(span, "Argument supplied more than once: '" + argument.name + "'", "call");
            bound[index] = argument.value;
        }
    }
    for (size_t index = 0; index < parameters.size(); index++) {
        const auto &parameter = parameters[index]; Value value;
        if (parameter.rest) { Object *list = allocate(Kind::List, span); list->items = rest; value = list; }
        else if (bound[index]) value = *bound[index];
        else if (parameter.defaultValue) {
            Object *scope = environment;
            if (function->kind == Kind::ClassType || parameterOwners[index] != function) {
                scope = allocate(Kind::Environment, span); roots.push_back(scope);
                scope->parent = parameterOwners[index]->classScope ? parameterOwners[index]->classScope : parameterOwners[index]->parent; scope->bindings = environment->bindings;
            }
            value = evaluate(parameter.defaultValue, scope);
        }
        else throw Error(span, "Missing argument '" + parameter.name + "'", "call");
        define(environment, parameter.name, value, false, parameter.type, span, resolveContract(parameter.type, parameterOwners[index]->parent, span));
    }
    if (function->kind == Kind::RecordType || function->kind == Kind::ClassType) {
        Object *record = allocate(function->kind == Kind::ClassType ? Kind::Instance : Kind::Record, span);
        record->origin = function; record->bindings = environment->bindings;
        if (function->kind == Kind::ClassType)
            for (size_t position = 0; position < parameters.size(); position++) {
                auto &binding = record->bindings.at(parameters[position].name); binding.mutableBinding = parameters[position].mutableBinding;
                binding.visibility = parameters[position].visibility; binding.declaringClass = parameterOwners[position];
            }
        if (function->kind == Kind::ClassType && classBinding(function, "init")) {
            roots.push_back(record);
            Value initialized = call(member(record, "init", span), {}, span);
            if (!std::holds_alternative<std::monostate>(initialized)) throw Error(span, "init must return Null", "type");
        }
        return record;
    }
    if (hasYield(function->code->children[0])) {
        Object *generator = allocate(Kind::Generator, span); generator->parent = environment; generator->code = function->code;
        generator->debugFrameId = ++nextDebugFrame;
        compileGenerator(generator, function->code->children[0]); generator->scopes.push_back(environment); return generator;
    }
    Value result;
    std::string frameName = receiver ? receiver->origin->text + "." + function->code->text : function->code->text;
    FrameScope frame(*this, frameName.empty() ? "<anonymous>" : frameName.c_str(), function->code->span, environment);
    try {
        debugPoint(function->code, environment, "call");
        try { executeBlock(function->code->children[0], environment, false, &result, false, true, function); }
        catch (const Returned &returned) { result = returned.value; }
        catch (const Broken &) { throw Error(span, "break cannot cross a function boundary", "syntax"); }
        catch (const Continued &) { throw Error(span, "continue cannot cross a function boundary", "syntax"); }
        roots.push_back(result); requireType(result, returnContract, span); return result;
    } catch (...) { captureFailure(std::current_exception()); throw; }
}

Value Runtime::invoke(Value function, const std::vector<Argument> &arguments) {
    requireRunning();
    steps = 0; size_t scratch = roots.size();
    try { Value result = call(function, arguments, hostSpan()); roots.resize(scratch); return result; }
    catch (...) { roots.resize(scratch); throw; }
}

bool Runtime::resume(Value iterable, Value &result, size_t &cursor) {
    requireRunning();
    steps = 0; size_t scratch = roots.size();
    try { bool available = next(iterable, result, cursor, hostSpan()); roots.resize(scratch); return available; }
    catch (const Thrown &thrown) { std::string message = display(thrown.value); roots.resize(scratch); Error error(thrown.span, message, "throw"); error.trace = thrown.trace; throw error; }
    catch (...) { roots.resize(scratch); throw; }
}

Value Runtime::spawn(Value generator) { requireRunning(); return spawnTask(generator, hostSpan()); }
size_t Runtime::pump(size_t turns) {
    requireRunning();
    steps = 0; size_t scratch = roots.size();
    try { size_t count = pumpTasks(turns, hostSpan()); roots.resize(scratch); return count; }
    catch (const Thrown &thrown) { std::string message = display(thrown.value); roots.resize(scratch); Error error(thrown.span, message, "throw"); error.trace = thrown.trace; throw error; }
    catch (...) { roots.resize(scratch); throw; }
}
void Runtime::cancel(Value task) {
    requireRunning();
    steps = 0; size_t scratch = roots.size();
    try { cancelTask(task, hostSpan()); roots.resize(scratch); }
    catch (const Thrown &thrown) { std::string message = display(thrown.value); roots.resize(scratch); Error error(thrown.span, message, "throw"); error.trace = thrown.trace; throw error; }
    catch (...) { roots.resize(scratch); throw; }
}

void Runtime::restoreTerminal() {
    requireRunning();
    if (!terminalOpen) return;
    config.terminalMode(false); terminalOpen = false;
}
void Runtime::cancelAll() {
    requireRunning();
    if (pumping || currentTask) throw Error(hostSpan(), "Cannot shut down tasks during execution", "state");
    steps = 0; size_t scratch = roots.size(); std::exception_ptr failure;
    try {
        runCleanup([&] {
            while (!activeTasks.empty()) {
                Object *task = activeTasks.front();
                while (task->taskParent) task = task->taskParent;
                roots.push_back(task);
                try { cancelTask(task, hostSpan()); }
                catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
            }
        });
        if (failure) std::rethrow_exception(failure);
        roots.resize(scratch);
    } catch (const Thrown &thrown) {
        std::string message = display(thrown.value); roots.resize(scratch); Error error(thrown.span, message, "throw"); error.trace = thrown.trace; throw error;
    } catch (...) { roots.resize(scratch); throw; }
}

Value Runtime::spawnTask(Value value, const Span &span) {
    Object *generator = object(value, span);
    if (generator->kind != Kind::Generator) throw Error(span, "spawn expects Generator", "type");
    if (generator->taskOwner || generator->finished || generator->instruction || generator->running)
        throw Error(span, "spawn requires a fresh unowned generator", "state");
    if (cleanupDepth) throw Error(span, "Cannot spawn tasks during cleanup", "state");
    if (currentTask && (currentTask->taskExiting || taskCancelled(currentTask))) throw Error(span, "Cannot spawn tasks while exiting", "state");
    if (currentTask && currentTask->taskDepth >= 128) throw Error(span, "Task nesting limit exceeded", "limit");
    Object *task = allocate(Kind::Task, span); task->origin = generator;
    task->debugTaskId = ++nextDebugTask;
    task->taskParent = currentTask; task->taskDepth = currentTask ? currentTask->taskDepth + 1 : 1;
    if (currentTask) currentTask->children.push_back(task);
    activeTasks.push_back(task); readyTasks.push_back(task); generator->taskOwner = task; return task;
}

void Runtime::finishTask(Object *task, TaskState state, std::exception_ptr failure) {
    task->taskState = state; task->taskFailure = failure; task->taskCarried = exceptionValue(failure);
    if (task->taskParent) {
        auto &siblings = task->taskParent->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), task), siblings.end()); task->taskParent = nullptr;
    }
    if (task->origin) { task->origin->taskOwner = nullptr; task->origin = nullptr; }
    readyTasks.erase(std::remove(readyTasks.begin(), readyTasks.end(), task), readyTasks.end());
    activeTasks.erase(std::remove(activeTasks.begin(), activeTasks.end(), task), activeTasks.end());
}

void Runtime::cancelTask(Value value, const Span &span) {
    Object *task = object(value, span);
    if (task->kind != Kind::Task) throw Error(span, "cancel expects Task", "type");
    if (terminal(task->taskState)) return;
    task->cancelRequested = true;
    if (task->taskState == TaskState::Running || task->taskState == TaskState::Cancelling) return;
    for (Object *ancestor = currentTask; ancestor; ancestor = ancestor->taskParent) if (ancestor == task) return;
    task->taskState = TaskState::Cancelling; task->taskExiting = true; roots.push_back(task);
    Object *previous = currentTask; currentTask = task;
    std::exception_ptr failure;
    try {
        runCleanup([&] {
            cancelChildren(task, failure, span);
            try { closeGenerator(task->origin, span); }
            catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
        });
    } catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
    finishTask(task, failure ? TaskState::Failed : TaskState::Cancelled, failure); currentTask = previous;
    if (failure) std::rethrow_exception(failure);
}

void Runtime::cancelChildren(Object *task, std::exception_ptr &failure, const Span &span) {
    while (!task->children.empty()) {
        Object *child = task->children.back(); roots.push_back(child);
        try { cancelTask(child, span); }
        catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
    }
}

void Runtime::prepareTaskExit(Object *generator, std::exception_ptr &failure) {
    Object *task = generator->taskOwner;
    if (!task || task->taskExiting) return;
    task->taskExiting = true;
    runCleanup([&] { cancelChildren(task, failure, generator->code->span); });
}

size_t Runtime::pumpTasks(size_t turns, const Span &span) {
    if (pumping || currentTask) throw Error(span, "Task pumping cannot be reentered", "state");
    pumping = true;
    struct Guard { bool &pumping; Object *&current; ~Guard() { pumping = false; current = nullptr; } } guard{pumping, currentTask};
    size_t count = 0;
    while (count < turns && !readyTasks.empty()) {
        Object *task = readyTasks.front(); readyTasks.pop_front();
        if (task->cancelRequested) { cancelTask(task, span); count++; continue; }
        roots.push_back(task); currentTask = task; task->taskState = TaskState::Running;
        Value value; size_t cursor = 0;
        try {
            bool available = next(task->origin, value, cursor, span);
            if (available) { task->taskValue = value; task->taskHasValue = true; task->taskState = TaskState::Ready; readyTasks.push_back(task); }
            else finishTask(task, task->cancelRequested ? TaskState::Cancelled : TaskState::Done);
        } catch (...) {
            std::exception_ptr failure = std::current_exception(); bool cancelled = false;
            try { std::rethrow_exception(failure); }
            catch (const Error &error) { cancelled = taskCancelled(task) && error.category == "cancelled"; }
            catch (...) {}
            std::vector<Object *> ancestors;
            for (Object *ancestor = task->taskParent; ancestor; ancestor = ancestor->taskParent) {
                ancestors.push_back(ancestor); roots.push_back(ancestor);
            }
            finishTask(task, cancelled ? TaskState::Cancelled : TaskState::Failed, cancelled ? std::exception_ptr{} : failure);
            currentTask = nullptr;
            if (!cancelled) {
                if (!ancestors.empty()) {
                    try { cancelTask(ancestors.back(), span); }
                    catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
                    for (Object *ancestor : ancestors) finishTask(ancestor, TaskState::Failed, failure);
                }
                std::rethrow_exception(failure);
            }
        }
        currentTask = nullptr; count++;
    }
    return count;
}

DeferredCall Runtime::captureDeferred(const Ast &node, Object *environment) {
    Ast expression = node->children[0];
    if (expression->kind != "call") throw Error(node->span, "defer requires a call", "type");
    DeferredCall action{evaluate(expression->children[0], environment), {}, node->span};
    for (size_t index = 1; index < expression->children.size(); index++)
        action.arguments.push_back({expression->names[index - 1], evaluate(expression->children[index], environment)});
    return action;
}

void Runtime::leaveGeneratorScope(Object *generator) {
    std::exception_ptr failure;
    if (!generator->deferred.empty()) {
        auto actions = std::move(generator->deferred.back()); generator->deferred.pop_back();
        for (const auto &action : actions) {
            roots.push_back(action.function);
            for (const auto &argument : action.arguments) roots.push_back(argument.value);
        }
        runCleanup([&] {
            for (auto action = actions.rbegin(); action != actions.rend(); ++action) {
                try { call(action->function, action->arguments, action->span); }
                catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
            }
        });
    }
    generator->scopes.pop_back();
    if (failure) std::rethrow_exception(failure);
}

void Runtime::finishGenerator(Object *generator) {
    generator->finished = true;
    std::exception_ptr failure;
    while (!generator->scopes.empty()) {
        try { leaveGeneratorScope(generator); }
        catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
    }
    generator->iterations.clear();
    generator->handlers.clear();
    generator->parent = nullptr;
    if (failure) std::rethrow_exception(failure);
}

void Runtime::close(Value value) {
    requireRunning();
    steps = 0;
    try { closeGenerator(value, hostSpan()); }
    catch (const Thrown &thrown) { Error error(thrown.span, display(thrown.value), "throw"); error.trace = thrown.trace; throw error; }
}

void Runtime::closeGenerator(Value value, const Span &span) {
    Object *generator = object(value, span);
    if (generator->kind != Kind::Generator) throw Error(span, "close expects Generator", "type");
    if (generator->finished) return;
    if (generator->taskOwner && generator->taskOwner != currentTask) throw Error(span, "Task-owned generators must be cancelled through their task", "state");
    if (generator->running) throw Error(span, "Generator is already running", "state");
    size_t scratch = roots.size(); roots.push_back(value);
    generator->closing = true;
    try { Value ignored; size_t cursor = 0; next(value, ignored, cursor, span); generator->closing = false; roots.resize(scratch); }
    catch (...) { generator->closing = false; roots.resize(scratch); throw; }
}

void Runtime::runCleanup(const std::function<void()> &action) {
    if (!cleanupDepth) cleanupSteps = 0;
    cleanupDepth++;
    try { action(); cleanupDepth--; }
    catch (...) { cleanupDepth--; throw; }
}

Value Runtime::errorKind(const std::string &category, const Span &) {
    for (const auto &kind : errorKinds()) if (kind.second == category) return errorKindType->bindings.at(kind.first).value;
    return errorKindType->bindings.at("Application").value;
}
Value Runtime::caughtValue(std::exception_ptr failure) {
    try { std::rethrow_exception(failure); }
    catch (const Thrown &thrown) { return thrown.value; }
    catch (const Error &error) {
        Object *record = allocate(Kind::Record, error.span); roots.push_back(record); record->origin = errorType;
        record->bindings["kind"] = {errorKind(error.category, error.span), false, "Error.Kind"};
        record->bindings["message"] = {text(error.what(), false, error.span), false, "String"};
        record->bindings["file"] = {text(error.span.source->file, false, error.span), false, "String"};
        record->bindings["line"] = {static_cast<int64_t>(error.span.line), false, "Int"};
        record->bindings["column"] = {static_cast<int64_t>(error.span.column), false, "Int"};
        return record;
    }
}

void Runtime::routeGenerator(Object *generator, Instruction transfer, std::exception_ptr failure) {
    roots.push_back(exceptionValue(failure));
    bool canCatch = false;
    for (const auto &handler : generator->handlers)
        if (handler.phase == HandlerPhase::Body && catchable(failure) && !generator->closing) canCatch = true;
    if (transfer.op == Op::Return && (!failure || !canCatch)) prepareTaskExit(generator, failure);
    auto unwind = [&](size_t depth) {
        while (generator->scopes.size() > depth) {
            try { leaveGeneratorScope(generator); }
            catch (...) { preferFailure(failure, std::current_exception()); roots.push_back(exceptionValue(failure)); }
        }
    };
    while (!generator->handlers.empty()) {
        Handler &handler = generator->handlers.back();
        if (!failure && transfer.scopes > handler.boundary.scopes) {
            unwind(transfer.scopes);
            if (!failure) break;
        }
        unwind(handler.boundary.scopes);
        generator->iterations.resize(handler.boundary.iterations);
        if (handler.phase == HandlerPhase::Finally) {
            if (handler.failure) {
                std::exception_ptr pending = handler.failure; preferFailure(pending, failure); failure = pending;
                roots.push_back(exceptionValue(failure));
            }
            generator->handlers.pop_back(); continue;
        }
        if (failure && handler.phase == HandlerPhase::Body && catchable(failure) && !generator->closing) {
            handler.phase = HandlerPhase::Catch;
            try {
                Value value = caughtValue(failure);
                Object *scope = allocate(Kind::Environment, handler.boundary.node->span); scope->parent = generator->scopes.back();
                define(scope, handler.boundary.node->text, value, false, "", handler.boundary.node->span);
                generator->scopes.push_back(scope); generator->deferred.emplace_back();
                generator->instruction = handler.boundary.target; return;
            } catch (...) { failure = std::current_exception(); roots.push_back(exceptionValue(failure)); }
        }
        if (handler.boundary.finalizer) {
            handler.phase = HandlerPhase::Finally; handler.transfer = transfer; handler.failure = failure;
            handler.carried = exceptionValue(failure); generator->instruction = handler.boundary.finalizer; return;
        }
        generator->handlers.pop_back();
    }
    if (failure) prepareTaskExit(generator, failure);
    unwind(failure ? 0 : transfer.scopes);
    generator->iterations.resize(failure ? 0 : transfer.iterations);
    if (failure) { generator->finished = true; generator->parent = nullptr; std::rethrow_exception(failure); }
    if (transfer.op == Op::Return) finishGenerator(generator);
    else generator->instruction = transfer.target;
}

void Runtime::executeBlock(const Ast &node, Object *environment, bool createScope, Value *result, bool echoLast, bool allowReturn, Object *callable) {
    if (createScope && node->needsScope) { Object *scope = allocate(Kind::Environment, node->span); scope->parent = environment; environment = scope; }
    size_t rootStart = roots.size(); roots.push_back(environment);
    struct RootsGuard { std::vector<Value> &roots; size_t start; ~RootsGuard() { roots.resize(start); } } rootsGuard{roots, rootStart};
    Object *execution = allocate(Kind::Generator, node->span); roots.push_back(execution);
    execution->controlExecution = true; execution->controlAllowReturn = allowReturn; execution->parent = environment; execution->code = node;
    execution->scopes.push_back(environment); execution->deferred.emplace_back();
    if (callable && callable->controlCode) execution->controlCode = callable->controlCode;
    else {
        compileGenerator(execution, node, false, echoLast);
        execution->controlCode = std::make_shared<const std::vector<Instruction>>(std::move(execution->instructions));
        if (callable) callable->controlCode = execution->controlCode;
    }
    Value ignored; size_t cursor = 0;
    while (next(execution, ignored, cursor, node->span)) {}
    if (result) *result = execution->taskValue;
}
void Runtime::implement(const Ast &node, Object *environment) {
    Object *trait = resolveObjectPath(node->text, environment, node->span);
    Object *target = resolveObjectPath(node->names[0], environment, node->span);
    roots.push_back(trait); roots.push_back(target);
    if (trait->kind != Kind::TraitType || (target->kind != Kind::ClassType && target->kind != Kind::RecordType))
        throw Error(node->span, "impl requires a trait and a class or record", "type");
    if (std::find(target->traits.begin(), target->traits.end(), trait) != target->traits.end())
        throw Error(node->span, "Trait already implemented for class", "type");
    std::unordered_map<std::string, Ast> additions;
    std::unordered_map<std::string, Ast> defaults;
    for (const auto &method : node->children) {
        bool required = false;
        for (const auto &signature : trait->code->children) if (signature->text == method->text) required = true;
        if (!required) throw Error(method->span, "Unknown implementation method '" + method->text + "'", "type");
        if (target->bindings.count(method->text)) throw Error(method->span, "Implementation cannot replace an existing method", "mutability");
        for (const auto &field : target->code->parameters) if (field.name == method->text)
            throw Error(method->span, "Implementation method conflicts with a field", "type");
        additions.emplace(method->text, method);
    }
    for (const auto &signature : trait->code->children) {
        Ast method; Object *owner = environment;
        auto added = additions.find(signature->text);
        if (added != additions.end()) method = added->second;
        else {
            auto existing = classBinding(target, signature->text);
            if (!existing) {
                if (signature->kind != "function") throw Error(node->span, "Missing trait method '" + signature->text + "'", "type");
                for (const auto &field : target->code->parameters) if (field.name == signature->text)
                    throw Error(node->span, "Default method conflicts with a field", "type");
                method = signature; owner = trait->parent; defaults.emplace(signature->text, signature);
            } else {
                if (existing->staticMember || existing->visibility != Visibility::Public) throw Error(node->span, "Trait methods must be public instance methods", "type");
                Object *function = object(existing->value, node->span); method = function->code; owner = function->parent;
                if (signature->kind == "function" && function->origin && function->origin->kind == Kind::TraitType && function->origin != trait)
                    throw Error(node->span, "Conflicting trait defaults; define an explicit method", "type");
            }
        }
        if (method->parameters.size() != signature->parameters.size() || hasYield(method->children[0]) ||
            !sameContract(resolveContract(method->type, owner, node->span), resolveContract(signature->type, trait->parent, node->span)))
            throw Error(method->span, "Trait method signature mismatch: '" + signature->text + "'", "type");
        for (size_t index = 0; index < signature->parameters.size(); index++) {
            const auto &expected = signature->parameters[index]; const auto &actual = method->parameters[index];
            if (actual.rest || actual.name != expected.name || !sameContract(resolveContract(actual.type, owner, node->span), resolveContract(expected.type, trait->parent, node->span)))
                throw Error(method->span, "Trait parameter signature mismatch: '" + signature->text + "'", "type");
        }
    }
    std::unordered_map<std::string, Binding> staged;
    Object *implementationScope = environment;
    if (!node->children.empty()) { implementationScope = allocate(Kind::Environment, node->span); roots.push_back(implementationScope); implementationScope->parent = environment; implementationScope->accessClass = target; }
    for (const auto &method : node->children) {
        Binding binding{evaluate(method, implementationScope), false, "Function", {}}; binding.declaringClass = target;
        staged.emplace(method->text, binding);
    }
    for (const auto &entry : defaults) {
        Value value = evaluate(entry.second, trait->parent); object(value, node->span)->origin = trait;
        staged.emplace(entry.first, Binding{value, false, "Function", {}});
    }
    target->bindings.insert(staged.begin(), staged.end()); target->traits.push_back(trait);
}
void Runtime::executeDeclaration(const Ast &node, Object *environment) {
    if (node->kind == "record" || node->kind == "class") {
        Object *record = allocate(node->kind == "class" ? Kind::ClassType : Kind::RecordType, node->span); record->text = node->text; record->code = node; record->parent = environment;
        if (node->kind == "class") { record->baseClass = objectType; record->inheritanceDepth = 1; }
        roots.push_back(record);
        if (!node->baseClass.empty()) {
            Object *base = resolveObjectPath(node->baseClass, environment, node->span);
            if (base->kind != Kind::ClassType) throw Error(node->span, "Base must be a user class", "type");
            if (base->inheritanceDepth >= 128) throw Error(node->span, "Class inheritance depth exceeded", "limit");
            record->baseClass = base; record->inheritanceDepth = base->inheritanceDepth + 1; record->traits = base->traits;
            record->collectionKind = base->collectionKind;
            std::set<std::string> fields;
            for (const auto &field : classFields(base)) fields.insert(field.first.name);
            for (const auto &field : node->parameters) if (fields.count(field.name) || classBinding(base, field.name)) throw Error(node->span, "Inherited field/member cannot be redeclared", "type");
            for (const auto &entry : node->children) {
                auto inherited = classBinding(base, entry->text);
                if (fields.count(entry->text)) throw Error(entry->span, "Member conflicts with an inherited field", "type");
                if (!inherited) continue;
                if (inherited->visibility == Visibility::Private || static_cast<int>(entry->visibility) > static_cast<int>(inherited->visibility)) throw Error(entry->span, "Override cannot replace a private member or narrow visibility", "access");
                if (entry->kind != "function" || inherited->staticMember != entry->staticMember || !std::holds_alternative<Object *>(inherited->value) ||
                    (object(inherited->value, entry->span)->kind != Kind::Function && object(inherited->value, entry->span)->kind != Kind::Native))
                    throw Error(entry->span, "Inherited member cannot be redeclared with a different kind", "type");
                Object *previous = object(inherited->value, entry->span);
                if (entry->text == "init") continue;
                if (previous->nativeMethod) {
                    if (entry->parameters.size() != previous->code->parameters.size() || hasYield(entry->children[0])) throw Error(entry->span, "Native override arity mismatch", "type");
                    if (previous->code->type != "Any" && !entry->type.empty() && entry->type != previous->code->type) throw Error(entry->span, "Native override return mismatch", "type");
                    continue;
                }
                if (entry->parameters.size() != previous->code->parameters.size() || hasYield(entry->children[0]) != hasYield(previous->code->children[0]) ||
                    !sameContract(resolveContract(entry->type, environment, entry->span), resolveContract(previous->code->type, previous->parent, entry->span)))
                    throw Error(entry->span, "Override signature mismatch", "type");
                for (size_t position = 0; position < entry->parameters.size(); position++) {
                    const auto &actual = entry->parameters[position]; const auto &expected = previous->code->parameters[position];
                    if (actual.name != expected.name || actual.rest != expected.rest || bool(actual.defaultValue) != bool(expected.defaultValue) ||
                        !sameContract(resolveContract(actual.type, environment, entry->span), resolveContract(expected.type, previous->parent, entry->span))) throw Error(entry->span, "Override parameter signature mismatch", "type");
                }
            }
        }
        Object *classScope = allocate(Kind::Environment, node->span); roots.push_back(classScope);
        classScope->parent = environment; classScope->accessClass = record; record->classScope = classScope;
        define(environment, node->text, record, false, "", node->span);
        {
            try {
                for (const auto &method : node->children) if (method->kind == "function") {
                    const auto *operatorSpec = namedOperatorHook(method->text);
                    if (operatorSpec || method->text == "toString") {
                        size_t count = operatorSpec ? operatorSpec->arguments : 0;
                        if (method->staticMember || method->visibility != Visibility::Public || method->parameters.size() != count || hasYield(method->children[0]))
                            throw Error(method->span, "Conversion/operator hook must be public, synchronous and have the expected arity", "type");
                        for (const auto &parameter : method->parameters) if (parameter.rest) throw Error(method->span, "Operator hooks cannot use rest parameters", "type");
                        if (method->text == "toString" && !method->type.empty() && method->type != "String" && method->type != "Any") throw Error(method->span, "toString must return String", "type");
                        if (operatorSpec && operatorSpec->predicate && !method->type.empty() && method->type != "Bool" && method->type != "Any") throw Error(method->span, "Comparison hook must return Bool", "type");
                        if (method->text == "__setitem__" && !method->type.empty() && method->type != "Null" && method->type != "Any") throw Error(method->span, "__setitem__ must return Null", "type");
                    }
                    Value function = evaluate(method, classScope);
                    define(record, method->text, function, false, "", method->span);
                    record->bindings.at(method->text).staticMember = method->staticMember;
                    record->bindings.at(method->text).visibility = method->visibility; record->bindings.at(method->text).declaringClass = record;
                    if (method->staticMember) object(function, method->span)->origin = record;
                }
                for (const auto &field : node->children) if (field->kind == "binding") {
                    Value value = evaluate(field->children[0], classScope);
                    define(record, field->text, value, field->mutableBinding, field->type, field->span);
                    record->bindings.at(field->text).staticMember = true;
                    record->bindings.at(field->text).visibility = field->visibility; record->bindings.at(field->text).declaringClass = record;
                }
                auto initializer = record->bindings.find("init");
                if (node->kind == "class" && initializer != record->bindings.end()) {
                    Ast hook = object(initializer->second.value, node->span)->code;
                    if ((!hook->type.empty() && hook->type != "Null") || hasYield(hook->children[0]))
                        throw Error(hook->span, "init must be synchronous and return Null", "type");
                }
                for (const auto &trait : node->names) {
                    Ast implementation = std::make_shared<Node>(); implementation->kind = "impl";
                    implementation->text = trait; implementation->names = {node->text}; implementation->span = node->span;
                    implement(implementation, environment);
                }
            } catch (...) { environment->bindings.erase(node->text); throw; }
        }
    } else if (node->kind == "trait") {
        for (const auto &signature : node->children) if (signature->kind == "function" && hasYield(signature->children[0]))
            throw Error(signature->span, "Trait defaults must be synchronous", "type");
        Object *trait = allocate(Kind::TraitType, node->span); trait->text = node->text; trait->code = node; trait->parent = environment;
        define(environment, node->text, trait, false, "", node->span);
    } else if (node->kind == "impl") implement(node, environment);
    else if (node->kind == "enum") {
        Object *enumeration = allocate(Kind::EnumType, node->span); enumeration->text = node->text; enumeration->code = node; enumeration->parent = environment;
        for (const Ast &variant : node->children) {
            if (!config.enforceContracts && variant->names.empty()) {
                Object *value = allocate(Kind::Variant, variant->span); value->origin = enumeration; value->text = variant->text;
                define(enumeration, variant->text, value, false, "", variant->span);
                continue;
            }
            Object *constructor = allocate(Kind::Constructor, variant->span); constructor->origin = enumeration; constructor->text = variant->text; constructor->code = variant;
            define(enumeration, variant->text, constructor, false, "", variant->span);
        }
        define(environment, node->text, enumeration, false, "", node->span);
    } else if (node->kind == "import") define(environment, node->names[0], loadModule(node->text, node->span), false, "", node->span);
    else if (node->kind == "from_import") {
        Object *module = loadModule(node->text, node->span); std::vector<std::pair<std::string, Value>> aliases;
        std::set<std::string> seen;
        for (size_t index = 0; index < node->names.size(); index += 2) {
            const std::string &name = node->names[index], &alias = node->names[index + 1];
            if (environment->bindings.count(alias) || !seen.insert(alias).second) throw Error(node->span, "Duplicate import alias '" + alias + "'", "name");
            auto found = module->bindings.find(name);
            if (found == module->bindings.end()) throw Error(node->span, "Member is not exported: '" + name + "'", "import");
            aliases.push_back({alias, found->second.value});
        }
        for (const auto &alias : aliases) define(environment, alias.first, alias.second, false, "", node->span);
    }
    else throw Error(node->span, "Unsupported statement '" + node->kind + "'", "internal");
}

Object *Runtime::loadModule(const std::string &name, const Span &span) {
    auto builtin = prelude->bindings.find(name);
    if (builtin != prelude->bindings.end() && std::holds_alternative<Object *>(builtin->second.value)) {
        Object *module = std::get<Object *>(builtin->second.value);
        if (module->kind == Kind::Module) return module;
    }
    auto cached = modules.find(name); if (cached != modules.end()) return cached->second;
    Ast program = config.resolveProgram ? config.resolveProgram(name) : nullptr;
    if (!program) {
        if (!config.resolveModule && !config.resolveSource) throw Error(span, "No host resolver for module '" + name + "'", "import");
        std::shared_ptr<const Source> source;
        try {
            source = config.resolveSource ? config.resolveSource(name) :
                std::make_shared<Source>(Source{name + ".k3", config.resolveModule(name)});
        }
        catch (const std::exception &error) { throw Error(span, "Module resolution failed: " + std::string(error.what()), "import"); }
        if (!source || source->text.empty()) throw Error(span, "Module not provided: '" + name + "'", "import");
        program = parse(source);
    }
    Object *environment = allocate(Kind::Environment, span); environment->parent = prelude;
    Object *module = allocate(Kind::Module, span); module->text = name; modules[name] = module; module->parent = environment;
    FrameScope frame(*this, "<module>", program->span, environment);
    try {
        executeBlock(program, environment, false);
        for (const Ast &statement : program->children) if (statement->exported) {
            Binding binding = lookup(environment, statement->text, statement->span); binding.mutableBinding = false;
            module->bindings[statement->text] = binding;
        }
    } catch (...) { modules.erase(name); throw; }
    return module;
}

void Runtime::compileGenerator(Object *generator, const Ast &body, bool enterRoot, bool echoLast) {
    auto &code = generator->instructions;
    struct Loop { size_t begin; size_t scopes; size_t iterations; std::vector<size_t> breaks; std::vector<size_t> continues = {}; };
    std::vector<Loop> loops;
    size_t scopeDepth = 1;
    size_t iterationDepth = 0;
    std::function<void(const Ast &)> emit = [&](const Ast &node) {
        if (node->kind == "block") {
            if (node == body && !enterRoot) {
                for (size_t index = 0; index < node->children.size(); index++) {
                    const auto &child = node->children[index];
                    if (echoLast && index + 1 == node->children.size() && child->kind == "expression") { code.push_back({Op::Bytecode, child}); code.back().result = true; }
                    else emit(child);
                }
                code.push_back({Op::Complete, node}); return;
            }
            code.push_back({Op::Enter, node, 0}); scopeDepth++;
            for (const Ast &child : node->children) emit(child);
            code.push_back({node == body ? Op::Complete : Op::Leave, node, 0}); scopeDepth--;
        }
        else if (node->kind == "yield") code.push_back({Op::Yield, node, 0});
        else if (node->kind == "return") code.push_back({Op::Return, node, 0});
        else if (node->kind == "if") {
            size_t branch = code.size(); code.push_back({Op::Branch, node->children[0], 0}); code.back().debugNode = node; emit(node->children[1]);
            size_t jump = code.size(); code.push_back({Op::Jump, node, 0}); code[branch].target = code.size();
            if (node->children.size() == 3) emit(node->children[2]); code[jump].target = code.size();
        } else if (node->kind == "while") {
            size_t begin = code.size(); code.push_back({Op::Branch, node->children[0], 0}); code.back().debugNode = node;
            loops.push_back({begin, scopeDepth, iterationDepth, {}}); emit(node->children[1]);
            code.push_back({Op::Jump, node, begin}); code[begin].target = code.size();
            for (size_t transfer : loops.back().breaks) code[transfer].target = code.size();
            loops.pop_back();
        } else if (node->kind == "for") {
            code.push_back({Op::Iterate, node, 0}); iterationDepth++;
            size_t begin = code.size(); code.push_back({Op::Next, node, 0});
            loops.push_back({begin, scopeDepth, iterationDepth, {}}); scopeDepth++;
            emit(node->children[1]); code.push_back({Op::Leave, node, 0}); scopeDepth--;
            code.push_back({Op::Jump, node, begin}); code[begin].target = code.size();
            for (size_t transfer : loops.back().breaks) {
                code[transfer].target = code.size(); code[transfer].iterations = iterationDepth - 1;
            }
            loops.pop_back(); iterationDepth--;
        } else if (node->kind == "for_c") {
            code.push_back({Op::Enter, node}); scopeDepth++;
            if (node->children[0]) emit(node->children[0]);
            size_t begin = code.size(); code.push_back({Op::Branch, node->children[1]}); code.back().debugNode = node; loops.push_back({begin, scopeDepth, iterationDepth, {}});
            emit(node->children[3]); size_t increment = code.size();
            for (size_t transfer : loops.back().continues) code[transfer].target = increment;
            if (node->children[2]) emit(node->children[2]);
            code.push_back({Op::Jump, node, begin}); code[begin].target = code.size();
            for (size_t transfer : loops.back().breaks) code[transfer].target = code.size();
            loops.pop_back(); code.push_back({Op::Leave, node}); scopeDepth--;
        } else if (node->kind == "defer") code.push_back({Op::Defer, node, 0});
        else if (node->kind == "break" || node->kind == "continue") {
            if (loops.empty()) throw Error(node->span, node->kind + " outside loop", "syntax");
            Loop &loop = loops.back();
            size_t transfer = code.size(); code.push_back({Op::Transfer, node, loop.begin, loop.scopes, loop.iterations});
            if (node->kind == "break") loop.breaks.push_back(transfer);
            else loop.continues.push_back(transfer);
        } else if (node->kind == "try") {
            size_t boundary = code.size(); code.push_back({Op::Try, node, 0, scopeDepth, iterationDepth});
            emit(node->children[0]); code.push_back({Op::EndTry, node});
            code[boundary].target = code.size(); scopeDepth++;
            emit(node->children[1]); code.push_back({Op::Leave, node}); scopeDepth--;
            code.push_back({Op::EndTry, node});
            if (node->children.size() == 3) {
                code[boundary].finalizer = code.size(); emit(node->children[2]); code.push_back({Op::EndFinally, node});
            }
            code[boundary].end = code.size();
        }
        else code.push_back({Op::Bytecode, node, 0});
    };
    emit(body);
    for (auto &instruction : code) {
        if (instruction.op == Op::Bytecode) instruction.bytecode = instruction.result ? compileExpression(instruction.node->children[0], config.optimizationLevel) : compileStatement(instruction.node, config.optimizationLevel);
        else if (instruction.op == Op::Branch) instruction.bytecode = compileExpression(instruction.node, config.optimizationLevel);
        else if (instruction.op == Op::Iterate) instruction.bytecode = compileExpression(instruction.node->children[0], config.optimizationLevel);
        else if ((instruction.op == Op::Yield || instruction.op == Op::Return) && !instruction.node->children.empty()) instruction.bytecode = compileExpression(instruction.node->children[0], config.optimizationLevel);
    }
}
bool Runtime::next(Value iterable, Value &result, size_t &cursor, const Span &span) {
    requireRunning();
    Object *source = object(iterable, span); roots.push_back(iterable);
    if (source->kind == Kind::Generator) {
        if (source->finished) return false;
        if (source->taskOwner && source->taskOwner != currentTask) throw Error(span, "Task-owned generators can only be resumed by their scheduler", "state");
        if (source->running) throw Error(span, "Generator is already running", "state");
        std::optional<FrameScope> frame;
        if (!source->controlExecution) frame.emplace(*this, source->code->text.empty() ? "<generator>" : source->code->text.c_str(), source->code->span, source->scopes.back(), source);
        source->running = true;
        bool cleanupActive = false;
        struct Guard {
            Object *source; size_t &depth; bool &active;
            ~Guard() { source->running = false; if (active) depth--; }
        } guard{source, cleanupDepth, cleanupActive};
        auto cleanupMode = [&] {
            if (!cleanupActive) { if (!cleanupDepth) cleanupSteps = 0; cleanupDepth++; cleanupActive = true; }
        };
        if (source->deferred.empty()) source->deferred.resize(source->scopes.size());
        try {
        if (source->closing) {
            cleanupMode(); bool inFinally = false;
            for (auto &handler : source->handlers) if (handler.phase == HandlerPhase::Finally) {
                handler.transfer = {Op::Return, source->code}; inFinally = true;
            }
            if (!inFinally) routeGenerator(source, {Op::Return, source->code});
        }
        const auto &instructions = source->controlCode ? *source->controlCode : source->instructions;
        while (!source->finished && source->instruction < instructions.size()) {
            bool forced = source->closing;
            for (const auto &handler : source->handlers)
                if (handler.failure && !catchable(handler.failure)) forced = true;
            if (forced) cleanupMode();
            const Instruction &instruction = instructions[source->instruction++];
            size_t scratch = roots.size();
            try {
            tick(instruction.node->span);
            Object *environment = source->scopes.back();
            if (instruction.op == Op::Defer || instruction.op == Op::Branch || instruction.op == Op::Transfer ||
                instruction.op == Op::Try || instruction.op == Op::Iterate || instruction.op == Op::Yield || instruction.op == Op::Return)
                debugPoint(instruction.debugNode ? instruction.debugNode : instruction.node, environment, instruction.debugNode ? instruction.debugNode->kind : instruction.op == Op::Branch ? "condition" : instruction.node->kind);
            switch (instruction.op) {
            case Op::Enter: {
                Object *scope = allocate(Kind::Environment, span); scope->parent = environment;
                source->scopes.push_back(scope); source->deferred.emplace_back(); break;
            }
            case Op::Leave: leaveGeneratorScope(source); break;
            case Op::Defer: source->deferred.back().push_back(captureDeferred(instruction.node, environment)); break;
            case Op::Bytecode: {
                debugPoint(instruction.node, environment);
                Value value = executeBytecode(*instruction.bytecode, environment, {}, instruction.node->span);
                if (instruction.result) source->taskValue = value;
                break;
            }
            case Op::Branch: if (!boolean(executeBytecode(*instruction.bytecode, environment, {}, instruction.node->span), instruction.node->span)) source->instruction = instruction.target; break;
            case Op::Jump: source->instruction = instruction.target; break;
            case Op::Transfer: routeGenerator(source, instruction); break;
            case Op::Try: source->handlers.push_back({instruction, HandlerPhase::Body, {Op::Return, {}}, {}, {}}); break;
            case Op::EndTry: {
                const auto &boundary = source->handlers.back().boundary;
                routeGenerator(source, {Op::Transfer, instruction.node, boundary.end, boundary.scopes, boundary.iterations}); break;
            }
            case Op::EndFinally: {
                Handler handler = source->handlers.back(); source->handlers.pop_back();
                routeGenerator(source, handler.transfer, handler.failure); break;
            }
            case Op::Iterate: source->iterations.push_back({executeBytecode(*instruction.bytecode, environment, {}, instruction.node->span), 0}); break;
            case Op::Next: {
                Value item; auto &iteration = source->iterations.back();
                if (iteration.started) debugPoint(instruction.node, environment);
                iteration.started = true;
                if (!next(iteration.source, item, iteration.index, span)) { source->iterations.pop_back(); source->instruction = instruction.target; }
                else {
                    Object *scope = allocate(Kind::Environment, span); scope->parent = environment;
                    define(scope, instruction.node->text, item, false, "", span); source->scopes.push_back(scope); source->deferred.emplace_back();
                }
                break;
            }
            case Op::Yield:
                result = instruction.bytecode ? executeBytecode(*instruction.bytecode, environment, {}, instruction.node->span) : Value{};
                if (!forced) { roots.resize(scratch); return true; } break;
            case Op::Return:
                source->taskValue = instruction.bytecode ? executeBytecode(*instruction.bytecode, environment, {}, instruction.node->span) : Value{};
                if (source->controlExecution && !source->controlAllowReturn) throw Returned{source->taskValue};
                routeGenerator(source, instruction); break;
            case Op::Complete: routeGenerator(source, {Op::Return, instruction.node}); break;
            }
            } catch (...) {
                std::exception_ptr failure = std::current_exception();
                captureFailure(failure);
                if (!catchable(failure)) cleanupMode();
                routeGenerator(source, {Op::Return, instruction.node}, failure);
            }
            roots.resize(scratch);
            collectionSafePoint();
        }
        finishGenerator(source); return false;
        } catch (...) {
            std::exception_ptr failure = std::current_exception();
            captureFailure(failure);
            roots.push_back(exceptionValue(failure));
            try { runCleanup([&] { finishGenerator(source); }); } catch (...) {}
            std::rethrow_exception(failure);
        }
    }
    if (source->kind == Kind::List || source->kind == Kind::Tuple || source->kind == Kind::Set) {
        if (cursor >= source->items.size()) return false;
        result = source->items[cursor++]; return true;
    }
    if (source->kind == Kind::String) {
        auto offsets = utf8Offsets(source->text, span); if (cursor + 1 >= offsets.size()) return false;
        result = text(source->text.substr(offsets[cursor], offsets[cursor + 1] - offsets[cursor]), false, span); cursor++; return true;
    }
    if (source->kind == Kind::Bytes) {
        if (cursor >= source->text.size()) return false;
        result = static_cast<int64_t>(static_cast<unsigned char>(source->text[cursor++])); return true;
    }
    throw Error(span, "Value is not iterable", "type");
}

void Runtime::mark(Value value) {
    if (!std::holds_alternative<Object *>(value)) return;
    std::vector<Object *> pending;
    auto enqueue = [&](Value candidate) {
        if (!std::holds_alternative<Object *>(candidate)) return;
        Object *instance = std::get<Object *>(candidate);
        if (instance && !instance->marked) { instance->marked = true; pending.push_back(instance); }
    };
    enqueue(value);
    std::function<void(const Contract &)> contractRoots = [&](const Contract &contract) {
        if (!contract) return;
        if (contract->nominal) enqueue(contract->nominal);
        contractRoots(contract->element);
        contractRoots(contract->key);
        for (const auto &part : contract->all) contractRoots(part);
    };
    while (!pending.empty()) {
        Object *instance = pending.back(); pending.pop_back();
        if (instance->parent) enqueue(instance->parent);
        if (instance->origin) enqueue(instance->origin);
        if (instance->baseClass) enqueue(instance->baseClass);
        if (instance->methodOwner) enqueue(instance->methodOwner);
        if (instance->accessClass) enqueue(instance->accessClass);
        if (instance->classScope) enqueue(instance->classScope);
        contractRoots(instance->elementContract);
        contractRoots(instance->keyContract);
        if (instance->taskOwner) enqueue(instance->taskOwner);
        if (instance->taskParent) enqueue(instance->taskParent);
        for (Object *child : instance->children) enqueue(child);
        for (Object *trait : instance->traits) enqueue(trait);
        enqueue(instance->taskValue); enqueue(instance->taskCarried);
        for (Value item : instance->items) enqueue(item);
        for (const auto &entry : instance->entries) { enqueue(entry.first); enqueue(entry.second); }
        for (const auto &binding : instance->bindings) { enqueue(binding.second.value); contractRoots(binding.second.contract); if (binding.second.declaringClass) enqueue(binding.second.declaringClass); }
        for (Object *scope : instance->scopes) enqueue(scope);
        for (const auto &iteration : instance->iterations) enqueue(iteration.source);
        for (const auto &scope : instance->deferred) for (const auto &action : scope) {
            enqueue(action.function);
            for (const auto &argument : action.arguments) enqueue(argument.value);
        }
        for (const auto &handler : instance->handlers) enqueue(handler.carried);
    }
}
void Runtime::collectionSafePoint() {
    if (collectionRequested || heap.size() >= nextCollection) { collectionRequested = false; collect(); }
}
void Runtime::collect() {
    ProfileClock profileBegin; size_t profileBefore = heap.size();
    if (config.profiler) { config.profiler->checkpoint(true); profileBegin = config.profiler->now(); config.profiler->enter("<GC>", hostSpan(), currentTask ? currentTask->debugTaskId : 0); }
    mark(global); for (Value root : roots) mark(root); for (const auto &handle : handles) if (handle) mark(*handle);
    for (Object *task : activeTasks) mark(task);
    if (config.debug) for (const auto &frame : debugFrames) { mark(frame.environment); if (frame.generator) mark(frame.generator); }
    for (size_t depth = 0; depth < byteFrameDepth; depth++) {
        for (Value value : byteFrames[depth].locals) mark(value);
        for (Value value : byteFrames[depth].stack) mark(value);
        for (const auto &method : byteFrames[depth].methods) { if (method.receiver) mark(method.receiver); if (method.owner) mark(method.owner); }
        std::function<void(const Contract &)> contractRoots = [&](const Contract &contract) {
            if (!contract) return;
            if (contract->nominal) mark(contract->nominal);
            contractRoots(contract->key); contractRoots(contract->element);
            for (const auto &part : contract->all) contractRoots(part);
        };
        for (const auto &contract : byteFrames[depth].contracts) contractRoots(contract);
    }
    for (const auto &module : modules) mark(module.second);
    for (const auto &type : nativeMethods) for (const auto &method : type.second) mark(method.second);
    heap.erase(std::remove_if(heap.begin(), heap.end(), [](const auto &value) { return !value->marked; }), heap.end());
    for (const auto &value : heap) value->marked = false;
    nextCollection = std::max<size_t>(4096, heap.size() * 2);
    if (config.profiler) { config.profiler->checkpoint(true); config.profiler->leave(); config.profiler->collection(profileBegin, profileBefore, heap.size()); }
}
size_t Runtime::retain(Value value) {
    requireRunning();
    for (size_t handle = 0; handle < handles.size(); handle++) if (!handles[handle]) { handles[handle] = value; return handle; }
    handles.push_back(value); return handles.size() - 1;
}
void Runtime::release(size_t handle) {
    requireRunning();
    if (handle >= handles.size() || !handles[handle]) throw std::out_of_range("Invalid handle");
    handles[handle].reset();
}
Value Runtime::retained(size_t handle) const {
    if (handle >= handles.size() || !handles[handle]) throw std::out_of_range("Invalid handle");
    return *handles[handle];
}
Value Runtime::get(const std::string &name) { return lookup(global, name, hostSpan()).value; }
int64_t Runtime::length(Value value, const Span &span) {
    Object *instance = object(value, span); size_t count;
    if ((instance->kind == Kind::Instance || instance->kind == Kind::Record) && std::find(instance->origin->traits.begin(), instance->origin->traits.end(), sizedTrait) != instance->origin->traits.end())
        return integer(call(member(value, "length", span), {}, span), span);
    if (instance->kind == Kind::String) count = utf8Offsets(instance->text, span).size() - 1;
    else if (instance->kind == Kind::Bytes) count = instance->text.size();
    else if (instance->kind == Kind::Map) count = instance->entries.size();
    else if (instance->kind == Kind::List || instance->kind == Kind::Tuple || instance->kind == Kind::Set) count = instance->items.size();
    else throw Error(span, "Value does not implement length", "type");
    return static_cast<int64_t>(count);
}
Value Runtime::getPath(const std::string &path) {
    if (path.empty()) throw Error(hostSpan(), "Expected a binding or dotted member path", "syntax");
    size_t start = 0; Value value;
    while (start < path.size()) {
        size_t end = path.find('.', start); if (end == std::string::npos) end = path.size();
        std::string name = path.substr(start, end - start);
        if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
            throw Error(hostSpan(), "Inspection accepts names/member paths, not expressions", "syntax");
        for (unsigned char byte : name) if (!std::isalnum(byte) && byte != '_')
            throw Error(hostSpan(), "Inspection accepts names/member paths, not expressions", "syntax");
        value = start ? member(value, name, hostSpan()) : get(name);
        if (end == path.size()) return value;
        start = end + 1;
    }
    throw Error(hostSpan(), "Incomplete member path", "syntax");
}
std::vector<DebugVariable> Runtime::inspectMembers(Value value) {
    std::vector<DebugVariable> result;
    if (!std::holds_alternative<Object *>(value)) return result;
    Object *instance = std::get<Object *>(value);
    auto members = [](Object *type) {
        std::unordered_map<std::string, Binding> result;
        for (Object *current = type; current; current = current->baseClass)
            for (const auto &entry : current->bindings) result.emplace(entry.first, entry.second);
        return result;
    };
    if (instance->kind == Kind::Record || instance->kind == Kind::Instance || instance->kind == Kind::Module || instance->kind == Kind::EnumType) {
        for (const auto &field : instance->bindings) if (canAccess(field.second)) result.push_back({field.first,
            field.second.type.empty() ? typeOf(field.second.value) : field.second.type,
            debugValue(field.second.value), field.second.mutableBinding});
        if (instance->kind == Kind::Instance || instance->kind == Kind::Record) for (const auto &method : members(instance->origin))
            if (!method.second.staticMember && canAccess(method.second)) result.push_back({method.first, "Function", "(bound method)", false});
    } else if (instance->kind == Kind::RecordType || instance->kind == Kind::ClassType) {
        for (const auto &entry : classFields(instance)) { const auto &field = entry.first; Binding access{}; access.visibility = field.visibility; access.declaringClass = entry.second;
            if (canAccess(access)) result.push_back({field.name, field.type, field.defaultValue ? "(constructor field with default)" : "(constructor field)", field.mutableBinding}); }
        for (const auto &method : members(instance)) {
            if (!canAccess(method.second)) continue;
            if (method.second.staticMember) result.push_back({method.first, typeOf(method.second.value), debugValue(method.second.value), method.second.mutableBinding});
            else result.push_back({method.first, "Function", "(instance method)", false});
        }
    } else if (instance->kind == Kind::TraitType) {
        for (const auto &signature : instance->code->children) result.push_back({signature->text, "Function", "(trait signature) -> " + signature->type, false});
    } else if (instance->kind == Kind::List) {
        for (const auto &method : listMembers()) result.push_back({method.name, "Function", method.detail, false});
    }
    else if (instance->kind == Kind::Set) { for (const auto &method : setMembers()) result.push_back({method.name, "Function", method.detail, false}); }
    else if (instance->kind == Kind::Map) {
        for (const auto &method : mapMembers()) result.push_back({method.name, "Function", method.detail, false});
    }
    else if (instance->kind == Kind::String) {
        for (const auto &method : stringMembers()) result.push_back({method.name, "Function", method.detail, false});
    }
    else if (instance->kind == Kind::Resource && instance->external) {
        result = {{"closed", "Bool", instance->external->closed ? "true" : "false", false},
            {"type_name", "String", shortened(instance->external->name), false}};
    }
    else if (instance->kind == Kind::Task) {
        result = {{"status", "String", taskStatus(instance->taskState), false}, {"done", "Bool", terminal(instance->taskState) ? "true" : "false", false},
            {"has_value", "Bool", instance->taskHasValue ? "true" : "false", false},
            {"value", typeOf(instance->taskValue), debugValue(instance->taskValue), false},
            {"error", "Any", instance->taskFailure ? "(task failed)" : "null", false}};
    }
    if (instance->kind == Kind::List || instance->kind == Kind::Tuple || instance->kind == Kind::Map || instance->kind == Kind::Set || instance->kind == Kind::String || instance->kind == Kind::Bytes)
        result.push_back({"length", "Function", "() -> Int", false});
    if (instance->origin && instance->origin->kind == Kind::ClassType && !instance->origin->builtinCollection && instance->kind != Kind::Instance) {
        for (const auto &field : instance->bindings) if (canAccess(field.second)) result.push_back({field.first, field.second.type, debugValue(field.second.value), field.second.mutableBinding});
        for (const auto &entry : members(instance->origin)) if (!entry.second.staticMember && entry.first != "init" && canAccess(entry.second)) result.push_back({entry.first, "Function", "(bound method)", false});
    }
    std::sort(result.begin(), result.end(), [](const auto &first, const auto &second) { return first.name < second.name; });
    result.erase(std::unique(result.begin(), result.end(), [](const auto &first, const auto &second) { return first.name == second.name; }), result.end());
    return result;
}
void Runtime::set(const std::string &name, Value value) { requireRunning(); define(global, name, value, true, "", hostSpan()); }
void Runtime::appendItem(Value list, Value item, const Span &span) {
    requireRunning(); Object *target = object(list, span);
    if (target->kind != Kind::List) throw Error(span, "append requires a List", "type");
    requireType(item, target->elementContract, span); target->items.push_back(item);
}
void Runtime::setItem(Value list, size_t position, Value item, const Span &span) {
    requireRunning(); Object *target = object(list, span);
    if (target->kind != Kind::List) throw Error(span, "set requires a List", "type");
    if (position >= target->items.size()) throw Error(span, "List index out of range", "index");
    requireType(item, target->elementContract, span); target->items[position] = item;
}
bool Runtime::lookupEntry(Value map, Value key, Value &value, const Span &span) {
    requireRunning(); Object *target = object(map, span);
    if (target->kind != Kind::Map || !primitiveKey(key)) throw Error(span, "Map lookup requires a Map and primitive key", "type");
    requireType(key, target->keyContract, span);
    for (const auto &entry : target->entries) if (equal(entry.first, key)) { value = entry.second; return true; }
    value = {}; return false;
}
void Runtime::setEntry(Value map, Value key, Value value, const Span &span) {
    requireRunning(); Object *target = object(map, span);
    if (target->kind != Kind::Map || !primitiveKey(key)) throw Error(span, "Map set requires a Map and primitive key", "type");
    requireType(key, target->keyContract, span); requireType(value, target->elementContract, span);
    for (auto &entry : target->entries) if (equal(entry.first, key)) { entry.second = value; return; }
    target->entries.emplace_back(key, value);
}
bool Runtime::removeEntry(Value map, Value key, const Span &span) {
    requireRunning(); Object *target = object(map, span);
    if (target->kind != Kind::Map || !primitiveKey(key)) throw Error(span, "Map remove requires a Map and primitive key", "type");
    requireType(key, target->keyContract, span);
    for (auto entry = target->entries.begin(); entry != target->entries.end(); ++entry)
        if (equal(entry->first, key)) { target->entries.erase(entry); return true; }
    return false;
}
Value Runtime::run(const std::string &source, const std::string &file) {
    requireRunning();
    Ast program;
    {
        std::optional<ProfileScope> profile;
        if (config.profiler) profile.emplace(config.profiler.get(), "Parse", "vm");
        program = parse(std::make_shared<Source>(Source{file, source}));
    }
    return run(program);
}
Value Runtime::run(const Ast &program, bool echoLastExpression) {
    requireRunning();
    steps = 0; size_t start = roots.size(); Value result;
    shadowGlobals = echoLastExpression && !config.enforceContracts;
    FrameScope frame(*this, "<script>", program->span, global);
    std::optional<ProfileScope> executionProfile;
    if (config.profiler) executionProfile.emplace(config.profiler.get(), "Execute", "vm", program->span);
    try {
        std::shared_ptr<const Bytecode> compiled;
        if (!config.debug) {
            std::optional<ProfileScope> profile;
            if (config.profiler) profile.emplace(config.profiler.get(), "Compile", "vm", program->span);
            compiled = compileBytecode(program, config.optimizationLevel, echoLastExpression, !config.enforceContracts);
        }
        if (compiled) { if (config.profiler) config.profiler->bytecodePrograms++; result = executeBytecode(*compiled, global, {}, program->span); }
        else { if (config.profiler) config.profiler->lexicalPrograms++; executeBlock(program, global, false, &result, echoLastExpression); }
        roots.push_back(result);
        if (echoLastExpression && !std::holds_alternative<std::monostate>(result) && config.write)
            config.write(stringify(result, program->span) + '\n');
    }
    catch (const Thrown &thrown) { roots.resize(start); Error error(thrown.span, display(thrown.value), "throw"); error.trace = thrown.trace; throw error; }
    catch (const Returned &) { roots.resize(start); throw Error(program->span, "return outside function", "syntax"); }
    catch (const Broken &) { roots.resize(start); throw Error(program->span, "break outside loop", "syntax"); }
    catch (const Continued &) { roots.resize(start); throw Error(program->span, "continue outside loop", "syntax"); }
    catch (...) { roots.resize(start); throw; }
    roots.resize(start); collect(); return {};
}

}