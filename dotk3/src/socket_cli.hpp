#pragma once
#include "runtime.hpp"
#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

class SocketDomain;
class PosixSocket final : public dk3::SocketHandle {
    int descriptor;
    bool listener;
    std::weak_ptr<SocketDomain> domain;
    uint16_t localPort;
    static std::runtime_error failure(const char *operation) { return std::runtime_error(std::string(operation) + ": " + std::strerror(errno)); }
    void requireOpen() const { if (closed) throw std::runtime_error("Socket is closed"); }
public:
    PosixSocket(int descriptor, bool listener, std::shared_ptr<SocketDomain> domain) : descriptor(descriptor), listener(listener), domain(domain) {
        sockaddr_in address{}; socklen_t length = sizeof(address);
        if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) < 0) throw failure("getsockname");
        localPort = ntohs(address.sin_port);
    }
    ~PosixSocket() override { close(); }
    void close() override { if (!closed) { ::close(descriptor); closed = true; } }
    uint16_t port() const override { requireOpen(); return localPort; }
    static bool wait(int descriptor, short events, std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            pollfd operation{descriptor, events, 0};
            int ready = poll(&operation, 1, static_cast<int>(remaining > 0 ? remaining : 0));
            if (ready < 0) { if (errno == EINTR) continue; throw failure("poll"); }
            if (!ready) return false;
            if (operation.revents & POLLNVAL) throw std::runtime_error("Invalid socket descriptor");
            return true;
        }
    }
    std::shared_ptr<dk3::SocketHandle> accept(uint64_t timeout) override;
    std::optional<std::string> receive(size_t maximum, uint64_t timeout) override {
        requireOpen(); if (listener) throw std::runtime_error("Cannot receive from a listener");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        for (;;) {
            if (!wait(descriptor, POLLIN, deadline)) return {};
            std::string bytes(maximum, '\0'); ssize_t count = recv(descriptor, bytes.data(), bytes.size(), 0);
            if (count < 0) { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) { if (std::chrono::steady_clock::now() >= deadline) return {}; continue; } throw failure("recv"); }
            bytes.resize(static_cast<size_t>(count)); return bytes;
        }
    }
    void send(const std::string &bytes, uint64_t timeout) override {
        requireOpen(); if (listener) throw std::runtime_error("Cannot send through a listener");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout); size_t sent = 0;
        while (sent < bytes.size()) {
            if (timeout && sent && std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Socket send timed out");
            if (!wait(descriptor, POLLOUT, deadline)) throw std::runtime_error("Socket send timed out");
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            ssize_t count = ::send(descriptor, bytes.data() + sent, bytes.size() - sent, flags);
            if (count < 0) { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) { if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Socket send timed out"); continue; } throw failure("send"); }
            if (!count) throw std::runtime_error("Socket send made no progress");
            sent += static_cast<size_t>(count);
        }
    }
    static void configure(int descriptor) {
        if (fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0 || fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) throw failure("fcntl");
#ifdef SO_NOSIGPIPE
        int enabled = 1; if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) throw failure("setsockopt");
#endif
    }
    static sockaddr_in address(const std::string &host, uint16_t port) {
        sockaddr_in result{}; result.sin_family = AF_INET; result.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &result.sin_addr) != 1) throw std::runtime_error("Use a numeric IPv4 socket address");
        return result;
    }
};

class SocketDomain : public std::enable_shared_from_this<SocketDomain> {
    std::vector<std::shared_ptr<PosixSocket>> sockets;
public:
    ~SocketDomain() { close(); }
    void close() { for (auto entry = sockets.rbegin(); entry != sockets.rend(); ++entry) (*entry)->close(); }
    std::shared_ptr<dk3::SocketHandle> adopt(int descriptor, bool listener) {
        std::shared_ptr<PosixSocket> result;
        try {
            sockets.erase(std::remove_if(sockets.begin(), sockets.end(), [](const auto &socket) { return socket->closed; }), sockets.end());
            if (sockets.size() >= 256) throw std::runtime_error("Open socket limit exceeded");
            PosixSocket::configure(descriptor);
            result = std::make_shared<PosixSocket>(descriptor, listener, shared_from_this());
            sockets.push_back(result); return result;
        } catch (...) { if (!result) ::close(descriptor); throw; }
    }
    std::shared_ptr<dk3::SocketHandle> listen(const std::string &host, uint16_t port) {
        auto address = PosixSocket::address(host, port); int descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) throw std::runtime_error("socket creation failed");
        int enabled = 1;
        if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
            bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || ::listen(descriptor, 32) < 0) {
            std::string message = std::strerror(errno); ::close(descriptor); throw std::runtime_error(message);
        }
        return adopt(descriptor, true);
    }
    std::shared_ptr<dk3::SocketHandle> connect(const std::string &host, uint16_t port, uint64_t timeout) {
        auto address = PosixSocket::address(host, port); int descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) throw std::runtime_error("socket creation failed");
        try {
            PosixSocket::configure(descriptor);
            if (::connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
                if (errno != EINPROGRESS) throw std::runtime_error(std::strerror(errno));
                if (!PosixSocket::wait(descriptor, POLLOUT, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout))) throw std::runtime_error("Socket connect timed out");
                int result = 0; socklen_t length = sizeof(result);
                if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &result, &length) < 0 || result) throw std::runtime_error(result ? std::strerror(result) : std::strerror(errno));
            }
        } catch (...) { ::close(descriptor); throw; }
        return adopt(descriptor, false);
    }
};
inline std::shared_ptr<dk3::SocketHandle> PosixSocket::accept(uint64_t timeout) {
    requireOpen(); if (!listener) throw std::runtime_error("Socket is not a listener");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        if (!wait(descriptor, POLLIN, deadline)) return {};
        int accepted = ::accept(descriptor, nullptr, nullptr);
        if (accepted < 0) { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) { if (std::chrono::steady_clock::now() >= deadline) return {}; continue; } throw failure("accept"); }
        auto owner = domain.lock(); if (!owner) { ::close(accepted); throw std::runtime_error("Socket owner is gone"); }
        return owner->adopt(accepted, false);
    }
}
#endif