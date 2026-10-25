#pragma once
#include "syntax.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dk3
{
    class Runtime;
    struct Object;

    // A failure point inside generated code; the runtime boundary raises it as an Error.
    struct NativeSite
    {
        Span span;
        std::string message, category;
        Ast closure = nullptr; // lambda created at this site (category "closure")
    };

    // Shadow-stack GC roots: generated frames link `count` Value slots (stored right after the header).
    struct NativeFrame
    {
        NativeFrame *previous;
        uint64_t count;
    };

    // Shared with generated code: field order and widths are part of the generated-code ABI.
    struct NativeContext
    {
        size_t *depth;
        uint64_t limit;
        uint64_t failure; // 1-based index into sites; nativeException when the runtime holds the exception
        NativeFrame *frames;
        Runtime *runtime;
        const void *const *helpers;
        const std::vector<NativeSite> *sites;
        Object *environment; // entry function's lexical scope
        const void *self;    // entry receiver (const Value *)
        void *result;        // entry boxed result (Value *)
        Object *function;    // entry function object; closures read their captures from it
        const uint64_t *epoch; // Runtime binding epoch validating inline global caches
        const bool *collect;   // set when allocation crossed the GC threshold; loops then take a safe point
    };
    constexpr uint64_t nativeException = ~uint64_t{0};

    // Runtime services callable from generated code through NativeContext::helpers (signatures in native_bridge.cpp).
    enum class NativeHelper : unsigned
    {
        Global,
        StoreGlobal,
        Member,
        PrepareMember,
        SetMember,
        Index,
        SetIndex,
        Binary,
        Call,
        CallMethod,
        Text,
        Collection,
        Format,
        IterInit,
        IterNext,
        Throw,
        SafePoint,
        Truthy,
        InstanceOf,
        Cast,
        CallNative,
        CallBuiltin,
        Capture,
        MakeClosure,
        Unwind,
        Classify,
        Append,
        Concat,
        Slice,
        Count
    };
    const void *const *nativeHelpers();

    // Byte layout of dk3::Value (payload at 0, 1-byte tag) and of Binding flags that generated code relies on;
    // unusable if the probe fails.
    struct NativeValueLayout
    {
        bool usable;
        unsigned size, tag;
        unsigned char null, boolean, integer, floating, object;
        unsigned bindingMutable, bindingInitialized; // Binding::value is at offset 0
        unsigned objectOrigin, objectSlots;          // Object::slots is read as {begin, end} pointers
        unsigned objectKind, objectItems, objectEntries; // kind is a 32-bit Kind; items/entries as {begin, end}
        unsigned kindList, kindTuple, kindSet, kindMap, kindRange;
    };
    NativeValueLayout nativeValueLayout();

    // Kinds: 'i' Int, 'f' Float, 'b' Bool, 'n' Null (results only), 'v' any boxed Value (passed by pointer).
    struct NativeFunction
    {
        uint64_t (*entry)(NativeContext *, const uint64_t *);
        std::string parameters;
        char result;
        const std::vector<NativeSite> *sites;
        // Top-level functions this code calls directly; all must be bound before native code may run.
        std::vector<std::string> dependencies;
        bool method = false; // receives the receiver through NativeContext::self
        mutable bool ready = false;
    };

    // Tables emitted by `dotk3 --build`. Every field is 8 bytes so generated IR and C++ agree on layout.
    using NativeEntry = uint64_t (*)(NativeContext *, const uint64_t *);
    struct EmbeddedSite
    {
        uint64_t offset, length, line, column;
        const char *message, *category;
    };
    struct EmbeddedFunction
    {
        const char *name;
        uint64_t line, column;
        NativeEntry entry;
        const char *parameters;
        uint64_t result;
        const char *const *dependencies;
        uint64_t dependencyCount;
        uint64_t method;
    };
    struct EmbeddedModule
    {
        const char *name; // path relative to the script directory, e.g. "util/math.k3"
        const char *source;
        uint64_t sourceLength;
    };
    struct EmbeddedProgram
    {
        uint64_t version;
        const char *file;
        const char *source;
        uint64_t sourceLength;
        const char *const *options;
        uint64_t optionCount;
        const EmbeddedSite *sites;
        uint64_t siteCount;
        const EmbeddedFunction *functions;
        uint64_t functionCount;
        const EmbeddedModule *modules;
        uint64_t moduleCount;
    };
    static_assert(sizeof(EmbeddedSite) == 6 * 8 && sizeof(EmbeddedFunction) == 9 * 8 && sizeof(EmbeddedModule) == 3 * 8 && sizeof(EmbeddedProgram) == 12 * 8,
                  "embedded tables must be 8-byte fields");
    constexpr uint64_t embeddedProgramVersion = 3;
}

// Strong definition in executables built by `dotk3 --build`; the CLI provides a weak null default.
extern "C" const dk3::EmbeddedProgram *dk3_embedded_program;
