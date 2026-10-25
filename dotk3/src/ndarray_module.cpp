#include "runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace dk3 {
namespace {
// One array holds at most 2^25 elements (512 MiB of Values).
constexpr uint64_t maxElements = uint64_t{1} << 25;
constexpr size_t maxDimensions = 32;

// Element (i0, ..., in-1) of a view is items[offset + i0 * strides[0] + ... + in-1 * strides[n-1]].
struct View {
    std::vector<Value> *items = nullptr;
    int64_t offset = 0;
    std::vector<int64_t> shape, strides;
    uint64_t size = 1;
};

std::string describe(const std::vector<int64_t> &shape) {
    std::string result = "(";
    for (size_t axis = 0; axis < shape.size(); axis++) result += (axis ? ", " : "") + std::to_string(shape[axis]);
    return result + (shape.size() == 1 ? ",)" : ")");
}

std::vector<Value> &listArgument(const std::vector<Value> &values, size_t index, const char *name, const Span &span) {
    if (index >= values.size() || !std::holds_alternative<Object *>(values[index]) || std::get<Object *>(values[index])->kind != Kind::List)
        throw Error(span, std::string(name) + " must be a List", "type");
    return std::get<Object *>(values[index])->items;
}

int64_t intArgument(const std::vector<Value> &values, size_t index, const char *name, const Span &span) {
    if (index >= values.size() || !std::holds_alternative<int64_t>(values[index])) throw Error(span, std::string(name) + " must be an Int", "type");
    return std::get<int64_t>(values[index]);
}

double number(const Value &value, const Span &span) {
    if (const double *floating = std::get_if<double>(&value)) return *floating;
    if (const int64_t *integer = std::get_if<int64_t>(&value)) return static_cast<double>(*integer);
    throw Error(span, "Array elements must be Int or Float", "type");
}

double floatArgument(const std::vector<Value> &values, size_t index, const char *name, const Span &span) {
    if (index >= values.size() || (!std::holds_alternative<double>(values[index]) && !std::holds_alternative<int64_t>(values[index])))
        throw Error(span, std::string(name) + " must be a number", "type");
    return number(values[index], span);
}

const std::string &textArgument(const std::vector<Value> &values, size_t index, const char *name, Kind kind, const Span &span) {
    if (index >= values.size() || !std::holds_alternative<Object *>(values[index]) || std::get<Object *>(values[index])->kind != kind)
        throw Error(span, std::string(name) + (kind == Kind::Bytes ? " must be Bytes" : " must be a String"), "type");
    return std::get<Object *>(values[index])->text;
}

uint64_t elementCount(const std::vector<int64_t> &shape, const Span &span) {
    for (int64_t dimension : shape)
        if (dimension < 0) throw Error(span, "Array dimensions cannot be negative", "value");
    if (std::find(shape.begin(), shape.end(), 0) != shape.end()) return 0;
    uint64_t size = 1;
    for (int64_t dimension : shape)
        if (__builtin_mul_overflow(size, static_cast<uint64_t>(dimension), &size) || size > maxElements)
            throw Error(span, "Array has more than " + std::to_string(maxElements) + " elements", "value");
    return size;
}

void checkCount(int64_t count, const Span &span) {
    if (count < 0) throw Error(span, "Element count cannot be negative", "value");
    if (static_cast<uint64_t>(count) > maxElements) throw Error(span, "Array has more than " + std::to_string(maxElements) + " elements", "value");
}

// data and its layout (offset, dimensions..., strides...) at values[index], values[index + 1]; every reachable element must exist.
View viewArgument(const std::vector<Value> &values, size_t index, const char *name, const Span &span) {
    View view;
    view.items = &listArgument(values, index, name, span);
    if (index + 1 >= values.size() || !std::holds_alternative<Object *>(values[index + 1]) || std::get<Object *>(values[index + 1])->kind != Kind::Tuple)
        throw Error(span, std::string(name) + " layout must be a Tuple of Int", "type");
    const auto &layout = std::get<Object *>(values[index + 1])->items;
    if (layout.size() % 2 == 0 || (layout.size() - 1) / 2 > maxDimensions)
        throw Error(span, std::string(name) + " layout must be (offset, dimensions..., strides...) with at most 32 dimensions", "value");
    auto entry = [&](size_t at) {
        if (!std::holds_alternative<int64_t>(layout[at])) throw Error(span, std::string(name) + " layout must be a Tuple of Int", "type");
        return std::get<int64_t>(layout[at]);
    };
    size_t dimensions = (layout.size() - 1) / 2;
    view.offset = entry(0);
    for (size_t axis = 0; axis < dimensions; axis++) {
        view.shape.push_back(entry(1 + axis));
        view.strides.push_back(entry(1 + dimensions + axis));
    }
    view.size = elementCount(view.shape, span);
    if (view.size) {
        int64_t low = view.offset, high = view.offset;
        for (size_t axis = 0; axis < dimensions; axis++) {
            int64_t reach = 0;
            bool overflow = __builtin_mul_overflow(view.strides[axis], view.shape[axis] - 1, &reach);
            overflow = overflow || (reach < 0 ? __builtin_add_overflow(low, reach, &low) : __builtin_add_overflow(high, reach, &high));
            if (overflow) throw Error(span, std::string(name) + " layout overflows", "index");
        }
        if (low < 0 || static_cast<uint64_t>(high) >= view.items->size())
            throw Error(span, std::string(name) + " layout reaches outside its " + std::to_string(view.items->size()) + " elements", "index");
    }
    return view;
}

void sameShape(const View &first, const View &second, const Span &span) {
    if (first.shape != second.shape) throw Error(span, "Array shapes " + describe(first.shape) + " and " + describe(second.shape) + " differ", "value");
}

// Calls visit(offsets) for every element of shape in row-major order, with each view's element offset.
template <size_t Count, typename Tick, typename Visit>
void walk(const std::vector<int64_t> &shape, const View *const (&views)[Count], Tick &&tick, Visit &&visit) {
    if (std::find(shape.begin(), shape.end(), 0) != shape.end()) return;
    size_t dimensions = shape.size();
    std::vector<int64_t> index(dimensions, 0);
    int64_t offsets[Count], inner[Count];
    for (size_t view = 0; view < Count; view++) {
        offsets[view] = views[view]->offset;
        inner[view] = dimensions ? views[view]->strides.back() : 0;
    }
    int64_t length = dimensions ? shape.back() : 1;
    for (;;) {
        int64_t at[Count];
        std::copy(offsets, offsets + Count, at);
        for (int64_t position = 0; position < length; position++) {
            tick();
            visit(static_cast<const int64_t *>(at));
            for (size_t view = 0; view < Count; view++) at[view] += inner[view];
        }
        ptrdiff_t axis = static_cast<ptrdiff_t>(dimensions) - 2;
        for (; axis >= 0; axis--) {
            for (size_t view = 0; view < Count; view++) offsets[view] += views[view]->strides[axis];
            if (++index[axis] < shape[axis]) break;
            for (size_t view = 0; view < Count; view++) offsets[view] -= views[view]->strides[axis] * shape[axis];
            index[axis] = 0;
        }
        if (axis < 0) return;
    }
}

using BinaryOperation = double (*)(double, double);
BinaryOperation binaryOperation(const std::string &name, const Span &span) {
    static const std::unordered_map<std::string, BinaryOperation> operations{
        {"add", [](double a, double b) { return a + b; }},
        {"sub", [](double a, double b) { return a - b; }},
        {"mul", [](double a, double b) { return a * b; }},
        {"div", [](double a, double b) { return a / b; }},
        {"floordiv", [](double a, double b) { return std::floor(a / b); }},
        // Result takes the divisor's sign, like Python and NumPy.
        {"mod", [](double a, double b) { double rest = std::fmod(a, b); return rest != 0 && (rest < 0) != (b < 0) ? rest + b : rest; }},
        {"pow", [](double a, double b) { return std::pow(a, b); }},
        {"min", [](double a, double b) { return std::isnan(a) || std::isnan(b) ? NAN : std::min(a, b); }},
        {"max", [](double a, double b) { return std::isnan(a) || std::isnan(b) ? NAN : std::max(a, b); }},
        {"atan2", [](double a, double b) { return std::atan2(a, b); }},
        {"hypot", [](double a, double b) { return std::hypot(a, b); }},
        {"lt", [](double a, double b) { return a < b ? 1.0 : 0.0; }},
        {"le", [](double a, double b) { return a <= b ? 1.0 : 0.0; }},
        {"gt", [](double a, double b) { return a > b ? 1.0 : 0.0; }},
        {"ge", [](double a, double b) { return a >= b ? 1.0 : 0.0; }},
        {"eq", [](double a, double b) { return a == b ? 1.0 : 0.0; }},
        {"ne", [](double a, double b) { return a != b ? 1.0 : 0.0; }},
        {"and", [](double a, double b) { return a != 0 && b != 0 ? 1.0 : 0.0; }},
        {"or", [](double a, double b) { return a != 0 || b != 0 ? 1.0 : 0.0; }},
        {"xor", [](double a, double b) { return (a != 0) != (b != 0) ? 1.0 : 0.0; }}};
    auto found = operations.find(name);
    if (found == operations.end()) throw Error(span, "Unknown binary array operation '" + name + "'", "value");
    return found->second;
}

using UnaryOperation = double (*)(double);
UnaryOperation unaryOperation(const std::string &name, const Span &span) {
    static const std::unordered_map<std::string, UnaryOperation> operations{
        {"neg", [](double a) { return -a; }},
        {"abs", [](double a) { return std::fabs(a); }},
        {"sqrt", [](double a) { return std::sqrt(a); }},
        {"exp", [](double a) { return std::exp(a); }},
        {"log", [](double a) { return std::log(a); }},
        {"log2", [](double a) { return std::log2(a); }},
        {"log10", [](double a) { return std::log10(a); }},
        {"sin", [](double a) { return std::sin(a); }},
        {"cos", [](double a) { return std::cos(a); }},
        {"tan", [](double a) { return std::tan(a); }},
        {"asin", [](double a) { return std::asin(a); }},
        {"acos", [](double a) { return std::acos(a); }},
        {"atan", [](double a) { return std::atan(a); }},
        {"sinh", [](double a) { return std::sinh(a); }},
        {"cosh", [](double a) { return std::cosh(a); }},
        {"tanh", [](double a) { return std::tanh(a); }},
        {"floor", [](double a) { return std::floor(a); }},
        {"ceil", [](double a) { return std::ceil(a); }},
        // Halves round to even, like NumPy.
        {"round", [](double a) { return std::nearbyint(a); }},
        {"sign", [](double a) { return std::isnan(a) ? a : a > 0 ? 1.0 : a < 0 ? -1.0 : 0.0; }},
        {"not", [](double a) { return a == 0 ? 1.0 : 0.0; }},
        {"isnan", [](double a) { return std::isnan(a) ? 1.0 : 0.0; }},
        {"isfinite", [](double a) { return std::isfinite(a) ? 1.0 : 0.0; }}};
    auto found = operations.find(name);
    if (found == operations.end()) throw Error(span, "Unknown unary array operation '" + name + "'", "value");
    return found->second;
}

// Running state of one reduction lane.
struct Reduction {
    enum class Kind { Sum, Product, Min, Max, Mean, ArgMin, ArgMax, Variance, Deviation, Any, All } kind;
    double value = 0, mean = 0, squares = 0;
    int64_t count = 0, best = -1;

    static Kind parse(const std::string &name, const Span &span) {
        static const std::unordered_map<std::string, Kind> kinds{{"sum", Kind::Sum}, {"prod", Kind::Product}, {"min", Kind::Min}, {"max", Kind::Max},
            {"mean", Kind::Mean}, {"argmin", Kind::ArgMin}, {"argmax", Kind::ArgMax}, {"var", Kind::Variance}, {"std", Kind::Deviation},
            {"any", Kind::Any}, {"all", Kind::All}};
        auto found = kinds.find(name);
        if (found == kinds.end()) throw Error(span, "Unknown array reduction '" + name + "'", "value");
        return found->second;
    }
    explicit Reduction(Kind kind) : kind(kind) { value = kind == Kind::Product || kind == Kind::All ? 1 : 0; }
    void add(double item) {
        switch (kind) {
        case Kind::Sum: value += item; break;
        case Kind::Product: value *= item; break;
        case Kind::Min: case Kind::Max: case Kind::ArgMin: case Kind::ArgMax: {
            // NaN wins and stays, as in NumPy.
            bool better = count == 0 || (!std::isnan(value) && (std::isnan(item) || (kind == Kind::Min || kind == Kind::ArgMin ? item < value : item > value)));
            if (better) { value = item; best = count; }
            break;
        }
        case Kind::Mean: case Kind::Variance: case Kind::Deviation: {
            double delta = item - mean;
            mean += delta / static_cast<double>(count + 1);
            squares += delta * (item - mean);
            break;
        }
        case Kind::Any: if (item != 0) value = 1; break;
        case Kind::All: if (item == 0) value = 0; break;
        }
        count++;
    }
    double result(const Span &span) const {
        switch (kind) {
        case Kind::Min: case Kind::Max: case Kind::ArgMin: case Kind::ArgMax:
            if (!count) throw Error(span, "Cannot take the minimum or maximum of an empty array", "value");
            return kind == Kind::ArgMin || kind == Kind::ArgMax ? static_cast<double>(best) : value;
        case Kind::Mean: return count ? mean : NAN;
        case Kind::Variance: return count ? squares / static_cast<double>(count) : NAN;
        case Kind::Deviation: return count ? std::sqrt(squares / static_cast<double>(count)) : NAN;
        default: return value;
        }
    }
};

// NumPy-style text: shared decimals and width, rows on separate lines, long axes summarised as first 3 ... last 3.
class Formatter {
public:
    Formatter(const View &view, int64_t precision, bool summarise, const Span &span) : view(view), precision(precision), summarise(summarise), span(span) {}
    template <typename Tick>
    std::string run(Tick &&tick) {
        std::vector<double> shown;
        gather(0, view.offset, shown, tick);
        double largest = 0, smallest = INFINITY;
        bool integral = true;
        for (double item : shown) {
            if (!std::isfinite(item)) continue;
            double magnitude = std::fabs(item);
            largest = std::max(largest, magnitude);
            if (magnitude) smallest = std::min(smallest, magnitude);
            integral = integral && item == std::floor(item);
        }
        scientific = largest >= 1e16 || (!integral && (largest >= 1e8 || smallest < 1e-4));
        decimals = 0;
        if (!integral || scientific)
            for (double item : shown)
                if (std::isfinite(item)) decimals = std::max(decimals, neededDecimals(item));
        for (double item : shown) width = std::max(width, cell(item).size());
        // A 0-d whole number prints as 7.0, like a NumPy scalar.
        if (view.shape.empty() && !scientific && !decimals && std::isfinite(shown[0])) return cell(shown[0]) + "0";
        std::string out;
        emit(0, view.offset, out, tick);
        return out;
    }

private:
    const View &view;
    int64_t precision;
    bool summarise, scientific = false;
    const Span &span;
    int decimals = 0;
    size_t width = 0;

    // Fewest digits after the point, up to precision, that print item exactly.
    int neededDecimals(double item) const {
        for (int digits = 0; digits < precision; digits++) {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, scientific ? "%.*e" : "%.*f", digits, item);
            if (std::strtod(buffer, nullptr) == item) return digits;
        }
        return static_cast<int>(precision);
    }
    std::string cell(double item) const {
        if (std::isnan(item)) return "nan";
        if (std::isinf(item)) return item < 0 ? "-inf" : "inf";
        char buffer[64];
        if (scientific) {
            std::snprintf(buffer, sizeof buffer, "%.*e", decimals, item);
            std::string text = buffer;
            if (!decimals) text.insert(text.find('e'), ".");
            return text;
        }
        std::snprintf(buffer, sizeof buffer, "%.*f", decimals, item);
        std::string text = buffer;
        // Trailing zeros become spaces so decimal points line up: "1.5 ", "2.25", "3.  ".
        size_t digits = decimals ? text.size() - 1 - text.find_last_not_of('0') : 0;
        text.erase(text.size() - digits);
        if (!decimals) text += '.';
        text.append(digits, ' ');
        return text;
    }
    bool skipped(size_t axis, int64_t position) const {
        return summarise && view.shape[axis] > 6 && position == 3;
    }
    int64_t next(size_t axis, int64_t position) const {
        return skipped(axis, position) ? view.shape[axis] - 3 : position + 1;
    }
    template <typename Tick>
    void gather(size_t axis, int64_t offset, std::vector<double> &shown, Tick &tick) const {
        if (axis == view.shape.size()) {
            tick();
            shown.push_back(number((*view.items)[static_cast<size_t>(offset)], span));
            return;
        }
        for (int64_t position = 0; position < view.shape[axis]; position = next(axis, position)) {
            if (skipped(axis, position)) continue;
            gather(axis + 1, offset + position * view.strides[axis], shown, tick);
        }
    }
    template <typename Tick>
    void emit(size_t axis, int64_t offset, std::string &out, Tick &tick) const {
        if (axis == view.shape.size()) {
            tick();
            std::string text = cell(number((*view.items)[static_cast<size_t>(offset)], span));
            out.append(width - text.size(), ' ');
            out += text;
            return;
        }
        out += '[';
        bool first = true;
        for (int64_t position = 0; position < view.shape[axis]; position = next(axis, position)) {
            if (!first) {
                if (axis + 1 == view.shape.size()) out += ' ';
                else {
                    out.append(view.shape.size() - axis - 1, '\n');
                    out.append(axis + 1, ' ');
                }
            }
            first = false;
            if (skipped(axis, position)) {
                out += "...";
                continue;
            }
            emit(axis + 1, offset + position * view.strides[axis], out, tick);
        }
        out += ']';
    }
};
}

void Runtime::installNDArray() {
    auto ticker = [this](const Span &span) {
        const Span *where = &span;
        return [this, where] { tick(*where); };
    };
    auto floats = [this](std::vector<double> &&values, const Span &span) -> Value {
        Object *result = allocate(Kind::List, span);
        result->items.reserve(values.size());
        for (double value : values) result->items.emplace_back(value);
        return result;
    };
    registerNative("NDKernel.fill", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 2) throw Error(span, "fill expects count, value", "call");
        int64_t count = intArgument(values, 0, "count", span);
        checkCount(count, span);
        double value = floatArgument(values, 1, "value", span);
        Object *result = allocate(Kind::List, span);
        result->items.assign(static_cast<size_t>(count), Value{value});
        return result;
    });
    registerNative("NDKernel.arange", [this, floats](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 3) throw Error(span, "arange expects start, stop, step", "call");
        double start = floatArgument(values, 0, "start", span), stop = floatArgument(values, 1, "stop", span), step = floatArgument(values, 2, "step", span);
        if (step == 0 || !std::isfinite(start) || !std::isfinite(stop) || !std::isfinite(step)) throw Error(span, "arange needs finite bounds and a nonzero step", "value");
        double count = std::max(0.0, std::ceil((stop - start) / step));
        if (count > static_cast<double>(maxElements)) throw Error(span, "Array has more than " + std::to_string(maxElements) + " elements", "value");
        std::vector<double> items(static_cast<size_t>(count));
        for (size_t index = 0; index < items.size(); index++) {
            tick(span);
            items[index] = start + static_cast<double>(index) * step;
        }
        return floats(std::move(items), span);
    });
    registerNative("NDKernel.linspace", [this, floats](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 3) throw Error(span, "linspace expects start, stop, count", "call");
        double start = floatArgument(values, 0, "start", span), stop = floatArgument(values, 1, "stop", span);
        int64_t count = intArgument(values, 2, "count", span);
        checkCount(count, span);
        std::vector<double> items(static_cast<size_t>(count));
        for (size_t index = 0; index < items.size(); index++) {
            tick(span);
            items[index] = count == 1 ? start : start + (stop - start) * static_cast<double>(index) / static_cast<double>(count - 1);
        }
        if (count > 1) items.back() = stop;
        return floats(std::move(items), span);
    });
    // Uniform [0, 1) and Box-Muller normal samples from the runtime generator, so Random.seed makes them repeatable.
    registerNative("NDKernel.random", [this, floats](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "random expects count", "call");
        int64_t count = intArgument(values, 0, "count", span);
        checkCount(count, span);
        std::vector<double> items(static_cast<size_t>(count));
        for (double &item : items) {
            tick(span);
            item = static_cast<double>(random() >> 11) * 0x1.0p-53;
        }
        return floats(std::move(items), span);
    });
    registerNative("NDKernel.normal", [this, floats](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 3) throw Error(span, "normal expects count, mean, deviation", "call");
        int64_t count = intArgument(values, 0, "count", span);
        checkCount(count, span);
        double mean = floatArgument(values, 1, "mean", span), deviation = floatArgument(values, 2, "deviation", span);
        std::vector<double> items(static_cast<size_t>(count));
        for (size_t index = 0; index < items.size(); index += 2) {
            tick(span);
            double uniform = 1.0 - static_cast<double>(random() >> 11) * 0x1.0p-53, angle = 6.283185307179586 * static_cast<double>(random() >> 11) * 0x1.0p-53;
            double radius = std::sqrt(-2 * std::log(uniform));
            items[index] = mean + deviation * radius * std::cos(angle);
            if (index + 1 < items.size()) items[index + 1] = mean + deviation * radius * std::sin(angle);
        }
        return floats(std::move(items), span);
    });
    registerNative("NDKernel.copy", [this, ticker](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 2) throw Error(span, "copy expects data, layout", "call");
        View source = viewArgument(values, 0, "data", span);
        Object *result = allocate(Kind::List, span);
        result->items.reserve(source.size);
        const View *views[] = {&source};
        walk(source.shape, views, ticker(span), [&](const int64_t *at) { result->items.emplace_back(number((*source.items)[at[0]], span)); });
        return result;
    });
    registerNative("NDKernel.binary", [this, ticker](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 5) throw Error(span, "binary expects operation, left, leftLayout, right, rightLayout", "call");
        BinaryOperation operation = binaryOperation(textArgument(values, 0, "operation", Kind::String, span), span);
        View left = viewArgument(values, 1, "left", span), right = viewArgument(values, 3, "right", span);
        sameShape(left, right, span);
        Object *result = allocate(Kind::List, span);
        result->items.reserve(left.size);
        const View *views[] = {&left, &right};
        walk(left.shape, views, ticker(span), [&](const int64_t *at) {
            result->items.emplace_back(operation(number((*left.items)[at[0]], span), number((*right.items)[at[1]], span)));
        });
        return result;
    });
    registerNative("NDKernel.unary", [this, ticker](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 3) throw Error(span, "unary expects operation, data, layout", "call");
        UnaryOperation operation = unaryOperation(textArgument(values, 0, "operation", Kind::String, span), span);
        View source = viewArgument(values, 1, "data", span);
        Object *result = allocate(Kind::List, span);
        result->items.reserve(source.size);
        const View *views[] = {&source};
        walk(source.shape, views, ticker(span), [&](const int64_t *at) { result->items.emplace_back(operation(number((*source.items)[at[0]], span))); });
        return result;
    });
    registerNative("NDKernel.where", [this, ticker](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 6) throw Error(span, "where expects mask, maskLayout, left, leftLayout, right, rightLayout", "call");
        View mask = viewArgument(values, 0, "mask", span), left = viewArgument(values, 2, "left", span), right = viewArgument(values, 4, "right", span);
        sameShape(mask, left, span);
        sameShape(mask, right, span);
        Object *result = allocate(Kind::List, span);
        result->items.reserve(mask.size);
        const View *views[] = {&mask, &left, &right};
        walk(mask.shape, views, ticker(span), [&](const int64_t *at) {
            bool chosen = number((*mask.items)[at[0]], span) != 0;
            result->items.emplace_back(number(chosen ? (*left.items)[at[1]] : (*right.items)[at[2]], span));
        });
        return result;
    });
    // axis -1 reduces everything to one element; otherwise the result has the source shape without that axis.
    registerNative("NDKernel.reduce", [this, ticker, floats](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 4) throw Error(span, "reduce expects operation, data, layout, axis", "call");
        auto kind = Reduction::parse(textArgument(values, 0, "operation", Kind::String, span), span);
        View source = viewArgument(values, 1, "data", span);
        int64_t axis = intArgument(values, 3, "axis", span);
        if (axis < -1 || axis >= static_cast<int64_t>(source.shape.size())) throw Error(span, "Axis " + std::to_string(axis) + " is out of range for shape " + describe(source.shape), "index");
        if (axis == -1) {
            Reduction lane(kind);
            const View *views[] = {&source};
            walk(source.shape, views, ticker(span), [&](const int64_t *at) { lane.add(number((*source.items)[at[0]], span)); });
            return floats({lane.result(span)}, span);
        }
        View outer = source;
        outer.shape.erase(outer.shape.begin() + axis);
        outer.strides.erase(outer.strides.begin() + axis);
        int64_t length = source.shape[axis], stride = source.strides[axis];
        std::vector<double> items;
        const View *views[] = {&outer};
        walk(outer.shape, views, ticker(span), [&](const int64_t *at) {
            Reduction lane(kind);
            for (int64_t position = 0; position < length; position++) {
                tick(span);
                lane.add(number((*source.items)[at[0] + position * stride], span));
            }
            items.push_back(lane.result(span));
        });
        return floats(std::move(items), span);
    });
    registerNative("NDKernel.matmul", [this, ticker, floats](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 4) throw Error(span, "matmul expects left, leftLayout, right, rightLayout", "call");
        View left = viewArgument(values, 0, "left", span), right = viewArgument(values, 2, "right", span);
        if (left.shape.size() != 2 || right.shape.size() != 2 || left.shape[1] != right.shape[0])
            throw Error(span, "matmul needs (n, k) and (k, m) arrays, got " + describe(left.shape) + " and " + describe(right.shape), "value");
        int64_t rows = left.shape[0], inner = left.shape[1], columns = right.shape[1];
        elementCount({rows, columns}, span);
        auto dense = [&](const View &view) {
            std::vector<double> items;
            items.reserve(view.size);
            const View *views[] = {&view};
            walk(view.shape, views, ticker(span), [&](const int64_t *at) { items.push_back(number((*view.items)[at[0]], span)); });
            return items;
        };
        std::vector<double> first = dense(left), second = dense(right), product(static_cast<size_t>(rows * columns), 0.0);
        for (int64_t row = 0; row < rows; row++)
            for (int64_t k = 0; k < inner; k++) {
                tick(span);
                double factor = first[static_cast<size_t>(row * inner + k)];
                const double *source = second.data() + k * columns;
                double *target = product.data() + row * columns;
                for (int64_t column = 0; column < columns; column++) target[column] += factor * source[column];
            }
        return floats(std::move(product), span);
    });
    registerNative("NDKernel.assign", [this, ticker](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 4) throw Error(span, "assign expects target, targetLayout, source, sourceLayout", "call");
        View target = viewArgument(values, 0, "target", span), source = viewArgument(values, 2, "source", span);
        sameShape(target, source, span);
        // Read everything first so overlapping views of one list copy correctly.
        std::vector<double> items;
        items.reserve(source.size);
        const View *sources[] = {&source};
        walk(source.shape, sources, ticker(span), [&](const int64_t *at) { items.push_back(number((*source.items)[at[0]], span)); });
        size_t next = 0;
        const View *targets[] = {&target};
        walk(target.shape, targets, ticker(span), [&](const int64_t *at) { (*target.items)[at[0]] = items[next++]; });
        return {};
    });
    registerNative("NDKernel.fromBytes", [this, floats](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 2) throw Error(span, "fromBytes expects data, scale", "call");
        const std::string &data = textArgument(values, 0, "data", Kind::Bytes, span);
        double scale = floatArgument(values, 1, "scale", span);
        if (data.size() > maxElements) throw Error(span, "Array has more than " + std::to_string(maxElements) + " elements", "value");
        std::vector<double> items(data.size());
        for (size_t index = 0; index < data.size(); index++) {
            tick(span);
            items[index] = static_cast<unsigned char>(data[index]) * scale;
        }
        return floats(std::move(items), span);
    });
    // Each element times scale, rounded and clamped to 0..255; NaN becomes 0.
    registerNative("NDKernel.toBytes", [this, ticker](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 3) throw Error(span, "toBytes expects data, layout, scale", "call");
        View source = viewArgument(values, 0, "data", span);
        double scale = floatArgument(values, 2, "scale", span);
        std::string bytes;
        bytes.reserve(source.size);
        const View *views[] = {&source};
        walk(source.shape, views, ticker(span), [&](const int64_t *at) {
            double item = std::nearbyint(number((*source.items)[at[0]], span) * scale);
            bytes += static_cast<char>(std::isnan(item) ? 0 : static_cast<int>(std::clamp(item, 0.0, 255.0)));
        });
        return text(std::move(bytes), true, span);
    });
    registerNative("NDKernel.format", [this, ticker](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 3) throw Error(span, "format expects data, layout, precision", "call");
        View source = viewArgument(values, 0, "data", span);
        int64_t precision = intArgument(values, 2, "precision", span);
        if (precision < 0 || precision > 17) throw Error(span, "precision must be from 0 to 17", "value");
        return text(Formatter(source, precision, source.size > 1000, span).run(ticker(span)), false, span);
    });
}
}
