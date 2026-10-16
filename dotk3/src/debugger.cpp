#include "debugger.hpp"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <istream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace dk3 {
namespace {
std::string normalized(const std::string &file) {
    if (!file.empty() && file[0] == '<') return file;
    return std::filesystem::absolute(file).lexically_normal().generic_string();
}
}
uint64_t Debugger::add(const std::string &file, int line) {
    if (line < 1) throw std::invalid_argument("Breakpoint line must be positive");
    std::string path = normalized(file);
    for (auto &breakpoint : breakpoints) if (breakpoint.file == path && breakpoint.line == line) {
        breakpoint.enabled = true; return breakpoint.id;
    }
    uint64_t id = nextBreakpoint++; breakpoints.push_back({id, path, line, true}); return id;
}
bool Debugger::remove(uint64_t id) {
    auto found = std::find_if(breakpoints.begin(), breakpoints.end(), [&](const auto &point) { return point.id == id; });
    if (found == breakpoints.end()) return false;
    breakpoints.erase(found); return true;
}
bool Debugger::enable(uint64_t id, bool enabled) {
    for (auto &point : breakpoints) if (point.id == id) { point.enabled = enabled; return true; }
    return false;
}
bool Debugger::pause(const DebugSnapshot &snapshot) const {
    if (quitting() || snapshot.frames.empty() || snapshot.sequence <= sequence) return false;
    std::string file = normalized(snapshot.location.source->file);
    for (const auto &point : breakpoints) if (point.enabled && point.file == file && point.line == snapshot.location.line) return true;
    if (action == DebugAction::Continue) return false;
    if (action == DebugAction::Step) return true;
    if (snapshot.frames[0].id == frame) return action == DebugAction::Next;
    for (const auto &active : snapshot.frames) if (active.id == frame) return false;
    if (generator && std::find(snapshot.liveGenerators.begin(), snapshot.liveGenerators.end(), frame) != snapshot.liveGenerators.end()) return false;
    return true;
}
void Debugger::resume(DebugAction next, const DebugSnapshot &snapshot) {
    action = next; sequence = snapshot.sequence;
    frame = snapshot.frames.empty() ? 0 : snapshot.frames[0].id;
    generator = !snapshot.frames.empty() && snapshot.frames[0].generator;
}

namespace {
template<class Integer> bool number(const std::string &text, Integer &value) {
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
size_t prefixBytes(const std::string &text, size_t start, size_t maximum) {
    size_t end = std::min(text.size(), start + maximum);
    while (end > start && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) end--;
    if (end == start && start < text.size()) {
        end = start + 1;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) end++;
    }
    return end - start;
}
std::string clipped(const std::string &text, size_t maximum) {
    return text.substr(0, prefixBytes(text, 0, maximum));
}
void sourceLine(std::ostream &output, const Span &span) {
    const std::string &text = span.source->text;
    size_t offset = std::min(span.offset, text.size());
    size_t start = offset ? text.rfind('\n', offset - 1) : std::string::npos;
    start = start == std::string::npos ? 0 : start + 1;
    int line = span.line;
    for (int previous = 0; previous < 2 && start; previous++) {
        size_t newline = start > 1 ? text.rfind('\n', start - 2) : std::string::npos;
        start = newline == std::string::npos ? 0 : newline + 1; line--;
    }
    int width = static_cast<int>(std::to_string(span.line + 2).size());
    output << '\n';
    while (start <= text.size() && line <= span.line + 2) {
        size_t end = text.find('\n', start); if (end == std::string::npos) end = text.size();
        size_t visible = std::min(end, start + 96);
        if (visible < end) while (visible > start && (static_cast<unsigned char>(text[visible]) & 0xc0) == 0x80) visible--;
        output << (line == span.line ? " => " : "    ") << std::right << std::setw(width) << line << " | ";
        for (size_t position = start; position < visible; position++) {
            unsigned char byte = static_cast<unsigned char>(text[position]);
            output << (byte < 32 || byte == 127 ? ' ' : static_cast<char>(byte));
        }
        if (visible < end) output << "...";
        output << '\n';
        if (end == text.size()) break;
        start = end + 1; line++;
    }
    output << '\n';
}
void variables(std::ostream &output, const std::vector<DebugVariable> &values) {
    if (values.empty()) { output << "  (no bindings)\n\n"; return; }
    size_t names = 10, types = 10;
    for (const auto &value : values) { names = std::max(names, value.name.size()); types = std::max(types, value.type.size()); }
    names = std::min<size_t>(names, 32); types = std::min<size_t>(types, 24);
    output << "  " << std::left << std::setw(9) << "Binding" << std::setw(static_cast<int>(names + 2)) << "Name"
           << std::setw(static_cast<int>(types + 2)) << "Type" << "Value\n";
    output << "  " << std::string(names + types + 28, '-') << '\n';
    size_t indentation = 15 + names + types;
    size_t valueWidth = std::max<size_t>(24, 100 - indentation);
    for (const auto &value : values) {
        output << "  " << std::left << std::setw(9) << (value.mutableBinding ? "var" : "let")
            << std::setw(static_cast<int>(names + 2)) << clipped(value.name, names)
            << std::setw(static_cast<int>(types + 2)) << clipped(value.type, types);
        size_t offset = 0;
        do {
            size_t length = prefixBytes(value.value, offset, valueWidth);
            output << value.value.substr(offset, length) << '\n'; offset += length;
            if (offset < value.value.size()) output << std::string(indentation, ' ');
        } while (offset < value.value.size());
    }
    output << std::right << '\n';
}
void tasks(std::ostream &output, const DebugSnapshot &snapshot) {
    output << "\nTasks | Active scheduler entries\n";
    if (snapshot.tasks.empty()) { output << "  (no active tasks)\n\n"; return; }
    output << "  Task    Parent  State       Frame   Function                  Location\n";
    uint64_t current = snapshot.frames.empty() ? 0 : snapshot.frames.front().task;
    for (const auto &task : snapshot.tasks) output << (task.id == current ? "=>" : "  ") << std::left
        << std::setw(8) << task.id << std::setw(8) << (task.parent ? std::to_string(task.parent) : "-")
        << std::setw(12) << task.status << std::setw(8) << task.frame << std::setw(26) << clipped(task.name, 24)
        << task.location.source->file << ':' << task.location.line << '\n';
    if (snapshot.tasksTruncated) output << "  ... task preview limited to 128 entries\n";
    output << std::right << '\n';
}
}
DebugAction debugConsole(Debugger &debugger, const DebugSnapshot &snapshot,
                         std::istream &input, std::ostream &output, bool terminal) {
    size_t selected = 0;
    const auto &current = snapshot.frames.front();
        output << "\nPaused " << snapshot.location.source->file << ':' << snapshot.location.line
            << " in " << current.name << '\n';
        output << "  Frame #0 (id " << current.id << ")  |  " << snapshot.kind;
        if (current.task) output << "  |  Task " << current.task;
        if (current.generator) output << "  |  Generator";
        output << '\n'; sourceLine(output, snapshot.location);
    std::string line;
    for (;;) {
        if (terminal) { output << "(debug) "; output.flush(); }
        if (!std::getline(input, line)) return DebugAction::Quit;
        std::istringstream words(line); std::string command, argument; words >> command;
        std::getline(words, argument);
        size_t first = argument.find_first_not_of(" \t\r");
        argument = first == std::string::npos ? "" : argument.substr(first, argument.find_last_not_of(" \t\r") - first + 1);
        if (command.empty()) continue;
        if (command == "continue" || command == "c" || command == "step" || command == "s" ||
            command == "next" || command == "n" || command == "finish" || command == "f" || command == "quit" || command == "q") {
            if (!argument.empty()) { output << "debug: Command takes no argument\n"; continue; }
            if (command == "step" || command == "s") return DebugAction::Step;
            if (command == "next" || command == "n") return DebugAction::Next;
            if (command == "finish" || command == "f") return DebugAction::Finish;
            if (command == "quit" || command == "q") return DebugAction::Quit;
            return DebugAction::Continue;
        }
        if (command == "break" || command == "b") {
            size_t colon = argument.rfind(':');
            std::string file = colon == std::string::npos ? snapshot.location.source->file : argument.substr(0, colon);
            std::string location = colon == std::string::npos ? argument : argument.substr(colon + 1);
            int target = 0;
            if (file.empty() || !number(location, target) || target < 1) { output << "debug: Expected [file:]positive-line\n"; continue; }
            try { output << "Breakpoint " << debugger.add(file, target) << " at " << file << ':' << target << '\n'; }
            catch (const std::filesystem::filesystem_error &error) { output << "debug: " << error.what() << '\n'; }
        } else if (command == "breakpoints") {
            output << "\nBreakpoints\n  ID      State      Location\n";
            if (debugger.list().empty()) output << "  (no breakpoints)\n";
            for (const auto &point : debugger.list()) output << "  " << std::left << std::setw(8) << point.id
                << std::setw(11) << (point.enabled ? "enabled" : "disabled") << point.file << ':' << point.line << '\n';
            output << std::right << '\n';
        } else if (command == "delete" || command == "enable" || command == "disable") {
            uint64_t id = 0;
            if (!number(argument, id) || !(command == "delete" ? debugger.remove(id) : debugger.enable(id, command == "enable")))
                output << "debug: Unknown breakpoint ID\n";
        } else if (command == "bt" || command == "where") {
            output << "\nStack\n  Frame   Function                  ID      Task    Location\n";
            for (size_t index = 0; index < snapshot.frames.size(); index++) {
                const auto &frame = snapshot.frames[index];
                output << (index == selected ? "=>" : "  ") << std::left << std::setw(8) << ("#" + std::to_string(index))
                    << std::setw(26) << clipped(frame.name, 24) << std::setw(8) << frame.id
                    << std::setw(8) << (frame.task ? std::to_string(frame.task) : "-")
                    << frame.location.source->file << ':' << frame.location.line << '\n';
            }
            output << std::right << '\n';
        } else if (command == "frame") {
            size_t target = 0;
            if (!number(argument, target) || target >= snapshot.frames.size()) output << "debug: Unknown frame\n";
            else { selected = target; output << "Frame #" << selected << ' ' << snapshot.frames[selected].name << '\n'; sourceLine(output, snapshot.frames[selected].location); }
        } else if (command == "locals") { output << "\nLocals | Frame #" << selected << ' ' << snapshot.frames[selected].name << '\n'; variables(output, snapshot.frames[selected].locals); }
        else if (command == "globals") { output << "\nGlobals\n"; variables(output, snapshot.globals); }
        else if (command == "tasks") tasks(output, snapshot);
        else if (command == "list") sourceLine(output, snapshot.frames[selected].location);
        else if (command == "print" || command == "p") {
            const DebugVariable *found = nullptr;
            for (const auto &value : snapshot.frames[selected].locals) if (value.name == argument) found = &value;
            if (!found) for (const auto &value : snapshot.globals) if (value.name == argument) found = &value;
            if (found) output << found->name << " = " << found->value << '\n';
            else output << "debug: Binding not in snapshot; expressions are not executed\n";
        } else if (command == "help" || command == "h")
            output << "\nExecution\n  continue (c)   step (s)   next (n)   finish (f)   quit (q)\n\nBreakpoints\n  break (b) [file:]line   breakpoints   delete/enable/disable id\n\nInspection\n  bt   frame index   locals   globals   tasks   print (p) name   list\n\nRead-only previews: 64 bindings, 128 active Tasks. Native callbacks are stepped over.\n\n";
        else output << "debug: Unknown command; use help\n";
    }
}
}