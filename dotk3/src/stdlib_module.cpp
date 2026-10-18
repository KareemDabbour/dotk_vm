#include "runtime.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <ctime>

namespace dk3 {
namespace {
Object *object(Value value, const Span &span) {
    if (!std::holds_alternative<Object *>(value)) throw Error(span, "Expected a managed value", "type");
    return std::get<Object *>(value);
}

// Proleptic Gregorian calendar arithmetic (days since 1970-01-01).
int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}
void civilFromDays(int64_t days, int64_t &year, unsigned &month, unsigned &day) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    year = static_cast<int64_t>(yearOfEra) + era * 400;
    unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    unsigned shifted = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * shifted + 2) / 5 + 1;
    month = shifted < 10 ? shifted + 3 : shifted - 9;
    year += month <= 2;
}
unsigned daysInMonth(int64_t year, unsigned month) {
    static const unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : lengths[month - 1];
}
struct Moment { int64_t year; unsigned month, day, hour, minute, second, millisecond, weekday, yearDay; };
Moment momentOf(int64_t millis) {
    int64_t days = millis / 86400000, rest = millis % 86400000;
    if (rest < 0) { rest += 86400000; days--; }
    Moment moment{}; civilFromDays(days, moment.year, moment.month, moment.day);
    moment.hour = static_cast<unsigned>(rest / 3600000); moment.minute = static_cast<unsigned>(rest / 60000 % 60);
    moment.second = static_cast<unsigned>(rest / 1000 % 60); moment.millisecond = static_cast<unsigned>(rest % 1000);
    moment.weekday = static_cast<unsigned>(((days % 7) + 11) % 7);
    moment.yearDay = static_cast<unsigned>(days - daysFromCivil(moment.year, 1, 1) + 1);
    return moment;
}

std::array<uint32_t, 8> sha256(const std::string &data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string padded = data; padded += static_cast<char>(0x80);
    while (padded.size() % 64 != 56) padded += '\0';
    uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) padded += static_cast<char>(bits >> shift);
    auto rotate = [](uint32_t value, unsigned count) { return (value >> count) | (value << (32 - count)); };
    for (size_t block = 0; block < padded.size(); block += 64) {
        uint32_t w[64];
        for (size_t i = 0; i < 16; i++) {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(padded.data()) + block + i * 4;
            w[i] = static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
        }
        for (size_t i = 16; i < 64; i++) {
            uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = state;
        for (size_t i = 0; i < 64; i++) {
            uint32_t s1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25), choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
            uint32_t t1 = v[7] + s1 + choose + k[i] + w[i];
            uint32_t s0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22), majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            uint32_t t2 = s0 + majority;
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (size_t i = 0; i < 8; i++) state[i] += v[i];
    }
    return state;
}
uint32_t crc32(const std::string &data) {
    uint32_t crc = 0xffffffffu;
    for (unsigned char byte : data) { crc ^= byte; for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return ~crc;
}
const char *hexDigits = "0123456789abcdef";
std::string toHex(const std::string &data) {
    std::string result;
    for (unsigned char byte : data) { result += hexDigits[byte >> 4]; result += hexDigits[byte & 15]; }
    return result;
}
int hexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}
const char *base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string trimmed(const std::string &source) {
    size_t start = 0, end = source.size();
    while (start < end && std::isspace(static_cast<unsigned char>(source[start]))) start++;
    while (end > start && std::isspace(static_cast<unsigned char>(source[end - 1]))) end--;
    return source.substr(start, end - start);
}
bool parseInteger(const std::string &source, int radix, int64_t &out) {
    std::string text = trimmed(source); size_t offset = !text.empty() && text[0] == '+' ? 1 : 0;
    if (text.size() == offset || (offset && (text[1] == '-' || text[1] == '+'))) return false;
    auto parsed = std::from_chars(text.data() + offset, text.data() + text.size(), out, radix);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool parseFloating(const std::string &source, double &out) {
    std::string text = trimmed(source);
    if (text.empty() || text.find_first_not_of("0123456789+-.eE") != std::string::npos) return false;
    char *end = nullptr; errno = 0; out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && errno != ERANGE && std::isfinite(out);
}

// Splits a POSIX-style path into the pieces Path.* works with.
std::string lastComponent(const std::string &path) {
    size_t end = path.size(); while (end > 1 && path[end - 1] == '/') end--;
    if (end == 1 && path[0] == '/') return "";
    size_t slash = path.rfind('/', end - 1);
    return path.substr(slash == std::string::npos ? 0 : slash + 1, end - (slash == std::string::npos ? 0 : slash + 1));
}
std::string normalizePath(const std::string &path) {
    bool absolute = !path.empty() && path[0] == '/'; std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start); if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else if (!absolute) parts.push_back(part); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        start = end + 1;
    }
    std::string result = absolute ? "/" : "";
    for (size_t index = 0; index < parts.size(); index++) { if (index) result += '/'; result += parts[index]; }
    return result.empty() ? "." : result;
}
}

void Runtime::installStdlib() {
    auto arity = [](const std::vector<Value> &values, size_t minimum, size_t maximum, const char *name, const Span &span) {
        if (values.size() < minimum || values.size() > maximum) throw Error(span, std::string(name) + " received the wrong number of arguments", "call");
    };
    auto hostCall = [](const Span &span, auto function) -> Value {
        try { return function(); }
        catch (const Error &) { throw; } catch (const std::bad_alloc &) { throw; }
        catch (const std::exception &error) { throw Error(span, error.what(), "io"); }
    };
    auto string = [](const std::vector<Value> &values, size_t index, const char *what, const Span &span) -> const std::string & {
        if (values.size() <= index || typeOf(values[index]) != "String") throw Error(span, std::string(what) + " must be a String", "type");
        return object(values[index], span)->text;
    };
    auto bytes = [](const std::vector<Value> &values, size_t index, const Span &span) -> const std::string & {
        std::string type = values.size() > index ? typeOf(values[index]) : "";
        if (type != "String" && type != "Bytes") throw Error(span, "Expected String or Bytes", "type");
        return object(values[index], span)->text;
    };

    // ---- Text parsing ----
    for (const std::string &name : {"parseInt", "tryParseInt"}) {
        registerNative("Text." + name, [this, name, arity, string](const auto &arguments, const Span &span) -> Value {
            auto values = positional(arguments, span); arity(values, 1, 2, name.c_str(), span);
            const std::string &source = string(values, 0, "Text", span);
            int64_t radix = values.size() > 1 ? integer(values[1], span) : 10;
            if (radix < 2 || radix > 36) throw Error(span, "Radix must be between 2 and 36", "value");
            int64_t result;
            if (parseInteger(source, static_cast<int>(radix), result)) return result;
            if (name == "tryParseInt") return {};
            throw Error(span, "Cannot parse '" + source + "' as an Int" + (radix == 10 ? "" : " in base " + std::to_string(radix)), "value");
        });
    }
    for (const std::string &name : {"parseFloat", "tryParseFloat"}) {
        registerNative("Text." + name, [this, name, arity, string](const auto &arguments, const Span &span) -> Value {
            auto values = positional(arguments, span); arity(values, 1, 1, name.c_str(), span);
            const std::string &source = string(values, 0, "Text", span); double result;
            if (parseFloating(source, result)) return result;
            if (name == "tryParseFloat") return {};
            throw Error(span, "Cannot parse '" + source + "' as a Float", "value");
        });
    }
    registerNative("Text.parseBool", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "parseBool", span);
        std::string source = trimmed(string(values, 0, "Text", span));
        std::transform(source.begin(), source.end(), source.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (source == "true") return true;
        if (source == "false") return false;
        throw Error(span, "Cannot parse '" + source + "' as a Bool; expected true or false", "value");
    });

    // ---- Time ----
    registerNative("Time.unixMillis", [this, arity, hostCall](const auto &arguments, const Span &span) -> Value {
        arity(positional(arguments, span), 0, 0, "unixMillis", span);
        if (!config.wallMillis) throw Error(span, "Wall clock is not enabled by the host", "capability");
        return hostCall(span, [&]() -> Value {
            uint64_t value = config.wallMillis();
            if (value > static_cast<uint64_t>(INT64_MAX)) throw Error(span, "Clock exceeds Int range", "overflow");
            return static_cast<int64_t>(value);
        });
    });
    registerNative("Time.monotonicMillis", [this, arity, hostCall](const auto &arguments, const Span &span) -> Value {
        arity(positional(arguments, span), 0, 0, "monotonicMillis", span);
        if (!config.monotonicMillis) throw Error(span, "Clock is not enabled by the host", "capability");
        return hostCall(span, [&]() -> Value {
            uint64_t value = config.monotonicMillis();
            if (value > static_cast<uint64_t>(INT64_MAX)) throw Error(span, "Clock exceeds Int range", "overflow");
            return static_cast<int64_t>(value);
        });
    });
    registerNative("Time.sleep", [this, arity, hostCall](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "sleep", span);
        int64_t duration = integer(values[0], span);
        if (duration < 0 || static_cast<uint64_t>(duration) > config.maxDelayMillis) throw Error(span, "Sleep exceeds host duration limit", "value");
        if (!config.delay) throw Error(span, "Sleeping is not enabled by the host", "capability");
        return hostCall(span, [&]() -> Value { config.delay(static_cast<uint64_t>(duration)); return {}; });
    });
    registerNative("Time.parts", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "parts", span);
        Moment moment = momentOf(integer(values[0], span));
        Object *result = allocate(Kind::Map, span); roots.push_back(result);
        const std::pair<const char *, int64_t> fields[] = {{"year", moment.year}, {"month", moment.month}, {"day", moment.day}, {"hour", moment.hour},
            {"minute", moment.minute}, {"second", moment.second}, {"millisecond", moment.millisecond}, {"weekday", moment.weekday}, {"yearDay", moment.yearDay}};
        for (const auto &field : fields) setEntry(result, text(field.first, false, span), field.second, span);
        return result;
    });
    registerNative("Time.make", [this, arity](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 3, 7, "make", span);
        int64_t part[7] = {0, 0, 0, 0, 0, 0, 0};
        for (size_t index = 0; index < values.size(); index++) part[index] = integer(values[index], span);
        if (part[0] < -9999 || part[0] > 9999 || part[1] < 1 || part[1] > 12 || part[2] < 1 || part[2] > static_cast<int64_t>(daysInMonth(part[0], static_cast<unsigned>(part[1]))) ||
            part[3] < 0 || part[3] > 23 || part[4] < 0 || part[4] > 59 || part[5] < 0 || part[5] > 59 || part[6] < 0 || part[6] > 999)
            throw Error(span, "Date or time component is out of range", "value");
        return daysFromCivil(part[0], static_cast<unsigned>(part[1]), static_cast<unsigned>(part[2])) * 86400000 + part[3] * 3600000 + part[4] * 60000 + part[5] * 1000 + part[6];
    });
    registerNative("Time.format", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 2, "format", span);
        Moment moment = momentOf(integer(values[0], span));
        std::string pattern = values.size() > 1 ? string(values, 1, "Pattern", span) : "%Y-%m-%dT%H:%M:%SZ";
        if (pattern.size() > 256 || pattern.find('\0') != std::string::npos) throw Error(span, "Invalid time pattern", "value");
        if (pattern.empty()) return text("", false, span);
        std::tm parts{}; parts.tm_year = static_cast<int>(moment.year - 1900); parts.tm_mon = static_cast<int>(moment.month) - 1; parts.tm_mday = static_cast<int>(moment.day);
        parts.tm_hour = static_cast<int>(moment.hour); parts.tm_min = static_cast<int>(moment.minute); parts.tm_sec = static_cast<int>(moment.second);
        parts.tm_wday = static_cast<int>(moment.weekday); parts.tm_yday = static_cast<int>(moment.yearDay) - 1;
        char buffer[1024]; size_t size = std::strftime(buffer, sizeof(buffer), pattern.c_str(), &parts);
        if (!size) throw Error(span, "Formatted time is too long", "value");
        return text(std::string(buffer, size), false, span);
    });
    registerNative("Time.parse", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "parse", span);
        const std::string &source = string(values, 0, "Time text", span); size_t at = 0;
        auto number = [&](size_t digits, int64_t &out) {
            if (at + digits > source.size()) return false;
            out = 0; for (size_t i = 0; i < digits; i++) { char c = source[at + i]; if (c < '0' || c > '9') return false; out = out * 10 + (c - '0'); }
            at += digits; return true;
        };
        auto expect = [&](char c) { if (at < source.size() && source[at] == c) { at++; return true; } return false; };
        int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0; bool ok = number(4, y) && expect('-') && number(2, mo) && expect('-') && number(2, d);
        if (ok && at < source.size() && (source[at] == 'T' || source[at] == ' ')) {
            at++; ok = number(2, h) && expect(':') && number(2, mi);
            if (ok && expect(':')) {
                ok = number(2, s);
                if (ok && expect('.')) { size_t digits = 0; ms = 0; while (at < source.size() && source[at] >= '0' && source[at] <= '9') { if (digits < 3) ms = ms * 10 + (source[at] - '0'); digits++; at++; } while (digits++ < 3) ms *= 10; ok = digits > 1; }
            }
        }
        if (ok) expect('Z');
        if (!ok || at != source.size() || mo < 1 || mo > 12 || d < 1 || d > static_cast<int64_t>(daysInMonth(y, static_cast<unsigned>(mo))) || h > 23 || mi > 59 || s > 59)
            throw Error(span, "Expected an ISO 8601 UTC time like 2024-05-17T13:45:30.250Z", "value");
        return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400000 + h * 3600000 + mi * 60000 + s * 1000 + ms;
    });

    // ---- Path ----
    registerNative("Path.join", [this, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.empty()) throw Error(span, "join expects at least one path", "call");
        std::string result;
        for (size_t index = 0; index < values.size(); index++) {
            const std::string &part = string(values, index, "Path", span);
            if (!part.empty() && part[0] == '/') result = part;
            else if (result.empty() || result.back() == '/') result += part;
            else if (!part.empty()) result += '/' + part;
        }
        return text(std::move(result), false, span);
    });
    registerNative("Path.normalize", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "normalize", span);
        return text(normalizePath(string(values, 0, "Path", span)), false, span);
    });
    registerNative("Path.name", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "name", span);
        return text(lastComponent(string(values, 0, "Path", span)), false, span);
    });
    registerNative("Path.stem", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "stem", span);
        std::string name = lastComponent(string(values, 0, "Path", span)); size_t dot = name.rfind('.');
        return text(dot == std::string::npos || dot == 0 ? name : name.substr(0, dot), false, span);
    });
    registerNative("Path.extension", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "extension", span);
        std::string name = lastComponent(string(values, 0, "Path", span)); size_t dot = name.rfind('.');
        return text(dot == std::string::npos || dot == 0 ? "" : name.substr(dot), false, span);
    });
    registerNative("Path.parent", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "parent", span);
        std::string path = string(values, 0, "Path", span); size_t end = path.size(); while (end > 1 && path[end - 1] == '/') end--;
        size_t slash = end ? path.rfind('/', end - 1) : std::string::npos;
        if (slash == std::string::npos) return text(path.empty() || path[0] != '/' ? "." : "/", false, span);
        while (slash > 1 && path[slash - 1] == '/') slash--;
        return text(slash == 0 ? "/" : path.substr(0, slash), false, span);
    });
    registerNative("Path.isAbsolute", [arity, string, this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "isAbsolute", span);
        const std::string &path = string(values, 0, "Path", span); return !path.empty() && path[0] == '/';
    });
    registerNative("Path.parts", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "parts", span);
        const std::string &path = string(values, 0, "Path", span);
        Object *result = allocate(Kind::List, span); roots.push_back(result);
        if (!path.empty() && path[0] == '/') result->items.push_back(text("/", false, span));
        size_t start = 0;
        while (start <= path.size()) {
            size_t end = path.find('/', start); if (end == std::string::npos) end = path.size();
            if (end > start) result->items.push_back(text(path.substr(start, end - start), false, span));
            start = end + 1;
        }
        return result;
    });

    // ---- Encoding ----
    registerNative("Encoding.toHex", [this, arity, bytes](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "toHex", span);
        return text(toHex(bytes(values, 0, span)), false, span);
    });
    registerNative("Encoding.fromHex", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "fromHex", span);
        const std::string &source = string(values, 0, "Hex text", span); std::string result;
        if (source.size() % 2) throw Error(span, "Hex text needs an even number of digits", "value");
        for (size_t index = 0; index < source.size(); index += 2) {
            int high = hexValue(source[index]), low = hexValue(source[index + 1]);
            if (high < 0 || low < 0) throw Error(span, "Invalid hex digit", "value");
            result += static_cast<char>(high << 4 | low);
        }
        return text(std::move(result), true, span);
    });
    registerNative("Encoding.toBase64", [this, arity, bytes](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "toBase64", span);
        const std::string &data = bytes(values, 0, span); std::string result;
        for (size_t index = 0; index < data.size(); index += 3) {
            uint32_t chunk = static_cast<unsigned char>(data[index]) << 16;
            if (index + 1 < data.size()) chunk |= static_cast<unsigned char>(data[index + 1]) << 8;
            if (index + 2 < data.size()) chunk |= static_cast<unsigned char>(data[index + 2]);
            result += base64Alphabet[chunk >> 18 & 63]; result += base64Alphabet[chunk >> 12 & 63];
            result += index + 1 < data.size() ? base64Alphabet[chunk >> 6 & 63] : '='; result += index + 2 < data.size() ? base64Alphabet[chunk & 63] : '=';
        }
        return text(std::move(result), false, span);
    });
    registerNative("Encoding.fromBase64", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "fromBase64", span);
        const std::string &source = string(values, 0, "Base64 text", span); std::string result; uint32_t buffer = 0; int bits = 0; size_t padding = 0;
        for (char character : source) {
            if (character == '=') { padding++; continue; }
            const char *found = padding ? nullptr : std::strchr(base64Alphabet, character);
            if (!found || !character) throw Error(span, "Invalid Base64 text", "value");
            buffer = buffer << 6 | static_cast<uint32_t>(found - base64Alphabet); bits += 6;
            if (bits >= 8) { bits -= 8; result += static_cast<char>(buffer >> bits & 255); }
        }
        if (padding > 2 || (source.size() % 4 && padding) || (!padding && source.size() % 4 == 1)) throw Error(span, "Invalid Base64 text", "value");
        return text(std::move(result), true, span);
    });
    registerNative("Encoding.urlEncode", [this, arity, bytes](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "urlEncode", span);
        std::string result;
        for (unsigned char byte : bytes(values, 0, span)) {
            if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') result += static_cast<char>(byte);
            else { result += '%'; result += static_cast<char>(std::toupper(hexDigits[byte >> 4])); result += static_cast<char>(std::toupper(hexDigits[byte & 15])); }
        }
        return text(std::move(result), false, span);
    });
    registerNative("Encoding.urlDecode", [this, arity, string](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "urlDecode", span);
        const std::string &source = string(values, 0, "URL text", span); std::string result;
        for (size_t index = 0; index < source.size(); index++) {
            if (source[index] != '%') { result += source[index] == '+' ? ' ' : source[index]; continue; }
            if (index + 2 >= source.size()) throw Error(span, "Invalid percent escape", "value");
            int high = hexValue(source[index + 1]), low = hexValue(source[index + 2]);
            if (high < 0 || low < 0) throw Error(span, "Invalid percent escape", "value");
            result += static_cast<char>(high << 4 | low); index += 2;
        }
        return text(std::move(result), false, span);
    });

    // ---- Hash ----
    registerNative("Hash.sha256", [this, arity, bytes](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "sha256", span);
        std::string digest; for (uint32_t word : sha256(bytes(values, 0, span))) for (int shift = 24; shift >= 0; shift -= 8) digest += static_cast<char>(word >> shift & 255);
        return text(toHex(digest), false, span);
    });
    registerNative("Hash.crc32", [arity, bytes, this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span); arity(values, 1, 1, "crc32", span);
        return static_cast<int64_t>(crc32(bytes(values, 0, span)));
    });

    // ---- Random extras ----
    registerNative("Random.uuid", [this, arity](const auto &arguments, const Span &span) -> Value {
        arity(positional(arguments, span), 0, 0, "uuid", span);
        unsigned char raw[16]; for (unsigned char &byte : raw) byte = static_cast<unsigned char>(random() >> 24);
        raw[6] = static_cast<unsigned char>((raw[6] & 0x0f) | 0x40); raw[8] = static_cast<unsigned char>((raw[8] & 0x3f) | 0x80);
        std::string digits = toHex(std::string(reinterpret_cast<char *>(raw), 16));
        return text(digits.substr(0, 8) + '-' + digits.substr(8, 4) + '-' + digits.substr(12, 4) + '-' + digits.substr(16, 4) + '-' + digits.substr(20), false, span);
    });
}
}
