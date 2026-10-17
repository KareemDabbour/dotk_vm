#pragma once
#include "syntax.hpp"
#include "debugger.hpp"
#include "bytecode.hpp"
#include "profiler.hpp"

#include <functional>
#include <exception>
#include <deque>
#include <optional>
#include <unordered_map>
#include <variant>
#include <random>

namespace dk3 {

struct Object;
using Value = std::variant<std::monostate, bool, int64_t, double, Object *>;
struct TypeContract {
    std::string name;
    bool nullable = false;
    Object *nominal = nullptr;
    std::shared_ptr<const TypeContract> key;
    std::shared_ptr<const TypeContract> element;
    std::vector<std::shared_ptr<const TypeContract>> all;
    std::vector<std::shared_ptr<const TypeContract>> alternatives;
};
using Contract = std::shared_ptr<const TypeContract>;
struct Binding { Value value; bool mutableBinding = false; std::string type; Contract contract = {}; bool staticMember = false; bool initialized = true; Visibility visibility = Visibility::Public; Object *declaringClass = nullptr; };
struct Argument { std::string name; Value value; };
struct MethodTarget { Object *receiver = nullptr, *owner = nullptr; };
struct NativeExport {
    std::string name, documentation;
    std::function<Value(const std::vector<Argument> &, const Span &)> function;
};
struct ResourceInfo {
    std::string name; bool closed = false;
    ResourceInfo(std::string name = "", bool closed = false) : name(std::move(name)), closed(closed) {}
    virtual ~ResourceInfo() = default;
};
struct SocketHandle : ResourceInfo {
    SocketHandle() : ResourceInfo("Socket") {}
    virtual void close() = 0;
    virtual std::shared_ptr<SocketHandle> accept(uint64_t timeout) = 0;
    virtual std::optional<std::string> receive(size_t maximum, uint64_t timeout) = 0;
    virtual void send(const std::string &bytes, uint64_t timeout) = 0;
    virtual uint16_t port() const = 0;
};

enum class Kind { Environment, String, Bytes, List, Tuple, Map, Function, Native,
                  RecordType, Record, ClassType, Instance, BoundMethod, TraitType, EnumType, Constructor, Variant, Module, Generator, Task, Resource, Super, Set };
class Runtime;
struct NativeMethod {
    using Body = Value (*)(Runtime &, Object *, const std::vector<Argument> &, const Span &);
    Kind receiver;
    std::string name, result;
    size_t minimumArguments, maximumArguments;
    Body body;
    bool constructionOnly = false;
};
enum class TaskState { Ready, Running, Cancelling, Done, Cancelled, Failed };

enum class Op { Bytecode, Enter, Leave, Branch, Jump, Iterate, Next, Yield, Return, Complete, Defer, Transfer, Try, EndTry, EndFinally };
struct Instruction {
    Op op; Ast node; size_t target = 0; size_t scopes = 0; size_t iterations = 0;
    size_t finalizer = 0; size_t end = 0;
    std::shared_ptr<const Bytecode> bytecode = {};
    bool result = false;
    Ast debugNode = {};
};
struct Iteration { Value source; size_t index = 0; bool started = false; };
struct DeferredCall { Value function; std::vector<Argument> arguments; Span span; };
enum class HandlerPhase { Body, Catch, Finally };
struct Handler {
    Instruction boundary;
    HandlerPhase phase = HandlerPhase::Body;
    Instruction transfer{Op::Return, {}};
    std::exception_ptr failure;
    Value carried;
};

struct Object {
    Kind kind;
    bool marked = false;
    std::string text;
    std::string documentation;
    std::string elementType;
    Contract elementContract;
    Contract keyContract;
    std::vector<Value> items;
    std::vector<std::pair<Value, Value>> entries;
    std::unordered_map<std::string, Binding> bindings;
    Object *parent = nullptr;
    Object *origin = nullptr;
    Object *baseClass = nullptr;
    Object *methodOwner = nullptr;
    Object *accessClass = nullptr;
    Object *classScope = nullptr;
    size_t inheritanceDepth = 0;
    Kind collectionKind = Kind::Instance;
    bool builtinCollection = false;
    const NativeMethod *nativeMethod = nullptr;
    Ast code;
    std::shared_ptr<const Bytecode> bytecode;
    bool bytecodeAttempted = false;
    std::function<Value(const std::vector<Argument> &, const Span &)> native;
    std::shared_ptr<ResourceInfo> external;
    std::vector<Instruction> instructions;
    std::shared_ptr<const std::vector<Instruction>> controlCode;
    size_t instruction = 0;
    bool finished = false;
    bool running = false;
    bool closing = false;
    bool controlExecution = false;
    bool controlAllowReturn = false;
    std::vector<Object *> scopes;
    std::vector<Iteration> iterations;
    std::vector<std::vector<DeferredCall>> deferred;
    std::vector<Handler> handlers;
    Object *taskOwner = nullptr;
    Object *taskParent = nullptr;
    std::vector<Object *> children;
    std::vector<Object *> traits;
    size_t taskDepth = 0;
    bool taskExiting = false;
    TaskState taskState = TaskState::Ready;
    bool cancelRequested = false;
    bool taskHasValue = false;
    Value taskValue;
    Value taskCarried;
    std::exception_ptr taskFailure;
    uint64_t debugFrameId = 0, debugTaskId = 0;
};

struct HttpRequest {
    std::string method, url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    uint64_t timeoutMillis = 0;
    size_t maxResponseBytes = 0;
    unsigned maxRedirects = 0;
};
struct HttpResponse {
    int status = 0;
    std::string reason, url, body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct Config {
    std::vector<std::string> arguments;
    std::function<void(const std::string &)> write;
    std::function<std::string(const std::string &)> resolveModule;
    std::function<std::shared_ptr<const Source>(const std::string &)> resolveSource;
    std::function<std::string(const std::string &, size_t)> readFile;
    std::function<std::optional<uint64_t>(const std::string &)> fileSize;
    size_t maxFileBytes = 4 * 1024 * 1024;
    std::function<std::optional<std::string>()> readLine;
    std::function<uint64_t()> monotonicMillis;
    std::function<void(uint64_t)> delay;
    size_t maxInputBytes = 64 * 1024;
    uint64_t maxDelayMillis = 1000;
    std::function<void(bool)> terminalMode;
    std::function<std::optional<uint8_t>(uint64_t)> readKey;
    uint64_t maxKeyWaitMillis = 1000;
    std::function<std::shared_ptr<SocketHandle>(const std::string &, uint16_t)> listenSocket;
    std::function<std::shared_ptr<SocketHandle>(const std::string &, uint16_t, uint64_t)> connectSocket;
    size_t maxNetworkBytes = 1024 * 1024;
    uint64_t maxNetworkWaitMillis = 1000;
    std::function<HttpResponse(const HttpRequest &)> httpRequest;
    uint64_t maxRequestMillis = 30000;
    std::function<bool()> interrupted;
    std::function<void(const DebugSnapshot &)> debug;
    uint64_t stepLimit = 1000000;
    // False once a program has passed the static type checker: annotations are then erased at run time.
    bool enforceContracts = true;
    // Pre-parsed (and type-annotated) module programs, preferred over resolveSource.
    std::function<Ast(const std::string &)> resolveProgram;
    size_t objectLimit = 10000000;
    unsigned optimizationLevel = 2;
    std::shared_ptr<Profiler> profiler;
};

class Runtime {
    std::vector<std::unique_ptr<Object>> heap;
    std::vector<Value> roots;
    std::vector<std::optional<Value>> handles;
    std::unordered_map<std::string, Object *> modules;
    std::unordered_map<Kind, Object *> collectionTypes;
    std::unordered_map<Kind, std::unordered_map<std::string, Object *>> nativeMethods;
    std::deque<NativeMethod> nativeMethodDefinitions;
    Config config;
    std::mt19937_64 random{0};
    Object *global;
    Object *prelude;
    Object *errorType;
    Object *objectType = nullptr;
    Object *errorKindType = nullptr;
    Object *sizedTrait = nullptr;
    Object *coreNamespace(const std::string &name);
    void installCore();
    void installJson();
    void installNetwork();
    void installRequests();
    void installNativeMethods();
    Value callNativeMethod(Object *receiver, const NativeMethod &method, const std::vector<Argument> &arguments, const Span &span);
    Value call(Value function, const std::vector<Argument> &arguments, const Span &span, Object *receiver, Object *methodOwner);
    friend class JsonReader;
    friend struct NativeMethods;
    friend struct ListMethods;
    friend struct MapMethods;
    friend struct SetMethods;
    friend struct TextMethods;
    uint64_t steps = 0;
    uint64_t cleanupSteps = 0;
    size_t cleanupDepth = 0;
    bool collectionRequested = false;
    size_t nextCollection = 4096;
    uint64_t bindingEpoch = 0;
    int contractForce = 0;
    size_t callDepth = 0;
    std::deque<Object *> readyTasks;
    std::vector<Object *> activeTasks;
    Object *currentTask = nullptr;
    bool pumping = false;
    std::vector<Object *> constructing;
    std::vector<Object *> converting;
    Object *accessClass = nullptr;
    bool terminalOpen = false;
    struct ByteFrame {
        std::vector<Value> locals, stack;
        std::vector<bool> initialized;
        std::vector<Contract> contracts;
        std::vector<Binding *> names;
        std::vector<MethodTarget> methods;
        uint64_t epoch = 0;
    };
    std::deque<ByteFrame> byteFrames;
    size_t byteFrameDepth = 0;
    struct ExecutionFrame { uint64_t id, task; std::string name; Span location; Object *environment; Object *generator; };
    std::vector<ExecutionFrame> debugFrames;
    uint64_t nextDebugFrame = 0, nextDebugTask = 0, debugSequence = 0;
    bool debugPaused = false;
    struct FrameScope {
        Runtime &runtime; size_t start;
        FrameScope(Runtime &runtime, const char *name, const Span &location, Object *environment, Object *generator = nullptr);
        ~FrameScope();
    };
    void requireRunning() const;
    void captureFailure(std::exception_ptr failure) noexcept;
    void debugPoint(const Ast &node, Object *environment, const std::string &kind = "");
    std::vector<DebugVariable> debugVariables(Object *environment);
    void implement(const Ast &node, Object *environment);
    void executeBlock(const Ast &node, Object *environment, bool createScope = true, Value *result = nullptr, bool echoLast = false, bool allowReturn = false, Object *callable = nullptr);
    Value executeBytecode(const Bytecode &code, Object *environment, const std::vector<Argument> &arguments, const Span &span);
    template<bool profiled> Value executeBytecodeLoop(const Bytecode &code, Object *environment, const std::vector<Argument> &arguments, const Span &span);
    void define(Object *environment, const std::string &name, Value value, bool mutableBinding,
                const std::string &type, const Span &span, Contract contract = {});
    Binding &lookup(Object *environment, const std::string &name, const Span &span);
    void mark(Value value);
    void collectionSafePoint();
    void tick(const Span &span);
    void tickExecution(const Span &span);
    bool boolean(Value value, const Span &span);
    int64_t integer(Value value, const Span &span);
    double floating(Value value, const Span &span, bool explicitConversion = false);
    Contract resolveContract(const std::string &type, Object *environment, const Span &span, bool checkIntersectionMethods = true, bool forTest = false);
    Object *resolveObjectPath(const std::string &name, Object *environment, const Span &span);
    void requireType(Value value, const Contract &contract, const Span &span);
    bool valueMatches(Value value, const Contract &contract, const Span &span);
    Value binary(const std::string &operation, Value left, Value right, const Span &span);
    std::string stringify(Value value, const Span &span);
    bool overloaded(const std::string &operation, Value receiver, const std::vector<Argument> &arguments, const Span &span, Value &result);
    Value member(Value value, const std::string &name, const Span &span, bool nativeOnly = false, MethodTarget *target = nullptr);
    Binding *classBinding(Object *type, const std::string &name, Object **owner = nullptr);
    void requireAccess(const Binding &binding, const Span &span) const;
    bool canAccess(const Binding &binding) const;
    std::vector<std::pair<Parameter, Object *>> classFields(Object *type);
    Value index(Value value, Value key, const Span &span);
    int64_t length(Value value, const Span &span);
    Value sliceValues(Value receiver, const std::optional<Value> &start, const std::optional<Value> &stop, const std::optional<Value> &step, const Span &span);
    Value literalValue(const Ast &node, bool negative = false);
    Object *prepareMemberWrite(Value receiver, const std::string &name, bool compound, const Span &span);
    void executeDeclaration(const Ast &node, Object *environment);
    void throwValue(Value value, const Span &span);
    Object *loadModule(const std::string &name, const Span &span);
    void compileGenerator(Object *generator, const Ast &node, bool enterRoot = true, bool echoLast = false);
    DeferredCall captureDeferred(const Ast &node, Object *environment);
    void leaveGeneratorScope(Object *generator);
    void finishGenerator(Object *generator);
    void closeGenerator(Value generator, const Span &span);
    void routeGenerator(Object *generator, Instruction transfer, std::exception_ptr failure = {});
    void runCleanup(const std::function<void()> &action);
    Value caughtValue(std::exception_ptr failure);
    Value errorKind(const std::string &category, const Span &span);
    Value spawnTask(Value generator, const Span &span);
    size_t pumpTasks(size_t turns, const Span &span);
    void cancelTask(Value task, const Span &span);
    void finishTask(Object *task, TaskState state, std::exception_ptr failure = {});
    void cancelChildren(Object *task, std::exception_ptr &failure, const Span &span);
    void prepareTaskExit(Object *generator, std::exception_ptr &failure);
    Value native(const std::string &name, std::function<Value(const std::vector<Argument> &, const Span &)> function);
    std::vector<Value> positional(const std::vector<Argument> &arguments, const Span &span);
public:
    explicit Runtime(Config options = {});
    Object *allocate(Kind kind, const Span &span);
    Value text(std::string bytes, bool binary, const Span &span);
    Value evaluate(const Ast &node, Object *environment);
    Value call(Value function, const std::vector<Argument> &arguments, const Span &span);
    Value invoke(Value function, const std::vector<Argument> &arguments);
    bool resume(Value iterable, Value &result, size_t &cursor);
    void close(Value generator);
    Value spawn(Value generator);
    size_t pump(size_t turns);
    void cancel(Value task);
    void cancelAll();
    void restoreTerminal();
    Value run(const std::string &source, const std::string &file = "<host>");
    Value run(const Ast &program, bool echoLastExpression = false);
    Value get(const std::string &name);
    Value getPath(const std::string &path);
    std::vector<DebugVariable> inspectMembers(Value value);
    void set(const std::string &name, Value value);
    void appendItem(Value list, Value item, const Span &span);
    void setItem(Value list, size_t position, Value item, const Span &span);
    bool lookupEntry(Value map, Value key, Value &value, const Span &span);
    void setEntry(Value map, Value key, Value value, const Span &span);
    bool removeEntry(Value map, Value key, const Span &span);
    void registerNative(const std::string &name, std::function<Value(const std::vector<Argument> &, const Span &)> function);
    void registerNativeMethod(const NativeMethod &method);
    void registerModule(const std::string &name, const std::string &documentation, const std::vector<NativeExport> &exports);
    void configureFiles(std::function<std::string(const std::string &, size_t)> read,
                        std::function<std::optional<uint64_t>(const std::string &)> info, size_t limit);
    bool next(Value iterable, Value &result, size_t &cursor, const Span &span);
    void collect();
    size_t liveObjects() const { return heap.size(); }
    size_t retain(Value value);
    void release(size_t handle);
    Value retained(size_t handle) const;
    static std::string typeOf(Value value);
    static std::string display(Value value);
    static bool equal(Value left, Value right);
};

}