#pragma once
#include "runtime.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// stdio-backed File resource for the CLI; every path is resolved through the sandbox callback first.
class StdioFile final : public dk3::FileHandle {
    std::FILE *file;
    char *lineBuffer = nullptr;
    size_t lineCapacity = 0;
    enum class Last { None, Read, Write } last = Last::None;
    static std::runtime_error failure(const char *operation) { return std::runtime_error(std::string(operation) + ": " + std::strerror(errno)); }
    void require() const { if (closed) throw std::runtime_error("File is closed"); }
    void switchTo(Last next) {
        if (last != Last::None && last != next && std::fseek(file, 0, SEEK_CUR) != 0) throw failure("seek");
        last = next;
    }
public:
    explicit StdioFile(std::FILE *file) : file(file) { std::setvbuf(file, nullptr, _IOFBF, 1 << 16); }
    ~StdioFile() override {
        std::free(lineBuffer);
        if (!closed) { closed = true; std::fclose(file); }
    }
    void close() override {
        if (closed) return;
        closed = true;
        if (std::fclose(file) != 0) throw failure("close");
    }
    // Grows with the data actually read, so a huge `maximum` does not preallocate.
    std::string read(size_t maximum) override {
        require(); switchTo(Last::Read);
        std::string bytes;
        char chunk[1 << 16];
        while (bytes.size() < maximum) {
            size_t wanted = std::min(sizeof(chunk), maximum - bytes.size());
            size_t count = std::fread(chunk, 1, wanted, file);
            bytes.append(chunk, count);
            if (count < wanted) {
                if (std::ferror(file)) throw failure("read");
                break;
            }
        }
        return bytes;
    }
    std::optional<std::string> readLine(size_t maximum) override {
        require(); switchTo(Last::Read);
        std::string line;
#ifdef _WIN32
        int byte;
        while ((byte = std::fgetc(file)) != EOF && byte != '\n')
            line += static_cast<char>(byte);
        if (byte == EOF) { if (std::ferror(file)) throw failure("read"); if (line.empty()) return {}; }
#else
        ssize_t count = ::getline(&lineBuffer, &lineCapacity, file);
        if (count < 0) { if (std::ferror(file)) throw failure("read"); return {}; }
        line.assign(lineBuffer, static_cast<size_t>(count));
        if (line.back() == '\n') line.pop_back();
#endif
        if (line.size() > maximum) throw std::length_error("File line exceeds host limit");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }
    void write(const std::string &bytes) override {
        require(); switchTo(Last::Write);
        if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) throw failure("write");
    }
    int64_t seek(int64_t offset, int origin) override {
        require();
        int whence = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
        if (_fseeki64(file, offset, whence) != 0) throw failure("seek");
        int64_t position = _ftelli64(file);
#else
        if (fseeko(file, static_cast<off_t>(offset), whence) != 0) throw failure("seek");
        int64_t position = static_cast<int64_t>(ftello(file));
#endif
        if (position < 0) throw failure("tell");
        last = Last::None; return position;
    }
    void flush() override { require(); if (std::fflush(file) != 0) throw failure("flush"); }
};

// `resolve` maps a script path to a real path below the sandbox root and throws if it escapes.
inline dk3::FileSystem sandboxedFileSystem(std::function<std::filesystem::path(const std::string &)> resolve, bool writable) {
    namespace fs = std::filesystem;
    dk3::FileSystem result;
    auto requireWrite = [writable] { if (!writable) throw std::runtime_error("Files root is read-only; pass --files-write to allow changes"); };
    result.open = [resolve, writable](const std::string &name, const std::string &mode) -> std::shared_ptr<dk3::FileHandle> {
        if (!writable && mode != "r") throw std::runtime_error("Files root is read-only; pass --files-write to allow changes");
        fs::path path = resolve(name);
        if (fs::exists(path) && !fs::is_regular_file(path)) throw std::runtime_error("Files path is not a regular file");
        std::FILE *file = std::fopen(path.string().c_str(), (mode + "b").c_str());
        if (!file) throw std::runtime_error("Cannot open '" + name + "': " + std::strerror(errno));
        return std::make_shared<StdioFile>(file);
    };
    result.list = [resolve](const std::string &name) {
        fs::path path = resolve(name);
        if (!fs::is_directory(path)) throw std::runtime_error("Files path is not a directory");
        std::vector<std::string> names;
        for (const auto &entry : fs::directory_iterator(path)) names.push_back(entry.path().filename().string());
        return names;
    };
    result.isDirectory = [resolve](const std::string &name) { return fs::is_directory(resolve(name)); };
    result.makeDirectory = [resolve, requireWrite](const std::string &name) {
        requireWrite(); fs::path path = resolve(name);
        if (fs::exists(path)) { if (fs::is_directory(path)) return; throw std::runtime_error("Path already exists and is not a directory"); }
        fs::create_directories(path);
    };
    result.remove = [resolve, requireWrite](const std::string &name) {
        requireWrite(); fs::path path = resolve(name);
        if (!fs::exists(path)) throw std::runtime_error("Path does not exist");
        if (fs::is_directory(path) && !fs::is_empty(path)) throw std::runtime_error("Directory is not empty");
        fs::remove(path);
    };
    result.rename = [resolve, requireWrite](const std::string &from, const std::string &to) {
        requireWrite(); fs::path source = resolve(from), target = resolve(to);
        if (!fs::exists(source)) throw std::runtime_error("Source path does not exist");
        if (fs::exists(target)) throw std::runtime_error("Target path already exists");
        fs::rename(source, target);
    };
    return result;
}
