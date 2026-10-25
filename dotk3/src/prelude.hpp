#pragma once

// Core library declarations for the typer, written in declaration-stub syntax
// (function bodies and static field initializers omitted).
namespace dk3 {
inline const char *preludeSource() {
    return R"STUB(
class Int {
    fn abs() -> Int
    fn clamp(min: Int, max: Int) -> Int
    fn coerceAtLeast(min: Int) -> Int
    fn coerceAtMost(max: Int) -> Int
    fn toFloat() -> Float
    fn toString(radix: Int = 10) -> String
    fn toBytes(size: Int, order: String = 'big') -> Bytes
}
class Float {
    fn abs() -> Float
    fn clamp(min: Float, max: Float) -> Float
    fn coerceAtLeast(min: Float) -> Float
    fn coerceAtMost(max: Float) -> Float
    fn toInt() -> Int
    fn floor() -> Int
    fn ceil() -> Int
    fn round() -> Int
    fn isNaN() -> Bool
    fn isInfinite() -> Bool
    fn toString() -> String
}
class Bool {
    fn toString() -> String
}
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
    static fn of(values: Iterable<Int>) -> Bytes
    fn length() -> Int
    fn toInt(order: String = 'big') -> Int
    fn toSignedInt(order: String = 'big') -> Int
    fn readInt(offset: Int, size: Int, order: String = 'big') -> Int
    fn readSignedInt(offset: Int, size: Int, order: String = 'big') -> Int
    fn find(needle: Bytes, start: Int = 0) -> Int
    fn contains(needle: Bytes) -> Bool
    fn startsWith(prefix: Bytes) -> Bool
    fn endsWith(suffix: Bytes) -> Bool
    fn join(parts: Iterable<Bytes>) -> Bytes
}
trait Collections_Sized {
    fn length() -> Int
}
class Collections_Slice {
    let start: Int? = null
    let stop: Int? = null
    let step: Int? = null
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
class Range implements Iterable<Int> {
    fn map<U>(callback: fn(Int) -> U) -> List<U>
    fn filter(predicate: fn(Int) -> Bool) -> List<Int>
    fn reduce<A>(callback: fn(A, Int) -> A, initial: A) -> A
    fn any(predicate: fn(Int) -> Bool) -> Bool
    fn all(predicate: fn(Int) -> Bool) -> Bool
    fn contains(value: Int) -> Bool
    fn toList() -> List<Int>
}
class Generator<T> implements Iterable<T> {
    fn map<U>(callback: fn(T) -> U) -> Generator<U>
    fn filter(predicate: fn(T) -> Bool) -> Generator<T>
    fn flatMap<U>(callback: fn(T) -> Iterable<U>) -> Generator<U>
    fn take(count: Int) -> Generator<T>
    fn skip(count: Int) -> Generator<T>
    fn takeWhile(predicate: fn(T) -> Bool) -> Generator<T>
    fn dropWhile(predicate: fn(T) -> Bool) -> Generator<T>
    fn toList() -> List<T>
    fn toSet() -> Set<T>
    fn forEach(callback: fn(T) -> Null) -> Null
    fn count() -> Int
    fn first() -> T?
    fn reduce<A>(callback: fn(A, T) -> A, initial: A) -> A
    fn any(predicate: fn(T) -> Bool) -> Bool
    fn all(predicate: fn(T) -> Bool) -> Bool
    fn toMap<K, V>(keyMapper: fn(T) -> K, valueMapper: fn(T) -> V) -> Map<K, V>
}
class Collectors {
    static fn toList<T>(items: Iterable<T>) -> List<T>
    static fn toSet<T>(items: Iterable<T>) -> Set<T>
    static fn count<T>(items: Iterable<T>) -> Int
    static fn joining(items: Iterable<String>, separator: String = '') -> String
    static fn toMap<T, K, V>(items: Iterable<T>, keyMapper: fn(T) -> K, valueMapper: fn(T) -> V) -> Map<K, V>
    static fn groupBy<T, K>(items: Iterable<T>, classifier: fn(T) -> K) -> Map<K, List<T>>
    static fn partitionBy<T>(items: Iterable<T>, predicate: fn(T) -> Bool) -> Map<Bool, List<T>>
}
class Type {
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
    static fn platform() -> String
    static fn env(name: String) -> String?
}
class Time {
    static fn unixMillis() -> Int
    static fn monotonicMillis() -> Int
    static fn sleep(milliseconds: Int) -> Null
    static fn make(year: Int, month: Int, day: Int, hour: Int = 0, minute: Int = 0, second: Int = 0, millisecond: Int = 0) -> Int
    static fn parts(millis: Int) -> Map<String, Int>
    static fn format(millis: Int, pattern: String = '%Y-%m-%dT%H:%M:%SZ') -> String
    static fn parse(text: String) -> Int
}
class Path {
    static fn join(first: String, *more: String) -> String
    static fn normalize(path: String) -> String
    static fn name(path: String) -> String
    static fn stem(path: String) -> String
    static fn extension(path: String) -> String
    static fn parent(path: String) -> String
    static fn isAbsolute(path: String) -> Bool
    static fn parts(path: String) -> List<String>
}
class Encoding {
    static fn toHex(data: String | Bytes) -> String
    static fn fromHex(text: String) -> Bytes
    static fn toBase64(data: String | Bytes) -> String
    static fn fromBase64(text: String) -> Bytes
    static fn urlEncode(data: String | Bytes) -> String
    static fn urlDecode(text: String) -> String
}
class Hash {
    static fn sha256(data: String | Bytes) -> String
    static fn crc32(data: String | Bytes) -> Int
}
class ImageCodec {
    static fn info(data: Bytes) -> Tuple<Int>
    static fn decode(data: Bytes) -> Bytes
    static fn resize(pixels: Bytes, width: Int, height: Int, newWidth: Int, newHeight: Int) -> Bytes
}
class NDKernel {
    static fn fill(count: Int, value: Float) -> List<Float>
    static fn arange(start: Float, stop: Float, step: Float) -> List<Float>
    static fn linspace(start: Float, stop: Float, count: Int) -> List<Float>
    static fn random(count: Int) -> List<Float>
    static fn normal(count: Int, mean: Float, deviation: Float) -> List<Float>
    static fn copy(data: List<Float>, layout: Tuple<Int>) -> List<Float>
    static fn binary(operation: String, left: List<Float>, leftLayout: Tuple<Int>, right: List<Float>, rightLayout: Tuple<Int>) -> List<Float>
    static fn unary(operation: String, data: List<Float>, layout: Tuple<Int>) -> List<Float>
    static fn where(mask: List<Float>, maskLayout: Tuple<Int>, left: List<Float>, leftLayout: Tuple<Int>, right: List<Float>, rightLayout: Tuple<Int>) -> List<Float>
    static fn reduce(operation: String, data: List<Float>, layout: Tuple<Int>, axis: Int) -> List<Float>
    static fn matmul(left: List<Float>, leftLayout: Tuple<Int>, right: List<Float>, rightLayout: Tuple<Int>) -> List<Float>
    static fn assign(target: List<Float>, targetLayout: Tuple<Int>, source: List<Float>, sourceLayout: Tuple<Int>) -> Null
    static fn fromBytes(data: Bytes, scale: Float) -> List<Float>
    static fn toBytes(data: List<Float>, layout: Tuple<Int>, scale: Float) -> Bytes
    static fn format(data: List<Float>, layout: Tuple<Int>, precision: Int) -> String
}
class GC {
    static fn collect() -> Null
    static fn stats() -> Map<String, Int>
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
    static fn range(first: Int, second: Int? = null, step: Int = 1) -> Range
    static fn stream<T>(items: Iterable<T>) -> Generator<T>
    static fn len(value: Collections.Sized) -> Int
    static fn sorted<T>(items: Iterable<T>, compare: (fn(T, T) -> Int)? = null) -> List<T>
    static fn reversed<T>(items: Iterable<T>) -> List<T>
    static fn min<T>(items: Iterable<T>) -> T
    static fn max<T>(items: Iterable<T>) -> T
    static fn enumerate<T>(items: Iterable<T>, start: Int = 0) -> List<Tuple<Object?>>
    static fn zip<A, B>(first: Iterable<A>, second: Iterable<B>) -> List<Tuple<Object?>>
}
class Text {
    static fn str(value: Object?) -> String
    static fn encode(value: String) -> Bytes
    static fn decode(value: Bytes) -> String
    static fn parseInt(text: String, radix: Int = 10) -> Int
    static fn tryParseInt(text: String, radix: Int = 10) -> Int?
    static fn parseFloat(text: String) -> Float
    static fn tryParseFloat(text: String) -> Float?
    static fn parseBool(text: String) -> Bool
}
class Random {
    static fn seed(value: Int) -> Null
    static fn float() -> Float
    static fn int(first: Int, second: Int? = null) -> Int
    static fn choice<T>(items: List<T>) -> T
    static fn shuffle<T>(items: List<T>) -> Null
    static fn uuid() -> String
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
    static fn annotations(value: Object?, member: String? = null) -> List<String>
    static fn annotation(value: Object?, name: String, member: String? = null) -> List<Object?>?
    static fn has(value: Object?, name: String) -> Bool
    static fn get(value: Object?, name: String) -> Object?
    static fn isInstance(value: Object?, type: Type) -> Bool
    static fn id(value: Object) -> Int
}
class Tasks {
    static fn spawn<T>(generator: Generator<T>) -> Task<T>
    static fn pump(turns: Int) -> Int
    static fn cancel<T>(task: Task<T>) -> Null
    static fn close<T>(generator: Generator<T>) -> Null
}
class IO {
    static fn write(value: String | Bytes) -> Null
    static fn readLine() -> String?
}
class Files {
    static fn readText(path: String) -> String
    static fn readBytes(path: String) -> Bytes
    static fn exists(path: String) -> Bool
    static fn size(path: String) -> Int
    static fn open(path: String, mode: String = 'r') -> Resource
    static fn read(file: Resource, maximum: Int) -> Bytes
    static fn readLine(file: Resource) -> String?
    static fn write(file: Resource, data: String | Bytes) -> Int
    static fn seek(file: Resource, offset: Int, origin: String = 'start') -> Int
    static fn tell(file: Resource) -> Int
    static fn flush(file: Resource) -> Null
    static fn close(file: Resource) -> Null
    static fn writeText(path: String, data: String | Bytes) -> Null
    static fn append(path: String, data: String | Bytes) -> Null
    static fn list(path: String) -> List<String>
    static fn isDirectory(path: String) -> Bool
    static fn mkdir(path: String) -> Null
    static fn remove(path: String) -> Null
    static fn rename(source: String, target: String) -> Null
}
class Terminal {
    static fn open() -> Null
    static fn close() -> Null
    static fn isOpen() -> Bool
    static fn readKey(timeoutMs: Int = 0) -> Int?
}
class Sockets {
    static fn listen(host: String, port: Int) -> Resource
    static fn connect(host: String, port: Int, timeoutMs: Int = 0) -> Resource
    static fn accept(listener: Resource, timeoutMs: Int = 0) -> Resource?
    static fn receive(socket: Resource, maximum: Int, timeoutMs: Int = 0) -> Bytes?
    static fn send(socket: Resource, bytes: String | Bytes, timeoutMs: Int = 0) -> Int
    static fn close(socket: Resource) -> Null
    static fn port(socket: Resource) -> Int
}
class Http {
    static fn readRequest(socket: Resource, timeoutMs: Int = 1000) -> Map<String, Object?>?
    static fn readResponse(socket: Resource, timeoutMs: Int = 1000) -> Map<String, Object?>?
    static fn writeRequest(socket: Resource, method: String, target: String, body: String | Bytes = '', timeoutMs: Int = 1000) -> Int
    static fn writeResponse(socket: Resource, status: Int, body: String | Bytes, contentType: String = 'text/plain; charset=utf-8', timeoutMs: Int = 1000) -> Int
}
class Requests {
    static fn request(method: String, url: String, options: Map<String, Object?> = {}) -> Map<String, Object?>
    static fn get(url: String, options: Map<String, Object?> = {}) -> Map<String, Object?>
    static fn head(url: String, options: Map<String, Object?> = {}) -> Map<String, Object?>
    static fn delete(url: String, options: Map<String, Object?> = {}) -> Map<String, Object?>
    static fn post(url: String, options: Map<String, Object?> = {}) -> Map<String, Object?>
    static fn put(url: String, options: Map<String, Object?> = {}) -> Map<String, Object?>
    static fn patch(url: String, options: Map<String, Object?> = {}) -> Map<String, Object?>
    static fn json(response: Map<String, Object?>) -> Object?
}
)STUB";
}
}
