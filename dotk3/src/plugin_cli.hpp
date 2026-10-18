#pragma once
#include "plugin_host.hpp"
#include "typer.hpp"
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#endif

// Native modules named with --native. They are declared to the type checker before a program is
// checked and installed on each Runtime the CLI creates (a REPL :reset makes a new one).
class NativePlugins {
    struct Plugin { void *library; const DK3NativeModule *module; std::string path; };
    std::vector<Plugin> plugins;
    std::vector<DK3VM *> hosts;
public:
    NativePlugins() = default;
    NativePlugins(const NativePlugins &) = delete;
    NativePlugins &operator=(const NativePlugins &) = delete;
    ~NativePlugins() { detach(); }  // libraries stay mapped: their code may still be running destructors
    bool empty() const { return plugins.empty(); }
    // Returns the module name; loading the same file again is a no-op.
    std::string load(const std::string &path) {
#ifdef _WIN32
        (void)path; throw std::runtime_error("--native is not supported on this platform");
#else
        for (const auto &plugin : plugins) if (plugin.path == path) return plugin.module->name;
        void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library) throw std::runtime_error(std::string("Cannot load native module: ") + dlerror());
        auto entry = reinterpret_cast<DK3ModuleEntry>(dlsym(library, DK3_MODULE_ENTRY_SYMBOL));
        const DK3NativeModule *module = entry ? entry() : nullptr;
        if (!module || module->size < sizeof(DK3NativeModule) || module->abi_version != DK3_MODULE_ABI_VERSION || !module->name || (module->count && !module->exports))
            throw std::runtime_error("'" + path + "' does not export a valid " DK3_MODULE_ENTRY_SYMBOL " (ABI version " + std::to_string(DK3_MODULE_ABI_VERSION) + ")");
        for (const auto &plugin : plugins) if (std::string(plugin.module->name) == module->name) throw std::runtime_error(std::string("Native module loaded twice: ") + module->name);
        plugins.push_back({library, module, path});
        return module->name;
#endif
    }
    void declare(dk3::TypeSession &session) const {
        for (const auto &plugin : plugins) {
            std::vector<std::pair<std::string, std::string>> signatures; std::string problem;
            for (size_t index = 0; index < plugin.module->count; index++) {
                const auto &entry = plugin.module->exports[index];
                if (!entry.name || !entry.signature) throw std::runtime_error(std::string("Native module ") + plugin.module->name + " has an export without a name or signature");
                signatures.emplace_back(entry.name, entry.signature);
            }
            if (!session.declareModule(plugin.module->name, signatures, problem))
                throw std::runtime_error(std::string("Invalid signature in native module ") + plugin.module->name + ": " + problem);
        }
    }
    void attach(dk3::Runtime &runtime) {
        detach();
        if (plugins.empty()) return;
        DK3VM *host = dk3_attach_runtime(&runtime);
        if (!host) throw std::runtime_error("Cannot create the native module host");
        hosts.push_back(host);
        for (const auto &plugin : plugins)
            if (dk3_install_native_module(host, plugin.module) != DK3_OK)
                throw std::runtime_error(std::string("Cannot install native module ") + plugin.module->name);
    }
    // Disposes resources created by plugins; call before the Runtime they belong to is destroyed.
    void detach() noexcept { for (DK3VM *host : hosts) dk3_destroy(host); hosts.clear(); }
};
