#include "native_methods.hpp"

namespace dk3 {
void Runtime::registerNativeMethod(const NativeMethod &definition) {
    requireRunning();
    if (!definition.body || definition.name.empty() || definition.minimumArguments > definition.maximumArguments)
        throw Error({}, "Invalid native method registration", "native");
    auto &methods = nativeMethods[definition.receiver];
    Object *method = nullptr;
    auto existing = methods.find(definition.name);
    if (existing != methods.end()) {
        throw Error({}, "Duplicate native method '" + definition.name + "'", "native");
    } else {
        Object *type = nullptr;
        auto collection = collectionTypes.find(definition.receiver);
        if (collection != collectionTypes.end()) type = collection->second;
        method = allocate(Kind::Native, {}); method->origin = type;
        method->text = (type ? type->text : definition.receiver == Kind::String ? "String" : definition.receiver == Kind::Bytes ? "Bytes" : "Native") + "." + definition.name;
        method->code = std::make_shared<Node>(); method->code->kind = "function"; method->code->text = definition.name; method->code->type = definition.result;
        for (size_t index = 0; index < definition.maximumArguments; index++) method->code->parameters.push_back({"argument" + std::to_string(index), "Any", {}, false});
        method->code->children.push_back(std::make_shared<Node>()); method->code->children[0]->kind = "block";
        if (type) { define(type, definition.name, method, false, "Function", {}); type->code->children.push_back(method->code); }
        methods.emplace(definition.name, method);
    }
    nativeMethodDefinitions.push_back(definition); method->nativeMethod = &nativeMethodDefinitions.back();
}
Value Runtime::callNativeMethod(Object *receiver, const NativeMethod &method, const std::vector<Argument> &arguments, const Span &span) {
    if (receiver->kind != method.receiver) throw Error(span, "Invalid receiver for native method '" + method.name + "'", "type");
    if (method.constructionOnly && (constructing.empty() || constructing.back() != receiver))
        throw Error(span, "Collection init is only valid during construction", "state");
    if (arguments.size() < method.minimumArguments || arguments.size() > method.maximumArguments)
        throw Error(span, "Wrong number of " + method.name + " arguments", "call");
    for (const auto &argument : arguments) if (!argument.name.empty()) throw Error(span, "This native expects positional arguments", "call");
    return method.body(*this, receiver, arguments, span);
}
void Runtime::registerPrimitiveMethod(const std::string &typeName, NativePrimitiveMethod method) {
    requireRunning();
    if (!method.body || method.name.empty() || method.minimumArguments > method.maximumArguments)
        throw Error({}, "Invalid primitive method registration", "native");
    auto &methods = primitiveMethods[typeName];
    if (methods.count(method.name)) throw Error({}, "Duplicate primitive method '" + typeName + "." + method.name + "'", "native");
    methods.emplace(method.name, std::move(method));
}
void Runtime::registerKindMethod(Kind kind, NativeKindMethod method) {
    requireRunning();
    if (!method.body || method.name.empty() || method.minimumArguments > method.maximumArguments)
        throw Error({}, "Invalid kind method registration", "native");
    auto &methods = kindMethods[kind];
    if (methods.count(method.name)) throw Error({}, "Duplicate kind method '" + method.name + "'", "native");
    methods.emplace(method.name, std::move(method));
}
Value Runtime::callPrimitiveMethod(Value receiver, const NativePrimitiveMethod &method, const std::vector<Argument> &arguments, const Span &span) {
    if (arguments.size() < method.minimumArguments || arguments.size() > method.maximumArguments)
        throw Error(span, "Wrong number of " + method.name + " arguments", "call");
    return method.body(*this, receiver, arguments, span);
}
void Runtime::installNativeMethods() {
    ListMethods::install(*this); MapMethods::install(*this); SetMethods::install(*this); TextMethods::install(*this);
    NativeMethods::collection(*this, Kind::Tuple);
    PrimitiveMethods::install(*this);
}
void NativeMethods::collection(Runtime &runtime, Kind kind) {
    runtime.registerNativeMethod({kind, "length", "Int", 0, 0, length});
    runtime.registerNativeMethod({kind, "init", "Null", 0, 1, initialize, true});
}
Value NativeMethods::length(Runtime &runtime, Object *receiver, const std::vector<Argument> &, const Span &span) {
    return runtime.length(receiver, span);
}
Value NativeMethods::initialize(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span) {
    Object *type = runtime.collectionTypes.at(receiver->kind);
    Value initialized = type->native(arguments, span); runtime.roots.push_back(initialized); Object *source = object(initialized, span);
    receiver->items = source->items; receiver->entries = source->entries; return {};
}
Object *NativeMethods::object(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected object, got " + Runtime::typeOf(value), "type");
    return std::get<Object *>(value);
}
Object *NativeMethods::callback(Value value, const Span &span) {
    Object *result = object(value, span);
    if (result->kind != Kind::Function && result->kind != Kind::Native && result->kind != Kind::BoundMethod && result->kind != Kind::Constructor)
        throw Error(span, "Expected a callback function", "type");
    return result;
}
void NativeMethods::key(Runtime &, Object *, Value value, const Span &span) {
    if (std::holds_alternative<Object *>(value)) {
        Kind kind = object(value, span)->kind;
        if (kind != Kind::String && kind != Kind::Bytes) throw Error(span, "Map keys must be primitive values", "type");
    }
}
int64_t NativeMethods::find(Runtime &runtime, Object *receiver, Value value, const Span &span) {
    for (size_t index = 0; index < receiver->items.size(); index++) { runtime.tick(span); if (Runtime::equal(receiver->items[index], value)) return static_cast<int64_t>(index); }
    return -1;
}
Object *NativeMethods::snapshot(Runtime &runtime, Object *receiver, const Span &span) {
    for (size_t index = 0; index < receiver->items.size(); index++) runtime.tick(span);
    Object *result = runtime.allocate(Kind::List, span); runtime.roots.push_back(result);
    result->items = receiver->items; return result;
}
}
