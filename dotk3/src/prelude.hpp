#pragma once

// Core library declarations for the typer, written in declaration-stub syntax
// (function bodies and static field initializers omitted).
namespace dk3 {
inline const char *preludeSource() {
    return R"STUB(
class String {
    fn length() -> Int
    fn contains(part: String) -> Bool
    fn startsWith(prefix: String) -> Bool
    fn endsWith(suffix: String) -> Bool
    fn find(part: String) -> Int
    fn split(separator: String) -> List<String>
    fn join(parts: List<String>) -> String
}
class Bytes {
    fn length() -> Int
}
class List<T> {
    fn length() -> Int
    fn append(item: T) -> Null
    fn insert(index: Int, item: T) -> Null
    fn removeAt(index: Int) -> T
    fn contains(item: T) -> Bool
    fn indexOf(item: T) -> Int
    fn clear() -> Null
    fn reverse() -> Null
    fn extend(items: List<T>) -> Null
    fn map<U>(transform: fn(T) -> U) -> List<U>
    fn filter(keep: fn(T) -> Bool) -> List<T>
    fn reduce<A>(combine: fn(A, T) -> A, initial: A) -> A
    fn find(matches: fn(T) -> Bool) -> T?
    fn findIndex(matches: fn(T) -> Bool) -> Int
}
class Map<K, V> {
    fn length() -> Int
    fn get(key: K) -> V?
    fn contains(key: K) -> Bool
    fn remove(key: K) -> Bool
    fn keys() -> List<K>
    fn values() -> List<V>
    fn clear() -> Null
}
class Set<T> {
    fn length() -> Int
    fn contains(item: T) -> Bool
    fn values() -> List<T>
}
class Tuple<T> {
    fn length() -> Int
}
class Generator<T> {
}
class Error {
    let message: String
}
class System {
    static fn print(*values: Object?) -> Null
    static fn gc() -> Null
}
class Math {
    static let PI: Float
    static let TAU: Float
    static let E: Float
    static fn sin(radians: Float) -> Float
    static fn cos(radians: Float) -> Float
    static fn tan(radians: Float) -> Float
    static fn sqrt(value: Float) -> Float
    static fn pow(base: Float, exponent: Float) -> Float
    static fn floor(value: Float) -> Int
    static fn ceil(value: Float) -> Int
    static fn round(value: Float) -> Int
    static fn toInt(value: Float) -> Int
    static fn toFloat(value: Int) -> Float
}
class Collections {
    static fn range(stop: Int) -> List<Int>
    static fn rangeFrom(start: Int, stop: Int, step: Int = 1) -> List<Int>
}
class Text {
    static fn str(value: Object?) -> String
}
)STUB";
}
}
