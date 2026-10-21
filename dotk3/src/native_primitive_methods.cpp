#include "native_methods.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace dk3
{

    static int64_t intArg(Value v, const Span &span)
    {
        if (!std::holds_alternative<int64_t>(v))
            throw Error(span, "Expected Int argument", "type");
        return std::get<int64_t>(v);
    }
    static double floatArg(Value v, const Span &span)
    {
        if (!std::holds_alternative<double>(v))
            throw Error(span, "Expected Float argument", "type");
        return std::get<double>(v);
    }

    void PrimitiveMethods::install(Runtime &runtime)
    {
        // ---- Range (no heap allocation — stored in kindMethods) -----------------
        runtime.registerKindMethod(Kind::Range, {"map", "List", 1, 1, [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                 {
                                                     Object *callback = NativeMethods::callback(arguments[0].value, span);
                                                     Object *result = runtime.allocate(Kind::List, span);
                                                     runtime.roots.push_back(result);
                                                     Value item;
                                                     size_t cursor = 0;
                                                     while (runtime.next(receiver, item, cursor, span))
                                                     {
                                                         runtime.tick(span);
                                                         result->items.push_back(runtime.call(callback, {{"", item}}, span));
                                                     }
                                                     return result;
                                                 }});
        runtime.registerKindMethod(Kind::Range, {"filter", "List", 1, 1, [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                 {
                                                     Object *callback = NativeMethods::callback(arguments[0].value, span);
                                                     Object *result = runtime.allocate(Kind::List, span);
                                                     runtime.roots.push_back(result);
                                                     Value item;
                                                     size_t cursor = 0;
                                                     while (runtime.next(receiver, item, cursor, span))
                                                     {
                                                         runtime.tick(span);
                                                         if (runtime.boolean(runtime.call(callback, {{"", item}}, span), span))
                                                             result->items.push_back(item);
                                                     }
                                                     return result;
                                                 }});
        runtime.registerKindMethod(Kind::Range, {"reduce", "Any", 2, 2, [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                 {
                                                     Object *callback = NativeMethods::callback(arguments[0].value, span);
                                                     Value acc = arguments[1].value;
                                                     Value item;
                                                     size_t cursor = 0;
                                                     while (runtime.next(receiver, item, cursor, span))
                                                     {
                                                         runtime.tick(span);
                                                         acc = runtime.call(callback, {{"", acc}, {"", item}}, span);
                                                         runtime.roots.push_back(acc);
                                                     }
                                                     return acc;
                                                 }});
        runtime.registerKindMethod(Kind::Range, {"any", "Bool", 1, 1, [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                 {
                                                     Object *callback = NativeMethods::callback(arguments[0].value, span);
                                                     Value item;
                                                     size_t cursor = 0;
                                                     while (runtime.next(receiver, item, cursor, span))
                                                     {
                                                         runtime.tick(span);
                                                         if (runtime.boolean(runtime.call(callback, {{"", item}}, span), span))
                                                             return true;
                                                     }
                                                     return false;
                                                 }});
        runtime.registerKindMethod(Kind::Range, {"all", "Bool", 1, 1, [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                 {
                                                     Object *callback = NativeMethods::callback(arguments[0].value, span);
                                                     Value item;
                                                     size_t cursor = 0;
                                                     while (runtime.next(receiver, item, cursor, span))
                                                     {
                                                         runtime.tick(span);
                                                         if (!runtime.boolean(runtime.call(callback, {{"", item}}, span), span))
                                                             return false;
                                                     }
                                                     return true;
                                                 }});
        runtime.registerKindMethod(Kind::Range, {"contains", "Bool", 1, 1, [](Runtime &, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                 {
                                                     if (!std::holds_alternative<int64_t>(arguments[0].value))
                                                         throw Error(span, "contains expects Int", "type");
                                                     int64_t start = std::get<int64_t>(receiver->items[0]), stop = std::get<int64_t>(receiver->items[1]), step = std::get<int64_t>(receiver->items[2]);
                                                     int64_t v = std::get<int64_t>(arguments[0].value);
                                                     if (step > 0 ? (v < start || v >= stop) : (v > start || v <= stop))
                                                         return false;
                                                     return (v - start) % step == 0;
                                                 }});
        runtime.registerKindMethod(Kind::Range, {"toList", "List", 0, 0, [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value
                                                 {
                                                     Object *result = runtime.allocate(Kind::List, span);
                                                     runtime.roots.push_back(result);
                                                     Value item;
                                                     size_t cursor = 0;
                                                     while (runtime.next(receiver, item, cursor, span))
                                                     {
                                                         runtime.tick(span);
                                                         result->items.push_back(item);
                                                     }
                                                     return result;
                                                 }});
        // reverse() on Range operates on raw items (for step-budget testing via corrupted Range objects)
        runtime.registerKindMethod(Kind::Range, {"reverse", "Null", 0, 0, [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value
                                                 {
                                                     for (size_t index = 0; index < receiver->items.size() / 2; index++)
                                                         runtime.tick(span);
                                                     std::reverse(receiver->items.begin(), receiver->items.end());
                                                     return {};
                                                 }});

        // ---- Int ----------------------------------------------------------------
        runtime.registerPrimitiveMethod("Int", {"abs", "Int", 0, 0, [](Runtime &, Value recv, const auto &, const Span &) -> Value
                                                {
                                                    int64_t v = std::get<int64_t>(recv);
                                                    return v == std::numeric_limits<int64_t>::min() ? v : (v < 0 ? -v : v);
                                                }});
        runtime.registerPrimitiveMethod("Int", {"clamp", "Int", 2, 2, [](Runtime &, Value recv, const auto &args, const Span &span) -> Value
                                                {
                                                    return std::clamp(std::get<int64_t>(recv), intArg(args[0].value, span), intArg(args[1].value, span));
                                                }});
        runtime.registerPrimitiveMethod("Int", {"coerceAtLeast", "Int", 1, 1, [](Runtime &, Value recv, const auto &args, const Span &span) -> Value
                                                {
                                                    return std::max(std::get<int64_t>(recv), intArg(args[0].value, span));
                                                }});
        runtime.registerPrimitiveMethod("Int", {"coerceAtMost", "Int", 1, 1, [](Runtime &, Value recv, const auto &args, const Span &span) -> Value
                                                {
                                                    return std::min(std::get<int64_t>(recv), intArg(args[0].value, span));
                                                }});
        runtime.registerPrimitiveMethod("Int", {"toFloat", "Float", 0, 0, [](Runtime &, Value recv, const auto &, const Span &) -> Value
                                                {
                                                    return static_cast<double>(std::get<int64_t>(recv));
                                                }});
        runtime.registerPrimitiveMethod("Int", {"toString", "String", 0, 1, [](Runtime &runtime, Value recv, const auto &args, const Span &span) -> Value
                                                {
                                                    int64_t v = std::get<int64_t>(recv);
                                                    int radix = args.empty() ? 10 : static_cast<int>(intArg(args[0].value, span));
                                                    if (radix < 2 || radix > 36)
                                                        throw Error(span, "toString radix must be between 2 and 36", "value");
                                                    if (radix == 10)
                                                        return runtime.text(std::to_string(v), false, span);
                                                    bool negative = v < 0;
                                                    uint64_t uv = negative ? static_cast<uint64_t>(-(v + 1)) + 1 : static_cast<uint64_t>(v);
                                                    std::string result;
                                                    do
                                                    {
                                                        result += "0123456789abcdefghijklmnopqrstuvwxyz"[uv % static_cast<uint64_t>(radix)];
                                                        uv /= static_cast<uint64_t>(radix);
                                                    } while (uv);
                                                    if (negative)
                                                        result += '-';
                                                    std::reverse(result.begin(), result.end());
                                                    return runtime.text(result, false, span);
                                                }});

        // ---- Float --------------------------------------------------------------
        runtime.registerPrimitiveMethod("Float", {"abs", "Float", 0, 0, [](Runtime &, Value recv, const auto &, const Span &) -> Value
                                                  {
                                                      return std::abs(std::get<double>(recv));
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"clamp", "Float", 2, 2, [](Runtime &, Value recv, const auto &args, const Span &span) -> Value
                                                  {
                                                      return std::clamp(std::get<double>(recv), floatArg(args[0].value, span), floatArg(args[1].value, span));
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"coerceAtLeast", "Float", 1, 1, [](Runtime &, Value recv, const auto &args, const Span &span) -> Value
                                                  {
                                                      return std::max(std::get<double>(recv), floatArg(args[0].value, span));
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"coerceAtMost", "Float", 1, 1, [](Runtime &, Value recv, const auto &args, const Span &span) -> Value
                                                  {
                                                      return std::min(std::get<double>(recv), floatArg(args[0].value, span));
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"toInt", "Int", 0, 0, [](Runtime &, Value recv, const auto &, const Span &span) -> Value
                                                  {
                                                      double v = std::get<double>(recv);
                                                      if (!std::isfinite(v) || v < static_cast<double>(std::numeric_limits<int64_t>::min()) || v > static_cast<double>(std::numeric_limits<int64_t>::max()))
                                                          throw Error(span, "Float is outside Int range", "overflow");
                                                      return static_cast<int64_t>(v);
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"floor", "Int", 0, 0, [](Runtime &, Value recv, const auto &, const Span &span) -> Value
                                                  {
                                                      double v = std::floor(std::get<double>(recv));
                                                      if (!std::isfinite(v))
                                                          throw Error(span, "Float is outside Int range", "overflow");
                                                      return static_cast<int64_t>(v);
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"ceil", "Int", 0, 0, [](Runtime &, Value recv, const auto &, const Span &span) -> Value
                                                  {
                                                      double v = std::ceil(std::get<double>(recv));
                                                      if (!std::isfinite(v))
                                                          throw Error(span, "Float is outside Int range", "overflow");
                                                      return static_cast<int64_t>(v);
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"round", "Int", 0, 0, [](Runtime &, Value recv, const auto &, const Span &span) -> Value
                                                  {
                                                      double v = std::round(std::get<double>(recv));
                                                      if (!std::isfinite(v))
                                                          throw Error(span, "Float is outside Int range", "overflow");
                                                      return static_cast<int64_t>(v);
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"isNaN", "Bool", 0, 0, [](Runtime &, Value recv, const auto &, const Span &) -> Value
                                                  {
                                                      return std::isnan(std::get<double>(recv));
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"isInfinite", "Bool", 0, 0, [](Runtime &, Value recv, const auto &, const Span &) -> Value
                                                  {
                                                      return std::isinf(std::get<double>(recv));
                                                  }});
        runtime.registerPrimitiveMethod("Float", {"toString", "String", 0, 0, [](Runtime &runtime, Value recv, const auto &, const Span &span) -> Value
                                                  {
                                                      double v = std::get<double>(recv);
                                                      if (std::isnan(v))
                                                          return runtime.text("NaN", false, span);
                                                      if (std::isinf(v))
                                                          return runtime.text(v > 0 ? "Infinity" : "-Infinity", false, span);
                                                      std::ostringstream os;
                                                      os.precision(17);
                                                      os << v;
                                                      std::string s = os.str();
                                                      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                                                          s += ".0";
                                                      return runtime.text(s, false, span);
                                                  }});

        // ---- Bool ---------------------------------------------------------------
        runtime.registerPrimitiveMethod("Bool", {"toString", "String", 0, 0, [](Runtime &runtime, Value recv, const auto &, const Span &span) -> Value
                                                 {
                                                     return runtime.text(std::get<bool>(recv) ? "true" : "false", false, span);
                                                 }});
    }

}
