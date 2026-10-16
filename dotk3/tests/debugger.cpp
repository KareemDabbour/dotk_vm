#include "debugger.hpp"
#include "runtime.hpp"
#include <cassert>
#include <iostream>
#include <set>
#include <sstream>

int main() {
    auto source = std::make_shared<dk3::Source>(dk3::Source{"debug.k3", ""});
    dk3::DebugSnapshot point; point.location = {source, 0, 0, 1, 1}; point.sequence = 1;
    point.frames.push_back({10, 0, false, "main", point.location, {}});
    dk3::Debugger debugger;
    assert(debugger.pause(point)); debugger.resume(dk3::DebugAction::Next, point);
    point.sequence++;
    point.frames.insert(point.frames.begin(), {20, 0, false, "child", point.location, {}});
    assert(!debugger.pause(point)); point.frames.erase(point.frames.begin()); point.sequence++;
    assert(debugger.pause(point)); debugger.resume(dk3::DebugAction::Continue, point);
    uint64_t breakpoint = debugger.add("./debug.k3", 3);
    point.location.line = 3; point.sequence++; assert(debugger.pause(point));
    assert(debugger.enable(breakpoint, false)); assert(!debugger.pause(point));
    assert(debugger.remove(breakpoint)); assert(!debugger.remove(breakpoint));
    point.frames[0].generator = true;
    debugger.resume(dk3::DebugAction::Finish, point);
    point.frames[0].id = 30; point.liveGenerators = {10}; point.sequence++;
    assert(!debugger.pause(point)); point.liveGenerators.clear(); point.sequence++;
    assert(debugger.pause(point)); debugger.resume(dk3::DebugAction::Quit, point);
    point.sequence++; assert(!debugger.pause(point));
    std::vector<dk3::DebugSnapshot> events;
    dk3::Runtime *active = nullptr;
    bool rejected = false;
    dk3::Config config;
    config.debug = [&](const dk3::DebugSnapshot &event) {
        events.push_back(event); active->collect();
        try { active->run("let forbidden = 1"); } catch (const dk3::Error &error) { rejected = error.category == "state"; }
    };
    dk3::Runtime runtime(config); active = &runtime;
    runtime.run("var inspectedConversions = 0\nclass InspectionText { fn toString() -> String { inspectedConversions += 1; return 'custom' } }\nlet inspectionText = InspectionText()\nReflection.members(inspectionText)\nlet unrelatedInspectionStep = 1", "conversion-inspection.k3");
    assert(std::get<int64_t>(runtime.get("inspectedConversions")) == 0);
    events.clear();
    runtime.run("fn inner(value) {\nlet local = value + 1\nreturn local\n}\nvar result = inner(2)\nresult += 1", "frames.k3");
    assert(rejected && dk3::Runtime::display(runtime.get("result")) == "4");
    bool locals = false;
    for (const auto &event : events) if (event.location.line == 3 && event.frames[0].name == "inner") {
        assert(event.frames.size() == 2);
        for (const auto &variable : event.frames[0].locals) if (variable.name == "local" && variable.value == "3") locals = true;
    }
    assert(locals);
    events.clear();
    runtime.run("class Counter { var value: Int = 2\nfn add() -> Int {\nself.value += 1\nreturn self.value\n}\n}\nlet counter = Counter()\nlet callback = counter.add\nlet classResult = callback()", "class-debug.k3");
    bool selfVisible = false;
    for (const auto &event : events) if (event.frames[0].name == "Counter.add")
        for (const auto &variable : event.frames[0].locals) if (variable.name == "self" && variable.type == "Counter") selfVisible = true;
    assert(selfVisible && dk3::Runtime::display(runtime.get("classResult")) == "3");
    events.clear();
    runtime.run("trait Readable { fn read() -> Int }\nclass Reader {}\nimpl Readable for Reader { fn read() -> Int { return 5 } }\nlet reader = Reader()\nlet readResult = reader.read()", "trait-debug.k3");
    bool traitFrame = false;
    for (const auto &event : events) if (event.frames[0].name == "Reader.read")
        for (const auto &variable : event.frames[0].locals) if (variable.name == "self" && variable.type == "Reader") traitFrame = true;
    assert(traitFrame && dk3::Runtime::display(runtime.get("readResult")) == "5");
    events.clear();
    runtime.run("fn sequence() {\nlet label = 'retained'\nyield label\nSystem.gc()\nyield label\n}\nlet iterator = sequence()", "generators.k3");
    dk3::Value iterator = runtime.get("iterator"), item; size_t cursor = 0;
    assert(runtime.resume(iterator, item, cursor)); runtime.collect(); assert(runtime.resume(iterator, item, cursor));
    assert(dk3::Runtime::display(item) == "retained");
    uint64_t identity = 0; size_t yields = 0;
    for (const auto &event : events) if (event.kind == "yield") {
        if (!identity) identity = event.frames[0].id;
        assert(event.frames[0].generator && event.frames[0].id == identity); yields++;
    }
    assert(yields == 2);
    events.clear();
    runtime.run("var loop = 0\nwhile loop < 2 { loop += 1 }\nfn shadow() { let label = 'outer'\n{ let label = 'inner'\nSystem.print(label) }\nSystem.print(label) }\nshadow()", "scopes.k3");
    size_t conditions = 0; bool inner = false, outer = false;
    for (const auto &event : events) {
        if (event.location.line == 2 && event.kind == "while") conditions++;
        if (event.location.line == 5 || event.location.line == 6)
            for (const auto &variable : event.frames[0].locals) if (variable.name == "label") {
                if (event.location.line == 5 && variable.value == "\"inner\"") inner = true;
                if (event.location.line == 6 && variable.value == "\"outer\"") outer = true;
            }
    }
    assert(conditions == 3 && inner && outer);
    events.clear();
    runtime.run("fn closure() { let captured = ['kept']\nreturn fn() { System.gc()\nreturn captured } }\nlet saved = closure()\nSystem.gc()\nlet capturedResult = saved()", "closure.k3");
    bool captured = false;
    for (const auto &event : events) if (event.kind == "return")
        for (const auto &variable : event.frames[0].locals) if (variable.name == "captured" && variable.value == "[\"kept\"]") captured = true;
    assert(captured);
    events.clear();
    runtime.run("fn worker(label) { defer System.print(label)\ntry { yield label\nthrow 'handled' } catch error { yield error } finally { System.gc()\nSystem.print('final') } }\nlet firstTask = Tasks.spawn(worker('a'))\nlet secondTask = Tasks.spawn(worker('b'))\nTasks.pump(6)", "task-debug.k3");
    std::set<uint64_t> taskIds;
    for (const auto &event : events) if (event.kind == "yield") { assert(event.frames[0].task); taskIds.insert(event.frames[0].task); }
    assert(taskIds.size() == 2);
    bool taskSnapshot = false;
    for (const auto &event : events) if (event.kind == "yield" && event.tasks.size() == 2)
        for (const auto &task : event.tasks) if (task.id == event.frames[0].task && task.status == "running" && task.frame == event.frames[0].id) taskSnapshot = true;
    assert(taskSnapshot);
    events.clear();
    runtime.run("fn childTask() { yield null }\nfn parentTask() { Tasks.spawn(childTask())\nyield null }\nlet familyTask = Tasks.spawn(parentTask())\nTasks.pump(1)", "task-family-debug.k3");
    bool childParent = false;
    for (const auto &event : events) if (event.kind == "yield")
        for (const auto &task : event.tasks) if (task.parent == event.frames[0].task && task.id != task.parent) childParent = true;
    assert(childParent); runtime.cancelAll();
    dk3::Runtime plain;
    plain.run("let answer = 3 + 4", "plain.k3"); assert(dk3::Runtime::display(plain.get("answer")) == "7");
    dk3::Debugger stepping;
    std::vector<int> stops;
    dk3::Config stepConfig;
    stepConfig.debug = [&](const dk3::DebugSnapshot &event) {
        if (!stepping.pause(event)) return;
        stops.push_back(event.location.line);
        dk3::DebugAction action = dk3::DebugAction::Next;
        if (stops.size() == 2 || stops.size() == 3) action = dk3::DebugAction::Step;
        if (stops.size() == 4) action = dk3::DebugAction::Finish;
        stepping.resume(action, event);
    };
    dk3::Runtime stepRuntime(stepConfig);
    stepRuntime.run("fn called() {\nlet inner = 1\nreturn inner\n}\nlet answer = called()\nlet done = answer + 1", "steps.k3");
    assert((stops == std::vector<int>{1, 5, 1, 2, 6}));
    dk3::Debugger generatorSteps;
    std::vector<int> generatorStops;
    dk3::Config generatorConfig;
    generatorConfig.debug = [&](const dk3::DebugSnapshot &event) {
        if (!generatorSteps.pause(event)) return;
        generatorStops.push_back(event.location.line);
        dk3::DebugAction action = event.frames[0].generator ? dk3::DebugAction::Next : dk3::DebugAction::Continue;
        generatorSteps.resume(action, event);
    };
    generatorSteps.add("gensteps.k3", 2);
    dk3::Runtime generatorRuntime(generatorConfig);
    generatorRuntime.run("fn suspended() {\nyield 1\nyield 2\n}\nlet active = suspended()\nfor value in active { let scratch = value }\nlet done = 3", "gensteps.k3");
    assert((generatorStops == std::vector<int>{1, 2, 3, 7}));
    dk3::Config isolatedConfig;
    size_t isolatedEvents = 0;
    isolatedConfig.debug = [&](const dk3::DebugSnapshot &) { isolatedEvents++; };
    dk3::Runtime isolated(isolatedConfig); isolated.run("let value = 1", "isolated.k3");
    assert(isolatedEvents == 1);
    dk3::DebugSnapshot copied;
    bool moduleFrame = false, arguments = false;
    dk3::Config moduleConfig; moduleConfig.arguments = {"token"};
    moduleConfig.resolveSource = [](const std::string &) {
        return std::make_shared<dk3::Source>(dk3::Source{"/virtual/library.k3", "export let value = 7"});
    };
    moduleConfig.debug = [&](const dk3::DebugSnapshot &event) {
        copied = event;
        if (event.location.source->file == "/virtual/library.k3") moduleFrame = event.frames[0].name == "<module>" && event.frames.size() == 2;
        for (const auto &variable : event.globals) if (variable.name == "System.args" && variable.value == "(\"token\")") arguments = true;
    };
    { dk3::Runtime moduleRuntime(moduleConfig); moduleRuntime.run("import library as library\nlet result = library.value", "imports.k3"); }
    assert(moduleFrame && arguments && copied.location.source->text.find("library.value") != std::string::npos);
    events.clear();
    runtime.run("var cycle = []\ncycle.append(cycle)\nlet huge = '" + std::string(1000, 'x') + "'\nlet observe = cycle", "bounded.k3");
    bool bounded = false;
    for (const auto &event : events) if (event.location.line == 4)
        for (const auto &variable : event.globals) if (variable.name == "huge") bounded = variable.value.size() < 160;
    assert(bounded);
    dk3::DebugSnapshot formatted;
    auto formattedSource = std::make_shared<dk3::Source>(dk3::Source{"format.k3", "let before = 1\nlet value = 2\nSystem.print(value)\n"});
    formatted.location = {formattedSource, 15, 3, 2, 1}; formatted.kind = "binding";
    formatted.frames.push_back({1, 7, false, "main", formatted.location, {{"value", "Int", "2", false}}});
    formatted.tasks.push_back({7, 2, 1, "main", "running", formatted.location});
    std::istringstream commands("locals\nbt\ntasks\nhelp\nc\n"); std::ostringstream screen;
    dk3::Debugger formatter;
    assert(dk3::debugConsole(formatter, formatted, commands, screen, false) == dk3::DebugAction::Continue);
    assert(screen.str().find("=> 2 | let value = 2") != std::string::npos);
    assert(screen.str().find("1 | let before = 1") != std::string::npos);
    assert(screen.str().find("3 | System.print(value)") != std::string::npos);
    assert(screen.str().find("Binding") != std::string::npos && screen.str().find("Locals | Frame #0") != std::string::npos);
    assert(screen.str().find("Tasks | Active scheduler entries") != std::string::npos);
    assert(screen.str().find("Parent") != std::string::npos && screen.str().find("Execution") != std::string::npos);
    formatted.frames[0].locals.push_back({"wide", "String", std::string(180, 'x'), false});
    std::istringstream wideCommands("locals\nc\n"); std::ostringstream wideScreen;
    dk3::debugConsole(formatter, formatted, wideCommands, wideScreen, false);
    assert(wideScreen.str().find(std::string(180, 'x')) == std::string::npos);
    assert(wideScreen.str().find(std::string(40, 'x')) != std::string::npos);
    std::cout << "debugger: PASS\n";
}