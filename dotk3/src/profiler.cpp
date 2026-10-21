#include "profiler.hpp"
#include "../vendor/json.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace dk3
{
    namespace
    {
        ProfileClock profileClock()
        {
            uint64_t wall = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#if defined(CLOCK_THREAD_CPUTIME_ID)
            timespec value{};
            if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
                throw std::runtime_error("Cannot read profiler thread CPU clock");
            return {wall, static_cast<uint64_t>(value.tv_sec) * 1000000000 + static_cast<uint64_t>(value.tv_nsec)};
#else
            return {wall, static_cast<uint64_t>(static_cast<long double>(std::clock()) * 1000000000 / CLOCKS_PER_SEC)};
#endif
        }
        std::string site(const Span &span) { return (span.source ? span.source->file : "<host>") + ":" + std::to_string(span.line) + ":" + std::to_string(span.column); }
        using Json = nlohmann::ordered_json;
    }
    Profiler::Profiler(ProfileOptions options, std::function<ProfileClock()> clock) : options(options), clock(std::move(clock))
    {
        if (!this->clock)
            this->clock = profileClock;
        if (!options.intervalNanos || !options.maxSamples || options.maxFrames < 2 || !options.maxEvents)
            throw std::invalid_argument("Invalid profiler bounds");
        started = lastSample = now();
        frames.push_back({"<profile-overflow>", "<profiler>", 1, 1, 0});
    }
    ProfileClock Profiler::now() const { return clock(); }
    size_t Profiler::intern(const ProfileFrame &frame)
    {
        auto bounded = [](const std::string &value, size_t maximum)
        {
            if (value.size() <= maximum)
                return value;
            while (maximum && (static_cast<unsigned char>(value[maximum]) & 0xc0) == 0x80)
                maximum--;
            return value.substr(0, maximum) + "...";
        };
        ProfileFrame stored{bounded(frame.name, 256), bounded(frame.file, 2048), frame.line, frame.column, frame.task};
        std::string key = stored.file + "\n" + stored.name + "\n" + std::to_string(stored.line) + ":" + std::to_string(stored.column) + ":" + std::to_string(stored.task);
        auto found = frameIds.find(key);
        if (found != frameIds.end())
            return found->second;
        if (frames.size() >= options.maxFrames)
        {
            droppedFrames++;
            return 0;
        }
        size_t index = frames.size();
        frames.push_back(std::move(stored));
        frameIds.emplace(std::move(key), index);
        return index;
    }
    void Profiler::enter(const std::string &name, const Span &span, uint64_t task)
    {
        if (active.empty())
            lastSample = now();
        bool detailed = options.source || options.vm == VMProfile::Verbose;
        size_t frame = 0;
        if (detailed)
        {
            const auto &cached = frames[lastActivationFrame];
            const std::string file = span.source ? span.source->file : "<host>";
            if (cached.name == name && cached.file == file && cached.line == span.line && cached.column == span.column && cached.task == task)
                frame = lastActivationFrame;
            else
            {
                frame = intern({name, file, span.line, span.column, task});
                lastActivationFrame = frame;
            }
        }
        active.push_back({frame, span});
        if (detailed && (calls.size() < options.maxFrames || calls.count(name)))
            calls[name]++;
        peakFrames = std::max(peakFrames, active.size());
    }
    void Profiler::fail(const char *message) noexcept
    {
        if (recordingFailed)
            return;
        recordingFailed = true;
        options.source = false;
        std::snprintf(failureMessage, sizeof(failureMessage), "%s", message);
        std::fprintf(stderr, "dotk3: profiler recording failed: %s\n", failureMessage);
    }
    void Profiler::leave() noexcept
    {
        try
        {
            if (!recordingFailed)
                checkpoint(active.size() == 1);
            if (!active.empty())
                active.pop_back();
            if (active.empty() && !recordingFailed)
                lastSample = now();
        }
        catch (const std::exception &error)
        {
            if (!active.empty())
                active.pop_back();
            fail(error.what());
        }
        catch (...)
        {
            if (!active.empty())
                active.pop_back();
            fail("Unknown recording failure");
        }
    }
    void Profiler::location(const Span &span)
    {
        if (!options.source || active.empty())
            return;
        active.back().location = span;
    }
    void Profiler::checkpoint(bool force)
    {
        if (!options.source || active.empty() || (!force && (++checkpoints & 63) != 0))
            return;
        ProfileClock current = now();
        uint64_t elapsed = current.cpu >= lastSample.cpu ? current.cpu - lastSample.cpu : 0;
        if (!force && elapsed < options.intervalNanos)
            return;
        lastSample = current;
        if (!elapsed)
            return;
        if (samples.size() >= options.maxSamples)
        {
            droppedSamples++;
            return;
        }
        ProfileSample sample;
        sample.cpu = elapsed;
        sample.stack.reserve(active.size());
        for (const auto &frame : active)
        {
            const auto definition = frames[frame.frame];
            sample.stack.push_back(intern({definition.name, frame.location.source ? frame.location.source->file : "<host>", frame.location.line, frame.location.column, definition.task}));
        }
        samples.push_back(std::move(sample));
    }
    void Profiler::allocation(const std::string &kind, const Span &span, size_t bytes, size_t live)
    {
        allocations++;
        estimatedAllocatedBytes += bytes;
        liveObjects = live;
        peakObjects = std::max(peakObjects, live);
        auto &entry = allocationKinds[kind];
        entry.objects++;
        entry.estimatedBytes += bytes;
        if (!options.source && options.vm != VMProfile::Verbose)
            return;
        std::string key = site(span);
        if (allocationSites.size() >= options.maxFrames && !allocationSites.count(key))
        {
            droppedSites++;
            return;
        }
        auto &source = allocationSites[key];
        source.objects++;
        source.estimatedBytes += bytes;
    }
    void Profiler::payload(const std::string &kind, const Span &span, size_t bytes)
    {
        estimatedAllocatedBytes += bytes;
        allocationKinds[kind].estimatedBytes += bytes;
        auto found = allocationSites.find(site(span));
        if (found != allocationSites.end())
            found->second.estimatedBytes += bytes;
    }
    void Profiler::timing(const std::string &name, const std::string &category, ProfileClock begin, uint64_t task)
    {
        ProfileClock end = now();
        if (timings.size() < options.maxFrames || timings.count(name))
        {
            auto &entry = timings[name];
            entry.calls++;
            entry.wall += end.wall - begin.wall;
            entry.cpu += end.cpu - begin.cpu;
        }
        if (options.vm == VMProfile::Verbose)
        {
            if (events.size() < options.maxEvents)
                events.push_back({name, category, begin.wall - started.wall, end.wall - begin.wall, task});
            else
                droppedEvents++;
        }
    }
    void Profiler::collection(ProfileClock begin, size_t before, size_t after)
    {
        uint64_t elapsed = now().wall - begin.wall;
        gcCollections++;
        gcNanos += elapsed;
        gcMaxNanos = std::max(gcMaxNanos, elapsed);
        reclaimed += before - after;
        liveObjects = after;
        if (gcPauses.size() < options.maxEvents)
            gcPauses.push_back(elapsed);
        else
            droppedEvents++;
        timing("GC", "gc", begin);
    }
    std::string Profiler::report(bool verbose) const
    {
        if (recordingFailed)
            return std::string("Profiler report incomplete: ") + failureMessage + '\n';
        ProfileClock end = now();
        std::ostringstream output;
        output << std::fixed << std::setprecision(3);
        auto milliseconds = [](uint64_t value)
        { return static_cast<double>(value) / 1000000; };
        if (options.source)
        {
            output << "DotK source CPU profile (safe-point sampled; not debugger mode)\n";
            std::vector<uint64_t> self(frames.size()), inclusive(frames.size());
            uint64_t total = 0;
            for (const auto &sample : samples)
            {
                total += sample.cpu;
                if (!sample.stack.empty())
                    self[sample.stack.back()] += sample.cpu;
                for (size_t frame : sample.stack)
                    inclusive[frame] += sample.cpu;
            }
            // Aggregate per-line interns by function name so each function appears once.
            struct FnEntry
            {
                uint64_t self = 0, inclusive = 0;
                std::string name, file;
                uint32_t line = 0, column = 0;
                uint64_t task = 0;
            };
            std::unordered_map<std::string, FnEntry> byName;
            for (size_t i = 0; i < frames.size(); i++)
            {
                if (!inclusive[i])
                    continue;
                const auto &fr = frames[i];
                std::string key = fr.name + '\n' + fr.file + '\n' + std::to_string(fr.task);
                auto &entry = byName[key];
                entry.self += self[i];
                entry.inclusive += inclusive[i];
                // Keep the definition-site line (lowest line number = function header).
                if (entry.name.empty() || fr.line < entry.line)
                {
                    entry.name = fr.name;
                    entry.file = fr.file;
                    entry.line = fr.line;
                    entry.column = fr.column;
                    entry.task = fr.task;
                }
            }
            std::vector<FnEntry> ranked;
            ranked.reserve(byName.size());
            for (auto &kv : byName)
                ranked.push_back(std::move(kv.second));
            std::sort(ranked.begin(), ranked.end(), [](const FnEntry &a, const FnEntry &b)
                      { return a.self > b.self; });
            output << "Samples: " << samples.size() << ", attributed CPU: " << milliseconds(total) << " ms\n  Self ms  Inclusive ms  Function / source\n";
            size_t shown = 0;
            for (const auto &entry : ranked)
            {
                if (shown++ >= 20)
                    break;
                output << "  " << std::setw(7) << milliseconds(entry.self) << "  " << std::setw(12) << milliseconds(entry.inclusive) << "  " << entry.name << "  " << entry.file << ':' << entry.line << ':' << entry.column << (entry.task ? " task=" + std::to_string(entry.task) : "") << '\n';
            }
            if (!total)
                output << "  No CPU samples: run a longer workload.\n";
        }
        if (options.vm != VMProfile::Off)
        {
            output << "DotK VM profile (" << (verbose ? "verbose" : "simple") << ")\n";
            output << "Engine: bytecode-only; optimization O" << optimizationLevel << '\n';
#if defined(CLOCK_THREAD_CPUTIME_ID)
            output << "CPU clock: VM thread\n";
#else
            output << "CPU clock: process fallback on this platform\n";
#endif
            output << "Wall: " << milliseconds(end.wall - started.wall) << " ms; CPU: " << milliseconds(end.cpu - started.cpu) << " ms\n";
            output << "Execution: " << bytecodeCalls << " slot-bytecode calls, " << lexicalCalls << " lexical-bytecode calls\n";
            output << "Programs: " << bytecodePrograms << " slot bytecode, " << lexicalPrograms << " lexical bytecode\n";
            output << "Allocations: " << allocations << " objects; estimated base/payload bytes: " << estimatedAllocatedBytes << "; peak live objects: " << peakObjects << "; live: " << liveObjects << '\n';
            auto pauses = gcPauses;
            std::sort(pauses.begin(), pauses.end());
            auto percentile = [&](size_t percent)
            { return pauses.empty() ? 0.0 : milliseconds(pauses[std::min(pauses.size() - 1, (pauses.size() - 1) * percent / 100)]); };
            output << "GC: " << gcCollections << " collections; " << reclaimed << " reclaimed; total " << milliseconds(gcNanos) << " ms; pauses p50/p95/max " << percentile(50) << '/' << percentile(95) << '/' << milliseconds(gcMaxNanos) << " ms\n";
            output << "Frames peak: " << peakFrames << "; operand values peak: " << peakOperandValues << '\n';
            for (const auto &name : {"Parse", "Compile", "Initialize", "Execute", "GC"})
            {
                auto found = timings.find(name);
                if (found != timings.end())
                    output << "  " << name << ": " << milliseconds(found->second.wall) << " ms wall, " << milliseconds(found->second.cpu) << " ms CPU (" << found->second.calls << ")\n";
            }
            if (verbose)
            {
                output << "Binding cache: " << cacheHits << " hits, " << cacheMisses << " misses, " << cacheInvalidations << " invalidations (env=" << cacheEnvMisses << " epoch=" << cacheEpochMisses << ")\n";
                output << "Arithmetic paths: " << integerSpecializations << " checked integer specializations, " << floatSpecializations << " float specializations, " << genericBinaryOperations << " generic operations\nOpcodes:\n";
                std::vector<std::pair<std::string, uint64_t>> ordered(opcodes.begin(), opcodes.end());
                std::sort(ordered.begin(), ordered.end(), [](const auto &first, const auto &second)
                          { return first.second > second.second; });
                for (const auto &entry : ordered)
                    output << "  " << entry.first << ": " << entry.second << '\n';
                output << "Allocation kinds:\n";
                for (const auto &entry : allocationKinds)
                    output << "  " << entry.first << ": " << entry.second.objects << " objects, estimated " << entry.second.estimatedBytes << " bytes\n";
            }
        }
        if (options.source || verbose)
        {
            output << "Function/native activations (recorded exact counts; generators count resumes):\n";
            std::vector<std::pair<std::string, uint64_t>> counts(calls.begin(), calls.end());
            std::sort(counts.begin(), counts.end(), [](const auto &first, const auto &second)
                      { return first.second > second.second; });
            for (size_t index = 0; index < std::min<size_t>(20, counts.size()); index++)
                output << "  " << counts[index].first << ": " << counts[index].second << '\n';
            output << "Native calls (wall includes blocking; CPU is separate):\n";
            for (const auto &entry : timings)
                if (entry.first.rfind("native:", 0) == 0)
                    output << "  " << entry.first.substr(7) << ": " << entry.second.calls << "; " << milliseconds(entry.second.wall) << " ms wall, " << milliseconds(entry.second.cpu) << " ms CPU\n";
            output << "Allocation sites (estimated, not RSS or exact container capacities):\n";
            std::vector<std::pair<std::string, ProfileAllocation>> ordered(allocationSites.begin(), allocationSites.end());
            std::sort(ordered.begin(), ordered.end(), [](const auto &first, const auto &second)
                      { return first.second.objects > second.second.objects; });
            for (size_t index = 0; index < std::min<size_t>(20, ordered.size()); index++)
                output << "  " << ordered[index].first << ": " << ordered[index].second.objects << " objects\n";
        }
        output << "Profiler bounds: dropped samples/frames/events/sites " << droppedSamples << '/' << droppedFrames << '/' << droppedEvents << '/' << droppedSites << '\n';
        return output.str();
    }
    std::string Profiler::speedscope() const
    {
        // Deduplicate per-execution-line interns back to canonical (definition-site) frames.
        // The profiler creates one intern per unique (name, file, line, col) observed during sampling,
        // so a function frozen in the call stack while a callee runs will generate multiple frame IDs
        // all with the same name but different source lines. Merge them by (name, file, task),
        // keeping the entry with the lowest line number (the function header).
        std::unordered_map<std::string, size_t> nameToCanonical; // name+'\n'+file+'\n'+task -> canonical frame index
        std::vector<size_t> frameToCanonical(frames.size());
        for (size_t i = 0; i < frames.size(); i++)
        {
            const auto &fr = frames[i];
            std::string key = fr.name + '\n' + fr.file + '\n' + std::to_string(fr.task);
            auto [it, inserted] = nameToCanonical.emplace(key, i);
            if (!inserted && fr.line < frames[it->second].line)
                it->second = i;
        }
        for (size_t i = 0; i < frames.size(); i++)
        {
            const auto &fr = frames[i];
            std::string key = fr.name + '\n' + fr.file + '\n' + std::to_string(fr.task);
            frameToCanonical[i] = nameToCanonical[key];
        }
        // Build a compact list of only canonical frames, in stable order.
        std::vector<size_t> compactOrder;
        std::unordered_map<size_t, size_t> canonicalToCompact;
        for (size_t i = 0; i < frames.size(); i++)
        {
            size_t c = frameToCanonical[i];
            if (canonicalToCompact.emplace(c, compactOrder.size()).second)
                compactOrder.push_back(c);
        }
        Json output{{"$schema", "https://www.speedscope.app/file-format-schema.json"}, {"name", "DotK source CPU profile"}, {"activeProfileIndex", 0}, {"shared", {{"frames", Json::array()}}}, {"profiles", Json::array()}};
        for (size_t ci : compactOrder)
        {
            const auto &fr = frames[ci];
            output["shared"]["frames"].push_back({{"name", fr.name + (fr.task ? " [task " + std::to_string(fr.task) + "]" : "")}, {"file", fr.file}, {"line", fr.line}, {"col", fr.column}});
        }
#if defined(CLOCK_THREAD_CPUTIME_ID)
        const char *clockName = "VM-thread CPU (safe-point sampled)";
#else
        const char *clockName = "Process CPU fallback (safe-point sampled)";
#endif
        Json profile{{"type", "sampled"}, {"name", clockName}, {"unit", "nanoseconds"}, {"startValue", 0}, {"endValue", 0}, {"samples", Json::array()}, {"weights", Json::array()}};
        uint64_t total = 0;
        for (const auto &sample : samples)
        {
            Json stack = Json::array();
            size_t lastCompact = std::numeric_limits<size_t>::max();
            for (size_t fid : sample.stack)
            {
                size_t compact = canonicalToCompact[frameToCanonical[fid]];
                if (compact != lastCompact)
                {
                    stack.push_back(compact);
                    lastCompact = compact;
                } // dedupe adjacent
            }
            profile["samples"].push_back(stack);
            profile["weights"].push_back(sample.cpu);
            total += sample.cpu;
        }
        profile["endValue"] = total;
        output["profiles"].push_back(std::move(profile));
        return output.dump(2);
    }
    std::string Profiler::perfetto() const
    {
        Json output{{"traceEvents", Json::array()}, {"displayTimeUnit", "ms"}};
        for (const auto &event : events)
            output["traceEvents"].push_back({{"name", event.name}, {"cat", event.category}, {"ph", "X"}, {"ts", event.start / 1000.0}, {"dur", event.duration / 1000.0}, {"pid", 1}, {"tid", event.task}, {"args", {{"task", event.task}}}});
        return output.dump(2);
    }
    ProfileScope::ProfileScope(Profiler *profiler, const std::string &name, const std::string &category, const Span &span, uint64_t task, bool frame)
        : profiler(profiler), name(name), category(category), task(task), frame(frame)
    {
        if (!profiler)
            return;
        profiler->checkpoint(true);
        begin = profiler->now();
        if (frame)
            profiler->enter(name, span, task);
    }
    ProfileScope::~ProfileScope() noexcept
    {
        if (!profiler)
            return;
        try
        {
            if (!profiler->failed())
            {
                profiler->checkpoint(true);
                profiler->timing(name, category, begin, task);
            }
        }
        catch (const std::exception &error)
        {
            profiler->fail(error.what());
        }
        catch (...)
        {
            profiler->fail("Unknown recording failure");
        }
        if (frame)
            profiler->leave();
    }
}