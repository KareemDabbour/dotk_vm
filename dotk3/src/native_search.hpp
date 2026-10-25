#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// Extra roots searched for `.k3` imports after the importing script's directory (DOTK3_PATH, in order).
inline const std::vector<std::filesystem::path> &moduleRoots() {
    static const std::vector<std::filesystem::path> roots = [] {
        std::vector<std::filesystem::path> found;
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        if (const char *variable = std::getenv("DOTK3_PATH")) {
            std::string entries = variable;
            for (size_t start = 0; start <= entries.size();) {
                size_t end = std::min(entries.find(separator, start), entries.size());
                if (end > start) found.emplace_back(entries.substr(start, end - start));
                start = end + 1;
            }
        }
        return found;
    }();
    return roots;
}

// Where a relative module file resolves: beside the script, then DOTK3_PATH, then (for std/...) the built-in
// library dir; a missing module resolves beside the script so errors name that path.
inline std::filesystem::path findModule(const std::filesystem::path &directory, const std::string &filename) {
    std::error_code error;
    std::filesystem::path local = directory / filename;
    if (std::filesystem::exists(local, error))
        return local;
    for (const auto &root : moduleRoots())
        if (std::filesystem::is_regular_file(root / filename, error))
            return root / filename;
#ifdef DK3_LIBRARY_DIR
    // Only the std namespace is served, so the library dir's other folders are not importable.
    if (filename.rfind("std/", 0) == 0 && std::filesystem::is_regular_file(std::filesystem::path(DK3_LIBRARY_DIR) / filename, error))
        return std::filesystem::path(DK3_LIBRARY_DIR) / filename;
#endif
    return local;
}

// Where `import Name` looks for a native library beside a script, in priority order.
inline std::vector<std::filesystem::path> nativeLibraryCandidates(const std::filesystem::path &directory, const std::string &name) {
#if defined(__APPLE__)
    const char *extension = ".dylib";
#elif defined(_WIN32)
    const char *extension = ".dll";
#else
    const char *extension = ".so";
#endif
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return {directory / (name + extension), directory / ("lib" + name + extension), directory / (lower + "_module" + extension)};
}
