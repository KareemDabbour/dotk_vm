#include "runtime.hpp"
#include "../vendor/json.hpp"
#include <cmath>
#include <limits>
#include <unordered_set>

namespace dk3 {
using Json = nlohmann::ordered_json;

// Length of the well-formed UTF-8 sequence at `index`, or 0 when it is malformed.
static size_t utf8Sequence(const std::string &text, size_t index) {
    auto at = [&](size_t offset) -> unsigned { return index + offset < text.size() ? static_cast<unsigned char>(text[index + offset]) : 0; };
    auto continues = [&](size_t offset, unsigned low = 0x80, unsigned high = 0xBF) { return at(offset) >= low && at(offset) <= high; };
    unsigned first = at(0);
    if (first >= 0xC2 && first <= 0xDF) return continues(1) ? 2 : 0;
    if (first == 0xE0) return continues(1, 0xA0) && continues(2) ? 3 : 0;
    if (first == 0xED) return continues(1, 0x80, 0x9F) && continues(2) ? 3 : 0;
    if (first >= 0xE1 && first <= 0xEF) return continues(1) && continues(2) ? 3 : 0;
    if (first == 0xF0) return continues(1, 0x90) && continues(2) && continues(3) ? 4 : 0;
    if (first >= 0xF1 && first <= 0xF3) return continues(1) && continues(2) && continues(3) ? 4 : 0;
    if (first == 0xF4) return continues(1, 0x80, 0x8F) && continues(2) && continues(3) ? 4 : 0;
    return 0;
}

// Same escaping as nlohmann's compact dump: non-ASCII stays verbatim, other controls become \u00xx.
static void appendJsonString(std::string &output, const std::string &text, const Span &span) {
    static const char hex[] = "0123456789abcdef";
    output += '"';
    size_t run = 0;
    for (size_t index = 0; index < text.size();) {
        unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte >= 0x20 && byte != '"' && byte != '\\' && byte < 0x80) { index++; continue; }
        output.append(text, run, index - run);
        if (byte >= 0x80) {
            size_t length = utf8Sequence(text, index);
            if (!length) throw Error(span, "Cannot encode JSON value", "json");
            output.append(text, index, length);
            index += length;
        } else {
            switch (byte) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += "\\u00"; output += hex[byte >> 4]; output += hex[byte & 15];
            }
            index++;
        }
        run = index;
    }
    output.append(text, run, std::string::npos);
    output += '"';
}

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
        JsonReader reader(*this, span);
        try {
            if (!Json::sax_parse(source.begin(), source.end(), &reader)) throw Error(span, "Invalid JSON", "json");
            return reader.value();
        } catch (const Json::exception &) { throw Error(span, "Invalid JSON", "json"); }
    });
    registerNative("Json.stringify", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "Json.stringify expects one value", "call");
        struct Frame { Object *object; size_t index; };
        std::vector<Frame> stack;
        std::unordered_set<Object *> active;
        std::string output;
        // Scalars are written immediately; containers open here and close in the loop below.
        auto emit = [&](const Value &value) {
            tick(span);
            if (std::holds_alternative<std::monostate>(value)) { output += "null"; return; }
            if (std::holds_alternative<bool>(value)) { output += std::get<bool>(value) ? "true" : "false"; return; }
            if (std::holds_alternative<int64_t>(value)) { output += std::to_string(std::get<int64_t>(value)); return; }
            if (std::holds_alternative<double>(value)) {
                double number = std::get<double>(value);
                if (!std::isfinite(number)) throw Error(span, "JSON Float is not finite", "json");
                output += Json(number).dump(); return;
            }
            Object *object = std::get<Object *>(value);
            if (object->kind == Kind::String) { appendJsonString(output, object->text, span); return; }
            if (object->kind != Kind::List && object->kind != Kind::Tuple && object->kind != Kind::Map)
                throw Error(span, "Unsupported JSON value: " + typeOf(value), "json");
            if (!active.insert(object).second) throw Error(span, "Cyclic value cannot be encoded as JSON", "json");
            output += object->kind == Kind::Map ? '{' : '[';
            stack.push_back({object, 0});
        };
        emit(values[0]);
        while (!stack.empty()) {
            Object *object = stack.back().object;
            bool map = object->kind == Kind::Map;
            size_t index = stack.back().index++;
            if (index == (map ? object->entries.size() : object->items.size())) {
                output += map ? '}' : ']';
                active.erase(object);
                stack.pop_back();
                continue;
            }
            if (index) output += ',';
            if (!map) { emit(object->items[index]); continue; }
            const Value &key = object->entries[index].first;
            if (!std::holds_alternative<Object *>(key) || std::get<Object *>(key)->kind != Kind::String)
                throw Error(span, "JSON object keys must be String", "json");
            appendJsonString(output, std::get<Object *>(key)->text, span);
            output += ':';
            emit(object->entries[index].second);
        }
        return text(std::move(output), false, span);
    });
}
}