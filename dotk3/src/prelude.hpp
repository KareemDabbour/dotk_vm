#pragma once

// Core library declarations for the typer, written in declaration-stub syntax
// (function bodies and static field initializers omitted).
namespace dk3 {
inline const char *preludeSource() {
    return R"STUB(
class String implements Collections_Sized, Iterable<String> {
    fn length() -> Int
    fn contains(needle: String) -> Bool
    fn startsWith(prefix: String) -> Bool
    fn endsWith(suffix: String) -> Bool
    fn find(needle: String) -> Int
    fn split(separator: String) -> List<String>
    fn join(values: Iterable<String>) -> String
}
class Bytes implements Collections_Sized, Iterable<Int> {
    fn length() -> Int
}
trait Collections_Sized {
    fn length() -> Int
}
trait Iterable<T> {
    fn __iter__() -> Generator<T>
}
class List<T> implements Collections_Sized, Iterable<T> {
    fn init(values: Iterable<T> = []) -> Null
    fn length() -> Int
    fn map<U>(callback: fn(T) -> U) -> List<U>
    fn filter(predicate: fn(T) -> Bool) -> List<T>
    fn reduce<A>(callback: fn(A, T) -> A, initial: A) -> A
    fn any(predicate: fn(T) -> Bool) -> Bool
    fn all(predicate: fn(T) -> Bool) -> Bool
    fn find(predicate: fn(T) -> Bool) -> T?
    fn findIndex(predicate: fn(T) -> Bool) -> Int
    fn append(value: T) -> Null
    fn clear() -> Null
    fn reverse() -> Null
    fn contains(value: T) -> Bool
    fn indexOf(value: T) -> Int
    fn insert(index: Int, value: T) -> Null
    fn removeAt(index: Int) -> T
    fn extend(values: Iterable<T>) -> Null
}
class Map<K, V> implements Collections_Sized, Iterable<K> {
    fn init(source: Map<K, V> = {}) -> Null
    fn length() -> Int
    fn clear() -> Null
    fn contains(key: K) -> Bool
    fn get(key: K) -> V?
    fn compute(key: K, callback: fn(K, V?) -> V?) -> V?
    fn computeIfAbsent(key: K, callback: fn(K) -> V?) -> V?
    fn computeIfPresent(key: K, callback: fn(K, V) -> V?) -> V?
    fn putIfAbsent(key: K, value: V) -> V?
    fn merge(key: K, value: V, callback: fn(V, V) -> V?) -> V?
    fn remove(key: K) -> Bool
    fn keys() -> List<K>
    fn values() -> List<V>
}
class Set<T> implements Collections_Sized, Iterable<T> {
    fn init(values: Iterable<T> = []) -> Null
    fn length() -> Int
    fn add(value: T) -> Bool
    fn remove(value: T) -> Bool
    fn contains(value: T) -> Bool
    fn clear() -> Null
    fn values() -> List<T>
    fn union(other: Set<T>) -> Set<T>
    fn intersection(other: Set<T>) -> Set<T>
    fn difference(other: Set<T>) -> Set<T>
    fn isSubsetOf(other: Set<T>) -> Bool
}
class Tuple<T> implements Collections_Sized, Iterable<T> {
    fn init(values: Iterable<T> = []) -> Null
    fn length() -> Int
}
class Generator<T> implements Iterable<T> {
}
class Task<T> {
    let status: String
    let done: Bool
    let value: T?
    let error: Error?
}
class Resource {
    let closed: Bool
    let type_name: String
}
enum Error_Kind {
    Syntax, Type, Name, Index, Key, Value, Overflow, Call, State, Access, Capability, IO, Json, Native, Thrown, Application, Limit, Cancelled, Match, Internal
}
class Error {
    let kind: Error.Kind
    let message: String
    let file: String = ''
    let line: Int = 0
    let column: Int = 0
}
class System {
    static let args: Tuple<String>
    static fn print(*values: Object?) -> Null
    static fn gc() -> Null
    static fn monotonicMillis() -> Int
    static fn delay(milliseconds: Int) -> Null
}
class Math {
    static let PI: Float
    static let TAU: Float
    static let E: Float
    static fn toInt(value: Float) -> Int
    static fn toFloat(value: Float) -> Float
    static fn sin(radians: Float) -> Float
    static fn cos(radians: Float) -> Float
    static fn tan(radians: Float) -> Float
    static fn asin(value: Float) -> Float
    static fn acos(value: Float) -> Float
    static fn atan(value: Float) -> Float
    static fn atan2(y: Float, x: Float) -> Float
    static fn sinh(value: Float) -> Float
    static fn cosh(value: Float) -> Float
    static fn tanh(value: Float) -> Float
    static fn toRadians(degrees: Float) -> Float
    static fn toDegrees(radians: Float) -> Float
    static fn sqrt(value: Float) -> Float
    static fn exp(value: Float) -> Float
    static fn log(value: Float) -> Float
    static fn log2(value: Float) -> Float
    static fn log10(value: Float) -> Float
    static fn pow(base: Float, exponent: Float) -> Float
    static fn hypot(x: Float, y: Float) -> Float
    static fn floor(value: Float) -> Int
    static fn ceil(value: Float) -> Int
    static fn round(value: Float) -> Int
}
class Collections {
    static fn range(stop: Int) -> List<Int>
    static fn rangeFrom(start: Int, stop: Int, step: Int = 1) -> List<Int>
    static fn len(value: Collections.Sized) -> Int
}
class Text {
    static fn str(value: Object?) -> String
    static fn encode(value: String) -> Bytes
    static fn decode(value: Bytes) -> String
}
class Random {
    static fn seed(value: Int) -> Null
    static fn float() -> Float
    static fn int(stop: Int) -> Int
    static fn intRange(start: Int, stop: Int) -> Int
    static fn choice<T>(items: List<T>) -> T
    static fn shuffle<T>(items: List<T>) -> Null
}
class Json {
    static fn parse(source: String) -> Object?
    static fn stringify(value: Object?) -> String
}
class Reflection {
    static fn type(value: Object?) -> String
    static fn members(value: Object?) -> List<String>
    static fn binding(name: String) -> Object?
    static fn bindings() -> List<String>
    static fn doc(value: Object?) -> String
}
class Tasks {
    static fn spawn<T>(generator: Generator<T>) -> Task<T>
    static fn pump(turns: Int) -> Int
    static fn cancel<T>(task: Task<T>) -> Null
    static fn close<T>(generator: Generator<T>) -> Null
}
)STUB";
}
}
