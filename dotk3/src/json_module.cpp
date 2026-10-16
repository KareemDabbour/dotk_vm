#include "runtime.hpp"
#include "../vendor/json.hpp"
#include <cmath>
#include <limits>
#include <unordered_set>

namespace dk3 {
using Json = nlohmann::ordered_json;
static constexpr size_t jsonByteLimit = 4 * 1024 * 1024;
static constexpr size_t jsonDepthLimit = 64;

class JsonReader : public nlohmann::json_sax<Json> {
    struct Frame { Object *value; std::string key; std::unordered_set<std::string> keys; };
    Runtime &runtime;
    Span span;
    std::vector<Frame> frames;
    Value result;
    bool add(Value value) {
        runtime.roots.push_back(value); runtime.tick(span);
        if (frames.empty()) result = value;
        else if (frames.back().value->kind == Kind::List) frames.back().value->items.push_back(value);
        else {
            Frame &frame = frames.back();
            frame.value->entries.emplace_back(runtime.text(frame.key, false, span), value);
            frame.key.clear();
        }
        return true;
    }
    bool begin(Kind kind) {
        if (frames.size() >= jsonDepthLimit) throw Error(span, "JSON nesting limit exceeded", "limit");
        Object *value = runtime.allocate(kind, span); add(value); frames.push_back({value, {}, {}}); return true;
    }
public:
    JsonReader(Runtime &owner, Span location) : runtime(owner), span(std::move(location)) {}
    Value value() const { return result; }
    bool null() override { return add({}); }
    bool boolean(bool value) override { return add(value); }
    bool number_integer(number_integer_t value) override { return add(static_cast<int64_t>(value)); }
    bool number_unsigned(number_unsigned_t value) override {
        if (value > static_cast<uint64_t>(INT64_MAX)) throw Error(span, "JSON integer exceeds Int64", "overflow");
        return add(static_cast<int64_t>(value));
    }
    bool number_float(number_float_t value, const string_t &token) override {
        if (!std::isfinite(value)) throw Error(span, "JSON Float is not finite", "overflow");
        if (token.find_first_of(".eE") == std::string::npos) throw Error(span, "JSON integer exceeds Int64", "overflow");
        return add(static_cast<double>(value));
    }
    bool string(string_t &value) override { return add(runtime.text(value, false, span)); }
    bool binary(binary_t &) override { throw Error(span, "JSON binary values are not supported", "json"); }
    bool start_object(std::size_t) override { return begin(Kind::Map); }
    bool key(string_t &value) override {
        runtime.tick(span);
        Frame &frame = frames.back();
        if (!frame.keys.insert(value).second) throw Error(span, "Duplicate JSON object key", "json");
        frame.key = value; return true;
    }
    bool end_object() override { frames.pop_back(); return true; }
    bool start_array(std::size_t) override { return begin(Kind::List); }
    bool end_array() override { frames.pop_back(); return true; }
    bool parse_error(std::size_t position, const std::string &, const Json::exception &) override {
        throw Error(span, "Invalid JSON at byte " + std::to_string(position), "json");
    }
};

void Runtime::installJson() {
    registerNative("Json.parse", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 1 || !std::holds_alternative<Object *>(values[0]) || std::get<Object *>(values[0])->kind != Kind::String)
            throw Error(span, "Json.parse expects one String", "call");
        const auto &source = std::get<Object *>(values[0])->text;
        if (source.size() > jsonByteLimit) throw Error(span, "JSON input exceeds byte limit", "limit");
        JsonReader reader(*this, span);
        try {
            if (!Json::sax_parse(source.begin(), source.end(), &reader)) throw Error(span, "Invalid JSON", "json");
            return reader.value();
        } catch (const Json::exception &) { throw Error(span, "Invalid JSON", "json"); }
    });
    registerNative("Json.stringify", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "Json.stringify expects one value", "call");
        std::unordered_set<Object *> active;
        size_t bytes = 0;
        auto account = [&](size_t amount) {
            if (amount > jsonByteLimit - bytes) throw Error(span, "JSON output exceeds byte limit", "limit");
            bytes += amount;
        };
        std::function<Json(Value, size_t)> encode = [&](Value value, size_t depth) -> Json {
            tick(span);
            if (std::holds_alternative<std::monostate>(value)) { account(4); return nullptr; }
            if (std::holds_alternative<bool>(value)) { account(5); return std::get<bool>(value); }
            if (std::holds_alternative<int64_t>(value)) { account(24); return std::get<int64_t>(value); }
            if (std::holds_alternative<double>(value)) {
                double number = std::get<double>(value);
                if (!std::isfinite(number)) throw Error(span, "JSON Float is not finite", "json");
                account(32); return number;
            }
            Object *object = std::get<Object *>(value);
            if (object->kind == Kind::String) { account(object->text.size() + 2); return object->text; }
            if (object->kind != Kind::List && object->kind != Kind::Tuple && object->kind != Kind::Map)
                throw Error(span, "Unsupported JSON value: " + typeOf(value), "json");
            if (depth >= jsonDepthLimit) throw Error(span, "JSON nesting limit exceeded", "limit");
            if (!active.insert(object).second) throw Error(span, "Cyclic value cannot be encoded as JSON", "json");
            account(2); Json result = object->kind == Kind::Map ? Json::object() : Json::array();
            if (object->kind == Kind::Map) {
                for (const auto &entry : object->entries) {
                    if (!std::holds_alternative<Object *>(entry.first) || std::get<Object *>(entry.first)->kind != Kind::String)
                        throw Error(span, "JSON object keys must be String", "json");
                    const auto &key = std::get<Object *>(entry.first)->text; account(key.size() + 4);
                    result[key] = encode(entry.second, depth + 1);
                }
            } else for (Value item : object->items) { account(1); result.push_back(encode(item, depth + 1)); }
            active.erase(object); return result;
        };
        try {
            std::string output = encode(values[0], 0).dump();
            if (output.size() > jsonByteLimit) throw Error(span, "JSON output exceeds byte limit", "limit");
            return text(std::move(output), false, span);
        } catch (const Json::exception &) { throw Error(span, "Cannot encode JSON value", "json"); }
    });
}
}