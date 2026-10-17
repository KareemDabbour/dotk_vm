#!/bin/sh
# Static typing pipeline: --static type-checks before running and erases run-time contracts.
set -eu
bin=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

cat > "$dir/ok.k3" <<'EOF'
class Stack<T> {
    var items: List<T> = []
    fn push(item: T) -> Null { self.items.append(item) }
    fn pop() -> T { return self.items.removeAt(-1) }
    fn size() -> Int { return self.items.length() }
}
enum Shape { Circle(Float), Square(Float), Empty }
fn area(shape: Shape) -> Float {
    return match shape { Shape.Circle(r) => 3 * r * r, Shape.Square(a) => a * a, Shape.Empty => 0 }
}
fn first<T>(items: List<T>) -> T? {
    if items.length() == 0 { return null }
    return items[0]
}
fn describe(value: Int | String) -> String {
    if value is Int { return "int " + (value + 1) }
    return "string " + value
}
let stack = Stack<Int>()
stack.push(1)
stack.push(2)
let top: Int = stack.pop()
let ratio: Float = 3
let doubled: List<Float> = [1, 2].map(fn(x) => x * 2.5)
let maybe: String? = null
let name: String = maybe ?? "nobody"
let size: Int? = maybe?.length()
let words: List<String> = ["a", "b"]
let head: String = first(words)!
let thing: Object? = 42
let n: Int = thing as Int
System.print(top, stack.size(), ratio, doubled, name, size, head, n)
System.print(area(Shape.Circle(2)), area(Shape.Empty), describe(4), describe("x"))
System.print(Math.sqrt(16), 7 // 2, 2 ** 10, ratio / 2)
EOF

expected='2 1 3 [2.5, 5] nobody null a 42
12 0 int 5 string x
4 3 1024 1.5'
actual=$("$bin" --static "$dir/ok.k3")
[ "$actual" = "$expected" ] || { printf 'static run output differs:\n%s\n' "$actual" >&2; exit 1; }

"$bin" --static --check "$dir/ok.k3" | grep -q 'check: PASS' || { echo 'static --check should pass' >&2; exit 1; }

printf 'System.print("ran")\nlet a: Int = "x"\n' > "$dir/bad.k3"
set +e
output=$("$bin" --static "$dir/bad.k3" 2>"$dir/errors")
code=$?
set -e
[ "$code" -eq 65 ] || { echo "type errors must exit 65, got $code" >&2; exit 1; }
[ -z "$output" ] || { echo 'a program with type errors must not run' >&2; exit 1; }
grep -q 'Expected Int, got String' "$dir/errors" || { echo 'missing type diagnostic' >&2; cat "$dir/errors" >&2; exit 1; }

printf 'fn f(v: Object?) -> Int { return v as Int }\nSystem.print(f(1))\nSystem.print(f("x"))\n' > "$dir/cast.k3"
set +e
"$bin" --static "$dir/cast.k3" >"$dir/out" 2>"$dir/errors"
code=$?
set -e
[ "$code" -eq 65 ] && [ "$(cat "$dir/out")" = "1" ] && grep -q 'Cannot cast String to Int' "$dir/errors" || { echo 'failed cast must throw at run time' >&2; exit 1; }

printf 'fn half(v: Int) -> Float { return v / 2 }\nlet x: Float = 3\nlet y: Float = x + 1\nSystem.print(Reflection.type(x), Reflection.type(y), Reflection.type(2 * 1.5), half(3), Reflection.type(half(4)))\n' > "$dir/widen.k3"
[ "$("$bin" --static "$dir/widen.k3")" = 'Float Float Float 1.5 Float' ] || { echo 'Int values must widen to Float' >&2; exit 1; }

cat > "$dir/slots.k3" <<'EOF'
fn counter(start: Int) -> fn() -> Int {
    let base = start * 2
    return fn() => base + 1
}
fn compose(f: fn(Int) -> Int, g: fn(Int) -> Int) -> fn(Int) -> Int {
    return fn(x) => f(g(x))
}
fn adders(n: Int) -> List<fn(Int) -> Int> {
    let result: List<fn(Int) -> Int> = []
    for i in Collections.range(n) {
        result.append(fn(x) => x + i)
    }
    return result
}
fn nested(a: Int) -> Int {
    let outer = fn(b: Int) => fn(c: Int) => a + b + c
    return outer(10)(100)
}
fn evens() {
    for i in Collections.range(10) {
        if i % 2 == 1 { continue }
        if i > 6 { break }
        yield i
    }
}
fn sumGenerator() -> Int {
    var total = 0
    for v in evens() { total += v }
    return total
}
fn loops() -> String {
    var out = ""
    for c in "abc" { out = out + c }
    for k in {"x": 1, "y": 2} { out = out + k }
    for item in [1, 2, 3, 4, 5] {
        if item == 2 { continue }
        if item == 5 { break }
        out = out + item
    }
    for t in (7, 8) { out = out + t }
    return out
}
enum Op { Add(Int), Scale(Int), Reset }
fn apply(op: Op, value: Int, bias: Int) -> Int {
    let extra = bias * 10
    return match op { Op.Add(n) => value + n + extra, Op.Scale(k) => value * k + bias, Op.Reset => extra }
}
System.print(counter(5)(), compose(fn(x) => x * 2, fn(x) => x + 1)(4), adders(3)[2](10), nested(1))
System.print(sumGenerator(), loops())
System.print(apply(Op.Add(1), 2, 3), apply(Op.Scale(4), 2, 3), apply(Op.Reset, 2, 3))
EOF
expected='11 10 12 111
12 abcxy13478
33 11 30'
[ "$("$bin" --static "$dir/slots.k3")" = "$expected" ] || { echo 'slot-mode closures, loops and match differ' >&2; "$bin" --static "$dir/slots.k3" >&2; exit 1; }

printf 'enum Shape { Circle(Float), Square(Float) }\nfn scaled(shape: Shape, k: Float) -> Float {\n    let base = 2.0\n    return match shape { Shape.Circle(r) => r * k + base, Shape.Square(a) => a * k }\n}\nSystem.print(scaled(Shape.Circle(1.0), 3.0), scaled(Shape.Square(2.0), 4.0))\n' > "$dir/match.k3"
[ "$("$bin" "$dir/match.k3")" = '5 8' ] && [ "$("$bin" --static "$dir/match.k3")" = '5 8' ] || { echo 'match arms must see enclosing locals' >&2; exit 1; }

printf 'from Math import sin as s, floor\nimport Math as M\nSystem.print(s(0.0), floor(2.7), M.hypot(3, 4), Math.toInt(7.9), Collections.range(3), Math.round(-2.5))\nSystem.print(Math.sqrt(-1.0))\n' > "$dir/native.k3"
set +e
"$bin" --static "$dir/native.k3" >"$dir/out" 2>"$dir/errors"
code=$?
set -e
[ "$code" -eq 65 ] && [ "$(cat "$dir/out")" = '0 2 5 7 [0, 1, 2] -3' ] && grep -q 'Math.sqrt is undefined' "$dir/errors" || { echo 'direct native calls differ' >&2; cat "$dir/out" "$dir/errors" >&2; exit 1; }

echo 'static: PASS'
