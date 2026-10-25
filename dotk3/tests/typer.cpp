#include "typer.hpp"
#include <cassert>
#include <iostream>
#include <map>

namespace {
std::map<std::string, std::string> modules;

std::vector<std::string> errorsOf(const std::string &text) {
    std::vector<std::string> messages;
    try {
        auto program = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"typer.k3", text}));
        auto loader = [](const std::string &name) -> std::shared_ptr<const dk3::Source> {
            auto found = modules.find(name);
            return found == modules.end() ? nullptr : std::make_shared<dk3::Source>(dk3::Source{name + ".k3", found->second});
        };
        for (const auto &diagnostic : dk3::typeCheck(program, loader)) if (!diagnostic.warning) messages.push_back(diagnostic.message);
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
    accepts("fn grid(w: Int, h: Int) -> List<List<String?>> { return Collections.range(h).map(fn(y) => Collections.range(w).map(fn(x) => null)) }");
    accepts("fn make<T>(w: Int) -> List<T?> { return Collections.range(w).map(fn(i: Int) -> T? => null) }\nfn fill<T>(b: List<T?>, v: T) -> List<T?> { return b.map(fn(x: T?) => v) }");
    accepts("fn reset(b: List<List<String?>>) -> List<List<String?>> { return b.map(fn(row: List<String?>) => row.map(fn(x: String?) => 'a')) }");
    accepts("class P { let a: Int; var b: String; constructor(x: Int) -> Null { self.a = x; if x > 0 { self.b = 'p' } else { self.b = 'n' } } }");
    accepts("class P { let a: Int; constructor(f: Bool) -> Null { if f { throw Error(Error.Kind.Value, 'x') }\nself.a = 1 } }");
    rejects("class P { let a: Int; constructor(f: Bool) -> Null { if f { self.a = 1 } } }", "may not initialize field 'a'");
    rejects("class P { let a: Int; constructor(f: Bool) -> Null { if f { return }\nself.a = 1 } }", "may return before initializing field 'a'");
    rejects("class A { fn read() -> Int { return 1 } }\nclass B extends A { fn read() -> String { return 'x' } }", "overrides A.read");
    accepts("class A { fn read() -> Int? { return 1 } }\nclass B extends A { fn read() -> Int { return 1 } }");
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
    accepts("enum Marker: Bytes { Start = b'\\xFF\\xD8', End = b'\\xFF\\xD9' }\nenum Code { Ok = 200, Gone = -410 }\nlet raw: Bytes = Marker.Start.value\nlet found: Marker? = Marker.fromValue(raw)\nlet code: Int = Code.Gone.value\nfn name(c: Code) -> String { return match c { Code.Ok => 'ok', Code.Gone => 'gone' } }");
    accepts("let base = 2\nenum Flag: Int { A = base, B = Flag.A.value * 2 }\nlet b: Int = Flag.B.value");
    rejects("enum Code { Ok = 200 }\nlet c: String = Code.Ok.value", "Expected String, got Int");
    rejects("enum Code { Ok = 200 }\nlet c: Code = Code.fromValue(200)", "may be null");
    rejects("enum Code: Int { Ok = 'x' }", "Expected Int, got String");
    rejects("enum Code { Ok = 1, Bad = 'x' }", "Expected Int, got String");
    rejects("enum Flag: Bool { On = true }", "Enum values must be Int, Float, String or Bytes");
    rejects("let base = 1\nenum Flag { A = base }", "Cannot infer the value type of enum Flag");
    rejects("enum Code { Ok = 1 }\nCode.Ok.value = 2", "Cannot assign");
    rejects("enum Code { value = 1 }", "Duplicate member 'value'");
    accepts("let a: Int = 0xF0 & 0x3C | 1 ^ 2\nlet b: Int = ~a << 2 >> 1\nvar c = 1\nc |= 4\nc <<= 1\nc >>= 1\nlet d: Bool = c & 1 == 1");
    accepts("class B { let v: Int\nfn __and__(o: B) -> B { return B(self.v & o.v) }\nfn __invert__() -> B { return B(~self.v) } }\nlet x: B = ~(B(1) & B(3))");
    rejects("let a = 1.5 & 1", "Operator '&' requires Int operands, got Float and Int");
    rejects("let a = true | false", "requires Int operands");
    rejects("let a = ~2.0", "Operator '~' requires an Int");
    // Subscripts, slice hooks, reflected and non-Bool comparison hooks.
    const std::string grid = "class G { fn __getitem__(key: Int | Tuple<Int>) -> Float { return 1.0 }\nfn __setitem__(key: Int | Tuple<Int>, value: Float) -> Null {}\n"
                             "fn __getslice__(key: Tuple<Int | Collections.Slice>) -> G { return self }\nfn __setslice__(key: Tuple<Int | Collections.Slice>, value: G | Float) -> Null {}\n"
                             "fn __rmul__(other: Float) -> G { return self }\nfn __gt__(other: Float) -> G { return self }\nfn __lt__(other: Float) -> G { return self } }\nlet g = G()\n";
    accepts(grid + "let a: Float = g[1]\nlet b: Float = g[1, -2]\ng[0, 0] = 2\ng[0, 0] += 1.5\nlet c: G = g[1:3]\nlet d: G = g[::2, 0]\nlet e: G = g[:, 1:]\ng[1:] = 0.5\ng[0, :] = g\n"
            "let f: G = 2 * g\nlet h: G = g > 0.5\nlet i: G = 0.5 < g\nlet s: Collections.Slice = Collections.Slice(1, null, -1)\nlet n: Int? = s.step");
    rejects(grid + "let a: G = g[1, 2]", "Expected G, got Float");
    rejects(grid + "g[0, 'x']", "Expected Int | Tuple<Int>");
    rejects(grid + "g['a':]", "Expected Int, got String");
    rejects(grid + "g[1:, 0] = 'x'", "Expected Float | G, got String");
    rejects(grid + "let x = 'a' * g", "Expected Float, got String");
    rejects(grid + "let x = g - 1", "cannot be applied");
    rejects(grid + "if g > 1.0 { }", "Expected Bool, got G");
    rejects("let xs = [1, 2]\nlet a = xs[0:1, 0]", "List<Int> cannot be sliced with several subscripts");
    rejects("let xs = [1, 2]\nxs[0:1] = [3]", "List<Int> does not support slice assignment");
    rejects("class N {}\nlet a = N()[1:]", "N cannot be sliced");
    rejects("let xs = [1, 2]\nxs[0:1] += [3]", "PARSE: Compound assignment to a slice is not supported");
    rejects("var a = 1\na ^= 'x'", "requires Int operands");
    accepts("let data = b'\\x00\\x10'\nlet n: Int = data.toInt() + data.readInt(0, 2, order: 'little') + data.readSignedInt(0, 1)\nlet b: Bytes = Bytes.of([1, 2]) + n.toBytes(4) + b''.join([data])\nlet at: Int = data.find(b'\\x10', 1)");
    rejects("let n: Bytes = b'\\x01'.toInt()", "Expected Bytes, got Int");
    rejects("let n = b'\\x01'.readInt('0', 1)", "Expected Int, got String");
    rejects("let b = Bytes.of(['x'])", "Expected");
    accepts("let png = b'x'\nlet info: Tuple<Int> = ImageCodec.info(png)\nlet w: Int = info[0]\nlet rgba: Bytes = ImageCodec.decode(png)\nlet small: Bytes = ImageCodec.resize(rgba, w, info[1], 4, 4)");
    rejects("let rgba = ImageCodec.decode('text')", "Expected Bytes");
    rejects("let small = ImageCodec.resize(b'x', 1, 1, 1)", "argument");
    rejects("let n: Bytes = ImageCodec.info(b'x')", "Expected Bytes");

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

    // User modules.
    modules["shapes"] = "export record Point { x: Int, y: Int }\nexport enum Kind { Round, Flat }\nexport type Pair = Tuple<Int>\nexport fn origin() -> Point { return Point(0, 0) }\nexport let unit: Int = 1\nfn hidden() -> Int { return 1 }";
    modules["loop_a"] = "import loop_b\nexport let a: Int = 1";
    modules["loop_b"] = "import loop_a\nexport let b: Int = 2";
    modules["broken"] = "export let n: Int = 'x'";
    accepts("import shapes\nlet p: shapes.Point = shapes.origin()\nlet q = shapes.Point(1, 2)\nlet n: Int = q.x + shapes.unit\nlet k: shapes.Kind = shapes.Kind.Round");
    accepts("from shapes import Point, origin as make, Kind\nlet p: Point = make()\nlet k: Kind = Kind.Flat\nfn f(k: Kind) -> Int { return match k { Kind.Round => 1, Kind.Flat => 2 } }");
    rejects("from shapes import hidden", "does not export 'hidden'");
    rejects("import shapes\nshapes.hidden()", "does not export 'hidden'");
    rejects("import shapes\nlet p: shapes.Point = shapes.Point('a', 2)", "Expected Int, got String");
    rejects("import shapes\nlet n: String = shapes.unit", "Expected String, got Int");
    rejects("import nowhere", "was not found");
    rejects("import loop_a", "Import cycle");
    rejects("import broken", "Expected Int, got String");

    // IO and network modules.
    accepts("IO.write('hi')\nlet line: String? = IO.readLine()\nlet t: String = Files.readText('a')\nlet s: Resource = IO.Sockets.listen('127.0.0.1', 0)\nlet c: Resource? = Sockets.accept(s, 10)\nlet r: Map<String, Object?> = Requests.get('http://x')\nTerminal.open()");
    accepts("from Sockets import receive as read, send\nfn f(s: Resource) -> Int { let b: Bytes? = read(s, 8)\nreturn send(s, 'x') }");
    accepts("fn f(o: Object, e: Error) -> Null { }");

    // Core library.
    accepts("let a: Float = Math.sin(Math.PI / 2)\nlet b: Int = Math.floor(2.7)\nlet r: Range = Collections.range(4)\nSystem.print(a, b, r)\nlet s: List<String> = 'a,b'.split(',')\nfor x in r { System.print(x) }\nfor c in 'ab' { let t: String = c }");
    rejects("for x in 5 { }", "is not iterable");
    rejects("let a: String = Math.sin(1.0)", "Expected String, got Float");

    // Annotations, directives and decorators.
    const std::string decorators = "fn twice(f: fn(Int) -> Int) -> fn(Int) -> Int { return fn(x: Int) -> Int { return f(f(x)) } }\nfn tag(label: String) -> fn(Type) -> Type { return fn(t: Type) -> Type { return t } }\nfn meta(n: Int) -> Int { return n }\n";
    accepts(decorators + "@twice fn inc(x: Int) -> Int { return x + 1 }\nlet y: Int = inc(1)\n@tag('m') class A { fn name() -> String { return 'a' } }\nclass B extends A { @override fn name() -> String { return 'b' }; @meta(-3) var n: Int = 0 }\n@test @pure @inline @jit fn t() -> Null {}");
    accepts("trait R { fn read() -> Int }\nclass C implements R { @override fn read() -> Int { return 1 } }\nclass D {}\nimpl R for D { @override fn read() -> Int { return 2 } }");
    const std::string memo = "fn memo<T, U>(f: fn(T) -> U) -> fn(T) -> U { return f }\n";
    accepts(memo + "@memo fn a(n: Int) -> Int { return n }\n@memo<Int, Int> fn b(n: Int) -> Int { return n }\nlet x: Int = a(1) + b(2)");
    rejects(memo + "@memo<Int, String> fn c(n: Int) -> Int { return n }", "Expected");
    rejects(decorators + "@twice fn bad(x: Int) -> String { return 'x' }", "Expected");
    rejects(decorators + "@twice fn inc(x: Int) -> Int { return x }\nlet s: String = inc(1)", "Expected String, got Int");
    rejects("class A { @override fn nothing() -> Int { return 1 } }", "does not override");
    rejects("class A { fn m() -> Null {} }\nclass B extends A { @override static fn m() -> Null {} }", "@override requires an instance method");
    rejects("@test fn needs(x: Int) -> Null {}", "cannot have required parameters");
    rejects("@jit @nojit fn both() -> Null {}", "cannot be combined");
    rejects("@deprecated(3) fn bad() -> Null {}", "optional String message");
    rejects("@pure(1) fn bad() -> Null {}", "@pure takes no arguments");
    rejects("@override fn top() -> Null {}", "@override cannot be applied to a function");
    rejects("@test class C {}", "@test cannot be applied to a type");
    rejects("fn f(@pure x: Int) -> Null {}", "@pure cannot be applied to a parameter");
    rejects("@missing fn m() -> Null {}", "Undefined name 'missing'");
    rejects(decorators + "@twice fn noType(x: Int) { return x }", "explicit return type");
    rejects("fn meta(n: Int) -> Int { return n }\nlet x = 1\nclass B { @meta(x) var y: Int = 0 }", "literal constants");
    rejects(decorators + "fn outer() -> Null { @twice fn inner(x: Int) -> Int { return x } }", "only to top-level functions and types");
    rejects("fn meta(n: Int) -> Int { return n }\nclass B { @meta('x') var y: Int = 0 }", "Expected Int, got String");
    {
        auto program = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"deprecated.k3", "@deprecated('use b') fn a() -> Int { return 1 }\nclass K { @deprecated fn old() -> Null {}; @deprecated var v: Int = 0 }\n@deprecated class Gone {}\nlet x = a()\nlet k = K()\nk.old()\nlet v = k.v\nlet g = Gone()"}));
        std::vector<std::string> warnings;
        for (const auto &diagnostic : dk3::typeCheck(program, {})) if (diagnostic.warning) warnings.push_back(diagnostic.message);
        assert(warnings.size() == 4 && warnings[0] == "'a' is deprecated: use b" && warnings[1] == "'old' is deprecated" && warnings[2] == "'v' is deprecated" && warnings[3] == "'Gone' is deprecated");
    }
    {
        // Method annotations are metadata; one shaped like a decorator of the method is flagged as not applied.
        auto program = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"method-decorator.k3", "fn twice(f: fn(Int) -> Int) -> fn(Int) -> Int { return f }\nfn tag(name: String) -> String { return name }\nclass C {\n    static fn log(f: fn(Int) -> Int) -> fn(Int) -> Int { return f }\n    @twice fn a(x: Int) -> Int { return x }\n    @C.log fn b(x: Int) -> Int { return x }\n    @tag('meta') fn c(x: Int) -> Int { return x }\n    @twice fn d(x: String) -> Int { return 1 }\n}"}));
        std::vector<std::string> warnings;
        for (const auto &diagnostic : dk3::typeCheck(program, {})) if (diagnostic.warning) warnings.push_back(diagnostic.message);
        assert(warnings.size() == 2 && warnings[0].rfind("@twice is not applied", 0) == 0 && warnings[1].rfind("@C.log is not applied", 0) == 0);
    }

    if (failures) { std::cerr << failures << " typer test(s) failed\n"; return 1; }
    std::cout << "typer: PASS\n";
}
