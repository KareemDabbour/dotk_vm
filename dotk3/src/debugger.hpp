#pragma once
#include "syntax.hpp"
#include <iosfwd>
#include <vector>

namespace dk3 {
struct DebugVariable {
    std::string name, type, value;
    bool mutableBinding = false;
};
struct DebugFrame {
    uint64_t id = 0, task = 0;
    bool generator = false;
    std::string name;
    Span location;
    std::vector<DebugVariable> locals;
};
struct DebugTask {
    uint64_t id = 0, parent = 0, frame = 0;
    std::string name, status;
    Span location;
};
struct DebugSnapshot {
    uint64_t sequence = 0;
    Span location;
    std::string kind;
    std::vector<DebugFrame> frames;
    std::vector<DebugVariable> globals;
    std::vector<uint64_t> liveGenerators;
    std::vector<DebugTask> tasks;
    bool tasksTruncated = false;
};
enum class DebugAction { Continue, Step, Next, Finish, Quit };
struct Breakpoint { uint64_t id; std::string file; int line; bool enabled = true; };

class Debugger {
    DebugAction action = DebugAction::Step;
    uint64_t sequence = 0, frame = 0, nextBreakpoint = 1;
    bool generator = false;
    std::vector<Breakpoint> breakpoints;
public:
    uint64_t add(const std::string &file, int line);
    bool remove(uint64_t id);
    bool enable(uint64_t id, bool enabled);
    const std::vector<Breakpoint> &list() const { return breakpoints; }
    bool pause(const DebugSnapshot &snapshot) const;
    void resume(DebugAction action, const DebugSnapshot &snapshot);
    bool quitting() const { return action == DebugAction::Quit; }
};
DebugAction debugConsole(Debugger &debugger, const DebugSnapshot &snapshot,
                         std::istream &input, std::ostream &output, bool terminal);
}