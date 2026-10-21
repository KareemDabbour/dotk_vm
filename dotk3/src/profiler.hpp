#pragma once
#include "syntax.hpp"
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace dk3
{
    enum class VMProfile
    {
        Off,
        Simple,
        Verbose
    };
    struct ProfileOptions
    {
        bool source = false;
        VMProfile vm = VMProfile::Off;
        uint64_t intervalNanos = 1000000;
        size_t maxSamples = 100000, maxFrames = 16384, maxEvents = 100000;
    };
    struct ProfileClock
    {
        uint64_t wall = 0, cpu = 0;
    };
    struct ProfileFrame
    {
        std::string name, file;
        int line = 1, column = 1;
        uint64_t task = 0;
    };
    struct ProfileSample
    {
        std::vector<size_t> stack;
        uint64_t cpu = 0;
    };
    struct ProfileEvent
    {
        std::string name, category;
        uint64_t start = 0, duration = 0, task = 0;
    };
    struct ProfileAllocation
    {
        uint64_t objects = 0, estimatedBytes = 0;
    };
    struct ProfileTiming
    {
        uint64_t calls = 0, wall = 0, cpu = 0;
    };

    class Profiler
    {
        struct Active
        {
            size_t frame;
            Span location;
        };
        ProfileOptions options;
        std::function<ProfileClock()> clock;
        ProfileClock started, lastSample;
        uint64_t checkpoints = 0;
        size_t lastActivationFrame = 0;
        bool recordingFailed = false;
        char failureMessage[256]{};
        std::vector<Active> active;
        std::unordered_map<std::string, size_t> frameIds;
        size_t intern(const ProfileFrame &frame);

    public:
        std::vector<ProfileFrame> frames;
        std::vector<ProfileSample> samples;
        std::vector<ProfileEvent> events;
        std::unordered_map<std::string, uint64_t> calls, opcodes;
        std::unordered_map<std::string, ProfileTiming> timings;
        std::unordered_map<std::string, ProfileAllocation> allocationSites, allocationKinds;
        std::vector<uint64_t> gcPauses;
        uint64_t allocations = 0, estimatedAllocatedBytes = 0, reclaimed = 0, gcCollections = 0, gcNanos = 0, gcMaxNanos = 0;
        uint64_t bytecodeCalls = 0, lexicalCalls = 0, cacheHits = 0, cacheMisses = 0, cacheInvalidations = 0;
        uint64_t cacheEnvMisses = 0, cacheEpochMisses = 0; // sub-breakdown of invalidations
        uint64_t integerSpecializations = 0, floatSpecializations = 0, genericBinaryOperations = 0;
        uint64_t droppedSamples = 0, droppedFrames = 0, droppedEvents = 0, droppedSites = 0;
        size_t peakObjects = 0, liveObjects = 0, peakFrames = 0, peakOperandValues = 0;
        unsigned optimizationLevel = 2;
        uint64_t bytecodePrograms = 0, lexicalPrograms = 0;
        explicit Profiler(ProfileOptions options = {}, std::function<ProfileClock()> clock = {});
        const ProfileOptions &settings() const { return options; }
        ProfileClock now() const;
        void enter(const std::string &name, const Span &span, uint64_t task = 0);
        void leave() noexcept;
        void fail(const char *message) noexcept;
        bool failed() const { return recordingFailed; }
        void location(const Span &span);
        void checkpoint(bool force = false);
        void allocation(const std::string &kind, const Span &span, size_t estimatedBytes, size_t live);
        void payload(const std::string &kind, const Span &span, size_t bytes);
        void timing(const std::string &name, const std::string &category, ProfileClock begin, uint64_t task = 0);
        void collection(ProfileClock begin, size_t before, size_t after);
        std::string report(bool verbose = false) const;
        std::string speedscope() const;
        std::string perfetto() const;
    };

    class ProfileScope
    {
        Profiler *profiler = nullptr;
        std::string name, category;
        ProfileClock begin;
        uint64_t task = 0;
        bool frame = false;

    public:
        ProfileScope(Profiler *profiler, const std::string &name, const std::string &category, const Span &span = {}, uint64_t task = 0, bool frame = false);
        ~ProfileScope() noexcept;
        ProfileScope(const ProfileScope &) = delete;
        ProfileScope &operator=(const ProfileScope &) = delete;
    };
}