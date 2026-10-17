#include "typer.hpp"
#include <cassert>
#include <iostream>

namespace {
std::vector<std::string> errorsOf(const std::string &text) {
    std::vector<std::string> messages;
    try {
        auto program = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"typer.k3", text}));
        for (const auto &diagnostic : dk3::typeCheck(program)) if (!diagnostic.warning) messages.push_back(diagnostic.message);
    } catch (const dk3::Error &error) { messages.push_back(std::string("PARSE: ") + error.what()); }
    return messages;
}

int failures = 0;

void accepts(const std::string &text) {
    auto errors = errorsOf(text);
    if (errors.empty()) return;
    std::cerr << "unexpected error for:\n" << text << "\n  -> " << errors[0] << '\n'; failures++;
}

void rejects(const std::string &text, const std::string &fragment) {
    auto errors = errorsOf(text);
    for (const auto &message : errors) if (message.find(fragment) != std::string::npos) return;
    std::cerr << "expected error containing '" << fragment << "' for:\n" << text << "\n  -> ";
    if (errors.empty()) std::cerr << "no errors\n"; else std::cerr << errors[0] << '\n';
    failures++;
}
}

int main() {
    // Literals, inference and annotations.
    accepts("let a = 1\nlet b: Int = a + 2\nlet c: Float = a\nlet d = 1.5 * a\nlet e: Float = d\nlet f = 'x' + 1\nlet g: String = f");
    rejects("let a: Int = 'x'", "Expected Int, got String");
    rejects("let a: Int = 1.5", "Expected Int, got Float");
    rejects("let a = 1\na = 2", "Cannot reassign");
    accepts("var a = 1\na = 2\na += 3\na++");
    rejects("var a = 1\na = 'x'", "Expected Int, got String");
    rejects("let a = missing", "Undefined name 'missing'");
    rejects("let a: Strng = 'x'", "Unknown type 'Strng'");
    rejects("let a = []", "empty list");
    accepts("let a: List<Int> = []\nlet b: Map<String, Int> = {}\nlet c = [1, 2, 3]\nlet d: List<Float> = [1, 2]\nlet e = {'a': 1}");
    rejects("let a: List<Int> = ['x']", "Expected Int, got String");

    // Operators.
    accepts("let a: Float = 1 / 2\nlet b: Int = 7 // 2\nlet c: Int = 2 ** 3\nlet d: Bool = 1 < 2.5 && 'a' < 'b'\nlet e: Bool = !d\nlet f: List<Int> = [1] + [2]");
    rejects("let a = 1 + true", "cannot be applied");
    rejects("let a: Int = 1 / 2", "Expected Int, got Float");
    rejects("let a = true < false", "Cannot compare");
    rejects("if 1 { }", "Expected Bool, got Int");

    // Functions, defaults, named arguments, inference of return types.
    accepts("fn add(a: Int, b: Int = 2) -> Int { return a + b }\nlet x: Int = add(1)\nlet y: Int = add(b: 3, a: 1)\nfn twice(v: Int) { return v * 2 }\nlet z: Int = twice(4)");
    rejects("fn f(a: Int) -> Int { return a }\nf('x')", "Expected Int, got String");
    rejects("fn f(a: Int) -> Int { return a }\nf()", "Missing argument 'a'");
    rejects("fn f(a: Int) -> Int { return a }\nf(1, 2)", "Too many arguments");
    rejects("fn f(a: Int) -> Int { return a }\nf(c: 1)", "Unknown argument 'c'");
    rejects("fn f(a) -> Int { return 1 }", "requires a type annotation");
    rejects("fn f() -> Int { if true { return 1 } }", "reach its end");
    accepts("fn f(a: Int) -> Int { if a > 0 { return 1 } else { return 2 } }\nfn g() -> Int { while true { } }");
    rejects("fn f() -> Int { return 'x' }", "Expected Int, got String");
    rejects("let a = later(1)\nfn later(v: Int) { return v }", "return type");
    rejects("fn loop(v: Int) { return loop(v) }", "Recursive");
    accepts("fn loop(v: Int) -> Int { return loop(v) }\nfn log(v: Int) { System.print(v) }\nlog(1)");

    // Generics and lambdas.
    accepts("fn first<T>(items: List<T>) -> T? { return null }\nlet a: Int? = first([1, 2])\nlet b: String? = first<String>(['x'])\nfn id<T>(value: T) -> T { return value }\nlet c: Int = id(3)\nlet d: String = id('x')");
    rejects("fn first<T>(items: List<T>) -> T? { return null }\nlet a: String? = first([1])", "Expected String?, got Int?");
    rejects("fn make<T>() -> List<T> { return [] }\nlet a = make()", "Cannot infer type argument 'T'");
    accepts("let values: List<Int> = [1, 2, 3]\nlet strings: List<String> = values.map(fn(v) => 'n' + v)\nlet total: Int = values.reduce(fn(sum, v) => sum + v, 0)\nlet found: Int? = values.find(fn(v) => v > 1)\nlet kept: List<Int> = values.filter(fn(v: Int) -> Bool => v > 1)");
    rejects("let values: List<Int> = [1]\nlet a = values.map(fn(v) => v.nope)", "has no member 'nope'");
    rejects("fn apply(f: fn(Int) -> Int) -> Int { return f(1) }\napply(fn(v) => 'x')", "Expected Int, got String");
    accepts("fn apply(f: fn(Int) -> Int) -> Int { return f(1) }\nlet a: Int = apply(fn(v) => v + 1)\nlet g: fn(Int) -> Int = fn(v) => v\nlet b: Int = g(2)");
    rejects("let f = fn(v) => v", "Cannot infer the type of parameter 'v'");
    accepts("fn largest<T extends Float>(a: T, b: T) -> T { if a > b { return a }\nreturn b }\nlet x: Float = largest(1.5, 2.5)");
    rejects("fn largest<T extends Float>(a: T, b: T) -> T { return a }\nlet x = largest('a', 'b')", "does not satisfy");

    // Classes, records, traits, enums.
    accepts("class Counter { var value: Int = 0\nfn add(n: Int) -> Int { self.value += n; return self.value } }\nlet c = Counter()\nlet n: Int = c.add(2)\nc.value = 5");
    rejects("class C { let v: Int = 0 }\nlet c = C()\nc.v = 1", "immutable");
    rejects("record P { x: Int, y: Int }\nlet p = P(1, 2)\np.x = 3", "immutable");
    accepts("record P { x: Int, y: Int = x + 1 }\nlet p = P(x: 1)\nlet n: Int = p.y");
    rejects("record P { x: Int }\nlet p = P('a')", "Expected Int, got String");
    rejects("record P { x: Int }\nlet p = P(1)\nlet q = p.z", "has no member 'z'");
    accepts("class Vault { private var secret: Int = 3\nprotected var shared: Int = 1\nfn read() -> Int { return self.secret } }\nclass Child extends Vault { fn peek() -> Int { return self.shared } }\nlet n: Int = Child().read()");
    rejects("class Vault { private var secret: Int = 3 }\nlet v = Vault()\nlet n = v.secret", "private");
    accepts("class Stack<T> { var items: List<T> = []\nfn push(item: T) -> Null { self.items.append(item) }\nfn pop() -> T { return self.items.removeAt(-1) } }\nlet s = Stack<Int>()\ns.push(1)\nlet n: Int = s.pop()");
    rejects("class Stack<T> { var items: List<T> = []\nfn push(item: T) -> Null { self.items.append(item) } }\nlet s = Stack<Int>()\ns.push('x')", "Expected Int, got String");
    accepts("trait Named { fn name() -> String\nfn greeting() -> String { return 'hi ' + self.name() } }\nclass User implements Named { fn name() -> String { return 'u' } }\nlet n: Named = User()\nlet g: String = n.greeting()");
    rejects("trait Named { fn name() -> String }\nclass User implements Named { }", "does not implement Named.name");
    rejects("trait Named { fn name() -> String }\nclass User implements Named { fn name() -> Int { return 1 } }", "requires");
    accepts("class A { fn f() -> Int { return 1 } }\nclass B extends A { }\nlet a: A = B()\nlet n: Int = a.f()");
    accepts("class Pt { var x: Int\nstatic fn origin() -> Pt { return Pt(0) }\nstatic let unit: Int = 1 }\nlet p = Pt.origin()\nlet u: Int = Pt.unit");
    accepts("class V { var n: Int\nfn __add__(other: V) -> V { return V(self.n + other.n) }\nfn __lt__(other: V) -> Bool { return self.n < other.n } }\nlet a = V(1) + V(2)\nlet b: Bool = a < V(5)");
    accepts("enum Shape { Circle(Float), Square(Float), Empty }\nfn area(s: Shape) -> Float { return match s { Shape.Circle(r) => r * r, Shape.Square(a) => a * a, Shape.Empty => 0.0 } }\nlet a: Float = area(Shape.Circle(1.0))\nlet e: Shape = Shape.Empty");
    rejects("enum Shape { Circle(Float), Empty }\nfn f(s: Shape) -> Float { return match s { Shape.Circle(r) => r } }", "not exhaustive");
    accepts("enum Opt<T> { Some(T), None }\nlet a: Opt<Int> = Opt.Some(3)\nlet b: Opt<String> = Opt.None\nfn get(o: Opt<Int>) -> Int { return match o { Opt.Some(v) => v, _ => 0 } }");
    rejects("enum Shape { Circle(Float) }\nlet s = Shape.Circle('x')", "Expected Float, got String");

    // Nullability, narrowing, unions, casts.
    accepts("fn f(s: String?) -> Int { if s != null { return s.length() }\nreturn 0 }\nfn g(s: String?) -> Int { if s == null { return 0 }\nreturn s.length() }\nfn h(s: String?) -> Int { return s?.length() ?? 0 }");
    rejects("fn f(s: String?) -> Int { return s.length() }", "may be null");
    rejects("fn f(s: String?) -> Int { let n: Int = s?.length()\nreturn 1 }", "Expected Int, got Int?");
    accepts("fn f(a: String?, b: String?) -> Int { if a != null && b != null { return a.length() + b.length() }\nreturn 0 }");
    accepts("fn f(v: Int | String) -> Int { if v is Int { return v + 1 }\nreturn 0 }\nfn g(v: Int | String) -> String { if v is Int { return 'n' }\nreturn v }");
    rejects("fn f(v: Int | String) -> Int { return v + 1 }", "cannot be applied");
    accepts("fn f(o: Object?) -> Int { let n: Int = o as Int\nreturn n }\nfn g(s: String?) -> String { return s! }");
    rejects("let a: Int = 'x' as Int", "Cannot cast");
    accepts("fn f(s: String?) -> Int { var t = s\nif t != null { return t.length() }\nreturn 0 }");
    rejects("fn f(s: String?) -> Int { if s != null { s = null\nreturn s.length() }\nreturn 0 }", "Cannot reassign");
    accepts("fn f(a: Object?) -> Null { if a is String { System.print(a.length()) } }");

    // Closures and effectively-final captures.
    accepts("fn make(x: Int) -> fn(Int) -> Int { return fn(y: Int) -> Int { return x + y } }\nlet f = make(1)\nlet n: Int = f(2)");
    accepts("fn make(x: Int) { return fn(y: Int) -> Int { return x + y } }\nlet f = make(1)\nlet n: Int = f(2)");
    accepts("fn f(s: String?) -> Int { var t = s\nwhile t != null { t = null }\nreturn 0 }");
    accepts("fn f() -> Int { let base = 3\nlet add = fn(v: Int) => v + base\nreturn add(1) }");
    rejects("fn f() -> Int { var base = 3\nlet add = fn(v: Int) => v + base\nbase = 4\nreturn add(1) }", "effectively final");
    accepts("var counter = 0\nfn bump() { counter += 1 }\nbump()");

    // Imports of core modules.
    accepts("from Math import sin, PI as pi\nimport Math as M\nlet a: Float = sin(pi)\nlet b: Float = M.cos(0.0)");
    rejects("from Math import nope", "has no member");
    rejects("import Missing", "was not found");

    // Generators.
    accepts("fn count(n: Int) { for i in Collections.range(n) { yield i } }\nfor v in count(3) { let x: Int = v }");
    accepts("fn words() -> Generator<String> { yield 'a'\nyield 'b' }\nfor w in words() { let s: String = w }");
    rejects("fn words() -> Generator<String> { yield 1 }", "Expected String, got Int");
    rejects("fn words() -> Generator<String> { yield 'a'\nreturn 'b' }", "cannot return a value");
    rejects("fn bad() -> Int { yield 1 }", "must return Generator");

    // impl blocks and intersection types.
    accepts("trait Named { fn name() -> String }\nclass D { private let id: Int = 1 }\nimpl Named for D { fn name() -> String { return Text.str(self.id) } }\nlet n: Named = D()");
    rejects("trait Named { fn name() -> String }\nclass D { }\nimpl Named for D { }", "does not implement");
    accepts("trait A { fn a() -> Int }\ntrait B { fn b() -> Int }\nclass C implements A, B { fn a() -> Int { return 1 }\nfn b() -> Int { return 2 } }\nfn both(x: A & B) -> Int { return x.a() + x.b() }\nlet n: Int = both(C())\nlet m: A = both2()\nfn both2() -> A & B { return C() }");
    rejects("trait A { fn a() -> Int }\ntrait B { fn b() -> Int }\nclass C implements A { fn a() -> Int { return 1 } }\nfn both(x: A & B) -> Int { return 1 }\nboth(C())", "Expected A & B");

    // Nested core types, iterables and builtin constructors.
    accepts("let kind: Error.Kind = Error.Kind.Index\nfn f(e: Error) -> Bool { return e.kind == Error.Kind.Type }");
    accepts("fn count(n: Int) { for i in Collections.range(n) { yield i } }\nlet xs = List<Int>(count(3))\nlet ys: List<Int> = []\nys.extend(count(2))\nlet s = Set<String>(['a'])\nlet m: Map<String, Int> = Map<String, Int>()");
    accepts("fn size(v: Collections.Sized) -> Int { return v.length() }\nlet n: Int = size([1]) + size('ab')");
    accepts("class Base extends Object { }\nlet b = Base()");
    accepts("fn f<T>(v: T) -> T? { return v }\nlet a: Int? = f(1)");
    rejects("fn f<T>(v: T) -> Object { return v }", "Expected Object, got T");
    accepts("var b = b'a'\nb += b'c'");
    rejects("let xs = List<Int>('x')", "Expected Iterable<Int>, got String");

    // Core library.
    accepts("let a: Float = Math.sin(Math.PI / 2)\nlet b: Int = Math.floor(2.7)\nlet r: List<Int> = Collections.range(4)\nSystem.print(a, b, r)\nlet s: List<String> = 'a,b'.split(',')\nfor x in r { System.print(x) }\nfor c in 'ab' { let t: String = c }");
    rejects("for x in 5 { }", "is not iterable");
    rejects("let a: String = Math.sin(1.0)", "Expected String, got Float");

    if (failures) { std::cerr << failures << " typer test(s) failed\n"; return 1; }
    std::cout << "typer: PASS\n";
}
