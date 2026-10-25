#!/bin/sh
# Native tier parity: --jit must print the same output and exit status as the interpreter.
set -eu
bin=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

same() {
    name=$1
    shift
    set +e
    "$bin" "$@" "$dir/$name.k3" > "$dir/interp.out" 2>&1
    interp=$?
    "$bin" --jit "$@" "$dir/$name.k3" > "$dir/native.out" 2>&1
    native=$?
    set -e
    if [ "$interp" -ne "$native" ] || ! cmp -s "$dir/interp.out" "$dir/native.out"; then
        echo "native parity failed for $name (exit $interp vs $native)"
        diff "$dir/interp.out" "$dir/native.out" || true
        exit 1
    fi
}

report() {
    "$bin" --jit-report "$@" > /dev/null 2> "$dir/report.out" || true
}

expect() {
    if ! grep -qF -- "$1" "$dir/report.out"; then
        echo "missing in native report: $1"
        cat "$dir/report.out"
        exit 1
    fi
}

reject() {
    if grep -qF -- "$1" "$dir/report.out"; then
        echo "unexpected in native report: $1"
        cat "$dir/report.out"
        exit 1
    fi
}

cat > "$dir/numbers.k3" <<'EOF'
fn fib(n: Int) -> Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
fn mix(a: Int, b: Float, flag: Bool) -> Float {
    var total = 0.0
    for (var i = 0; i < a; i++) {
        if flag and i % 3 == 0 { total += b * 2.0 } else { total -= 1.0 }
        if i > 100 { break }
    }
    return total / 2.0
}
fn floors(a: Int, b: Int) -> Int { return a // b * 1000 + a % b }
fn widen(a: Int) -> Float { return a / 4 }
fn logic(a: Bool, b: Bool) -> Bool { return not a or (b and a != b) }
fn smallest() -> Int { return -9223372036854775807 - 1 }
fn wraps(a: Int) -> Int { return a * 3 + 9223372036854775807 }
fn countdown(n: Int) -> Int {
    var steps = 0
    var left = n
    while true {
        if left <= 0 { break }
        left -= 2
        steps += 1
        if left == 3 { continue }
    }
    return steps
}
System.print(fib(25), mix(10, 1.5, true), mix(5, 2.0, false), floors(-7, 3), floors(7, -3))
System.print(widen(7), logic(true, false), logic(false, true), smallest() // -1, smallest() % 7, wraps(5))
System.print(countdown(11), 0.1 + 0.2, 1.0 / 3.0 != 0.3)
EOF
same numbers
report --jit "$dir/numbers.k3"
for name in fib mix floors widen logic smallest wraps countdown; do expect "native: $name"; done

cat > "$dir/errors.k3" <<'EOF'
fn divide(a: Int, b: Int) -> Int { return a // b }
fn remainder(a: Int, b: Int) -> Int { return a % b }
fn ratio(a: Float, b: Float) -> Float { return a / b }
fn outer(a: Int) -> Int { return divide(a, 0) + 1 }
fn deep(n: Int) -> Int { if n == 0 { return 0 }; return deep(n - 1) + 1 }
try { divide(1, 0) } catch error { System.print(error.kind, error.message, error.line) }
try { remainder(1, 0) } catch error { System.print(error.kind, error.message, error.line) }
try { ratio(1.0, 0.0) } catch error { System.print(error.kind, error.message, error.line) }
try { outer(3) } catch error { System.print(error.kind, error.message, error.line) }
System.print(deep(150), divide(-9223372036854775807 - 1, -1))
outer(1)
EOF
same errors

# Native frames are not listed in stack traces, so compare only the error and exit status.
cat > "$dir/overflow.k3" <<'EOF'
fn deep(n: Int) -> Int { if n == 0 { return 0 }; return deep(n - 1) + 1 }
System.print(deep(100000))
EOF
set +e
"$bin" "$dir/overflow.k3" > "$dir/interp.out" 2>&1
interp=$?
"$bin" --jit "$dir/overflow.k3" > "$dir/native.out" 2>&1
native=$?
set -e
if [ "$interp" -ne "$native" ] || [ "$(head -n 1 "$dir/interp.out")" != "$(head -n 1 "$dir/native.out")" ]; then
    echo "native overflow differs (exit $interp vs $native)"
    head -n 3 "$dir/interp.out" "$dir/native.out"
    exit 1
fi

cat > "$dir/order.k3" <<'EOF'
fn early(n: Int) -> Int { return later(n) + 1 }
System.print("before")
try { System.print(early(1)) } catch error { System.print(error.message) }
fn later(n: Int) -> Int { return n * 2 }
System.print(early(2))
EOF
same order
report --jit "$dir/order.k3"
expect "native: early"
expect "native: later"

cat > "$dir/limits.k3" <<'EOF'
fn spin(n: Int) -> Int { var t = 0; while t < n { t += 1 }; return t }
System.print(spin(100000))
EOF
same limits --steps 2000

cat > "$dir/opt_in.k3" <<'EOF'
fn keep(f: fn(Int) -> Int) -> fn(Int) -> Int { return f }
@keep fn wrapped(n: Int) -> Int { return n + 1 }
fn viaWrapped(n: Int) -> Int { return wrapped(n) }
@jit fn hot(n: Int) -> Int { var t = 0; for (var i = 0; i < n; i++) { t += square(i) % 7 }; return t }
fn square(n: Int) -> Int { return n * n }
fn idle(n: Int) -> Int { return n }
@nojit fn cold(n: Int) -> Int { return n }
fn text(n: Int) -> String { return "n" + n }
class Box { @jit fn size() -> Int { return 1 } }
@jit fn guarded(n: Int) -> Int { try { return n // 0 } catch error { return -1 } }
System.print(hot(10), viaWrapped(1), idle(2), cold(3), text(4), Box().size(), guarded(5))
EOF
same opt_in
report "$dir/opt_in.k3"
expect "native: hot"
expect "native: square"
expect "native: Box.size"
expect "keeps 'guarded' interpreted: it uses 'try' statements"
reject "native: idle"
reject "native: wrapped"
report --jit "$dir/opt_in.k3"
expect "native: idle"
expect "native: viaWrapped"
expect "native: text"
reject "native: cold"

# Objects: methods, fields, collections, strings, globals and runtime calls through the helper bridge.
cat > "$dir/objects.k3" <<'EOF'
class Vec {
    var x: Float
    var y: Float
    fn add(other: Vec) -> Vec { return Vec(self.x + other.x, self.y + other.y) }
    fn scale(k: Float) -> Null { self.x *= k; self.y *= k }
    fn length2() -> Float { return self.x * self.x + self.y * self.y }
    fn describe() -> String { return f"Vec({self.x}, {self.y})" }
    static fn origin() -> Vec { return Vec(0.0, 0.0) }
}
class Counter {
    private var count: Int = 0
    fn bump(by: Int) -> Int { self.count += by; return self.count }
    fn twice() -> Int { self.bump(1); return self.bump(1) }
}
class Square extends Vec {
    fn length2() -> Float { return self.x * 10.0 }
}
record Money {
    cents: Int
    fn __add__(other: Money) -> Money { return Money(self.cents + other.cents) }
    fn dollars() -> Float { return Math.toFloat(self.cents) / 100.0 }
}
var calls = 0
let base = 10
fn sumList(items: List<Int>) -> Int {
    var total = 0
    for item in items { total += item }
    calls += 1
    return total + base
}
fn squares(n: Int) -> List<Int> {
    var result: List<Int> = []
    for (var i = 0; i < n; i++) { result.append(i * i) }
    result[0] = 100
    result[1] += 5
    return result
}
fn tally(words: List<String>) -> Map<String, Int> {
    var counts: Map<String, Int> = {}
    for word in words {
        if counts.contains(word) { counts[word] += 1 } else { counts[word] = 1 }
    }
    return counts
}
fn keysOf(counts: Map<String, Int>) -> String {
    var text = ""
    for key in counts { text += key + "=" + Text.str(counts[key]) + ";" }
    return text
}
fn walk(steps: Int) -> Float {
    var position = Vec.origin()
    let step = Vec(1.5, -0.5)
    for (var i = 0; i < steps; i++) { position = position.add(step) }
    position.scale(2.0)
    return position.length2()
}
fn churn(n: Int) -> Int {
    var kept = 0
    for (var i = 0; i < n; i++) {
        let item = [i, Text.str(i), (i, i)]
        if i % 1000 == 0 { GC.collect() }
        kept += item.length()
    }
    return kept
}
fn lengths(shapes: List<Vec>) -> Float {
    var total = 0.0
    for shape in shapes { total += shape.length2() }
    return total
}
fn pick(items: List<String>, index: Int) -> String { return items[index] }
fn raise(code: Int) -> Int {
    if code > 0 { throw Error(Error.Kind.Application, f"code {code}") }
    return code
}
fn nullable(flag: Bool, name: String?, values: List<Int?>) -> String {
    var sum = 0
    for value in values { sum += value ?? 100 }
    let missing: List<String>? = null
    if flag and name != null { return name!! + Text.str(sum + (missing?.length() ?? -1)) }
    return "none"
}
fn kinds(values: List<Object>) -> String {
    var text = ""
    for value in values {
        if value is Int { text += "i" + Text.str((value as Int) + 1) } else { text += "o" }
    }
    return text
}
fn total(amounts: List<Money>) -> Money {
    var sum = Money(0)
    for amount in amounts { sum = sum + amount }
    return sum
}
fn counting(counter: Counter) -> Int { return counter.twice() + counter.bump(10) }
let v = Vec(3.0, 4.0)
System.print(v.length2(), v.add(Vec(1.0, 1.0)).describe(), Vec.origin().describe())
System.print(sumList([1, 2, 3]), sumList(squares(5)), calls, squares(4), tally(["a", "b", "a"]), keysOf(tally(["x", "y", "x"])))
System.print(walk(1000), churn(5000), lengths([Vec(1.0, 2.0), Square(3.0, 4.0)]))
System.print(nullable(true, "ada", [1, null, 3]), nullable(false, null, []), kinds([1, "a", 2.5, 4]))
System.print(total([Money(150), Money(275)]).dollars(), counting(Counter()))
try { raise(7) } catch error { System.print(error.kind, error.message, error.line) }
try { pick(["a"], 3) } catch error { System.print(error.kind, error.message, error.line) }
pick(["only"], 5)
EOF
same objects
report --jit "$dir/objects.k3"
for name in Vec.add Vec.scale Vec.describe Vec.origin Counter.bump Counter.twice Square.length2 Money.__add__ sumList squares tally keysOf walk churn lengths pick raise nullable kinds total counting; do
    expect "native: $name"
done
reject "interpreted"

# Lambdas: captures from native and interpreted creators, nesting, self, inferred types, builtin callbacks.
cat > "$dir/enums.k3" <<'EOF'
enum Code { Ok = 200, Gone = -410 }
enum Ratio: Float { Half = 0.5, Third = 0.25 }
enum Marker: Bytes { Start = b"\xFF\xD8", End = b"\xFF\xD9" }
enum Word { Hi = "hi", Bye = "bye" }
fn total(n: Int) -> Int { var t = 0; for (var i = 0; i < n; i++) { t += Code.Ok.value + Code.Gone.value }; return t }
fn scaled(x: Float) -> Float { return x * Ratio.Half.value + Ratio.Third.value }
fn marker(data: Bytes) -> Bool { return Marker.fromValue(data) == Marker.End }
fn greet(w: Word) -> String { return w.value + "!" }
fn find(code: Int) -> Code? { return Code.fromValue(code) }
System.print(total(3), scaled(2.0), marker(b"\xFF\xD9"), marker(b"\x00\x00"), greet(Word.Bye), find(200) == Code.Ok, find(1))
EOF
same enums
report --jit "$dir/enums.k3"
for name in total scaled marker greet find; do expect "native: $name"; done
reject "interpreted"

# Bitwise operators, shifts past 64 bits, compound forms and hex/binary literals.
cat > "$dir/bits.k3" <<'EOF'
fn crc(data: Bytes) -> Int {
    var c = 0xFFFF_FFFF
    for (var i = 0; i < data.length(); i++) {
        c ^= data[i]
        for (var k = 0; k < 8; k++) {
            if c & 1 == 1 { c = (c >> 1) ^ 0xEDB88320 } else { c >>= 1 }
        }
    }
    return c ^ 0xFFFFFFFF
}
fn mix(a: Int, b: Int) -> Int { return (a << 3 | b >> 2) & ~0b11 ^ -a }
fn shifts(n: Int) -> Int { var t = 0; for (var s = 0; s < 70; s += 7) { t += (n << s) + (-n >> s) }; return t }
fn bad(n: Int) -> Int { return 1 << n }
System.print(crc(b"123456789"), mix(0x1234, 0b1011), shifts(5), bad(3))
System.print(bad(-1))
EOF
same bits
report --jit "$dir/bits.k3"
for name in crc mix shifts bad; do expect "native: $name"; done
reject "interpreted"

# Byte parsing: slices (receiver checked before bounds), integer reads and search from native code.
cat > "$dir/parse.k3" <<'EOF'
fn markers(data: Bytes) -> List<String> {
    var found: List<String> = []
    var i = 0
    while i + 4 <= data.length() {
        if data[i] == 0xFF and data[i + 1] != 0 {
            let size = data[i + 2:i + 4].toInt()
            found.append(f"{data[i:i + 2]}@{i}:{size}:{data.readInt(i, 2, order: "little")}")
            i += 2 + size
        } else {
            i += 1
        }
    }
    return found
}
fn pieces(xs: List<Int>, s: String) -> String { return f"{xs[1:]} {xs[::-1]} {xs[:-1:2]} {s[1:3]} {xs[5:1:-1]}" }
fn seek(data: Bytes, tag: Bytes) -> Int { return data.find(tag, 1) }
fn mark(log: List<String>, n: Int) -> Int { log.append(f"{n}"); return n }
fn sliced(xs: List<Int>, log: List<String>, step: Int) -> List<Int> { return xs[mark(log, 1):mark(log, 3):step] }
fn flatten(parts: List<Bytes>) -> List<Int> { return Collections.stream(parts).flatMap(fn(b: Bytes) -> Bytes => b).filter(fn(x: Int) -> Bool => x > 1).toList() }
let data = b"\xFF\xD8\x00\x02\xFF\xE0\x00\x04JF\xFF\xD9\x00\x02"
System.print(markers(data), pieces([1, 2, 3, 4], "hello"), seek(data, b"\xFF"), Bytes.of([0x4A, 0x46]) == data[8:10])
let log: List<String> = []
System.print(sliced([5, 6, 7, 8], log, 1), log)
try { sliced([1], log, 0) } catch error { System.print(error.message, log) }
System.print(flatten([b"\x01\x02", b"\x03"]))
EOF
same parse
report --jit "$dir/parse.k3"
for name in markers pieces seek mark sliced flatten; do expect "native: $name"; done
reject "interpreted"

# Image decoding and resizing natives called from compiled code.
cat > "$dir/pixels.k3" <<'EOF'
fn brightness(data: Bytes) -> Int {
    let info = ImageCodec.info(data)
    let rgba = ImageCodec.decode(data)
    var total = 0
    for (var i = 0; i < rgba.length(); i += 4) { total += rgba[i] + rgba[i + 1] + rgba[i + 2] }
    return total * info[2]
}
fn shrink(data: Bytes) -> Bytes { return ImageCodec.resize(ImageCodec.decode(data), 2, 2, 1, 1) }
fn broken(data: Bytes) -> Int { return ImageCodec.info(data)[0] }
let png = Encoding.fromHex("89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001349444154789c63f8cfc0f01f0c8134083400004949097828a0db770000000049454e44ae426082")
System.print(brightness(png), shrink(png))
try { broken(b"nope") } catch error { System.print(error.kind, error.message, error.line) }
EOF
same pixels
report --jit "$dir/pixels.k3"
for name in brightness shrink broken; do expect "native: $name"; done
reject "interpreted"

# Multi-subscripts, slice hooks, reflected and mask-returning operators, and std.ndarray from compiled code.
cat > "$dir/arrays.k3" <<'EOF'
import std.ndarray as np
class Grid {
    let width: Int
    let cells: List<Float>
    fn __getitem__(key: Int | Tuple<Int>) -> Float {
        if key is Int { return self.cells[key] }
        let pair = key as Tuple<Int>
        return self.cells[pair[0] * self.width + pair[1]]
    }
    fn __setitem__(key: Int | Tuple<Int>, value: Float) -> Null {
        let pair = key as Tuple<Int>
        self.cells[pair[0] * self.width + pair[1]] = value
    }
    fn __getslice__(key: Tuple<Int | Collections.Slice>) -> String { return Text.str(key) }
    fn __rmul__(other: Float) -> Float { return other * self.cells.length() }
    fn __gt__(other: Float) -> List<Bool> { return self.cells.map(fn(x: Float) -> Bool => x > other) }
}
fn fill(grid: Grid) -> Float {
    var total = 0.0
    for (var i = 0; i < 2; i++) {
        for (var j = 0; j < 3; j++) { grid[i, j] = i * 10 + j; total += grid[i, j] }
    }
    grid[1, 1] += 0.5
    return total
}
fn views(grid: Grid) -> String { return f"{grid[1:2]} {grid[::-1, 0]} {2 * grid} {grid > 10.5} {0.5 < grid}" }
fn stats(n: Int) -> String {
    let a = np.arange(Math.toFloat(n)).reshape(2, -1)
    a[:, 0] = -1
    let b = 2 * a - 1
    return f"{b.sumAxis(1)} {(a > 1).sum()} {a.dot(a.transpose())} {a[1, -1]}"
}
let grid = Grid(3, [0.0, 0.0, 0.0, 0.0, 0.0, 0.0])
System.print(fill(grid), grid.cells, views(grid), stats(6))
try { stats(5) } catch error { System.print(error.kind, error.message) }
EOF
same arrays
report --jit "$dir/arrays.k3"
for name in fill views stats Grid.__getitem__ Grid.__setitem__; do expect "native: $name"; done

cat > "$dir/lambdas.k3" <<'EOF'
class Scaler {
    let factor: Int
    fn apply(items: List<Int>) -> List<Int> { return items.map(fn(x: Int) -> Int => x * self.factor) }
    fn adder() -> fn(Int) -> Int {
        let offset = self.factor + 1
        return fn(x: Int) -> Int => x + offset
    }
}
let limit = 3
fn makeCounter(start: Int) -> fn() -> Int {
    let base = start * 2
    return fn() -> Int => base + limit
}
fn compose(f: fn(Int) -> Int, g: fn(Int) -> Int) -> fn(Int) -> Int { return fn(x: Int) -> Int => g(f(x)) }
fn pipeline(words: List<String>) -> String {
    let prefix = "<"
    let wrapped = words.map(fn(word: String) -> String => prefix + word + ">")
    return ",".join(wrapped.filter(fn(word: String) -> Bool => word.length() > 4))
}
fn totals(values: List<Int>) -> Int {
    let bonus = 10
    return values.reduce(fn(acc: Int, item: Int) -> Int => acc + item + bonus, 0)
}
fn nested(n: Int) -> Int {
    let outer = fn(a: Int) -> fn(Int) -> Int {
        let doubled = a * 2
        return fn(b: Int) -> Int => doubled + b + n
    }
    return outer(5)(7)
}
fn loopClosures(n: Int) -> Int {
    var fns: List<fn() -> Int> = []
    for (var i = 0; i < n; i++) {
        let captured = i * i
        fns.append(fn() -> Int => captured)
    }
    var total = 0
    for f in fns { total += f() }
    return total
}
var made: List<fn() -> Int> = []
for (var i = 0; i < 3; i++) {
    let k = i * 10
    made.append(fn() -> Int => k + limit)
}
System.print(Scaler(3).apply([1, 2, 3]), Scaler(4).adder()(10), makeCounter(5)(), compose(fn(x: Int) -> Int => x + 1, fn(x: Int) -> Int => x * 10)(4))
System.print(pipeline(["a", "bb", "ccc"]), totals([1, 2, 3]), Collections.sorted([3, 1, 2], fn(a: Int, b: Int) -> Int => b - a), nested(1), loopClosures(5))
System.print([1, 2, 3].map(fn(x) => x * 3), made[2](), made.map(fn(f: fn() -> Int) -> Int => f()))
EOF
same lambdas
report --jit "$dir/lambdas.k3"
for name in lambda@3:64 lambda@6:16 lambda@12:12 lambda@25:17 lambda@27:16 lambda@44:17 loop@42:1 Scaler.apply makeCounter nested loopClosures; do expect "native: $name"; done
reject "interpreted"

# Top-level loops run natively in place of their bytecode; globals use inline caches.
cat > "$dir/script.k3" <<'EOF'
var total = 0
var x = 0.0
let names = ["a", "b", "c"]
let table: Map<String, Int> = {"one": 1, "two": 2}
fn bump(n: Int) -> Int { return n + 1 }
for (var i = 0; i < 10000; i++) {
    if i % 7 == 0 { continue }
    total += bump(i) % 13
    x = x + 0.5
    if total > 1000000 { break }
}
var count = 0
while count < 5 { count += 1 }
for key in table { total += table[key] }
for name in names {
    let upper = name + name
    total += upper.length()
}
System.print(total, x, count)
EOF
same script
same script --steps 500
report --jit "$dir/script.k3"
for name in loop@6:1 loop@13:1 loop@14:1 loop@15:1; do expect "native: $name"; done

# Inline collection paths: index/store/length/append/concat/for-in, their errors, and impl-added hooks.
cat > "$dir/collections.k3" <<'EOF'
fn at(v: List<Int>, i: Int) -> Int { return v[i] }
fn put(v: List<Int>, i: Int, x: Int) -> Null { v[i] = x }
fn sizes(a: List<Int>, t: Tuple<Int>, s: Set<Int>, m: Map<String, Int>) -> Int { return a.length() * 1000 + t.length() * 100 + s.length() * 10 + m.length() }
fn grow(n: Int) -> List<Int> { var v: List<Int> = []; for (var i = 0; i < n; i++) { v.append(i * i) }; return v }
fn glue(parts: List<String>) -> String { var s = ""; for p in parts { s += p; s = s + "." }; return s }
fn sumRange(n: Int) -> Int { var t = 0; for i in Collections.range(n) { t += i }; return t }
fn sumAll(t: Tuple<Int>, s: Set<Int>, m: Map<String, Int>) -> Int { var r = 0; for x in t { r += x }; for x in s { r += x * 10 }; for k in m { r += m[k] * 100 }; return r }
fn mutate(v: List<Int>) -> Int { var seen = 0; for x in v { seen += 1; if v.length() < 6 { v.append(x) } }; return seen }
fn swap(v: List<Int>) -> List<Int> { let first = v[0]; v[0] = v[-1]; v[-1] = first; return v }
let xs = [10, 20, 30]
System.print(at(xs, 0), at(xs, -1), sizes(xs, (1, 2), Set([1, 1, 2]), {"a": 1}))
put(xs, 1, 99)
System.print(xs, grow(5), glue(["a", "b"]), sumRange(10), sumAll((1, 2), Set([3]), {"k": 4}))
System.print(mutate([1, 2, 3]), swap([1, 2, 3]))
try { at(xs, 3) } catch error { System.print(error.kind, error.message, error.line) }
try { at(xs, -4) } catch error { System.print(error.kind, error.message, error.line) }
try { put(xs, 7, 1) } catch error { System.print(error.kind, error.message, error.line) }
trait Shifted { fn __getitem__(i: Int) -> Int }
impl Shifted for List { fn __getitem__(i: Int) -> Int { return 100 + i } }
System.print(at(xs, 0), xs[2])
EOF
same collections
same collections --steps 5000
report --jit "$dir/collections.k3"
for name in at put sizes grow glue sumRange sumAll mutate swap; do expect "native: $name"; done
reject "interpreted"

# --build: standalone executables must behave like the interpreter, with options baked in.
built() {
    name=$1
    shift
    if ! "$bin" "$@" --build "$dir/$name.k3" -o "$dir/$name.bin" > "$dir/build.out" 2>&1; then
        echo "build failed for $name"
        cat "$dir/build.out"
        exit 1
    fi
    set +e
    "$bin" "$@" "$dir/$name.k3" 2>&1 | grep -v '^Warning: native tier' > "$dir/interp.out"
    "$bin" "$@" "$dir/$name.k3" > /dev/null 2>&1
    interp=$?
    "$dir/$name.bin" > "$dir/native.out" 2>&1
    native=$?
    set -e
    if [ "$interp" -ne "$native" ] || ! cmp -s "$dir/interp.out" "$dir/native.out"; then
        echo "built executable differs for $name (exit $interp vs $native)"
        diff "$dir/interp.out" "$dir/native.out" || true
        exit 1
    fi
}
built numbers
built errors
built order
built limits --steps 2000
built opt_in
if ! grep -qF "keeps 'guarded' interpreted" "$dir/build.out"; then
    echo "--build must report @jit functions it cannot compile"
    cat "$dir/build.out"
    exit 1
fi
built objects
built enums
built bits
built parse
built pixels
built arrays
built lambdas
built script
built collections

mkdir "$dir/shapes"
cat > "$dir/shapes/util.k3" <<'EOF'
export fn square(n: Int) -> Int { return n * n }
EOF
cat > "$dir/modules.k3" <<'EOF'
from shapes.util import square
fn total(n: Int) -> Int { var t = 0; for (var i = 0; i < n; i++) { t += i * i }; return t }
System.print(square(12), total(100), System.args)
EOF
"$bin" "$dir/modules.k3" one --two > "$dir/interp.out" 2>&1
"$bin" --build "$dir/modules.k3" -o "$dir/modules.bin" > /dev/null
mv "$dir/shapes" "$dir/shapes.moved"
mv "$dir/modules.k3" "$dir/modules.moved"
"$dir/modules.bin" one --two > "$dir/native.out" 2>&1
if ! cmp -s "$dir/interp.out" "$dir/native.out"; then
    echo "built executable needs its sources or drops arguments"
    diff "$dir/interp.out" "$dir/native.out" || true
    exit 1
fi
if command -v otool > /dev/null 2>&1; then deps=$(otool -L "$dir/modules.bin"); else deps=$(ldd "$dir/modules.bin" 2>&1 || true); fi
case $deps in *LLVM* | *llvm*)
    echo "built executable links LLVM"
    exit 1
    ;;
esac

printf 'fn bad() -> Int { return "x" }\n' > "$dir/bad.k3"
set +e
"$bin" --build "$dir/bad.k3" -o "$dir/bad.bin" > /dev/null 2>&1
code=$?
set -e
if [ "$code" -ne 65 ] || [ -e "$dir/bad.bin" ]; then
    echo "--build must reject programs with type errors (exit $code)"
    exit 1
fi

echo "native tier tests passed"
