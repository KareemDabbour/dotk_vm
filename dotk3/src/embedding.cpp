#include "runtime.hpp"
#include "typer.hpp"
#include "../include/dotk3.h"
#include "plugin_host.hpp"
#include <mutex>
#include <cmath>
#include <thread>
#include <algorithm>

struct ResourceState : dk3::ResourceInfo {
    void *payload;
    void *userdata;
    DK3Dispose dispose;
    std::thread::id thread;
    std::shared_ptr<ResourceState> parent = {};
    ResourceState(std::string name, void *payload, void *userdata, DK3Dispose dispose, std::thread::id thread, std::shared_ptr<ResourceState> parent)
        : ResourceInfo(std::move(name)), payload(payload), userdata(userdata), dispose(dispose), thread(thread), parent(std::move(parent)) {}
};

struct DK3Value {
    DK3VM *owner;
    size_t handle;
    size_t cursor = 0;
    bool alive = true;
};
struct DK3VM {
    std::unique_ptr<dk3::TypeSession> session;
    std::unique_ptr<dk3::Runtime> runtime;
    std::vector<std::unique_ptr<DK3Value>> values;
    std::vector<std::shared_ptr<ResourceState>> resources;
    size_t resourceLimit = 100000;
    std::string diagnostic;
    std::mutex lock;
    bool poisoned = false;
    bool borrowed = false;  // the runtime belongs to the CLI, not to this handle
    ~DK3VM() { if (borrowed) runtime.release(); }
};
struct DK3Call {
    DK3VM *owner;
    const std::vector<dk3::Argument> *arguments;
    dk3::Value result;
    std::string error;
    dk3::Span span;
    std::optional<size_t> resultRoot = {};
    ~DK3Call() { if (resultRoot) owner->runtime->release(*resultRoot); }
};

namespace {
thread_local DK3Call *activeCall = nullptr;
thread_local bool callbackAccess = false;
template<class Function> DK3Status protect(DK3VM *vm, Function function) {
    if (!vm) return DK3_INVALID;
    std::unique_lock<std::mutex> lock(vm->lock, std::defer_lock);
    if (!(callbackAccess && activeCall && activeCall->owner == vm)) {
        if (!lock.try_lock()) return DK3_BUSY;
    }
    if (vm->poisoned) return DK3_OUT_OF_MEMORY;
    vm->diagnostic.clear();
    try { return function(); }
    catch (const dk3::Error &error) {
        vm->diagnostic = dk3::diagnose(error);
        return error.category == "limit" || error.category == "cancelled" ? DK3_LIMIT : DK3_ERROR;
    } catch (const std::bad_alloc &) {
        vm->poisoned = true; vm->diagnostic = "Allocation failed; destroy this VM"; return DK3_OUT_OF_MEMORY;
    } catch (const std::exception &error) { vm->diagnostic = error.what(); return DK3_ERROR; }
    catch (...) { vm->diagnostic = "Host operation failed"; return DK3_ERROR; }
}
template<class Function> DK3Status nativeAccess(DK3Call *call, Function operation) {
    if (!call || activeCall != call) return DK3_INVALID;
    bool previous = callbackAccess; callbackAccess = true;
    struct Restore { bool previous; ~Restore() { callbackAccess = previous; } } restore{previous};
    return protect(call->owner, [&] { return operation(); });
}
DK3Status nativeResult(DK3Call *call, dk3::Value value) {
    if (!call || activeCall != call) return DK3_INVALID;
    try {
        size_t root = call->owner->runtime->retain(value);
        if (call->resultRoot) call->owner->runtime->release(*call->resultRoot);
        call->result = value; call->resultRoot = root; return DK3_OK;
    } catch (const std::bad_alloc &) { call->owner->poisoned = true; return DK3_OUT_OF_MEMORY; }
}
bool valid(const DK3Value *value, DK3VM *owner) { return value && value->alive && value->owner == owner; }
DK3Status retain(DK3VM *vm, dk3::Value value, DK3Value **result) {
    auto handle = std::make_unique<DK3Value>(); handle->owner = vm; handle->handle = vm->runtime->retain(value);
    size_t root = handle->handle;
    try { vm->values.push_back(std::move(handle)); }
    catch (...) { vm->runtime->release(root); throw; }
    *result = vm->values.back().get(); return DK3_OK;
}
dk3::Span hostSpan() { return {std::make_shared<dk3::Source>(dk3::Source{"<host>", ""}), 0, 0, 1, 1}; }
dk3::Object *collection(DK3VM *vm, const DK3Value *value) {
    auto item = vm->runtime->retained(value->handle);
    if (!std::holds_alternative<dk3::Object *>(item)) return nullptr;
    return std::get<dk3::Object *>(item);
}
ResourceState *resourceState(const DK3Value *value) {
    if (!valid(value, value->owner)) return nullptr;
    auto item = collection(value->owner, value);
    return item && item->kind == dk3::Kind::Resource ? dynamic_cast<ResourceState *>(item->external.get()) : nullptr;
}
void disposeResource(ResourceState &state) {
    if (state.closed) return;
    state.closed = true;
    void *payload = state.payload; state.payload = nullptr;
    DK3Call *previousCall = activeCall; bool previousAccess = callbackAccess;
    activeCall = nullptr; callbackAccess = false;
    struct Restore {
        DK3Call *call; bool access;
        ~Restore() { activeCall = call; callbackAccess = access; }
    } restore{previousCall, previousAccess};
    state.dispose(state.userdata, payload);
}
DK3Status shutdownResources(DK3VM *vm) {
    for (const auto &state : vm->resources)
        if (!state->closed && state->thread != std::this_thread::get_id()) return DK3_BUSY;
    DK3Status result = DK3_OK;
    for (auto entry = vm->resources.rbegin(); entry != vm->resources.rend(); ++entry) {
        try { disposeResource(**entry); } catch (...) { result = DK3_ERROR; }
    }
    return result;
}
std::function<dk3::Value(const std::vector<dk3::Argument> &, const dk3::Span &)> nativeFunction(DK3VM *vm, DK3Native function, void *userdata, bool positionalOnly = false) {
    return [vm, function, userdata, positionalOnly](const auto &arguments, const dk3::Span &span) {
        if (positionalOnly) for (const auto &argument : arguments)
            if (!argument.name.empty()) throw dk3::Error(span, "Native module callbacks accept positional arguments", "call");
        DK3Call call{vm, &arguments, {}, "", span};
        DK3Call *previous = activeCall; activeCall = &call;
        struct Restore { DK3Call *previous; ~Restore() { activeCall = previous; } } restore{previous};
        DK3Status result = function(&call, userdata);
        if (result != DK3_OK || !call.error.empty()) throw dk3::Error(span, call.error.empty() ? "Native callback failed" : call.error, "native");
        return call.result;
    };
}
DK3Status installModule(DK3VM *vm, const DK3NativeModule *module) {
    std::vector<dk3::NativeExport> exports;
    for (size_t index = 0; index < module->count; index++) {
        const auto &entry = module->exports[index];
        if (!entry.name || !entry.function || !entry.signature) return DK3_INVALID;
        exports.push_back({entry.name, entry.documentation ? entry.documentation : "", nativeFunction(vm, entry.function, entry.userdata, true)});
    }
    vm->runtime->registerModule(module->name, module->documentation ? module->documentation : "", exports);
    return DK3_OK;
}
DK3Status sequence(DK3VM *vm, size_t count, const DK3Value *const *items, DK3Value **result, dk3::Kind kind) {
    if (!result || (count && !items)) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] {
        std::vector<dk3::Value> values;
        for (size_t index = 0; index < count; index++) {
            if (!valid(items[index], vm)) return DK3_INVALID;
            values.push_back(vm->runtime->retained(items[index]->handle));
        }
        auto value = vm->runtime->allocate(kind, hostSpan()); value->items = std::move(values);
        return retain(vm, value, result);
    });
}
dk3::Object *taskObject(DK3VM *vm, const DK3Value *task) {
    dk3::Value value = vm->runtime->retained(task->handle);
    if (!std::holds_alternative<dk3::Object *>(value)) return nullptr;
    dk3::Object *instance = std::get<dk3::Object *>(value);
    return instance->kind == dk3::Kind::Task ? instance : nullptr;
}
}

extern "C" {
void dk3_module_init(DK3NativeModule *module) {
    if (!module) return;
    *module = {}; module->size = sizeof(*module); module->abi_version = DK3_MODULE_ABI_VERSION;
}
DK3Status dk3_register_module(DK3VM *vm, const DK3NativeModule *module) {
    if (!module || module->size < sizeof(DK3NativeModule) || module->abi_version != DK3_MODULE_ABI_VERSION ||
        !module->name || (module->count && !module->exports)) return DK3_INVALID;
    return protect(vm, [&] {
        DK3Status installed = installModule(vm, module);
        if (installed != DK3_OK) return installed;
        std::vector<std::pair<std::string, std::string>> signatures; std::string problem;
        for (size_t index = 0; index < module->count; index++) signatures.emplace_back(module->exports[index].name, module->exports[index].signature);
        if (vm->session && !vm->session->declareModule(module->name, signatures, problem)) { vm->diagnostic = "Invalid native signature: " + problem; return DK3_INVALID; }
        return DK3_OK;
    });
}
DK3VM *dk3_attach_runtime(dk3::Runtime *runtime) {
    try {
        auto instance = std::make_unique<DK3VM>();
        instance->runtime.reset(runtime); instance->borrowed = true;
        return instance.release();
    } catch (...) { return nullptr; }
}
DK3Status dk3_install_native_module(DK3VM *vm, const DK3NativeModule *module) {
    if (!module || module->size < sizeof(DK3NativeModule) || module->abi_version != DK3_MODULE_ABI_VERSION ||
        !module->name || (module->count && !module->exports)) return DK3_INVALID;
    return protect(vm, [&] { return installModule(vm, module); });
}
void dk3_resource_init(DK3Resource *resource) {
    if (!resource) return;
    *resource = {}; resource->size = sizeof(*resource); resource->abi_version = DK3_RESOURCE_ABI_VERSION;
}
DK3Status dk3_resource(DK3VM *vm, const DK3Resource *provided, DK3Value **result) {
    if (!result) return DK3_INVALID; *result = nullptr;
    if (!provided || provided->size < sizeof(DK3Resource) || provided->abi_version != DK3_RESOURCE_ABI_VERSION ||
        !provided->type_name || !*provided->type_name || !provided->payload || !provided->dispose) return DK3_INVALID;
    return protect(vm, [&] {
        std::shared_ptr<ResourceState> parent;
        if (provided->parent) {
            if (!valid(provided->parent, vm)) return DK3_INVALID;
            auto object = collection(vm, provided->parent);
            if (!object || object->kind != dk3::Kind::Resource) return DK3_INVALID;
            parent = std::dynamic_pointer_cast<ResourceState>(object->external); if (!parent) return DK3_INVALID;
            if (parent->closed) return DK3_ERROR;
            if (parent->thread != std::this_thread::get_id()) return DK3_BUSY;
        }
        vm->resources.erase(std::remove_if(vm->resources.begin(), vm->resources.end(), [](const auto &state) { return state->closed; }), vm->resources.end());
        if (vm->resources.size() >= vm->resourceLimit) { vm->diagnostic = "Open resource limit exceeded"; return DK3_LIMIT; }
        auto state = std::make_shared<ResourceState>(provided->type_name, provided->payload,
            provided->userdata, provided->dispose, std::this_thread::get_id(), std::move(parent));
        vm->runtime->text(state->name, false, hostSpan());
        auto object = vm->runtime->allocate(dk3::Kind::Resource, hostSpan()); object->external = state;
        retain(vm, object, result);
        try { vm->resources.push_back(std::move(state)); }
        catch (...) { vm->runtime->release((*result)->handle); (*result)->alive = false; *result = nullptr; throw; }
        return DK3_OK;
    });
}
DK3Status dk3_resource_get(const DK3Value *value, const char *type_name, void **payload) {
    if (!value || !type_name || !payload) return DK3_INVALID; *payload = nullptr;
    return protect(value->owner, [&] {
        auto state = resourceState(value);
        if (!state || state->name != type_name) return DK3_INVALID;
        if (state->closed) { value->owner->diagnostic = "Resource is closed"; return DK3_ERROR; }
        if (state->thread != std::this_thread::get_id()) return DK3_BUSY;
        *payload = state->payload; return DK3_OK;
    });
}
DK3Status dk3_resource_closed(const DK3Value *value, int *closed) {
    if (!value || !closed) return DK3_INVALID;
    return protect(value->owner, [&] {
        auto state = resourceState(value); if (!state) return DK3_INVALID;
        *closed = state->closed; return DK3_OK;
    });
}
DK3Status dk3_resource_close(DK3Value *value) {
    if (!value) return DK3_INVALID;
    return protect(value->owner, [&] {
        auto state = resourceState(value); if (!state) return DK3_INVALID;
        if (!state->closed && state->thread != std::this_thread::get_id()) return DK3_BUSY;
        for (const auto &child : value->owner->resources)
            if (!child->closed && child->parent.get() == state) return DK3_BUSY;
        disposeResource(*state); return DK3_OK;
    });
}
DK3Status dk3_close_resources(DK3VM *vm) {
    if (!vm) return DK3_INVALID;
    std::unique_lock<std::mutex> lock(vm->lock, std::try_to_lock);
    return lock.owns_lock() ? shutdownResources(vm) : DK3_BUSY;
}
DK3Status dk3_native_resource(DK3Call *call, const DK3Resource *resource, DK3Value **result) { return nativeAccess(call, [&] { return dk3_resource(call->owner, resource, result); }); }
DK3Status dk3_native_resource_get(DK3Call *call, const DK3Value *value, const char *name, void **payload) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_resource_get(value, name, payload) : DK3_INVALID; }); }
DK3Status dk3_native_resource_closed(DK3Call *call, const DK3Value *value, int *closed) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_resource_closed(value, closed) : DK3_INVALID; }); }
DK3Status dk3_native_resource_close(DK3Call *call, DK3Value *value) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_resource_close(value) : DK3_INVALID; }); }
void dk3_config_init(DK3Config *config) {
    if (!config) return;
    *config = {}; config->size = sizeof(*config); config->step_limit = 1000000; config->object_limit = 100000;
}
void dk3_files_init(DK3Files *files) {
    if (!files) return;
    *files = {}; files->size = sizeof(*files); files->abi_version = DK3_FILES_ABI_VERSION; files->max_bytes = 4 * 1024 * 1024;
}
DK3Status dk3_set_files(DK3VM *vm, const DK3Files *provided) {
    if (provided && (provided->size < sizeof(DK3Files) || provided->abi_version != DK3_FILES_ABI_VERSION)) return DK3_INVALID;
    return protect(vm, [&] {
        if (!provided) { vm->runtime->configureFiles({}, {}, 0); return DK3_OK; }
        DK3Files files = *provided;
        std::function<std::string(const std::string &, size_t)> read;
        std::function<std::optional<uint64_t>(const std::string &)> info;
        if (files.read) read = [files](const std::string &path, size_t limit) {
            const char *bytes = nullptr; size_t length = 0;
            DK3Status result = files.read(files.userdata, path.c_str(), limit, &bytes, &length);
            if (result != DK3_OK) throw dk3::Error(hostSpan(), "Host file read failed", result == DK3_LIMIT ? "limit" : "io");
            if (length > limit) throw dk3::Error(hostSpan(), "Host file result exceeds read limit", "limit");
            if (!bytes && length) throw dk3::Error(hostSpan(), "Host returned an invalid file buffer", "io");
            return length ? std::string(bytes, length) : std::string{};
        };
        if (files.info) info = [files](const std::string &path) -> std::optional<uint64_t> {
            int exists = 0; uint64_t size = 0;
            DK3Status result = files.info(files.userdata, path.c_str(), &exists, &size);
            if (result != DK3_OK) throw dk3::Error(hostSpan(), "Host file metadata failed", result == DK3_LIMIT ? "limit" : "io");
            if (exists != 0 && exists != 1) throw dk3::Error(hostSpan(), "Host returned an invalid existence flag", "io");
            return exists ? std::optional<uint64_t>{size} : std::nullopt;
        };
        vm->runtime->configureFiles(std::move(read), std::move(info), files.max_bytes); return DK3_OK;
    });
}
DK3VM *dk3_create(const DK3Config *provided) {
    DK3Config defaults; dk3_config_init(&defaults); const DK3Config &config = provided ? *provided : defaults;
    if (config.size < sizeof(DK3Config)) return nullptr;
    try {
        auto instance = std::make_unique<DK3VM>(); dk3::Config options;
        instance->resourceLimit = config.object_limit;
        options.stepLimit = config.step_limit; options.objectLimit = config.object_limit;
        if (config.write) options.write = [config](const std::string &text) { config.write(config.userdata, text.data(), text.size()); };
        if (config.resolve) options.resolveModule = [config](const std::string &name) {
            const char *source = config.resolve(config.userdata, name.c_str()); return source ? std::string(source) : std::string{};
        };
        dk3::ModuleLoader loader = [config](const std::string &name) -> std::shared_ptr<const dk3::Source> {
            if (!config.resolve) return nullptr;
            const char *source = config.resolve(config.userdata, name.c_str());
            return source ? std::make_shared<const dk3::Source>(dk3::Source{name + ".k3", source}) : nullptr;
        };
        instance->session = std::make_unique<dk3::TypeSession>(loader);
        dk3::TypeSession *session = instance->session.get();
        options.resolveProgram = [session](const std::string &name) { return session->module(name); };
        instance->runtime = std::make_unique<dk3::Runtime>(std::move(options)); return instance.release();
    } catch (...) { return nullptr; }
}
DK3Status dk3_destroy(DK3VM *vm) {
    if (!vm) return DK3_INVALID;
    std::unique_lock<std::mutex> lock(vm->lock, std::try_to_lock);
    if (!lock.owns_lock()) return DK3_BUSY;
    DK3Status result = shutdownResources(vm); if (result != DK3_OK) return result;
    lock.unlock(); delete vm; return DK3_OK;
}
const char *dk3_diagnostic(const DK3VM *vm) { return vm ? vm->diagnostic.c_str() : "Invalid VM"; }
DK3Status dk3_eval(DK3VM *vm, const char *source, const char *file) {
    if (!source) return DK3_INVALID;
    return protect(vm, [&] {
        auto program = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{file ? file : "<host>", source}));
        std::string problems;
        for (const auto &found : vm->session->check(program, false)) {
            if (found.warning) continue;
            problems += (problems.empty() ? "" : "\n") + dk3::formatDiagnostic(found);
        }
        if (!problems.empty()) { vm->diagnostic = problems; return DK3_ERROR; }
        vm->runtime->run(program); return DK3_OK;
    });
}
DK3Status dk3_get(DK3VM *vm, const char *name, DK3Value **result) {
    if (!name || !result) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, vm->runtime->get(name), result); });
}
DK3Status dk3_int(DK3VM *vm, int64_t number, DK3Value **result) {
    if (!result) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, number, result); });
}
DK3Status dk3_string(DK3VM *vm, const char *bytes, size_t length, DK3Value **result) {
    if ((!bytes && length) || !result) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, vm->runtime->text(length ? std::string(bytes, length) : std::string{}, false, hostSpan()), result); });
}
DK3Status dk3_null(DK3VM *vm, DK3Value **result) {
    if (!result) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, {}, result); });
}
DK3Status dk3_list(DK3VM *vm, size_t count, const DK3Value *const *items, DK3Value **result) {
    return sequence(vm, count, items, result, dk3::Kind::List);
}
DK3Status dk3_tuple(DK3VM *vm, size_t count, const DK3Value *const *items, DK3Value **result) {
    return sequence(vm, count, items, result, dk3::Kind::Tuple);
}
DK3Status dk3_sequence_size(const DK3Value *sequence, size_t *result) {
    if (!sequence || !result) return DK3_INVALID;
    return protect(sequence->owner, [&] {
        if (!valid(sequence, sequence->owner)) return DK3_INVALID;
        auto value = collection(sequence->owner, sequence);
        if (!value || (value->kind != dk3::Kind::List && value->kind != dk3::Kind::Tuple)) return DK3_INVALID;
        *result = value->items.size(); return DK3_OK;
    });
}
DK3Status dk3_sequence_get(const DK3Value *sequence, size_t index, DK3Value **result) {
    if (!sequence || !result) return DK3_INVALID; *result = nullptr;
    return protect(sequence->owner, [&] {
        if (!valid(sequence, sequence->owner)) return DK3_INVALID;
        auto value = collection(sequence->owner, sequence);
        if (!value || (value->kind != dk3::Kind::List && value->kind != dk3::Kind::Tuple) || index >= value->items.size()) return DK3_INVALID;
        return retain(sequence->owner, value->items[index], result);
    });
}
DK3Status dk3_list_append(DK3Value *list, const DK3Value *value) {
    if (!list) return DK3_INVALID;
    return protect(list->owner, [&] {
        if (!valid(list, list->owner) || !valid(value, list->owner)) return DK3_INVALID;
        auto target = collection(list->owner, list); if (!target || target->kind != dk3::Kind::List) return DK3_INVALID;
        list->owner->runtime->appendItem(target, list->owner->runtime->retained(value->handle), hostSpan()); return DK3_OK;
    });
}
DK3Status dk3_list_set(DK3Value *list, size_t index, const DK3Value *value) {
    if (!list) return DK3_INVALID;
    return protect(list->owner, [&] {
        if (!valid(list, list->owner) || !valid(value, list->owner)) return DK3_INVALID;
        auto target = collection(list->owner, list);
        if (!target || target->kind != dk3::Kind::List || index >= target->items.size()) return DK3_INVALID;
        list->owner->runtime->setItem(target, index, list->owner->runtime->retained(value->handle), hostSpan()); return DK3_OK;
    });
}
DK3Status dk3_bool(DK3VM *vm, int boolean, DK3Value **result) {
    if (!result || (boolean != 0 && boolean != 1)) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, boolean != 0, result); });
}
DK3Status dk3_float(DK3VM *vm, double number, DK3Value **result) {
    if (!result || !std::isfinite(number)) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, number, result); });
}
DK3Status dk3_bytes(DK3VM *vm, const char *bytes, size_t length, DK3Value **result) {
    if (!result || (!bytes && length)) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, vm->runtime->text(length ? std::string(bytes, length) : std::string{}, true, hostSpan()), result); });
}
DK3Status dk3_map(DK3VM *vm, DK3Value **result) {
    if (!result) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] { return retain(vm, vm->runtime->allocate(dk3::Kind::Map, hostSpan()), result); });
}
DK3Status dk3_map_size(const DK3Value *map, size_t *result) {
    if (!map || !result) return DK3_INVALID;
    return protect(map->owner, [&] {
        if (!valid(map, map->owner)) return DK3_INVALID;
        auto value = collection(map->owner, map); if (!value || value->kind != dk3::Kind::Map) return DK3_INVALID;
        *result = value->entries.size(); return DK3_OK;
    });
}
DK3Status dk3_map_get(const DK3Value *map, const DK3Value *key, int *found, DK3Value **result) {
    if (!map || !found || !result) return DK3_INVALID; *found = 0; *result = nullptr;
    return protect(map->owner, [&] {
        if (!valid(map, map->owner) || !valid(key, map->owner)) return DK3_INVALID;
        auto target = collection(map->owner, map); if (!target || target->kind != dk3::Kind::Map) return DK3_INVALID;
        dk3::Value value;
        if (!map->owner->runtime->lookupEntry(target, map->owner->runtime->retained(key->handle), value, hostSpan())) return DK3_OK;
        DK3Status resultStatus = retain(map->owner, value, result); *found = resultStatus == DK3_OK; return resultStatus;
    });
}
DK3Status dk3_map_set(DK3Value *map, const DK3Value *key, const DK3Value *value) {
    if (!map) return DK3_INVALID;
    return protect(map->owner, [&] {
        if (!valid(map, map->owner) || !valid(key, map->owner) || !valid(value, map->owner)) return DK3_INVALID;
        auto target = collection(map->owner, map); if (!target || target->kind != dk3::Kind::Map) return DK3_INVALID;
        map->owner->runtime->setEntry(target, map->owner->runtime->retained(key->handle), map->owner->runtime->retained(value->handle), hostSpan()); return DK3_OK;
    });
}
DK3Status dk3_map_remove(DK3Value *map, const DK3Value *key, int *removed) {
    if (!map || !removed) return DK3_INVALID; *removed = 0;
    return protect(map->owner, [&] {
        if (!valid(map, map->owner) || !valid(key, map->owner)) return DK3_INVALID;
        auto target = collection(map->owner, map); if (!target || target->kind != dk3::Kind::Map) return DK3_INVALID;
        *removed = map->owner->runtime->removeEntry(target, map->owner->runtime->retained(key->handle), hostSpan()) ? 1 : 0; return DK3_OK;
    });
}
DK3Status dk3_map_entry(const DK3Value *map, size_t index, DK3Value **key, DK3Value **value) {
    if (!map || !key || !value || key == value) return DK3_INVALID; *key = nullptr; *value = nullptr;
    return protect(map->owner, [&] {
        if (!valid(map, map->owner)) return DK3_INVALID;
        auto target = collection(map->owner, map);
        if (!target || target->kind != dk3::Kind::Map || index >= target->entries.size()) return DK3_INVALID;
        retain(map->owner, target->entries[index].first, key);
        try { return retain(map->owner, target->entries[index].second, value); }
        catch (...) { map->owner->runtime->release((*key)->handle); (*key)->alive = false; *key = nullptr; throw; }
    });
}
DK3Status dk3_get_type(const DK3Value *value, DK3Type *result) {
    if (!value || !result) return DK3_INVALID;
    return protect(value->owner, [&] {
        if (!valid(value, value->owner)) return DK3_INVALID;
        dk3::Value item = value->owner->runtime->retained(value->handle);
        if (std::holds_alternative<std::monostate>(item)) *result = DK3_NULL;
        else if (std::holds_alternative<bool>(item)) *result = DK3_BOOL;
        else if (std::holds_alternative<int64_t>(item)) *result = DK3_INT;
        else if (std::holds_alternative<double>(item)) *result = DK3_FLOAT;
        else {
            switch (std::get<dk3::Object *>(item)->kind) {
            case dk3::Kind::String: *result = DK3_STRING; break;
            case dk3::Kind::Bytes: *result = DK3_BYTES; break;
            case dk3::Kind::List: *result = DK3_LIST; break;
            case dk3::Kind::Tuple: *result = DK3_TUPLE; break;
            case dk3::Kind::Map: *result = DK3_MAP; break;
            case dk3::Kind::Set: *result = DK3_SET; break;
            case dk3::Kind::Function: case dk3::Kind::Native: case dk3::Kind::BoundMethod: case dk3::Kind::Constructor: *result = DK3_FUNCTION; break;
            case dk3::Kind::RecordType: case dk3::Kind::ClassType: case dk3::Kind::TraitType: case dk3::Kind::EnumType: *result = DK3_TYPE; break;
            case dk3::Kind::Module: *result = DK3_MODULE; break;
            case dk3::Kind::Generator: *result = DK3_GENERATOR; break;
            case dk3::Kind::Task: *result = DK3_TASK; break;
            case dk3::Kind::Resource: *result = DK3_RESOURCE; break;
            default: *result = DK3_OBJECT; break;
            }
        }
        return DK3_OK;
    });
}
DK3Status dk3_get_bool(const DK3Value *value, int *result) {
    if (!value || !result) return DK3_INVALID;
    return protect(value->owner, [&] {
        if (!valid(value, value->owner)) return DK3_INVALID;
        auto item = value->owner->runtime->retained(value->handle);
        if (!std::holds_alternative<bool>(item)) return DK3_INVALID;
        *result = std::get<bool>(item) ? 1 : 0; return DK3_OK;
    });
}
DK3Status dk3_get_float(const DK3Value *value, double *result) {
    if (!value || !result) return DK3_INVALID;
    return protect(value->owner, [&] {
        if (!valid(value, value->owner)) return DK3_INVALID;
        auto item = value->owner->runtime->retained(value->handle);
        if (!std::holds_alternative<double>(item)) return DK3_INVALID;
        *result = std::get<double>(item); return DK3_OK;
    });
}
DK3Status dk3_get_bytes(const DK3Value *value, const char **bytes, size_t *length) {
    if (!value || !bytes || !length) return DK3_INVALID;
    return protect(value->owner, [&] {
        if (!valid(value, value->owner)) return DK3_INVALID;
        auto item = value->owner->runtime->retained(value->handle);
        if (!std::holds_alternative<dk3::Object *>(item) || std::get<dk3::Object *>(item)->kind != dk3::Kind::Bytes) return DK3_INVALID;
        auto instance = std::get<dk3::Object *>(item); *bytes = instance->text.data(); *length = instance->text.size(); return DK3_OK;
    });
}
DK3Status dk3_get_int(const DK3Value *value, int64_t *result) {
    if (!value || !result) return DK3_INVALID;
    return protect(value->owner, [&] {
        if (!valid(value, value->owner)) return DK3_INVALID;
        dk3::Value item = value->owner->runtime->retained(value->handle);
        if (!std::holds_alternative<int64_t>(item)) return DK3_INVALID;
        *result = std::get<int64_t>(item); return DK3_OK;
    });
}
DK3Status dk3_get_string(const DK3Value *value, const char **bytes, size_t *length) {
    if (!value || !bytes || !length) return DK3_INVALID;
    return protect(value->owner, [&] {
        if (!valid(value, value->owner)) return DK3_INVALID;
        dk3::Value item = value->owner->runtime->retained(value->handle);
        if (!std::holds_alternative<dk3::Object *>(item) || std::get<dk3::Object *>(item)->kind != dk3::Kind::String) return DK3_INVALID;
        auto object = std::get<dk3::Object *>(item); *bytes = object->text.data(); *length = object->text.size(); return DK3_OK;
    });
}
DK3Status dk3_call(DK3VM *vm, const DK3Value *function, size_t count, const DK3Value *const *arguments, DK3Value **result) {
    if (!valid(function, vm) || !result || (count && !arguments)) return DK3_INVALID; *result = nullptr;
    return protect(vm, [&] {
        std::vector<dk3::Argument> values;
        for (size_t index = 0; index < count; index++) {
            if (!valid(arguments[index], vm)) return DK3_INVALID;
            values.push_back({"", vm->runtime->retained(arguments[index]->handle)});
        }
        return retain(vm, vm->runtime->invoke(vm->runtime->retained(function->handle), values), result);
    });
}
DK3Status dk3_resume(DK3VM *vm, DK3Value *generator, int *available, DK3Value **result) {
    if (!valid(generator, vm) || !available || !result) return DK3_INVALID; *result = nullptr; *available = 0;
    return protect(vm, [&] {
        dk3::Value value;
        if (!vm->runtime->resume(vm->runtime->retained(generator->handle), value, generator->cursor)) return DK3_OK;
        *available = 1; return retain(vm, value, result);
    });
}
DK3Status dk3_release(DK3Value *value) {
    if (!value) return DK3_INVALID;
    return protect(value->owner, [&] {
        if (!value->alive) return DK3_INVALID;
        value->owner->runtime->release(value->handle); value->alive = false; return DK3_OK;
    });
}
DK3Status dk3_close(DK3VM *vm, DK3Value *generator) {
    if (!valid(generator, vm)) return DK3_INVALID;
    return protect(vm, [&] { vm->runtime->close(vm->runtime->retained(generator->handle)); return DK3_OK; });
}
DK3Status dk3_collect(DK3VM *vm) { return protect(vm, [&] { vm->runtime->collect(); return DK3_OK; }); }
DK3Status dk3_spawn(DK3VM *vm, const DK3Value *generator, DK3Value **task) {
    if (!valid(generator, vm) || !task) return DK3_INVALID; *task = nullptr;
    return protect(vm, [&] { return retain(vm, vm->runtime->spawn(vm->runtime->retained(generator->handle)), task); });
}
DK3Status dk3_pump(DK3VM *vm, size_t turns, size_t *completed) {
    if (!completed) return DK3_INVALID; *completed = 0;
    return protect(vm, [&] { *completed = vm->runtime->pump(turns); return DK3_OK; });
}
DK3Status dk3_cancel(DK3VM *vm, const DK3Value *task) {
    if (!valid(task, vm)) return DK3_INVALID;
    return protect(vm, [&] { vm->runtime->cancel(vm->runtime->retained(task->handle)); return DK3_OK; });
}
DK3Status dk3_task_state(const DK3Value *task, DK3TaskState *state) {
    if (!task || !state) return DK3_INVALID;
    return protect(task->owner, [&] {
        if (!valid(task, task->owner)) return DK3_INVALID;
        dk3::Object *instance = taskObject(task->owner, task); if (!instance) return DK3_INVALID;
        switch (instance->taskState) {
        case dk3::TaskState::Ready: *state = DK3_TASK_READY; break;
        case dk3::TaskState::Running: *state = DK3_TASK_RUNNING; break;
        case dk3::TaskState::Cancelling: *state = DK3_TASK_CANCELLING; break;
        case dk3::TaskState::Done: *state = DK3_TASK_DONE; break;
        case dk3::TaskState::Cancelled: *state = DK3_TASK_CANCELLED; break;
        case dk3::TaskState::Failed: *state = DK3_TASK_FAILED; break;
        }
        return DK3_OK;
    });
}
DK3Status dk3_task_value(DK3VM *vm, const DK3Value *task, int *available, DK3Value **result) {
    if (!valid(task, vm) || !available || !result) return DK3_INVALID; *available = 0; *result = nullptr;
    return protect(vm, [&] {
        dk3::Object *instance = taskObject(vm, task); if (!instance) return DK3_INVALID;
        if (!instance->taskHasValue) return DK3_OK;
        *available = 1; return retain(vm, instance->taskValue, result);
    });
}
DK3Status dk3_define_native(DK3VM *vm, const char *name, const char *signature, DK3Native function, void *userdata) {
    if (!name || !signature || !function) return DK3_INVALID;
    return protect(vm, [&] {
        vm->runtime->registerNative(name, nativeFunction(vm, function, userdata));
        std::string problem;
        if (!vm->session->declareNative(name, signature, problem)) { vm->diagnostic = "Invalid native signature: " + problem; return DK3_INVALID; }
        return DK3_OK;
    });
}
size_t dk3_native_count(const DK3Call *call) { return call && activeCall == call ? call->arguments->size() : 0; }
DK3VM *dk3_native_vm(const DK3Call *call) { return call && activeCall == call ? call->owner : nullptr; }
DK3Status dk3_native_argument(DK3Call *call, size_t index, DK3Value **result) {
    if (!result) return DK3_INVALID; *result = nullptr;
    return nativeAccess(call, [&] { return index < call->arguments->size() ? retain(call->owner, (*call->arguments)[index].value, result) : DK3_INVALID; });
}
DK3Status dk3_native_release(DK3Call *call, DK3Value *value) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_release(value) : DK3_INVALID; }); }
DK3Status dk3_native_make_int(DK3Call *call, int64_t number, DK3Value **result) { return nativeAccess(call, [&] { return dk3_int(call->owner, number, result); }); }
DK3Status dk3_native_make_null(DK3Call *call, DK3Value **result) { return nativeAccess(call, [&] { return dk3_null(call->owner, result); }); }
DK3Status dk3_native_make_bool(DK3Call *call, int boolean, DK3Value **result) { return nativeAccess(call, [&] { return dk3_bool(call->owner, boolean, result); }); }
DK3Status dk3_native_make_float(DK3Call *call, double number, DK3Value **result) { return nativeAccess(call, [&] { return dk3_float(call->owner, number, result); }); }
DK3Status dk3_native_make_bytes(DK3Call *call, const char *bytes, size_t length, DK3Value **result) { return nativeAccess(call, [&] { return dk3_bytes(call->owner, bytes, length, result); }); }
DK3Status dk3_native_make_string(DK3Call *call, const char *bytes, size_t length, DK3Value **result) { return nativeAccess(call, [&] { return dk3_string(call->owner, bytes, length, result); }); }
DK3Status dk3_native_list(DK3Call *call, size_t count, const DK3Value *const *items, DK3Value **result) { return nativeAccess(call, [&] { return dk3_list(call->owner, count, items, result); }); }
DK3Status dk3_native_tuple(DK3Call *call, size_t count, const DK3Value *const *items, DK3Value **result) { return nativeAccess(call, [&] { return dk3_tuple(call->owner, count, items, result); }); }
DK3Status dk3_native_map(DK3Call *call, DK3Value **result) { return nativeAccess(call, [&] { return dk3_map(call->owner, result); }); }
DK3Status dk3_native_value_type(DK3Call *call, const DK3Value *value, DK3Type *result) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_get_type(value, result) : DK3_INVALID; }); }
DK3Status dk3_native_value_int(DK3Call *call, const DK3Value *value, int64_t *result) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_get_int(value, result) : DK3_INVALID; }); }
DK3Status dk3_native_value_bool(DK3Call *call, const DK3Value *value, int *result) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_get_bool(value, result) : DK3_INVALID; }); }
DK3Status dk3_native_value_float(DK3Call *call, const DK3Value *value, double *result) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_get_float(value, result) : DK3_INVALID; }); }
DK3Status dk3_native_value_bytes(DK3Call *call, const DK3Value *value, const char **bytes, size_t *length) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_get_bytes(value, bytes, length) : DK3_INVALID; }); }
DK3Status dk3_native_value_string(DK3Call *call, const DK3Value *value, const char **bytes, size_t *length) { return nativeAccess(call, [&] { return valid(value, call->owner) ? dk3_get_string(value, bytes, length) : DK3_INVALID; }); }
DK3Status dk3_native_sequence_size(DK3Call *call, const DK3Value *sequence, size_t *result) { return nativeAccess(call, [&] { return valid(sequence, call->owner) ? dk3_sequence_size(sequence, result) : DK3_INVALID; }); }
DK3Status dk3_native_sequence_get(DK3Call *call, const DK3Value *sequence, size_t index, DK3Value **result) { return nativeAccess(call, [&] { return valid(sequence, call->owner) ? dk3_sequence_get(sequence, index, result) : DK3_INVALID; }); }
DK3Status dk3_native_list_append(DK3Call *call, DK3Value *list, const DK3Value *value) { return nativeAccess(call, [&] { return valid(list, call->owner) ? dk3_list_append(list, value) : DK3_INVALID; }); }
DK3Status dk3_native_list_set(DK3Call *call, DK3Value *list, size_t index, const DK3Value *value) { return nativeAccess(call, [&] { return valid(list, call->owner) ? dk3_list_set(list, index, value) : DK3_INVALID; }); }
DK3Status dk3_native_map_size(DK3Call *call, const DK3Value *map, size_t *result) { return nativeAccess(call, [&] { return valid(map, call->owner) ? dk3_map_size(map, result) : DK3_INVALID; }); }
DK3Status dk3_native_map_get(DK3Call *call, const DK3Value *map, const DK3Value *key, int *found, DK3Value **result) { return nativeAccess(call, [&] { return valid(map, call->owner) ? dk3_map_get(map, key, found, result) : DK3_INVALID; }); }
DK3Status dk3_native_map_set(DK3Call *call, DK3Value *map, const DK3Value *key, const DK3Value *value) { return nativeAccess(call, [&] { return valid(map, call->owner) ? dk3_map_set(map, key, value) : DK3_INVALID; }); }
DK3Status dk3_native_map_remove(DK3Call *call, DK3Value *map, const DK3Value *key, int *removed) { return nativeAccess(call, [&] { return valid(map, call->owner) ? dk3_map_remove(map, key, removed) : DK3_INVALID; }); }
DK3Status dk3_native_map_entry(DK3Call *call, const DK3Value *map, size_t index, DK3Value **key, DK3Value **value) { return nativeAccess(call, [&] { return valid(map, call->owner) ? dk3_map_entry(map, index, key, value) : DK3_INVALID; }); }
DK3Status dk3_native_int(const DK3Call *call, size_t index, int64_t *result) {
    if (!call || activeCall != call || !result || index >= call->arguments->size() || !std::holds_alternative<int64_t>((*call->arguments)[index].value)) return DK3_INVALID;
    *result = std::get<int64_t>((*call->arguments)[index].value); return DK3_OK;
}
DK3Status dk3_native_return_int(DK3Call *call, int64_t result) { return nativeResult(call, result); }
DK3Status dk3_native_bool(const DK3Call *call, size_t index, int *result) {
    if (!call || activeCall != call || !result || index >= call->arguments->size()) return DK3_INVALID;
    auto value = (*call->arguments)[index].value;
    if (!std::holds_alternative<bool>(value)) return DK3_INVALID;
    *result = std::get<bool>(value) ? 1 : 0; return DK3_OK;
}
DK3Status dk3_native_float(const DK3Call *call, size_t index, double *result) {
    if (!call || activeCall != call || !result || index >= call->arguments->size()) return DK3_INVALID;
    auto value = (*call->arguments)[index].value;
    if (!std::holds_alternative<double>(value)) return DK3_INVALID;
    *result = std::get<double>(value); return DK3_OK;
}
DK3Status dk3_native_string(const DK3Call *call, size_t index, const char **bytes, size_t *length) {
    if (!call || activeCall != call || !bytes || !length || index >= call->arguments->size()) return DK3_INVALID;
    auto value = (*call->arguments)[index].value;
    if (!std::holds_alternative<dk3::Object *>(value) || std::get<dk3::Object *>(value)->kind != dk3::Kind::String) return DK3_INVALID;
    auto instance = std::get<dk3::Object *>(value); *bytes = instance->text.data(); *length = instance->text.size(); return DK3_OK;
}
DK3Status dk3_native_bytes(const DK3Call *call, size_t index, const char **bytes, size_t *length) {
    if (!call || activeCall != call || !bytes || !length || index >= call->arguments->size()) return DK3_INVALID;
    auto value = (*call->arguments)[index].value;
    if (!std::holds_alternative<dk3::Object *>(value) || std::get<dk3::Object *>(value)->kind != dk3::Kind::Bytes) return DK3_INVALID;
    auto instance = std::get<dk3::Object *>(value); *bytes = instance->text.data(); *length = instance->text.size(); return DK3_OK;
}
DK3Status dk3_native_return_null(DK3Call *call) { return nativeResult(call, {}); }
DK3Status dk3_native_return_bool(DK3Call *call, int result) {
    if (!call || activeCall != call || (result != 0 && result != 1)) return DK3_INVALID;
    return nativeResult(call, result != 0);
}
DK3Status dk3_native_return_float(DK3Call *call, double result) {
    if (!call || activeCall != call || !std::isfinite(result)) return DK3_INVALID;
    return nativeResult(call, result);
}
DK3Status dk3_native_return_value(DK3Call *call, const DK3Value *value) {
    if (!call || activeCall != call || !valid(value, call->owner)) return DK3_INVALID;
    return nativeResult(call, call->owner->runtime->retained(value->handle));
}
DK3Status dk3_native_return_bytes(DK3Call *call, const char *bytes, size_t length) {
    if (!call || activeCall != call || (!bytes && length)) return DK3_INVALID;
    try { return nativeResult(call, call->owner->runtime->text(length ? std::string(bytes, length) : std::string{}, true, call->span)); }
    catch (const std::bad_alloc &) { call->owner->poisoned = true; return DK3_OUT_OF_MEMORY; }
    catch (const std::exception &error) { call->error = error.what(); return DK3_ERROR; }
}
DK3Status dk3_native_return_string(DK3Call *call, const char *bytes, size_t length) {
    if (!call || activeCall != call || (!bytes && length)) return DK3_INVALID;
    try { return nativeResult(call, call->owner->runtime->text(length ? std::string(bytes, length) : std::string{}, false, call->span)); }
    catch (const std::bad_alloc &) { call->owner->poisoned = true; return DK3_OUT_OF_MEMORY; }
    catch (const std::exception &error) { call->error = error.what(); return DK3_ERROR; }
}
DK3Status dk3_native_error(DK3Call *call, const char *message) { if (!call || activeCall != call || !message) return DK3_INVALID; call->error = message; return DK3_ERROR; }
}