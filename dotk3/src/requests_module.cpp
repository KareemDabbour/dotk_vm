#include "runtime.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace dk3 {
namespace {
constexpr uint64_t defaultTimeoutMillis = 10000;
constexpr unsigned defaultRedirects = 5, maximumRedirects = 10;
constexpr size_t maximumUrlBytes = 8192;

Object *object(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected a managed value", "type");
    return std::get<Object *>(value);
}
bool validUtf8(const std::string &bytes) {
    try { utf8Offsets(bytes, Span{}); return true; } catch (const Error &) { return false; }
}
std::string lower(std::string value) {
    for (char &character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}
std::string encode(const std::string &value) {
    static const char *digits = "0123456789ABCDEF";
    std::string result;
    for (unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '.' || character == '_' || character == '~') result += static_cast<char>(character);
        else { result += '%'; result += digits[character >> 4]; result += digits[character & 15]; }
    }
    return result;
}
bool token(const std::string &name) {
    if (name.empty()) return false;
    for (unsigned char character : name)
        if (!(std::isalnum(character) || std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(character)) != std::string::npos)) return false;
    return true;
}
}

void Runtime::installRequests() {
    auto string = [](Value value, const char *what, const Span &span) -> const std::string & {
        Object *result = object(value, span);
        if (result->kind != Kind::String) throw Error(span, std::string(what) + " must be a String", "type");
        return result->text;
    };
    auto scalar = [this](Value value, const char *what, const Span &span) -> std::string {
        if (std::holds_alternative<std::monostate>(value)) throw Error(span, std::string(what) + " cannot contain null", "type");
        if (std::holds_alternative<Object *>(value)) {
            Object *result = std::get<Object *>(value);
            if (result->kind != Kind::String) throw Error(span, std::string(what) + " must be String, Int, Float or Bool values", "type");
            return result->text;
        }
        return stringify(value, span);
    };
    auto perform = [this, string, scalar](std::string method, Value urlValue, std::optional<Value> optionsValue, const Span &span) -> Value {
        tick(span);
        if (!config.httpRequest) throw Error(span, "HTTP requests are not enabled by the host", "capability");
        for (char &character : method) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        if (!std::set<std::string>{"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"}.count(method)) throw Error(span, "Unsupported HTTP method", "value");
        HttpRequest request; request.method = method; request.url = string(urlValue, "URL", span);
        request.timeoutMillis = std::min<uint64_t>(defaultTimeoutMillis, config.maxRequestMillis);
        request.maxResponseBytes = config.maxNetworkBytes; request.maxRedirects = defaultRedirects;
        std::string query; bool hasBody = false, hasJson = false;
        auto setHeader = [&](const std::string &name, const std::string &value) {
            if (!token(name)) throw Error(span, "Invalid HTTP header name", "value");
            std::string key = lower(name);
            if (key == "content-length" || key == "transfer-encoding") throw Error(span, "Header is managed by the host: " + name, "value");
            for (unsigned char character : value) if ((character < 32 && character != '\t') || character == 127) throw Error(span, "Invalid HTTP header value", "value");
            request.headers.erase(std::remove_if(request.headers.begin(), request.headers.end(), [&](const auto &entry) { return lower(entry.first) == key; }), request.headers.end());
            request.headers.emplace_back(name, value);
        };
        Value jsonValue;
        if (optionsValue && !std::holds_alternative<std::monostate>(*optionsValue)) {
            Object *options = object(*optionsValue, span);
            if (options->kind != Kind::Map) throw Error(span, "Requests options must be a Map", "type");
            for (const auto &option : options->entries) {
                tick(span);
                const std::string &key = string(option.first, "Option name", span); Value value = option.second;
                if (key == "headers") {
                    Object *headers = object(value, span);
                    if (headers->kind != Kind::Map) throw Error(span, "headers must be a Map", "type");
                    for (const auto &header : headers->entries) { tick(span); setHeader(string(header.first, "Header name", span), string(header.second, "Header value", span)); }
                } else if (key == "params") {
                    Object *params = object(value, span);
                    if (params->kind != Kind::Map) throw Error(span, "params must be a Map", "type");
                    for (const auto &param : params->entries) {
                        tick(span);
                        std::string name = encode(string(param.first, "Parameter name", span));
                        auto append = [&](const std::string &text) { query += (query.empty() ? "" : "&") + name + "=" + encode(text); };
                        if (std::holds_alternative<Object *>(param.second) && object(param.second, span)->kind == Kind::List)
                            for (Value item : object(param.second, span)->items) append(scalar(item, "params", span));
                        else append(scalar(param.second, "params", span));
                    }
                } else if (key == "body") {
                    Object *body = object(value, span);
                    if (body->kind != Kind::String && body->kind != Kind::Bytes) throw Error(span, "body must be String or Bytes", "type");
                    request.body = body->text; hasBody = true;
                } else if (key == "json") { jsonValue = value; hasJson = true; }
                else if (key == "timeoutMs") {
                    int64_t timeout = integer(value, span);
                    if (timeout < 1 || static_cast<uint64_t>(timeout) > config.maxRequestMillis) throw Error(span, "Request timeout exceeds host limit", "value");
                    request.timeoutMillis = static_cast<uint64_t>(timeout);
                } else if (key == "redirects") {
                    if (std::holds_alternative<bool>(value)) request.maxRedirects = std::get<bool>(value) ? defaultRedirects : 0;
                    else {
                        int64_t count = integer(value, span);
                        if (count < 0 || count > maximumRedirects) throw Error(span, "redirects must be a Bool or 0..10", "value");
                        request.maxRedirects = static_cast<unsigned>(count);
                    }
                } else throw Error(span, "Unknown Requests option: " + key, "value");
            }
        }
        if (hasBody && hasJson) throw Error(span, "Use either body or json, not both", "value");
        if (hasJson) {
            Value stringifier = coreNamespace("Json")->bindings.at("stringify").value;
            Value encoded = call(stringifier, {{"", jsonValue}}, span); roots.push_back(encoded);
            request.body = object(encoded, span)->text; hasBody = true;
            bool typed = false; for (const auto &header : request.headers) if (lower(header.first) == "content-type") typed = true;
            if (!typed) setHeader("Content-Type", "application/json");
        }
        if (method == "HEAD" && hasBody) throw Error(span, "HEAD requests cannot have a body", "value");
        if (request.body.size() > config.maxNetworkBytes) throw Error(span, "Request body exceeds host byte limit", "limit");
        if (!query.empty()) {
            size_t fragment = request.url.find('#'); std::string tail = fragment == std::string::npos ? "" : request.url.substr(fragment);
            if (fragment != std::string::npos) request.url.resize(fragment);
            request.url += (request.url.find('?') == std::string::npos ? "?" : (request.url.back() == '?' || request.url.back() == '&' ? "" : "&")) + query + tail;
        }
        std::string scheme = lower(request.url.substr(0, 8));
        if (scheme.rfind("http://", 0) != 0 && scheme.rfind("https://", 0) != 0) throw Error(span, "URL must start with http:// or https://", "value");
        if (request.url.size() > maximumUrlBytes) throw Error(span, "URL exceeds length limit", "limit");
        for (unsigned char character : request.url) if (character <= 32 || character == 127) throw Error(span, "URL contains spaces or control characters; percent-encode them", "value");

        HttpResponse response;
        try { response = config.httpRequest(request); }
        catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; }
        catch (const std::length_error &error) { throw Error(span, error.what(), "limit"); }
        catch (const std::exception &error) { throw Error(span, error.what(), "io"); }
        if (response.status < 100 || response.status > 599) throw Error(span, "Host returned an invalid HTTP status", "io");
        if (response.body.size() > config.maxNetworkBytes) throw Error(span, "HTTP response exceeds host byte limit", "limit");

        Object *result = allocate(Kind::Map, span); roots.push_back(result);
        Object *headers = allocate(Kind::Map, span); roots.push_back(headers);
        auto add = [&](const std::string &key, Value value) { result->entries.emplace_back(text(key, false, span), value); };
        for (auto &header : response.headers) {
            tick(span);
            std::string key = lower(header.first), value = header.second;
            if (!validUtf8(key)) continue;
            if (!validUtf8(value)) for (char &character : value) if (static_cast<unsigned char>(character) > 127) character = '?';
            Object *existing = nullptr;
            for (auto &entry : headers->entries) if (object(entry.first, span)->text == key) { existing = object(entry.second, span); break; }
            if (existing) existing->text += ", " + value;
            else headers->entries.emplace_back(text(key, false, span), text(value, false, span));
        }
        add("status", static_cast<int64_t>(response.status));
        add("ok", response.status >= 200 && response.status < 300);
        add("reason", text(validUtf8(response.reason) ? response.reason : "", false, span));
        add("url", text(response.url.empty() ? request.url : response.url, false, span));
        add("headers", headers);
        add("body", text(response.body, !validUtf8(response.body), span));
        return result;
    };
    registerNative("Requests.request", [this, perform](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() < 2 || values.size() > 3) throw Error(span, "request expects method, url and optional options", "call");
        Object *method = object(values[0], span);
        if (method->kind != Kind::String) throw Error(span, "HTTP method must be a String", "type");
        return perform(method->text, values[1], values.size() > 2 ? std::optional<Value>(values[2]) : std::nullopt, span);
    });
    for (const std::string &name : {"get", "head", "delete", "post", "put", "patch"}) {
        registerNative("Requests." + name, [this, name, perform](const auto &arguments, const Span &span) -> Value {
            auto values = positional(arguments, span);
            if (values.empty() || values.size() > 2) throw Error(span, name + " expects url and optional options", "call");
            return perform(name, values[0], values.size() > 1 ? std::optional<Value>(values[1]) : std::nullopt, span);
        });
    }
    registerNative("Requests.json", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "json expects one response Map", "call");
        Object *response = object(values[0], span);
        if (response->kind != Kind::Map) throw Error(span, "json expects a response Map", "type");
        for (const auto &entry : response->entries) {
            if (!std::holds_alternative<Object *>(entry.first) || object(entry.first, span)->kind != Kind::String || object(entry.first, span)->text != "body") continue;
            if (!std::holds_alternative<Object *>(entry.second) || object(entry.second, span)->kind != Kind::String) break;
            Value parser = coreNamespace("Json")->bindings.at("parse").value;
            return call(parser, {{"", entry.second}}, span);
        }
        throw Error(span, "json expects a response Map with a String body", "type");
    });
}
}
