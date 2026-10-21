#include "aot.hpp"
#include "file_cli.hpp"
#include "http_cli.hpp"
#include "native_search.hpp"
#include "plugin_cli.hpp"
#include "runtime.hpp"
#include "socket_cli.hpp"
#include "terminal.hpp"
#include "typer.hpp"
#include <charconv>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <cstdlib>
#include <editline/readline.h>
#endif
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

static std::string readFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read " + path.string());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
static std::filesystem::path filePath(const std::filesystem::path &root, const std::string &name)
{
    std::filesystem::path relative(name);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw std::runtime_error("Files paths must be relative to the enabled root");
    for (const auto &part : relative)
        if (part == "..")
            throw std::runtime_error("Parent paths are not allowed for Files");
    auto resolved = std::filesystem::weakly_canonical(root / relative);
    auto inside = resolved.lexically_relative(root);
    if (inside.empty())
        throw std::runtime_error("Cannot resolve Files path below root");
    for (const auto &part : inside)
        if (part == "..")
            throw std::runtime_error("Files path resolves outside enabled root");
    return resolved;
}
static std::string readBoundedFile(const std::filesystem::path &path, size_t limit)
{
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("Files path is not a regular file");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open requested file");
    std::string bytes;
    char buffer[65536];
    for (;;)
    {
        size_t count = std::min(sizeof(buffer), limit - bytes.size());
        if (!count)
        {
            char extra;
            if (file.get(extra))
                throw std::length_error("File exceeds configured read limit");
            if (file.bad())
                throw std::runtime_error("File read failed");
            break;
        }
        file.read(buffer, static_cast<std::streamsize>(count));
        bytes.append(buffer, static_cast<size_t>(file.gcount()));
        if (file.bad())
            throw std::runtime_error("File read failed");
        if (file.eof())
            break;
        if (file.fail())
            throw std::runtime_error("File read failed");
    }
    return bytes;
}

static constexpr const char *version = "DotK 3 experimental alpha-63";
static const char *usage =
    "Usage: dotk3 [FILE.k3 [args...]]\n"
    "       dotk3 --repl\n"
    "       dotk3 -i FILE.k3 [args...]\n"
    "       dotk3 -e SOURCE [args...]\n"
    "       dotk3 - [args...]\n"
    "       dotk3 --check PATH [PATH...]\n"
    "       dotk3 --no-check [usual CLI arguments] (report type errors as warnings and run without type information)\n"
    "       dotk3 --ast FILE.k3\n"
    "       dotk3 --debug FILE.k3 [args...]\n"
    "       dotk3 --files-root DIR [usual CLI arguments]\n"
    "       dotk3 --files-root DIR --files-write [usual CLI arguments] (allow write/append/mkdir/remove/rename below DIR)\n"
    "       dotk3 --env [usual CLI arguments] (expose environment variables to System.env)\n"
    "       dotk3 --native LIB [--native LIB...] [usual CLI arguments] (load native modules; see examples/native)\n"
    "       dotk3 --net [usual CLI arguments]\n"
    "       dotk3 --steps N [usual CLI arguments] (0 disables step limit)\n"
    "       dotk3 --engine bytecode [usual CLI arguments]\n"
    "       dotk3 -O0|-O1|-O2 [usual CLI arguments] (default -O2)\n"
    "       dotk3 --byte-code FILE.k3 (print compiled bytecode and exit)\n"
    "       dotk3 --compile FILE.k3 [-o FILE.dotbc] (compile to portable bytecode)\n"
    "       dotk3 FILE.dotbc (run a pre-compiled .dotbc file)\n"
    "       dotk3 --profile [--profile-output FILE] [usual CLI arguments]\n"
    "       dotk3 --profile-vm simple|verbose [--profile-trace FILE] [usual CLI arguments]\n"
    "       dotk3 --profile-interval-ms N [profiling arguments]\n"
    "       dotk3 --profile-samples N [profiling arguments]\n"
    "       dotk3 --help | --version\n";

static bool terminalInput()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

static void report(const std::exception &error)
{
    bool color = false;
#ifdef _WIN32
    color = _isatty(_fileno(stderr)) != 0;
#else
    color = isatty(STDERR_FILENO) != 0;
#endif
    const char *term = std::getenv("TERM");
    color = color && !std::getenv("NO_COLOR") && (!term || std::string(term) != "dumb");
    if (const auto *language = dynamic_cast<const dk3::Error *>(&error))
        std::cerr << dk3::diagnose(*language, color) << '\n';
    else
        std::cerr << "dotk3: " << error.what() << '\n';
}
static bool debuggerStopped(const std::exception &error, const dk3::Debugger &debugger)
{
    const auto *language = dynamic_cast<const dk3::Error *>(&error);
    return debugger.quitting() && language && language->category == "cancelled" &&
           std::string(language->what()) == "Debugger stopped execution";
}
using SessionHandle = std::shared_ptr<std::shared_ptr<dk3::TypeSession>>;
static bool noCheck = false; // --no-check: diagnostics become warnings and the program runs without type annotations
static NativePlugins nativePlugins;

static dk3::ModuleLoader moduleLoader(const std::filesystem::path &directory)
{
    return [directory](const std::string &name)
    {
        std::filesystem::path relative(name);
        if (relative.is_absolute() || name.find("..") != std::string::npos)
            throw std::runtime_error("Module paths must remain below the script directory");
        std::string filename = name;
        if (relative.extension() != ".k3")
        {
            std::replace(filename.begin(), filename.end(), '.', '/');
            filename += ".k3";
        }
        std::filesystem::path path = directory / filename;
        return std::make_shared<const dk3::Source>(dk3::Source{path.string(), readFile(path)});
    };
}

// Prints diagnostics and returns true when there were no errors.
static bool printDiagnostics(const std::vector<dk3::Diagnostic> &diagnostics, bool showWarnings)
{
    bool clean = true;
    for (const auto &diagnostic : diagnostics)
    {
        if (diagnostic.warning && !showWarnings)
            continue;
        bool color = false;
#ifndef _WIN32
        const char *term = std::getenv("TERM");
        color = isatty(STDERR_FILENO) && !std::getenv("NO_COLOR") && (!term || std::string(term) != "dumb");
#endif
        std::cerr << dk3::formatDiagnostic(diagnostic, color) << '\n';
        if (!diagnostic.warning)
            clean = false;
    }
    return clean;
}

// `import Name` loads a native library named Name.<ext>, libName.<ext> or name_module.<ext> beside the script
// when no Name.k3 exists. Imported .k3 modules are searched too so their native imports are found.
static void loadImportedNatives(const dk3::Ast &node, const std::filesystem::path &directory, std::set<std::string> &seen)
{
    if (!node)
        return;
    if (node->kind == "import" || node->kind == "from_import")
    {
        const std::string &name = node->text;
        bool plain = !name.empty() && name.find_first_of("./\\") == std::string::npos;
        if (plain && seen.insert(name).second)
        {
            std::filesystem::path source = directory / (name + ".k3");
            if (std::filesystem::is_regular_file(source))
            {
                try
                {
                    loadImportedNatives(dk3::parse(std::make_shared<dk3::Source>(dk3::Source{source.string(), readFile(source)})), directory, seen);
                }
                catch (const std::exception &)
                {
                } // reported when the module is checked
            }
            else
            {
                for (const auto &library : nativeLibraryCandidates(directory, name))
                {
                    if (!std::filesystem::is_regular_file(library))
                        continue;
                    if (nativePlugins.load(library.string()) != name)
                        throw std::runtime_error("'" + library.string() + "' does not provide a module named " + name);
                    break;
                }
            }
        }
    }
    for (const auto &child : node->children)
        loadImportedNatives(child, directory, seen);
}

// Type-checks one REPL submission; with --no-check problems are only reported.
static bool checkSubmission(const std::shared_ptr<dk3::TypeSession> &session, const dk3::Ast &program)
{
    auto diagnostics = session->check(program);
    if (noCheck)
    {
        for (auto &diagnostic : diagnostics)
            diagnostic.warning = true;
        printDiagnostics(diagnostics, true);
        return true;
    }
    return printDiagnostics(diagnostics, false);
}

static bool staticCheck(const dk3::Ast &program, const std::filesystem::path &directory)
{
    dk3::TypeSession session(moduleLoader(directory));
    nativePlugins.declare(session);
    return printDiagnostics(session.check(program, false), true);
}

static int checkPaths(const std::vector<std::filesystem::path> &inputs)
{
    std::vector<std::filesystem::path> files;
    std::set<std::filesystem::path> seen;
    int exitCode = 0;
    bool expanded = inputs.size() > 1;
    auto add = [&](const std::filesystem::path &path)
    {
        auto identity = path == "-" ? path : std::filesystem::absolute(path).lexically_normal();
        if (seen.insert(identity).second)
            files.push_back(path);
    };
    for (const auto &path : inputs)
    {
        try
        {
            if (std::filesystem::is_directory(path))
            {
                expanded = true;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(path))
                    if (entry.is_regular_file() && entry.path().extension() == ".k3")
                        add(entry.path());
            }
            else
                add(path);
        }
        catch (const std::exception &error)
        {
            report(error);
            exitCode = 70;
        }
    }
    if (files.empty())
    {
        if (!exitCode)
        {
            std::cerr << "dotk3: No .k3 files found to check\n";
            return 64;
        }
        return exitCode;
    }
    std::sort(files.begin(), files.end());
    size_t passed = 0;
    for (const auto &file : files)
    {
        try
        {
            std::string text, filename = file == "-" ? "<stdin>" : file.string();
            if (file == "-")
            {
                std::ostringstream contents;
                contents << std::cin.rdbuf();
                text = contents.str();
            }
            else
                text = readFile(file);
            auto program = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{filename, text}));
            std::filesystem::path directory = file == "-" ? std::filesystem::current_path() : std::filesystem::absolute(file).parent_path();
            if (!staticCheck(program, directory))
            {
                if (!exitCode)
                    exitCode = 65;
                continue;
            }
            passed++;
            std::cout << "check: PASS" << (expanded ? " " + filename : "") << '\n';
        }
        catch (const dk3::Error &error)
        {
            report(error);
            if (!exitCode)
                exitCode = 65;
        }
        catch (const std::exception &error)
        {
            report(error);
            exitCode = 70;
        }
    }
    if (expanded)
        std::cout << "check: " << passed << '/' << files.size() << " files passed\n";
    return exitCode;
}
static void printBytecode(const dk3::Bytecode &code, const std::string &label, int depth = 0)
{
    std::string indent(static_cast<size_t>(depth * 2), ' ');
    std::cout << indent << "fn " << label
              << " (params=" << code.parameters
              << " captures=" << code.captures
              << " locals=" << code.locals.size() << ")\n";
    for (size_t i = 0; i < code.instructions.size(); i++)
    {
        const auto &ins = code.instructions[i];
        std::cout << indent << "  " << std::setw(4) << i << "  " << dk3::byteOpName(ins.op);
        switch (ins.op)
        {
        case dk3::ByteOp::Int:
            std::cout << ' ' << ins.integer;
            break;
        case dk3::ByteOp::Float:
            std::cout << ' ' << ins.floating;
            break;
        case dk3::ByteOp::Bool:
            std::cout << ' ' << (ins.operand ? "true" : "false");
            break;
        case dk3::ByteOp::Text:
            if (ins.node)
                std::cout << ' ' << std::quoted(ins.node->text);
            break;
        case dk3::ByteOp::LoadLocal:
        case dk3::ByteOp::StoreLocal:
        case dk3::ByteOp::DeclareLocal:
        case dk3::ByteOp::ClearLocal:
        case dk3::ByteOp::CheckLocal:
            std::cout << ' ' << ins.operand;
            if (ins.operand < code.locals.size())
                std::cout << " (" << code.locals[ins.operand].name << ')';
            break;
        case dk3::ByteOp::LoadName:
        case dk3::ByteOp::StoreName:
        case dk3::ByteOp::DeclareName:
        case dk3::ByteOp::CheckName:
            if (ins.operand < code.names.size())
                std::cout << ' ' << code.names[ins.operand];
            break;
        case dk3::ByteOp::Jump:
        case dk3::ByteOp::JumpFalse:
        case dk3::ByteOp::JumpTrue:
        case dk3::ByteOp::JumpPresent:
        case dk3::ByteOp::JumpNull:
            std::cout << ' ' << ins.operand;
            break;
        case dk3::ByteOp::Call:
        case dk3::ByteOp::CallMethod:
        case dk3::ByteOp::InvokeMethod:
        case dk3::ByteOp::List:
        case dk3::ByteOp::Tuple:
        case dk3::ByteOp::Map:
        case dk3::ByteOp::Format:
            std::cout << ' ' << ins.operand;
            break;
        case dk3::ByteOp::Closure:
            std::cout << ' ' << ins.operand;
            break;
        case dk3::ByteOp::Member:
        case dk3::ByteOp::SetMember:
        case dk3::ByteOp::PrepareMember:
        case dk3::ByteOp::ResolveMethod:
        case dk3::ByteOp::Import:
        case dk3::ByteOp::DefineFunction:
        case dk3::ByteOp::DeclareType:
            if (ins.node)
                std::cout << ' ' << ins.node->text;
            break;
        case dk3::ByteOp::Binary:
        case dk3::ByteOp::Unary:
            if (ins.node)
                std::cout << ' ' << ins.node->text;
            break;
        default:
            break;
        }
        if (ins.node)
            std::cout << "  ; " << ins.node->span.line << ':' << ins.node->span.column;
        std::cout << '\n';
    }
    // Print nested closures
    for (size_t i = 0; i < code.closures.size(); i++)
    {
        if (code.closures[i].code)
            printBytecode(*code.closures[i].code, label + "/closure[" + std::to_string(i) + "]", depth + 1);
    }
    // Print match arm bytecode
    for (size_t i = 0; i < code.matches.size(); i++)
    {
        for (size_t j = 0; j < code.matches[i].arms.size(); j++)
        {
            if (code.matches[i].arms[j].code)
                printBytecode(*code.matches[i].arms[j].code, label + "/match[" + std::to_string(i) + "].arm[" + std::to_string(j) + "]", depth + 1);
        }
    }
}

static void printAst(const dk3::Ast &node, int depth = 0)
{
    if (!node)
        return;
    std::cout << std::string(static_cast<size_t>(depth), ' ') << node->kind << ' ' << node->text
              << " @" << node->span.line << ':' << node->span.column << '\n';
    for (const auto &child : node->children)
        printAst(child, depth + 2);
}

static int shell(std::unique_ptr<dk3::Runtime> &runtime, const dk3::Config &config, bool terminal, const SessionHandle &session, const std::filesystem::path &directory)
{
    if (terminal)
        std::cout << version << "\n:help for shell commands\n";
#ifndef _WIN32
    if (terminal)
    {
        using_history();
        stifle_history(500);
    }
#endif
    std::string pending, line;
    size_t submission = 1;
    for (;;)
    {
        runtime->restoreTerminal();
#ifndef _WIN32
        if (terminal)
        {
            char *edited = readline(pending.empty() ? "> " : "... ");
            if (!edited)
                break;
            line = edited;
            std::free(edited);
            if (!line.empty())
                add_history(line.c_str());
        }
        else
#endif
        {
            if (terminal)
            {
                std::cout << (pending.empty() ? "> " : "... ");
                std::cout.flush();
            }
            if (!std::getline(std::cin, line))
                break;
        }
        if (pending.empty() && !line.empty() && line[0] == ':')
        {
            std::istringstream input(line);
            std::string command;
            input >> command;
            if (command == ":members")
            {
                std::string path, extra;
                input >> path;
                if (path.empty() || (input >> extra))
                {
                    std::cerr << "dotk3: :members NAME[.MEMBER...]\n";
                    continue;
                }
                try
                {
                    auto value = runtime->getPath(path);
                    std::cout << dk3::Runtime::typeOf(value) << " members\n";
                    auto fields = runtime->inspectMembers(value);
                    if (fields.empty())
                        std::cout << "  (no named members)\n";
                    for (const auto &field : fields)
                        std::cout << "  " << field.name << ": " << field.type << " = " << field.value << '\n';
                }
                catch (const std::exception &error)
                {
                    report(error);
                }
                continue;
            }
            if (command == ":load" || command == ":check" || command == ":ast")
            {
                std::vector<std::filesystem::path> paths;
                std::string argument;
                bool invalidPath = false;
                while (input >> std::ws && input.peek() != std::char_traits<char>::eof())
                {
                    if (!(input >> std::quoted(argument)))
                    {
                        invalidPath = true;
                        break;
                    }
                    paths.emplace_back(argument);
                }
                if (invalidPath)
                {
                    std::cerr << "dotk3: Invalid quoted path\n";
                    continue;
                }
                if (paths.empty() || (command != ":check" && paths.size() != 1) ||
                    std::any_of(paths.begin(), paths.end(), [](const auto &path)
                                { return path == "-" || path.empty(); }))
                {
                    std::cerr << "dotk3: " << command << (command == ":check" ? " PATH [PATH...]\n" : " FILE.k3\n");
                    continue;
                }
                try
                {
                    if (command == ":check")
                        checkPaths(paths);
                    else
                    {
                        const auto &path = paths.front();
                        auto source = std::make_shared<dk3::Source>(dk3::Source{path.string(), readFile(path)});
                        if (command == ":ast")
                            printAst(dk3::parse(source));
                        else
                        {
                            auto program = dk3::parse(source);
                            if (checkSubmission(*session, program))
                                runtime->run(program);
                        }
                    }
                }
                catch (const std::bad_alloc &)
                {
                    throw;
                }
                catch (const std::exception &error)
                {
                    report(error);
                }
                continue;
            }
            if (line == ":quit" || line == ":q")
                break;
            if (line == ":help")
                std::cout << ":help  :reset  :quit\n:load FILE.k3  :ast FILE.k3  :check PATH [PATH...]\n:members NAME[.MEMBER...]\nQuote paths containing spaces. Load runs code in this session; check/ast do not.\n";
            else if (line == ":reset")
            {
                try
                {
                    runtime->cancelAll();
                }
                catch (const std::exception &error)
                {
                    report(error);
                }
                runtime = std::make_unique<dk3::Runtime>(config);
                *session = std::make_shared<dk3::TypeSession>(moduleLoader(directory));
                nativePlugins.attach(*runtime);
                nativePlugins.declare(**session);
            }
            else
                std::cerr << "dotk3: Unknown shell command; use :help\n";
            continue;
        }
        pending += line;
        dk3::Ast program;
        try
        {
            program = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"<repl:" + std::to_string(submission) + ">", pending}));
        }
        catch (const dk3::Error &error)
        {
            if (error.incomplete)
            {
                pending += '\n';
                continue;
            }
            report(error);
            pending.clear();
            submission++;
            continue;
        }
        try
        {
            if (checkSubmission(*session, program))
                runtime->run(program, true);
        }
        catch (const std::bad_alloc &)
        {
            throw;
        }
        catch (const std::exception &error)
        {
            report(error);
        }
        pending.clear();
        submission++;
    }
    if (!pending.empty())
    {
        try
        {
            dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"<repl:" + std::to_string(submission) + ">", pending}));
        }
        catch (const dk3::Error &error)
        {
            report(error);
            return 65;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool terminal = terminalInput();
    int start = 1;
    std::optional<std::filesystem::path> filesRoot;
    bool networkEnabled = false;
    bool filesWrite = false, environmentEnabled = false;
    std::vector<std::string> nativePaths;
    uint64_t stepLimit = 0;
    unsigned optimizationLevel = 2;
    dk3::ProfileOptions profileOptions;
    std::optional<std::filesystem::path> profileOutput, profileTrace;
    while (start < argc)
    {
        if (std::string(argv[start]) == "--net")
        {
            networkEnabled = true;
            start++;
        }
        else if (std::string(argv[start]) == "--static")
            start++;
        else if (std::string(argv[start]) == "--no-check")
        {
            noCheck = true;
            start++;
        }
        else if (std::string(argv[start]) == "--profile")
        {
            profileOptions.source = true;
            start++;
        }
        else if (std::string(argv[start]) == "--profile-vm")
        {
            if (start + 1 >= argc || (std::string(argv[start + 1]) != "simple" && std::string(argv[start + 1]) != "verbose"))
            {
                std::cerr << usage;
                return 64;
            }
            profileOptions.vm = std::string(argv[start + 1]) == "simple" ? dk3::VMProfile::Simple : dk3::VMProfile::Verbose;
            start += 2;
        }
        else if (std::string(argv[start]) == "--profile-output" || std::string(argv[start]) == "--profile-trace")
        {
            if (start + 1 >= argc || !*argv[start + 1])
            {
                std::cerr << usage;
                return 64;
            }
            if (std::string(argv[start]) == "--profile-output")
            {
                profileOutput = argv[start + 1];
                profileOptions.source = true;
            }
            else
                profileTrace = argv[start + 1];
            start += 2;
        }
        else if (std::string(argv[start]) == "--profile-interval-ms")
        {
            if (start + 1 >= argc)
            {
                std::cerr << usage;
                return 64;
            }
            std::string value = argv[start + 1];
            uint64_t interval = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), interval);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || interval < 1 || interval > 1000)
            {
                std::cerr << "dotk3: Profile interval must be 1..1000 ms\n";
                return 64;
            }
            profileOptions.intervalNanos = interval * 1000000;
            start += 2;
        }
        else if (std::string(argv[start]) == "--profile-samples")
        {
            if (start + 1 >= argc)
            {
                std::cerr << usage;
                return 64;
            }
            std::string value = argv[start + 1];
            size_t samples = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), samples);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || samples < 1)
            {
                std::cerr << "dotk3: Profile sample limit must be a positive integer\n";
                return 64;
            }
            profileOptions.maxSamples = samples;
            start += 2;
        }
        else if (std::string(argv[start]) == "-O0" || std::string(argv[start]) == "-O1" || std::string(argv[start]) == "-O2")
        {
            optimizationLevel = static_cast<unsigned>(argv[start][2] - '0');
            start++;
        }
        else if (std::string(argv[start]) == "--engine")
        {
            if (start + 1 >= argc || std::string(argv[start + 1]) != "bytecode")
            {
                std::cerr << "dotk3: The execution engine is bytecode; AST interpretation is not available\n";
                return 64;
            }
            start += 2;
        }
        else if (std::string(argv[start]) == "--steps")
        {
            if (start + 1 >= argc)
            {
                std::cerr << usage;
                return 64;
            }
            std::string value = argv[start + 1];
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), stepLimit);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            {
                std::cerr << "dotk3: Invalid step limit\n";
                return 64;
            }
            start += 2;
        }
        else if (std::string(argv[start]) == "--files-write")
        {
            filesWrite = true;
            start++;
        }
        else if (std::string(argv[start]) == "--env")
        {
            environmentEnabled = true;
            start++;
        }
        else if (std::string(argv[start]) == "--native")
        {
            if (start + 1 >= argc || !*argv[start + 1])
            {
                std::cerr << usage;
                return 64;
            }
            nativePaths.push_back(argv[start + 1]);
            start += 2;
        }
        else if (std::string(argv[start]) == "--files-root")
        {
            if (start + 1 >= argc || !*argv[start + 1])
            {
                std::cerr << usage;
                return 64;
            }
            filesRoot = std::filesystem::path(argv[start + 1]);
            start += 2;
        }
        else
            break;
    }
    int remaining = argc - start;
    std::string first = remaining ? argv[start] : "";
    if (remaining == 1 && (first == "--help" || first == "-h"))
    {
        std::cout << version << '\n'
                  << usage;
        return 0;
    }
    if (remaining == 1 && first == "--version")
    {
        std::cout << version << '\n';
        return 0;
    }
    bool byteCodeOnly = first == "--byte-code";
    bool compileOnly = first == "--compile";
    std::optional<std::filesystem::path> compileOutput;
    bool astOnly = first == "--ast", checkOnly = first == "--check";
    bool profiling = profileOptions.source || profileOptions.vm != dk3::VMProfile::Off;
    if (profileOutput && profileTrace && std::filesystem::absolute(*profileOutput).lexically_normal() == std::filesystem::absolute(*profileTrace).lexically_normal())
    {
        std::cerr << "dotk3: Source and VM trace exports need different paths\n";
        return 64;
    }
    if ((profileTrace && profileOptions.vm != dk3::VMProfile::Verbose) || (profiling && (astOnly || byteCodeOnly || checkOnly || first == "--debug")))
    {
        std::cerr << "dotk3: Profiling requires execution without --debug; --profile-trace requires --profile-vm verbose\n";
        return 64;
    }
    bool debug = first == "--debug";
    bool evaluate = first == "-e" || first == "--eval";
    bool interactiveAfter = first == "-i";
    // Detect .dotbc run mode: dotk3 FILE.dotbc
    bool dotbcRun = remaining >= 1 && std::filesystem::path(first).extension() == ".dotbc" && first[0] != '-';
    bool repl = first == "--repl" || (!remaining && terminal) || (interactiveAfter && remaining == 1);
    int index = start + (astOnly || byteCodeOnly || compileOnly || checkOnly || evaluate || interactiveAfter || debug || first == "--" ? 1 : 0);
    // --compile may have -o <output> after the source file
    if (compileOnly && index + 1 < argc && std::string(argv[index + 1]) == "-o" && index + 2 < argc)
    {
        compileOutput = argv[index + 2];
    }
    if ((first == "--repl" && remaining != 1) || (astOnly && remaining != 2) || (byteCodeOnly && remaining != 2) || (compileOnly && remaining < 2) || (checkOnly && remaining < 2) ||
        (evaluate && remaining < 2) || (debug && (remaining < 2 || std::string(argv[start + 1]) == "-")) || (first == "--" && remaining < 2) ||
        (!repl && remaining && index == start && first != "-" && !first.empty() && first[0] == '-'))
    {
        std::cerr << usage;
        return 64;
    }
    std::unique_ptr<dk3::Runtime> runtime;
    dk3::Debugger debugger;
    std::shared_ptr<dk3::Profiler> profiler;
#ifndef _WIN32
    std::shared_ptr<SocketDomain> sockets;
#endif
    int exitCode = 0;
    try
    {
        for (const auto &path : nativePaths)
            nativePlugins.load(path);
        if (checkOnly)
        {
            std::vector<std::filesystem::path> inputs;
            for (int argument = start + 1; argument < argc; argument++)
                inputs.emplace_back(argv[argument]);
            return checkPaths(inputs);
        }
        bool fromStdin = !repl && (!remaining || (index < argc && std::string(argv[index]) == "-"));
        std::string filename = evaluate ? "<eval>" : fromStdin  ? "<stdin>"
                                                 : index < argc ? argv[index]
                                                                : "<repl>";
        std::filesystem::path directory = evaluate || fromStdin || repl ? std::filesystem::current_path() : std::filesystem::absolute(filename).parent_path();
        dk3::Config config;
        config.stepLimit = stepLimit;
        config.optimizationLevel = optimizationLevel;
        if (profiling)
        {
            profiler = std::make_shared<dk3::Profiler>(profileOptions);
            config.profiler = profiler;
        }
#ifndef _WIN32
        if (networkEnabled && !astOnly)
        {
            sockets = std::make_shared<SocketDomain>();
            config.listenSocket = [sockets](const std::string &host, uint16_t port)
            { return sockets->listen(host, port); };
            config.connectSocket = [sockets](const std::string &host, uint16_t port, uint64_t timeout)
            { return sockets->connect(host, port, timeout); };
            config.httpRequest = [](const dk3::HttpRequest &request)
            { return CurlHttp::perform(request); };
        }
#else
        if (networkEnabled)
            throw std::runtime_error("CLI sockets are not supported on this platform");
#endif
        if (!repl)
            for (int argument = index + 1; argument < argc; argument++)
                config.arguments.push_back(argv[argument]);
        config.write = [](const std::string &output)
        { std::cout << output; std::cout.flush(); };
        config.readLine = []() -> std::optional<std::string>
        {
            std::string line;
            char byte;
            while (std::cin.get(byte))
            {
                if (byte == '\n')
                    break;
                if (line.size() == 64 * 1024)
                    throw std::runtime_error("Input line exceeds CLI limit");
                line += byte;
            }
            if (line.empty() && !std::cin)
                return {};
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return line;
        };
        config.monotonicMillis = []
        { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
        config.delay = [](uint64_t milliseconds)
        { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); };
        config.wallMillis = []
        { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
        if (environmentEnabled)
            config.environment = [](const std::string &name) -> std::optional<std::string>
            {
                const char *value = std::getenv(name.c_str());
                if (!value)
                    return {};
                return std::string(value);
            };
        if (filesWrite && !filesRoot)
            throw std::runtime_error("--files-write requires --files-root");
#ifndef _WIN32
        if (terminal && !fromStdin && !debug && !astOnly)
        {
            auto session = std::make_shared<TerminalSession>();
            config.terminalMode = [session](bool enabled)
            { session->mode(enabled); };
            config.readKey = [session](uint64_t timeout)
            { return session->read(timeout); };
        }
#endif
        if (filesRoot && !astOnly)
        {
            auto root = std::filesystem::canonical(*filesRoot);
            if (!std::filesystem::is_directory(root))
                throw std::runtime_error("Files root must be a directory");
            config.readFile = [root](const std::string &name, size_t limit)
            { return readBoundedFile(filePath(root, name), limit); };
            config.fileSize = [root](const std::string &name) -> std::optional<uint64_t>
            {
                auto path = filePath(root, name);
                if (!std::filesystem::exists(path))
                    return {};
                if (!std::filesystem::is_regular_file(path))
                    throw std::runtime_error("Files path is not a regular file");
                return std::filesystem::file_size(path);
            };
            config.fileSystem = sandboxedFileSystem([root](const std::string &name)
                                                    { return filePath(root, name); }, filesWrite);
        }
        if (debug)
            config.debug = [&](const dk3::DebugSnapshot &snapshot)
            {
                if (!debugger.pause(snapshot))
                    return;
                debugger.resume(dk3::debugConsole(debugger, snapshot, std::cin, std::cout, terminal), snapshot);
                if (debugger.quitting())
                    throw dk3::Error(snapshot.location, "Debugger stopped execution", "cancelled");
            };
        SessionHandle session = std::make_shared<std::shared_ptr<dk3::TypeSession>>();
        *session = std::make_shared<dk3::TypeSession>(moduleLoader(directory));
        if (!noCheck)
            config.resolveProgram = [session](const std::string &name) -> dk3::Ast
            { return (*session)->module(name); };
        config.resolveSource = [directory](const std::string &name)
        {
            std::filesystem::path relative(name);
            if (relative.is_absolute() || name.find("..") != std::string::npos)
                throw std::runtime_error("Module paths must remain below the script directory");
            std::string filename = name;
            if (relative.extension() != ".k3")
            {
                std::replace(filename.begin(), filename.end(), '.', '/');
                filename += ".k3";
            }
            std::filesystem::path path = directory / filename;
            return std::make_shared<dk3::Source>(dk3::Source{path.string(), readFile(path)});
        };
        if (!repl)
        {
            std::string text;
            if (evaluate)
                text = argv[index];
            else if (fromStdin)
            {
                std::ostringstream input;
                input << std::cin.rdbuf();
                text = input.str();
            }
            else
                text = readFile(filename);
            auto source = std::make_shared<dk3::Source>(dk3::Source{filename, text});
            if (astOnly)
            {
                printAst(dk3::parse(source));
                return 0;
            }
            if (byteCodeOnly)
            {
                auto code = dk3::compileBytecode(dk3::parse(source), optimizationLevel, false, true);
                if (!code)
                {
                    std::cerr << "dotk3: Failed to compile bytecode\n";
                    return 65;
                }
                printBytecode(*code, filename);
                return 0;
            }
            if (compileOnly)
            {
                dk3::Ast program = dk3::parse(source);
                {
                    std::set<std::string> seen;
                    loadImportedNatives(program, directory, seen);
                }
                nativePlugins.declare(**session);
                if (!printDiagnostics((*session)->check(program, false), false))
                    return 65;
                auto pool = dk3::compileAllBytecodes(program, optimizationLevel);
                auto bytes = dk3::serializeAot(filename, text, pool);
                std::filesystem::path outPath = compileOutput.value_or(
                    std::filesystem::path(filename).replace_extension(".dotbc"));
                std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
                if (!out || !(out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) || !out.flush())
                    throw std::runtime_error("Cannot write " + outPath.string());
                std::cout << "Compiled " << filename << " -> " << outPath.string()
                          << " (" << pool.size() << " functions, " << bytes.size() << " bytes)\n";
                return 0;
            }
            if (dotbcRun)
            {
                // Load pre-compiled .dotbc: extract source and inject AOT bytecodes.
                auto aot = dk3::deserializeAot(std::vector<uint8_t>(
                    reinterpret_cast<const uint8_t *>(text.data()),
                    reinterpret_cast<const uint8_t *>(text.data()) + text.size()));
                // Build shared AOT map with the tuple key format Config expects.
                using AotMap = std::map<std::tuple<std::string, uint32_t, uint32_t, bool>,
                                        std::shared_ptr<const dk3::Bytecode>>;
                auto aotMap = std::make_shared<AotMap>();
                for (auto &[k, v] : aot.pool)
                    (*aotMap)[{k.file, k.line, k.col, k.hasSelf}] = v;
                config.aotBytecodes = aotMap;
                // Re-parse from bundled source (skip type-checking via noCheck path).
                auto aotSource = std::make_shared<dk3::Source>(dk3::Source{aot.sourceFile, aot.source});
                directory = std::filesystem::absolute(aot.sourceFile).parent_path();
                {
                    std::set<std::string> seen;
                    loadImportedNatives(dk3::parse(aotSource), directory, seen);
                }
                nativePlugins.declare(**session);
                auto diagnostics = (*session)->check(dk3::parse(aotSource), false);
                for (auto &d : diagnostics)
                    d.warning = true;
                printDiagnostics(diagnostics, true);
                runtime = std::make_unique<dk3::Runtime>(config);
                nativePlugins.attach(*runtime);
                runtime->run(aot.source, aot.sourceFile);
                goto done;
            }
            dk3::Ast checked = dk3::parse(source);
            if (!evaluate && !fromStdin)
            {
                std::set<std::string> seen;
                loadImportedNatives(checked, directory, seen);
            }
            nativePlugins.declare(**session);
            if (noCheck)
            {
                auto diagnostics = (*session)->check(checked, false);
                for (auto &diagnostic : diagnostics)
                    diagnostic.warning = true;
                printDiagnostics(diagnostics, true);
                runtime = std::make_unique<dk3::Runtime>(config);
                nativePlugins.attach(*runtime);
                runtime->run(text, filename);
            }
            else
            {
                if (!printDiagnostics((*session)->check(checked, false), false))
                    return 65;
                runtime = std::make_unique<dk3::Runtime>(config);
                nativePlugins.attach(*runtime);
                runtime->run(checked);
            }
        }
        else
        {
            nativePlugins.declare(**session);
            runtime = std::make_unique<dk3::Runtime>(config);
            nativePlugins.attach(*runtime);
        }
        if (repl || interactiveAfter)
            exitCode = shell(runtime, config, terminal, session, directory);
    done:;
    }
    catch (const dk3::Error &error)
    {
        if (!debuggerStopped(error, debugger))
        {
            report(error);
            exitCode = 65;
        }
    }
    catch (const std::exception &error)
    {
        report(error);
        exitCode = 70;
    }
    if (runtime)
    {
        try
        {
            runtime->cancelAll();
        }
        catch (const std::exception &error)
        {
            if (!debuggerStopped(error, debugger))
            {
                report(error);
                if (!exitCode)
                    exitCode = 65;
            }
        }
        try
        {
            runtime->restoreTerminal();
        }
        catch (const std::exception &error)
        {
            report(error);
            if (!exitCode)
                exitCode = 70;
        }
    }
    if (debug && !exitCode)
        std::cout << (debugger.quitting() ? "debug: stopped\n" : "debug: finished\n");
    nativePlugins.detach();
#ifndef _WIN32
    if (sockets)
        sockets->close();
#endif
    if (profiler)
    {
        try
        {
            std::cerr << profiler->report(profileOptions.vm == dk3::VMProfile::Verbose);
            if (profiler->failed())
                throw std::runtime_error("Profiler recording failed; exports are incomplete");
            auto writeProfile = [](const std::filesystem::path &path, const std::string &text)
            {
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                if (!output || !(output << text << '\n') || !output.flush())
                    throw std::runtime_error("Cannot write profile output: " + path.string());
            };
            if (profileOutput)
                writeProfile(*profileOutput, profiler->speedscope());
            if (profileTrace)
                writeProfile(*profileTrace, profiler->perfetto());
        }
        catch (const std::exception &error)
        {
            report(error);
            if (!exitCode)
                exitCode = 70;
        }
    }
    return exitCode;
}