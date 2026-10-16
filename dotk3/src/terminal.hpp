#pragma once
#ifndef _WIN32
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>

class TerminalSession {
    termios saved{};
    bool active = false;
    static std::runtime_error failure(const char *operation) { return std::runtime_error(std::string(operation) + ": " + std::strerror(errno)); }
public:
    ~TerminalSession() { if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
    void mode(bool enabled) {
        if (enabled == active) return;
        if (enabled) {
            if (!isatty(STDIN_FILENO)) throw std::runtime_error("Raw input requires a terminal");
            if (tcgetattr(STDIN_FILENO, &saved) < 0) throw failure("tcgetattr");
            termios raw = saved;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN | ISIG));
            raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | INLCR | IGNCR));
            raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) throw failure("tcsetattr");
            active = true;
        } else {
            if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) < 0) throw failure("tcsetattr");
            active = false;
        }
    }
    std::optional<uint8_t> read(uint64_t timeout) {
        if (!active) throw std::runtime_error("Raw input session is closed");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        for (;;) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            pollfd input{STDIN_FILENO, POLLIN, 0};
            int ready = poll(&input, 1, static_cast<int>(remaining > 0 ? remaining : 0));
            if (ready < 0) { if (errno == EINTR) continue; throw failure("poll"); }
            if (!ready) return {};
            if (input.revents & (POLLERR | POLLNVAL)) throw std::runtime_error("Terminal polling failed");
            if (!(input.revents & POLLIN)) throw std::runtime_error("Terminal input reached EOF");
            uint8_t value; ssize_t count = ::read(STDIN_FILENO, &value, 1);
            if (count < 0) { if (errno == EINTR) continue; throw failure("read"); }
            if (!count) throw std::runtime_error("Terminal input reached EOF");
            return value;
        }
    }
};
#endif