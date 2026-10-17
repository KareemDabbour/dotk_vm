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

echo 'static: PASS'
