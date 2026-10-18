#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

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
