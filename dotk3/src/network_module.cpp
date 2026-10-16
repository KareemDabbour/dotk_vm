#include "runtime.hpp"
#include "../vendor/picohttpparser.h"
#include <climits>
#include <chrono>
#include <cctype>

namespace dk3 {
namespace {
Object *object(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected a managed value", "type");
    return std::get<Object *>(value);
}
std::shared_ptr<SocketHandle> socket(Value value, const Span &span) {
    Object *resource = object(value, span);
    auto handle = resource->kind == Kind::Resource ? std::dynamic_pointer_cast<SocketHandle>(resource->external) : nullptr;
    if (!handle) throw Error(span, "Expected a Socket resource", "type");
    return handle;
}
}
void Runtime::installNetwork() {
    auto wrap = [this](std::shared_ptr<SocketHandle> handle, const Span &span, bool optional = false) -> Value {
        if (!handle) { if (optional) return {}; throw Error(span, "Host did not provide a Socket", "io"); }
        try { Object *resource = allocate(Kind::Resource, span); resource->external = handle; return resource; }
        catch (...) { handle->close(); throw; }
    };
    auto timeout = [this](const std::vector<Value> &values, size_t index, const Span &span) {
        int64_t duration = values.size() > index ? integer(values[index], span) : 0;
        if (duration < 0 || static_cast<uint64_t>(duration) > config.maxNetworkWaitMillis) throw Error(span, "Network timeout exceeds host limit", "value");
        return static_cast<uint64_t>(duration);
    };
    for (const std::string &name : {"listen", "connect", "accept", "receive", "send", "close", "port"}) {
        registerNative("Sockets." + name, [this, name, wrap, timeout](const auto &arguments, const Span &span) -> Value {
            auto values = positional(arguments, span);
            try {
                if (name == "listen" || name == "connect") {
                    size_t minimum = 2, maximum = name == "listen" ? 2 : 3;
                    if (values.size() < minimum || values.size() > maximum) throw Error(span, "Wrong socket constructor arguments", "call");
                    Object *host = object(values[0], span); if (host->kind != Kind::String || host->text.empty() || host->text.find('\0') != std::string::npos) throw Error(span, "Socket host must be a nonempty String", "type");
                    int64_t port = integer(values[1], span); if (port < 0 || port > 65535 || (name == "connect" && port == 0)) throw Error(span, "Invalid TCP port", "value");
                    if (name == "listen") {
                        if (!config.listenSocket) throw Error(span, "Networking is not enabled by the host", "capability");
                        return wrap(config.listenSocket(host->text, static_cast<uint16_t>(port)), span);
                    }
                    uint64_t wait = timeout(values, 2, span);
                    if (!config.connectSocket) throw Error(span, "Networking is not enabled by the host", "capability");
                    return wrap(config.connectSocket(host->text, static_cast<uint16_t>(port), wait), span);
                }
                if (values.empty()) throw Error(span, "Missing Socket argument", "call");
                auto handle = socket(values[0], span);
                if (name == "close") { if (values.size() != 1) throw Error(span, "close expects one Socket", "call"); handle->close(); return {}; }
                if (handle->closed) throw Error(span, "Socket is closed", "state");
                if (name == "port") { if (values.size() != 1) throw Error(span, "port expects one Socket", "call"); return static_cast<int64_t>(handle->port()); }
                if (name == "accept") { if (values.size() > 2) throw Error(span, "accept expects Socket and optional timeout", "call"); return wrap(handle->accept(timeout(values, 1, span)), span, true); }
                if (values.size() < 2 || values.size() > 3) throw Error(span, "Wrong Socket operation arguments", "call");
                uint64_t wait = timeout(values, 2, span);
                if (name == "receive") {
                    int64_t maximum = integer(values[1], span); if (maximum <= 0 || static_cast<uint64_t>(maximum) > config.maxNetworkBytes) throw Error(span, "Receive size exceeds host limit", "value");
                    auto bytes = handle->receive(static_cast<size_t>(maximum), wait); if (!bytes) return {};
                    if (bytes->size() > static_cast<size_t>(maximum)) throw Error(span, "Host receive exceeds requested maximum", "limit");
                    return text(*bytes, true, span);
                }
                Object *bytes = object(values[1], span); if (bytes->kind != Kind::String && bytes->kind != Kind::Bytes) throw Error(span, "send expects String or Bytes", "type");
                if (bytes->text.size() > config.maxNetworkBytes) throw Error(span, "Send size exceeds host limit", "limit");
                try { handle->send(bytes->text, wait); } catch (...) { handle->close(); throw; }
                return static_cast<int64_t>(bytes->text.size());
            } catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; } catch (const std::exception &error) { throw Error(span, error.what(), "io"); }
        });
    }
    define(coreNamespace("IO"), "Sockets", coreNamespace("Sockets"), false, "", Span{});
    auto readMessage = [this](const std::shared_ptr<SocketHandle> &handle, bool request, uint64_t wait, const Span &span) -> Value {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait);
        std::string buffer;
        try {
            for (;;) {
                tick(span);
                const char *method = nullptr, *target = nullptr, *reason = nullptr;
                size_t methodSize = 0, targetSize = 0, reasonSize = 0;
                int minor = 0, status = 0; phr_header headers[64]; size_t count = 64;
                int parsed = request ? phr_parse_request(buffer.data(), buffer.size(), &method, &methodSize, &target, &targetSize, &minor, headers, &count, 0)
                    : phr_parse_response(buffer.data(), buffer.size(), &minor, &status, &reason, &reasonSize, headers, &count, 0);
                if (parsed == -1) throw Error(span, "Malformed HTTP headers", "io");
                size_t bodySize = 0; bool hasLength = false;
                std::vector<std::pair<std::string, std::string>> fields;
                if (parsed >= 0) {
                    if (minor != 1 || parsed > 16384 || (!request && (status < 200 || status > 599))) throw Error(span, "Unsupported HTTP version, status or header size", "io");
                    for (size_t index = 0; index < static_cast<size_t>(parsed); index++) {
                        if ((buffer[index] == '\n' && (index == 0 || buffer[index - 1] != '\r')) ||
                            (buffer[index] == '\r' && (index + 1 == static_cast<size_t>(parsed) || buffer[index + 1] != '\n')))
                            throw Error(span, "HTTP requires CRLF framing", "io");
                    }
                    for (size_t index = 0; index < count; index++) {
                        if (!headers[index].name) throw Error(span, "Folded HTTP headers are unsupported", "io");
                        std::string key(headers[index].name, headers[index].name_len), value(headers[index].value, headers[index].value_len);
                        for (char &character : key) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                        for (const auto &field : fields) if (field.first == key) throw Error(span, "Duplicate HTTP header: " + key, "io");
                        if (key == "transfer-encoding" || key == "expect") throw Error(span, "Unsupported HTTP transfer or expectation", "io");
                        if (key == "content-length") {
                            if (value.empty()) throw Error(span, "Empty Content-Length", "io");
                            for (char digit : value) {
                                if (digit < '0' || digit > '9' || static_cast<size_t>(digit - '0') > config.maxNetworkBytes || bodySize > (config.maxNetworkBytes - static_cast<size_t>(digit - '0')) / 10)
                                    throw Error(span, "Invalid or excessive Content-Length", "io");
                                bodySize = bodySize * 10 + static_cast<size_t>(digit - '0');
                            }
                            hasLength = true;
                        }
                        fields.emplace_back(std::move(key), std::move(value));
                    }
                    if (request) {
                        bool host = false; for (const auto &field : fields) if (field.first == "host" && !field.second.empty()) host = true;
                        if (!host || !targetSize || *target != '/') throw Error(span, "HTTP request needs Host and origin-form target", "io");
                    } else {
                        if (!hasLength && status != 204 && status != 304) throw Error(span, "Response requires Content-Length", "io");
                        if ((status == 204 || status == 304) && bodySize) throw Error(span, "Body is forbidden for this response status", "io");
                    }
                    if (static_cast<size_t>(parsed) > config.maxNetworkBytes || bodySize > config.maxNetworkBytes - static_cast<size_t>(parsed)) throw Error(span, "HTTP message exceeds host byte limit", "io");
                    size_t complete = static_cast<size_t>(parsed) + bodySize;
                    if (buffer.size() > complete) throw Error(span, "HTTP pipelining or trailing bytes are unsupported", "io");
                    if (buffer.size() == complete) {
                        Object *result = allocate(Kind::Map, span); roots.push_back(result);
                        Object *headerMap = allocate(Kind::Map, span); roots.push_back(headerMap);
                        for (const auto &field : fields) headerMap->entries.emplace_back(text(field.first, false, span), text(field.second, false, span));
                        auto add = [&](const std::string &key, Value value) { result->entries.emplace_back(text(key, false, span), value); };
                        if (request) { add("method", text(std::string(method, methodSize), false, span)); add("target", text(std::string(target, targetSize), false, span)); }
                        else { add("status", static_cast<int64_t>(status)); add("reason", text(std::string(reason, reasonSize), false, span)); }
                        add("headers", headerMap); add("body", text(buffer.substr(static_cast<size_t>(parsed)), true, span)); return result;
                    }
                } else if (buffer.size() >= 16384) throw Error(span, "HTTP headers exceed 16 KiB", "io");
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                if (!buffer.empty() && remaining <= 0) throw Error(span, "Incomplete HTTP message timed out", "io");
                size_t maximum = std::min<size_t>(4096, config.maxNetworkBytes > buffer.size() ? config.maxNetworkBytes - buffer.size() : 0);
                if (!maximum) throw Error(span, "HTTP message exceeds host byte limit", "io");
                auto bytes = handle->receive(maximum, remaining > 0 ? static_cast<uint64_t>(remaining) : 0);
                if (!bytes || bytes->empty()) {
                    if (buffer.empty()) return {};
                    throw Error(span, bytes ? "Incomplete HTTP message at EOF" : "Incomplete HTTP message timed out", "io");
                }
                if (bytes->size() > maximum) throw Error(span, "Host receive exceeds HTTP byte limit", "limit");
                buffer += *bytes;
            }
        } catch (...) { handle->close(); throw; }
    };
    for (const std::string &name : {"readRequest", "readResponse", "writeRequest", "writeResponse"}) {
        registerNative("Http." + name, [this, name, readMessage](const auto &arguments, const Span &span) -> Value {
            auto values = positional(arguments, span); bool reading = name.rfind("read", 0) == 0, request = name == "readRequest" || name == "writeRequest";
            size_t minimum = reading ? 1 : 3, maximum = reading ? 2 : 5;
            if (values.size() < minimum || values.size() > maximum) throw Error(span, "Wrong HTTP arguments", "call");
            auto handle = socket(values[0], span); if (handle->closed) throw Error(span, "Socket is closed", "state");
            size_t waitIndex = reading ? 1 : 4;
            int64_t wait = values.size() > waitIndex ? integer(values[waitIndex], span) : static_cast<int64_t>(std::min<uint64_t>(1000, config.maxNetworkWaitMillis));
            if (wait < 0 || static_cast<uint64_t>(wait) > config.maxNetworkWaitMillis) throw Error(span, "HTTP timeout exceeds host limit", "value");
            try {
                if (reading) return readMessage(handle, request, static_cast<uint64_t>(wait), span);
                auto string = [&](size_t index) { Object *value = object(values[index], span); if (value->kind != Kind::String) throw Error(span, "Expected HTTP String argument", "type"); return value->text; };
                auto clean = [&](const std::string &value, bool spaces) { if (value.empty()) throw Error(span, "Empty HTTP token", "value"); for (unsigned char character : value) if (character < (spaces ? 32 : 33) || character > 126) throw Error(span, "Invalid HTTP token", "value"); };
                std::string header, body;
                if (request) {
                    std::string method = string(1), target = string(2); clean(method, false); clean(target, false);
                    if (method != "GET" && method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE" && method != "OPTIONS") throw Error(span, "Unsupported HTTP method", "value");
                    if (target.front() != '/' || target.find('#') != std::string::npos) throw Error(span, "Expected HTTP origin-form target", "value");
                    header = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n";
                    if (values.size() > 3) { Object *value = object(values[3], span); if (value->kind != Kind::String && value->kind != Kind::Bytes) throw Error(span, "HTTP body must be String or Bytes", "type"); body = value->text; }
                } else {
                    int64_t status = integer(values[1], span); if (status < 200 || status > 599) throw Error(span, "HTTP status must be 200..599", "value");
                    Object *value = object(values[2], span); if (value->kind != Kind::String && value->kind != Kind::Bytes) throw Error(span, "HTTP body must be String or Bytes", "type"); body = value->text;
                    if ((status == 204 || status == 304) && !body.empty()) throw Error(span, "Body is forbidden for this response status", "value");
                    std::string contentType = values.size() > 3 ? string(3) : "text/plain; charset=utf-8"; clean(contentType, true);
                    header = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK\r\n" : " Response\r\n") + "Content-Type: " + contentType + "\r\n";
                }
                header += "Connection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
                if (header.size() > 16384 || header.size() > config.maxNetworkBytes || body.size() > config.maxNetworkBytes - header.size()) throw Error(span, "HTTP output exceeds host byte limit", "limit");
                std::string message = header + body;
                try { handle->send(message, static_cast<uint64_t>(wait)); } catch (...) { handle->close(); throw; }
                return static_cast<int64_t>(message.size());
            } catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; } catch (const std::exception &error) { throw Error(span, error.what(), "io"); }
        });
    }
    define(coreNamespace("IO"), "Http", coreNamespace("Http"), false, "", Span{});
}
}