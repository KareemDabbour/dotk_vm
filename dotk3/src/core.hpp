#pragma once
#include "syntax.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace dk3 {
struct OperatorHook { const char *symbol; const char *name; size_t arguments; bool predicate; };
inline const std::vector<OperatorHook> &operatorHooks() {
    static const std::vector<OperatorHook> hooks{{"+", "__add__", 1, false}, {"-", "__sub__", 1, false}, {"*", "__mul__", 1, false},
        {"/", "__div__", 1, false}, {"//", "__floordiv__", 1, false}, {"%", "__mod__", 1, false}, {"**", "__pow__", 1, false},
        {"<", "__lt__", 1, true}, {"<=", "__le__", 1, true}, {">", "__gt__", 1, true}, {">=", "__ge__", 1, true},
        {"unary-", "__neg__", 0, false}, {"unary+", "__pos__", 0, false},
        {"getitem", "__getitem__", 1, false}, {"setitem", "__setitem__", 2, false}};
    return hooks;
}
inline const OperatorHook *operatorHook(const std::string &symbol) { for (const auto &hook : operatorHooks()) if (hook.symbol == symbol) return &hook; return nullptr; }
inline const OperatorHook *namedOperatorHook(const std::string &name) { for (const auto &hook : operatorHooks()) if (hook.name == name) return &hook; return nullptr; }
inline const std::vector<std::pair<std::string, std::string>> &errorKinds() {
    static const std::vector<std::pair<std::string, std::string>> kinds{{"Syntax", "syntax"}, {"Type", "type"}, {"Name", "name"}, {"Index", "index"},
        {"Key", "key"}, {"Value", "value"}, {"Overflow", "overflow"}, {"Call", "call"}, {"State", "state"}, {"Access", "access"},
        {"Capability", "capability"}, {"IO", "io"}, {"Json", "json"}, {"Native", "native"}, {"Thrown", "throw"}, {"Application", "application"},
        {"Limit", "limit"}, {"Cancelled", "cancelled"}, {"Match", "match"}, {"Internal", "internal"}};
    return kinds;
}
inline std::string expressionPath(const Ast &node) {
    if (!node) return "";
    if (node->kind == "name") return node->text;
    if (node->kind == "member") { std::string parent = expressionPath(node->children[0]); return parent.empty() ? "" : parent + "." + node->text; }
    return "";
}
inline const Ast &errorKindDeclaration() {
    static const Ast type = [] { Ast result = std::make_shared<Node>(); result->kind = "enum"; result->text = "Error.Kind";
        for (const auto &kind : errorKinds()) { Ast value = std::make_shared<Node>(); value->kind = "variant"; value->text = kind.first; result->children.push_back(value); } return result; }();
    return type;
}
inline bool callbackYields(const Ast &node) {
    if (!node || node->kind == "function") return false;
    if (node->kind == "yield") return true;
    for (const auto &child : node->children) if (callbackYields(child)) return true;
    return false;
}
inline std::string callbackResult(const Ast &node) {
    if (!node || (node->kind != "function" && node->kind != "signature")) return "Any";
    if (node->kind == "function" && !node->children.empty() && callbackYields(node->children[0])) return "Generator";
    return node->type.empty() ? "Any" : node->type;
}
struct StringMember { const char *name; const char *result; const char *detail; size_t arguments; };
inline const std::vector<StringMember> &stringMembers() {
    static const std::vector<StringMember> members{
        {"contains", "Bool", "contains(needle: String) -> Bool", 1},
        {"startsWith", "Bool", "startsWith(prefix: String) -> Bool", 1},
        {"endsWith", "Bool", "endsWith(suffix: String) -> Bool", 1},
        {"find", "Int", "find(needle: String) -> Int", 1},
        {"split", "List[String]", "split(separator: String) -> List[String]", 1},
        {"join", "String", "join(iterable: String values) -> String", 1}};
    return members;
}
inline const StringMember *stringMember(const std::string &name) {
    for (const auto &member : stringMembers()) if (member.name == name) return &member;
    return nullptr;
}
struct SetMember { const char *name; const char *result; const char *detail; size_t arguments; };
inline const std::vector<SetMember> &setMembers() {
    static const std::vector<SetMember> members{{"add", "Bool", "add(value: T) -> Bool", 1}, {"remove", "Bool", "remove(value: T) -> Bool", 1},
        {"contains", "Bool", "contains(value: T) -> Bool", 1}, {"clear", "Null", "clear() -> Null", 0}, {"values", "List", "values() -> List[T]", 0},
        {"union", "Set", "union(other: Set) -> Set[T]", 1}, {"intersection", "Set", "intersection(other: Set) -> Set[T]", 1},
        {"difference", "Set", "difference(other: Set) -> Set[T]", 1}, {"isSubsetOf", "Bool", "isSubsetOf(other: Set) -> Bool", 1}};
    return members;
}
inline const SetMember *setMember(const std::string &name) { for (const auto &member : setMembers()) if (member.name == name) return &member; return nullptr; }
inline std::string setResult(const SetMember &member, const std::string &receiver) {
    auto arguments = typeArguments(receiver);
    if (std::string(member.result) == "Set") return arguments.empty() ? "Set" : "Set[" + arguments[0] + "]";
    if (std::string(member.name) == "values") return arguments.empty() ? "List" : "List[" + arguments[0] + "]";
    return member.result;
}
struct MapMember { const char *name; const char *result; const char *detail; size_t arguments; int component = -1; };
inline const std::vector<MapMember> &mapMembers() {
    static const std::vector<MapMember> members{
        {"clear", "Null", "clear() -> Null", 0},
        {"contains", "Bool", "contains(key) -> Bool", 1},
        {"get", "Any", "get(key) -> V?", 1},
        {"compute", "Any", "compute(key: K, callback: fn(K, V?) -> V?) -> V?", 2},
        {"computeIfAbsent", "Any", "computeIfAbsent(key: K, callback: fn(K) -> V?) -> V?", 2},
        {"computeIfPresent", "Any", "computeIfPresent(key: K, callback: fn(K, V) -> V?) -> V?", 2},
        {"putIfAbsent", "Any", "putIfAbsent(key: K, value: V) -> V? (previous)", 2},
        {"merge", "Any", "merge(key: K, value: V, callback: fn(V, V) -> V?) -> V?", 3},
        {"remove", "Bool", "remove(key) -> Bool", 1},
        {"keys", "List", "keys() -> List[K]", 0, 0},
        {"values", "List", "values() -> List[V]", 0, 1}};
    return members;
}
struct ListMember { const char *name; const char *result; const char *detail; size_t arguments; };
inline const std::vector<ListMember> &listMembers() {
    static const std::vector<ListMember> members{
        {"map", "List", "map(callback: fn(T) -> U) -> List[U]", 1},
        {"filter", "List", "filter(predicate: fn(T) -> Bool) -> List[T]", 1},
        {"reduce", "Any", "reduce(callback: fn(A, T) -> A, initial: A) -> A", 2},
        {"any", "Bool", "any(predicate: fn(T) -> Bool) -> Bool", 1},
        {"all", "Bool", "all(predicate: fn(T) -> Bool) -> Bool", 1},
        {"find", "Any", "find(predicate: fn(T) -> Bool) -> T?", 1},
        {"findIndex", "Int", "findIndex(predicate: fn(T) -> Bool) -> Int", 1},
        {"append", "Null", "append(value: T) -> Null", 1},
        {"clear", "Null", "clear() -> Null", 0},
        {"reverse", "Null", "reverse() -> Null", 0},
        {"contains", "Bool", "contains(value: T) -> Bool", 1},
        {"indexOf", "Int", "indexOf(value: T) -> Int", 1},
        {"insert", "Null", "insert(index: Int, value: T) -> Null", 2},
        {"removeAt", "Any", "removeAt(index: Int) -> T", 1},
        {"extend", "Null", "extend(iterable) -> Null", 1}};
    return members;
}
inline const ListMember *listMember(const std::string &name) {
    for (const auto &member : listMembers()) if (member.name == name) return &member;
    return nullptr;
}
inline std::string listResult(const ListMember &member, const std::string &receiver) {
    if (std::string(member.name) == "filter") return receiver;
    if (std::string(member.name) != "removeAt" && std::string(member.name) != "find") return member.result;
    auto arguments = typeArguments(receiver);
    std::string result = arguments.size() == 1 ? arguments[0] : "Any";
    return std::string(member.name) == "find" && result != "Any" && result.back() != '?' ? result + '?' : result;
}
inline const MapMember *mapMember(const std::string &name) {
    for (const auto &member : mapMembers()) if (member.name == name) return &member;
    return nullptr;
}
inline std::string mapResult(const MapMember &member, const std::string &receiver) {
    if (std::string(member.name) == "get" || std::string(member.name) == "compute" || std::string(member.name) == "computeIfAbsent" ||
        std::string(member.name) == "computeIfPresent" || std::string(member.name) == "putIfAbsent" || std::string(member.name) == "merge") {
        auto arguments = typeArguments(receiver);
        if (arguments.size() != 2) return "Any";
        std::string result = arguments[1];
        return result == "Any" || result.back() == '?' ? result : result + '?';
    }
    if (member.component < 0) return member.result;
    auto arguments = typeArguments(receiver);
    return arguments.size() == 2 ? "List[" + arguments[member.component] + "]" : "List";
}
struct CoreMember {
    const char *space;
    const char *name;
    const char *type;
    const char *result;
    const char *detail;
    int minimum, maximum;
};
inline const std::vector<CoreMember> &coreMembers() {
    static const std::vector<CoreMember> members{
        {"System", "print", "Function", "Null", "print(*values)", 0, -1},
        {"System", "Object", "Type", "Object", "Object() -> Object; common non-null value type", 0, 0},
        {"System", "gc", "Function", "Null", "gc()", 0, 0},
        {"System", "monotonicMillis", "Function", "Int", "monotonicMillis() -> Int (host clock)", 0, 0},
        {"System", "delay", "Function", "Null", "delay(milliseconds: Int) -> Null (bounded host delay)", 1, 1},
        {"IO", "write", "Function", "Null", "write(value: String | Bytes) -> Null", 1, 1},
        {"IO", "readLine", "Function", "String?", "readLine() -> String? (null at EOF)", 0, 0},
        {"IO", "Files", "Module", "Module", "Files: read-only host capability", 0, 0},
        {"IO", "Terminal", "Module", "Module", "Terminal: raw/polled host capability", 0, 0},
        {"IO", "Sockets", "Module", "Module", "Sockets: bounded TCP host capability", 0, 0},
        {"IO", "Http", "Module", "Module", "Http: bounded HTTP/1.1 exchange", 0, 0},
        {"Sockets", "listen", "Function", "Resource", "listen(host: String, port: Int) -> Resource", 2, 2},
        {"Sockets", "connect", "Function", "Resource", "connect(host: String, port: Int, timeoutMs: Int = 0) -> Resource", 2, 3},
        {"Sockets", "accept", "Function", "Resource?", "accept(listener: Resource, timeoutMs: Int = 0) -> Resource?", 1, 2},
        {"Sockets", "receive", "Function", "Bytes?", "receive(socket: Resource, maximum: Int, timeoutMs: Int = 0) -> Bytes? (empty Bytes at EOF)", 2, 3},
        {"Sockets", "send", "Function", "Int", "send(socket: Resource, bytes: String | Bytes, timeoutMs: Int = 0) -> Int", 2, 3},
        {"Sockets", "close", "Function", "Null", "close(socket: Resource) -> Null (idempotent)", 1, 1},
        {"Sockets", "port", "Function", "Int", "port(socket: Resource) -> Int (local port)", 1, 1},
        {"Http", "readRequest", "Function", "Map?", "readRequest(socket: Resource, timeoutMs: Int = 1000) -> Map?", 1, 2},
        {"Http", "readResponse", "Function", "Map?", "readResponse(socket: Resource, timeoutMs: Int = 1000) -> Map?", 1, 2},
        {"Http", "writeRequest", "Function", "Int", "writeRequest(socket: Resource, method: String, target: String, body: String | Bytes = '', timeoutMs: Int = 1000) -> Int", 3, 5},
        {"Http", "writeResponse", "Function", "Int", "writeResponse(socket: Resource, status: Int, body: String | Bytes, contentType: String = 'text/plain; charset=utf-8', timeoutMs: Int = 1000) -> Int", 3, 5},
        {"Requests", "request", "Function", "Map", "request(method: String, url: String, options: Map = {}) -> Map; options: headers, params, body, json, timeoutMs, redirects", 2, 3},
        {"Requests", "get", "Function", "Map", "get(url: String, options: Map = {}) -> Map {status, ok, reason, url, headers, body}", 1, 2},
        {"Requests", "head", "Function", "Map", "head(url: String, options: Map = {}) -> Map", 1, 2},
        {"Requests", "delete", "Function", "Map", "delete(url: String, options: Map = {}) -> Map", 1, 2},
        {"Requests", "post", "Function", "Map", "post(url: String, options: Map = {}) -> Map", 1, 2},
        {"Requests", "put", "Function", "Map", "put(url: String, options: Map = {}) -> Map", 1, 2},
        {"Requests", "patch", "Function", "Map", "patch(url: String, options: Map = {}) -> Map", 1, 2},
        {"Requests", "json", "Function", "Any", "json(response: Map) -> Any (parses the response body)", 1, 1},
        {"Terminal", "open", "Function", "Null", "open() -> Null", 0, 0},
        {"Terminal", "close", "Function", "Null", "close() -> Null (idempotent)", 0, 0},
        {"Terminal", "isOpen", "Function", "Bool", "isOpen() -> Bool", 0, 0},
        {"Terminal", "readKey", "Function", "Int?", "readKey(timeoutMs: Int = 0) -> Int? (byte or timeout)", 0, 1},
        {"System", "args", "Tuple", "Tuple", "args: Tuple[String]", 0, 0},
        {"System", "Error", "Type", "Error", "Error(kind: String, message: String, file: String = '', line: Int = 0, column: Int = 0)", 2, 5},
        {"Files", "readText", "Function", "String", "readText(path: String) -> String", 1, 1},
        {"Files", "readBytes", "Function", "Bytes", "readBytes(path: String) -> Bytes", 1, 1},
        {"Files", "exists", "Function", "Bool", "exists(path: String) -> Bool", 1, 1},
        {"Files", "size", "Function", "Int", "size(path: String) -> Int", 1, 1},
        {"Json", "parse", "Function", "Any", "parse(source: String) -> Any", 1, 1},
        {"Json", "stringify", "Function", "String", "stringify(value) -> String", 1, 1},
        {"Random", "seed", "Function", "Null", "seed(value: Int) -> Null", 1, 1},
        {"Random", "float", "Function", "Float", "float() -> Float in [0, 1)", 0, 0},
        {"Random", "int", "Function", "Int", "int(stop: Int) | int(start: Int, stop: Int) -> Int in [start, stop)", 1, 2},
        {"Random", "choice", "Function", "Any", "choice(sequence: List | Tuple | String | Bytes) -> element", 1, 1},
        {"Random", "shuffle", "Function", "Null", "shuffle(values: List) -> Null (in-place)", 1, 1},
        {"Reflection", "type", "Function", "String", "type(value) -> String", 1, 1},
        {"Reflection", "members", "Function", "List[String]", "members(value) -> List[String]", 1, 1},
        {"Reflection", "binding", "Function", "Any", "binding(name: String) -> Any", 1, 1},
        {"Collections", "Sized", "Type", "Collections.Sized", "trait Sized { fn length() -> Int }", 0, 0},
        {"Reflection", "bindings", "Function", "List[String]", "bindings() -> List[String]", 0, 0},
        {"Reflection", "doc", "Function", "String", "doc(value) -> String", 1, 1},
        {"Collections", "len", "Function", "Int", "len(value) -> Int", 1, 1},
        {"Collections", "range", "Function", "List[Int]", "range(stop) | range(start, stop, step = 1)", 1, 3},
        {"Collections", "List", "Type", "List", "List(iterable = empty) -> List", 0, 1},
        {"Collections", "Map", "Type", "Map", "Map(source = empty) -> Map", 0, 1},
        {"Collections", "Tuple", "Type", "Tuple", "Tuple(iterable = empty) -> Tuple", 0, 1},
        {"Collections", "Set", "Type", "Set", "Set(iterable = empty) -> Set", 0, 1},
        {"Text", "str", "Function", "String", "str(value) -> String", 1, 1},
        {"Text", "encode", "Function", "Bytes", "encode(value: String) -> Bytes", 1, 1},
        {"Text", "decode", "Function", "String", "decode(value: Bytes) -> String", 1, 1},
        {"Math", "toInt", "Function", "Int", "toInt(value) -> Int", 1, 1},
        {"Math", "toFloat", "Function", "Float", "toFloat(value) -> Float", 1, 1},
        {"Math", "PI", "Float", "Float", "PI: Float (3.14159...)", 0, 0},
        {"Math", "TAU", "Float", "Float", "TAU: Float (2 * PI)", 0, 0},
        {"Math", "E", "Float", "Float", "E: Float (Euler's number)", 0, 0},
        {"Math", "sin", "Function", "Float", "sin(radians) -> Float", 1, 1},
        {"Math", "cos", "Function", "Float", "cos(radians) -> Float", 1, 1},
        {"Math", "tan", "Function", "Float", "tan(radians) -> Float", 1, 1},
        {"Math", "asin", "Function", "Float", "asin(value) -> Float in [-PI/2, PI/2]; value must be in [-1, 1]", 1, 1},
        {"Math", "acos", "Function", "Float", "acos(value) -> Float in [0, PI]; value must be in [-1, 1]", 1, 1},
        {"Math", "atan", "Function", "Float", "atan(value) -> Float in (-PI/2, PI/2)", 1, 1},
        {"Math", "atan2", "Function", "Float", "atan2(y, x) -> Float in [-PI, PI]", 2, 2},
        {"Math", "sinh", "Function", "Float", "sinh(value) -> Float", 1, 1},
        {"Math", "cosh", "Function", "Float", "cosh(value) -> Float", 1, 1},
        {"Math", "tanh", "Function", "Float", "tanh(value) -> Float", 1, 1},
        {"Math", "toRadians", "Function", "Float", "toRadians(degrees) -> Float", 1, 1},
        {"Math", "toDegrees", "Function", "Float", "toDegrees(radians) -> Float", 1, 1},
        {"Math", "sqrt", "Function", "Float", "sqrt(value) -> Float; value must be >= 0", 1, 1},
        {"Math", "exp", "Function", "Float", "exp(value) -> Float", 1, 1},
        {"Math", "log", "Function", "Float", "log(value) -> Float (natural); value must be > 0", 1, 1},
        {"Math", "log2", "Function", "Float", "log2(value) -> Float; value must be > 0", 1, 1},
        {"Math", "log10", "Function", "Float", "log10(value) -> Float; value must be > 0", 1, 1},
        {"Math", "pow", "Function", "Float", "pow(base, exponent) -> Float (same as ** on Floats)", 2, 2},
        {"Math", "hypot", "Function", "Float", "hypot(x, y) -> Float", 2, 2},
        {"Math", "floor", "Function", "Int", "floor(value) -> Int (toward negative infinity)", 1, 1},
        {"Math", "ceil", "Function", "Int", "ceil(value) -> Int (toward positive infinity)", 1, 1},
        {"Math", "round", "Function", "Int", "round(value) -> Int (half away from zero)", 1, 1},
        {"Tasks", "close", "Function", "Null", "close(generator)", 1, 1},
        {"Tasks", "spawn", "Function", "Task", "spawn(generator) -> Task", 1, 1},
        {"Tasks", "pump", "Function", "Int", "pump(turns: Int) -> Int", 1, 1},
        {"Tasks", "cancel", "Function", "Null", "cancel(task)", 1, 1}
    };
    return members;
}
inline const CoreMember *coreMember(const std::string &space, const std::string &name) {
    for (const auto &member : coreMembers()) if (member.space == space && member.name == name) return &member;
    return nullptr;
}
inline std::string randomResult(const CoreMember &member, const std::string &source) {
    if (std::string(member.name) != "choice") return member.result;
    if (source == "String") return "String";
    if (source == "Bytes") return "Int";
    auto arguments = typeArguments(source);
    return arguments.size() == 1 ? arguments[0] : "Any";
}
inline Ast collectionDeclaration(const std::string &name) {
    static const std::unordered_map<std::string, Ast> declarations = [] {
        std::unordered_map<std::string, Ast> result;
        for (const std::string &kind : {"Object", "List", "Map", "Tuple", "Set"}) {
            Ast type = std::make_shared<Node>(); type->kind = "class"; type->text = kind; type->builtinCollection = true;
            auto add = [&](const std::string &methodName, size_t count, const std::string &returnType) {
                Ast method = std::make_shared<Node>(); method->kind = "signature"; method->text = methodName; method->type = returnType; method->builtinCollection = true;
                for (size_t position = 0; position < count; position++) method->parameters.push_back({"argument" + std::to_string(position), "Any", {}, false});
                type->children.push_back(method);
            };
            if (kind != "Object") {
                add("length", 0, "Int"); add("init", 1, "Null");
                Ast empty = std::make_shared<Node>(); empty->kind = "literal"; empty->text = "null"; empty->literalKind = TokenKind::Name;
                type->children.back()->parameters[0].defaultValue = empty;
            }
            if (kind == "List") for (const auto &method : listMembers()) add(method.name, method.arguments, method.result);
            if (kind == "Map") for (const auto &method : mapMembers()) add(method.name, method.arguments, method.result);
            if (kind == "Set") for (const auto &method : setMembers()) add(method.name, method.arguments, method.result);
            result.emplace(kind, type);
        }
        return result;
    }();
    auto found = declarations.find(name); return found == declarations.end() ? Ast{} : found->second;
}
inline const Ast &sizedDeclaration() {
    static const Ast declaration = parse(std::make_shared<Source>(Source{"<core>", "trait Sized { \"\"\"A value with a length in Int units.\"\"\"\nfn length() -> Int }"}))->children[0];
    return declaration;
}
}