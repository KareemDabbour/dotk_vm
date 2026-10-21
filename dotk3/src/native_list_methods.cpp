#include "native_methods.hpp"

namespace dk3
{
    template <ListMethods::Transform operation>
    Value ListMethods::transform(Runtime &runtime, Object *receiver, const std::vector<Argument> &arguments, const Span &span)
    {
        Object *callback = NativeMethods::callback(arguments[0].value, span);
        Object *snapshot = NativeMethods::snapshot(runtime, receiver, span);
        Object *result = nullptr;
        if constexpr (operation == Transform::Map || operation == Transform::Filter)
        {
            result = runtime.allocate(Kind::List, span);
            runtime.roots.push_back(result);
        }
        Value accumulator;
        if constexpr (operation == Transform::Reduce)
            accumulator = arguments[1].value;
        for (size_t index = 0; index < snapshot->items.size(); index++)
        {
            runtime.tick(span);
            Value item = snapshot->items[index];
            Value mapped;
            if constexpr (operation == Transform::Reduce)
                mapped = runtime.call(callback, {{"", accumulator}, {"", item}}, span);
            else
                mapped = runtime.call(callback, {{"", item}}, span);
            if constexpr (operation == Transform::Map)
                result->items.push_back(mapped);
            else if constexpr (operation == Transform::Reduce)
            {
                accumulator = mapped;
                runtime.roots.push_back(accumulator);
            }
            else
            {
                bool matched = runtime.boolean(mapped, span);
                if constexpr (operation == Transform::Filter)
                {
                    if (matched)
                        result->items.push_back(item);
                }
                if constexpr (operation == Transform::Any)
                {
                    if (matched)
                        return true;
                }
                if constexpr (operation == Transform::All)
                {
                    if (!matched)
                        return false;
                }
                if constexpr (operation == Transform::Find)
                {
                    if (matched)
                        return item;
                }
                if constexpr (operation == Transform::FindIndex)
                {
                    if (matched)
                        return static_cast<int64_t>(index);
                }
            }
        }
        if constexpr (operation == Transform::Map || operation == Transform::Filter)
            return result;
        if constexpr (operation == Transform::Reduce)
            return accumulator;
        if constexpr (operation == Transform::Any)
            return false;
        if constexpr (operation == Transform::All)
            return true;
        if constexpr (operation == Transform::FindIndex)
            return int64_t{-1};
        return {};
    }
    void ListMethods::install(Runtime &runtime)
    {
        NativeMethods::collection(runtime, Kind::List);
        NativeMethods::install(runtime, Kind::List, listMembers(), {{"map", transform<Transform::Map>}, {"filter", transform<Transform::Filter>}, {"reduce", transform<Transform::Reduce>}, {"any", transform<Transform::Any>}, {"all", transform<Transform::All>}, {"find", transform<Transform::Find>}, {"findIndex", transform<Transform::FindIndex>}, {"append", [](Runtime &, Object *receiver, const auto &arguments, const Span &) -> Value
                                                                                                                                                                                                                                                                                                                                                           {
                                                                                                                                                                                                                                                                                                                                                               Value value = arguments[0].value;
                                                                                                                                                                                                                                                                                                                                                               receiver->items.push_back(value);
                                                                                                                                                                                                                                                                                                                                                               return {};
                                                                                                                                                                                                                                                                                                                                                           }},
                                                                    {"clear", [](Runtime &, Object *receiver, const auto &, const Span &) -> Value
                                                                     { receiver->items.clear(); return {}; }},
                                                                    {"reverse", [](Runtime &runtime, Object *receiver, const auto &, const Span &span) -> Value
                                                                     {
                                                                         for (size_t index = 0; index < receiver->items.size() / 2; index++)
                                                                             runtime.tick(span);
                                                                         std::reverse(receiver->items.begin(), receiver->items.end());
                                                                         return {};
                                                                     }},
                                                                    {"contains", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                                     {
                                                                         return NativeMethods::find(runtime, receiver, arguments[0].value, span) >= 0;
                                                                     }},
                                                                    {"indexOf", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                                     {
                                                                         return NativeMethods::find(runtime, receiver, arguments[0].value, span);
                                                                     }},
                                                                    {"insert", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                                     {
                                                                         int64_t position = runtime.integer(arguments[0].value, span);
                                                                         if (position < 0)
                                                                             position += static_cast<int64_t>(receiver->items.size());
                                                                         if (position < 0 || static_cast<uint64_t>(position) > receiver->items.size())
                                                                             throw Error(span, "Insert index out of range", "index");
                                                                         Value value = arguments[1].value;
                                                                         receiver->items.insert(receiver->items.begin() + position, value);
                                                                         return {};
                                                                     }},
                                                                    {"removeAt", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                                     {
                                                                         int64_t position = runtime.integer(arguments[0].value, span);
                                                                         if (position < 0)
                                                                             position += static_cast<int64_t>(receiver->items.size());
                                                                         if (position < 0 || static_cast<uint64_t>(position) >= receiver->items.size())
                                                                             throw Error(span, "Remove index out of range", "index");
                                                                         Value result = receiver->items[position];
                                                                         receiver->items.erase(receiver->items.begin() + position);
                                                                         return result;
                                                                     }},
                                                                    {"extend", [](Runtime &runtime, Object *receiver, const auto &arguments, const Span &span) -> Value
                                                                     {
                                                                         Object *staged = runtime.allocate(Kind::List, span);
                                                                         runtime.roots.push_back(staged);
                                                                         Value item;
                                                                         size_t cursor = 0;
                                                                         while (runtime.next(arguments[0].value, item, cursor, span))
                                                                         {
                                                                             runtime.roots.push_back(item);
                                                                             runtime.tick(span);
                                                                             staged->items.push_back(item);
                                                                         }
                                                                         receiver->items.insert(receiver->items.end(), staged->items.begin(), staged->items.end());
                                                                         return {};
                                                                     }}});
    }
}