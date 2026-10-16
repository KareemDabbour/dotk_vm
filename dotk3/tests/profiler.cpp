#include "profiler.hpp"
#include "runtime.hpp"
#include "../vendor/json.hpp"
#include <cassert>
#include <iostream>

int main() {
    auto source = std::make_shared<dk3::Source>(dk3::Source{"profile.k3", ""});
    dk3::ProfileClock clock{0, 0}; dk3::ProfileOptions options; options.source = true; options.vm = dk3::VMProfile::Verbose; options.intervalNanos = 10;
    dk3::Profiler profiler(options, [&] { return clock; });
    profiler.enter("outer", {source, 0, 0, 1, 1}); clock = {20, 10}; profiler.checkpoint(true);
    profiler.enter("inner", {source, 0, 0, 2, 1}, 7); clock = {40, 30}; profiler.checkpoint(true); profiler.leave();
    clock = {1000000, 30}; profiler.checkpoint(true);
    assert(profiler.samples.size() == 2 && profiler.samples[0].cpu == 10 && profiler.samples[1].cpu == 20 && profiler.samples[1].stack.size() == 2);
    profiler.leave(); clock = {2000000, 100}; profiler.enter("resumed", {source, 0, 0, 3, 1}, 7); clock = {2000020, 110}; profiler.checkpoint(true); profiler.leave();
    assert(profiler.samples.back().cpu == 10);
    profiler.allocation("List", {source, 0, 0, 4, 1}, 100, 3); profiler.payload("List", {source, 0, 0, 4, 1}, 50);
    auto began = clock; clock.wall += 100; clock.cpu += 40; profiler.collection(began, 3, 1);
    assert(profiler.allocations == 1 && profiler.estimatedAllocatedBytes == 150 && profiler.reclaimed == 2 && profiler.gcMaxNanos == 100);
    auto speedscope = nlohmann::json::parse(profiler.speedscope());
    assert(speedscope["profiles"][0]["endValue"] == 40 && speedscope["profiles"][0]["samples"].size() == 3);
    auto trace = nlohmann::json::parse(profiler.perfetto()); assert(trace["traceEvents"].size() == 1 && trace["traceEvents"][0]["name"] == "GC");
    assert(profiler.report(true).find("profile.k3:2:1 task=7") != std::string::npos);
    options.maxSamples = 1; options.maxFrames = 2; options.maxEvents = 1;
    dk3::Profiler bounded(options, [&] { return clock; });
    bounded.enter("first", {source, 0, 0, 1, 1}); clock.cpu += 20; bounded.checkpoint(true); bounded.enter("overflow", {source, 0, 0, 2, 1}); clock.cpu += 20; bounded.checkpoint(true);
    bounded.timing("one", "vm", clock); bounded.timing("two", "vm", clock);
    assert(bounded.samples.size() == 1 && bounded.frames.size() == 2 && bounded.events.size() == 1 && bounded.droppedSamples && bounded.droppedFrames && bounded.droppedEvents);
    bool brokenClock = false;
    dk3::Profiler failureSafe(options, [&] { if (brokenClock) throw std::runtime_error("test clock unavailable"); return clock; });
    {
        dk3::ProfileScope phase(&failureSafe, "native:test", "native", {}, 0, true);
        brokenClock = true;
    }
    assert(failureSafe.failed() && failureSafe.report().find("incomplete") != std::string::npos);
    for (unsigned level : {0u, 1u, 2u}) {
        dk3::Config config; config.optimizationLevel = level;
        config.profiler = std::make_shared<dk3::Profiler>(options, [&] { clock.cpu += 10; clock.wall += 20; return clock; });
        dk3::Runtime runtime(config);
        runtime.run("fn twice(value: Int) -> Int { return value * 2 }; let result = twice(21); System.gc()", "profile-runtime.k3");
        assert(std::get<int64_t>(runtime.get("result")) == 42 && config.profiler->bytecodeCalls == 1 && config.profiler->lexicalCalls == 0 && config.profiler->gcCollections > 0);
        assert(config.profiler->optimizationLevel == level && config.profiler->opcodes.count("Call"));
    }
    for (unsigned level : {0u, 1u, 2u}) {
        dk3::ProfileOptions nativeOptions; nativeOptions.vm = dk3::VMProfile::Verbose;
        dk3::Config config; config.optimizationLevel = level; config.profiler = std::make_shared<dk3::Profiler>(nativeOptions);
        dk3::Runtime runtime(config);
        runtime.run(R"(
            let values = [1, 2, 3]; let pair = (1, 2); let unique = Set([1, 2]);
            let mapping = {'x': 1}; let key = 'x'; let word = 'alpha'; let needle = 'ph'; let bytes = b'ab';
            fn nativeCalls() {
                for (var index = 0; index < 600; index++) {
                    values.length(); values.append(4); values.removeAt(-1);
                    pair.length(); unique.contains(2); mapping.get(key);
                    word.contains(needle); word.length(); bytes.length();
                }
            }
        )", "native-allocations.k3");
        runtime.invoke(runtime.get("nativeCalls"), {});
        auto allocations = config.profiler->allocations;
        runtime.invoke(runtime.get("nativeCalls"), {});
        assert(config.profiler->allocations == allocations);
        assert(config.profiler->calls.at("native:List.length") == 1200 && !config.profiler->calls.count("native:length"));
        assert(config.profiler->opcodes.at("ResolveMethod") >= 10800 && config.profiler->opcodes.at("InvokeMethod") >= 10800);
        auto bindings = config.profiler->allocationKinds["BoundMethod"].objects;
        auto natives = config.profiler->allocationKinds["Native"].objects;
        runtime.run("let detachedLength = values.length; let detachedContains = word.contains; System.gc(); let detachedResult = detachedLength() + bytes.length()", "detached-allocations.k3");
        assert(config.profiler->allocationKinds["BoundMethod"].objects == bindings + 2 && config.profiler->allocationKinds["Native"].objects == natives);
        assert(std::get<int64_t>(runtime.get("detachedResult")) == 5);
    }
    std::cout << "profiler: PASS\n";
}