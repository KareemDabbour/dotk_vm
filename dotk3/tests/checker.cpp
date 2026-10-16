#include "checker.hpp"
#include <cassert>
#include <iostream>

int main() {
    auto parse = [](const std::string &text) { return dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"check.k3", text})); };
    dk3::check(parse("var total = 0; for (var index: Int = 0; index < 10; index++) { if index == 2 { continue }; if index == 5 { break }; total += index }; for (;;) { break }"));
    dk3::check(parse("IO.Terminal.open(); let key: Int? = IO.Terminal.readKey(0); let open: Bool = IO.Terminal.isOpen(); IO.Terminal.close()"));
    dk3::check(parse("import Sockets as Tcp; from Sockets import receive as read; from Http import writeResponse as reply; let server: Resource = Tcp.listen('127.0.0.1', 0); let client: Resource? = IO.Sockets.accept(server, 0); let bytes: Bytes? = read(client, 8, 0); let request: Map? = IO.Http.readRequest(client, 0); let sent: Int = reply(client, 200, b'body'); let closed: Null = Tcp.close(server)"));
    dk3::check(parse("let value: Object = 1; let optional: Object? = null; class Child extends Object {}; let child: Object = Child()"));
    dk3::check(parse("from System import Object as ValueObject; let value: ValueObject = 1; let qualified: System.Object = 'value'; IO.Files.readText('path')"));
    dk3::check(parse("let name: String = 'Ada'; let result: String = f'Hello {name} {1 + 2}'"));
    dk3::check(parse("class Indexed { var value: Int = 0; fn __getitem__(key: String) -> Int { return self.value }; fn __setitem__(key: String, value: Int) -> Null { self.value = value } }; let value = Indexed(); value['x'] = 1; let result: Int = value['x']"));
    dk3::check(parse("record Vector { x: Int; fn toString() -> String { return Text.str(self.x) }; fn __add__(other: Vector) -> Vector { return Vector(self.x + other.x) }; fn __mul__(amount: Int) -> Vector { return Vector(self.x * amount) }; fn __neg__() -> Vector { return Vector(-self.x) }; fn __lt__(other: Vector) -> Bool { return self.x < other.x } }\nvar value: Vector = Vector(2)\nvalue *= 3\nlet sum: Vector = value + Vector(1)\nlet negative: Vector = -value\nlet smaller: Bool = value < Vector(10)"));
    dk3::check(parse("class Vault { private var secret: Int = 3; protected var shared: Int = 1; private static let code: Int = 7; public fn read() -> Int { return self.secret + Vault.code } }\nclass Child extends Vault { fn readShared() -> Int { self.shared += 1; return self.shared } }\nlet value: Int = Child().read()"));
    dk3::check(parse("class C { private static let seed: Int = 5; private let value: Int = C.seed; fn read() -> Int { return self.value } }\ntrait T { fn read() -> Int }; class D { private let value: Int = 11 }; impl T for D { fn read() -> Int { return self.value } }"));
    dk3::check(parse("class DefaultConstructed { let value: Int; constructor() -> Null { self.value = 42 } }; let value: Int = DefaultConstructed().value"));
    dk3::check(parse("class PlayerInit { let name: String; fn init(name: String) -> Null { self.name = name } }\nlet player = PlayerInit('Ada')\nclass Tagged extends Collections.List { let label: String = 'items'; fn append(value: Int) -> Null { super.append(value) } }\nlet values: List[Int] = Tagged()\nlet unique: Set[Int] = Set([1,2])\nlet present: Bool = unique.contains(1)\nlet snapshot: List[Int] = unique.values()"));
    dk3::check(parse("from Collections import List as Sequence\nlet values: Collections.List[Int] = Sequence([1,2])\nlet alias: Sequence[Int] = values"));
    dk3::check(parse("class ParentCheck { static var count: Int = 0; var value: Int; fn read() -> Int { return self.value } }\nclass ChildCheck extends ParentCheck { let label: String = 'child'; fn read() -> Int { return super.read() + 1 } }\nlet child: ParentCheck = ChildCheck(3)\nChildCheck.count += 1\nchild.value = 4\nlet read: Int = child.read()"));
    dk3::check(parse("trait LateTrait { fn read() -> Int }\nclass LateBase { fn read() -> Int { return 9 } }\nclass LateChild extends LateBase {}\nimpl LateTrait for LateBase {}\nlet value: LateTrait = LateChild()"));
    dk3::check(parse("class StaticCheck { static var count: Int = 0; static let label: String = 'value'; var value: Int; static fn create(value: Int) -> StaticCheck { StaticCheck.count += 1; return StaticCheck(value) }; fn read() -> Int { return self.value } }\nlet value: StaticCheck = StaticCheck.create(3)\nlet count: Int = StaticCheck.count\nlet label: String = StaticCheck.label\nlet number: Int = value.read()"));
    dk3::check(parse("trait NamedBoth { fn name() -> String }\nclass BagBoth implements NamedBoth, Collections.Sized { fn name() -> String { return 'bag' }; fn length() -> Int { return 1 } }\nfn describe(value: Collections.Sized & NamedBoth) -> String { let count: Int = value.length(); return value.name() }\nlet both: NamedBoth & Collections.Sized = BagBoth()\nlet values: List[Collections.Sized & NamedBoth] = [both]\nlet optional: (Collections.Sized & NamedBoth)? = null\nlet name: String = describe(both)"));
    dk3::check(parse("trait NamedOrder { fn name() -> String }\ntrait ConsumerOrder { fn consume(value: NamedOrder & Collections.Sized) -> Int }\nclass ConsumerImpl implements ConsumerOrder { fn consume(value: Collections.Sized & NamedOrder) -> Int { return value.length() } }"));
    dk3::check(parse("let values: Map[String, Int] = {}\nlet value: Int? = values.compute('x', fn(key: String, old: Int?) -> Int => (old ?? 0) + 1)\nvalues.computeIfAbsent('y', fn(key: String) -> Int => 2)\nvalues.computeIfPresent('y', fn(key: String, old: Int) -> Int? => null)\nvalues.putIfAbsent('z', 3)\nvalues.merge('z', 2, fn(old: Int, added: Int) -> Int => old + added)"));
    dk3::check(parse("let values: List[Int] = [1,2]\nlet mapped: List[String] = values.map(fn(value: Int) -> String => Text.str(value))\nlet filtered: List[Int] = values.filter(fn(value: Int) -> Bool => value > 1)\nlet reduced: Int = values.reduce(fn(total: Int, value: Int) -> Int => total + value, 0)\nlet found: Int? = values.find(fn(value: Int) -> Bool => value == 2)\nlet position: Int = values.findIndex(fn(value: Int) -> Bool => value == 2)"));
    dk3::check(parse("let value: Int? = [].reduce(fn(total: Int?, value: Int) -> Int => (total ?? 0) + value, null)"));
    dk3::check(parse("from Random import seed, int as draw, choice\nseed(42)\nlet value: Int = draw(1, 7)\nlet values: List[Int] = [1,2]\nlet chosen: Int = choice(values)\nlet unit: Float = Random.float()\nRandom.shuffle(values)\nlet letter: String = Random.choice('ab')\nlet byte: Int = Random.choice(b'ab')"));
    dk3::check(parse("fn inspect(value: Resource) -> String { let closed: Bool = value.closed; return value.type_name }"));
    dk3::check(parse("from Json import parse, stringify\nlet data: Map[String, List[Int]] = parse('{\"x\":[1]}')\nlet encoded: String = stringify(data)"));
    dk3::check(parse("from Files import readText as read\nlet text: String = read('data.txt')\nlet bytes: Bytes = Files.readBytes('data.bin')\nlet exists: Bool = Files.exists('data.txt')\nlet size: Int = Files.size('data.txt')"));
    dk3::check(parse("let word = 'abc'\nlet found: Bool = word.contains('b')"));
    dk3::check(parse("from Requests import get as fetch\nlet response: Map = Requests.post('https://x.test', {'json': {'a': 1}})\nlet other: Map = fetch('https://x.test')\nlet any: Map = Requests.request('PUT', 'https://x.test', {'body': 'a'})\nlet data: Any = Requests.json(response)\nlet status: Int = response['status']"));
    dk3::check(parse("let result: Bool = 'abc'.startsWith('a')"));
    dk3::check(parse("let result: Bool = 'abc'.endsWith('c')"));
    dk3::check(parse("let result: Int = 'abc'.find('c')"));
    dk3::check(parse("let result: List[String] = 'a,b'.split(',')\nlet part: String = result[0]"));
    dk3::check(parse("let values: List[String] = ['a', 'b']\nlet result: String = ','.join(values)"));
    dk3::check(parse("let add: Function = fn(first: Int, second: Int = 2) -> Int => first + second\nlet value: Int = add(second: 3, first: 1)\nlet direct: Int = fn(value: Int) -> Int { return value + 1 }(2)\nlet stream = fn() { yield 1 }\nlet iterator: Generator = stream()"));
    dk3::check(parse("let data: List[Int?] = [null]\nlet found: Bool = data.contains(null)"));
    dk3::check(parse("let data: List[Int] = [1]\nlet found: Int = data.indexOf(1)"));
    dk3::check(parse("let data: List[Int] = [1]\nlet cleared: Null = data.clear()\ndata.append(2)"));
    dk3::check(parse("let data: List[Int] = [1]\nlet reversed: Null = data.reverse()"));
    dk3::check(parse("let data: Map[String, Int] = {}\nlet cleared: Null = data.clear()\ndata['x'] = 1"));
    dk3::check(parse("let data: Map[String, Int] = {}\nlet value: Int? = data.get('missing')\nlet fallback: Int = value ?? 0"));
    dk3::check(parse("let data: List[Int] = [1]\ndata.insert(0, 2)"));
    dk3::check(parse("let data: List[Int] = [1]\nlet removed: Int = data.removeAt(-1)"));
    dk3::check(parse("let data: List[Int] = []\ndata.extend([1, 2])\ndata.extend((3, 4))\ndata.extend(b'ab')"));
    dk3::check(parse("let data: Map[String, Int?] = {'x': null}\nlet present: Bool = data.contains('x')"));
    dk3::check(parse("let data: Map[String, Int] = {'x': 1}\nlet removed: Bool = data.remove('x')"));
    dk3::check(parse("let data: Map[String, Int] = {'x': 1}\nlet keys: List[String] = data.keys()\nlet values: List[Int] = data.values()\nlet value: Int = data.values()[0]"));
    dk3::check(parse("class Initialized { var value: Int = 0; fn init() -> Null { self.value += 1 } }\nlet initialized = Initialized(2)"));
    dk3::check(parse("trait Reader { fn read() -> Int; fn twice() -> Int { return self.read() * 2 } }\nclass C implements Reader { fn read() -> Int { return 3 } }\nlet result: Int = C().twice()"));
    dk3::check(parse("trait Named { fn name() -> String { return 'record' } }\nrecord Point implements Named, Collections.Sized { x: Int; fn length() -> Int { return 1 }; fn copy() -> Point { return self } }\nlet value: Collections.Sized = Point(2)\nlet result: Int = value.length()\nlet named: Named = Point(3)"));
    dk3::check(parse("let name: String = 'DotK'\nvar count: Int = 0\nfn increment(value: Int) -> Int { return value + 1 }\ncount = increment(count)\n"));
    dk3::check(parse("record Point { x: Int, y: Int = x + 1 }\nlet point = Point(x: 2)\nlet value: Int = point.y\nSystem.print(System.args)"));
    dk3::check(parse("enum Result { Ok(Int), Error(String) }\nfn unwrap(result: Result) -> Int? { return match result { Result.Ok(value) => value, Result.Error(message) => null } }\nlet value: Int? = unwrap(Result.Ok(2))\nlet present: Int = value ?? 0"));
    dk3::check(parse("enum Event { Move(Int), Quit }\nlet event = Event.Move(2)\nlet value: Int = match event { Event.Move(number) => number, _ => 0 }"));
    dk3::check(parse("record Box { values: List[Int] }\nlet box = Box([1])\nlet copy: List[Int] = box.values + [2]"));
    dk3::check(parse("fn accepts(first: Int, second: Int = 2, *rest) -> Int { return first + second }\nlet result = accepts(1, 2, 3, 4)"));
    dk3::check(parse("fn dynamic() -> Any? { return null }\nlet value: Int? = dynamic()"));
    dk3::check(parse("import model as model\nfn accepts(value: model.Point) -> model.Point { return value }\nlet point: model.Point = model.Point(1)\nlet result = accepts(point)\nlet values: List[model.Point?] = [point, null]"));
    dk3::check(parse("let scores: Map[String, Int] = {'Ada': 1}\nscores['Grace'] = 2\nlet value: Int = scores['Ada']"));
    dk3::check(parse("let nested: Map[String, List[Int]] = {'group': [1, 2]}\nlet score: Int = nested['group'][0]"));
    dk3::check(parse("from System import print as say\nfrom Collections import List as NewList\nlet data = NewList()\nlet size: Int = data.length()\nsay(size)"));
    dk3::check(parse("from Collections import Sized\ntrait Readable { fn read() -> Int }\nclass Box { let value: Int }\nimpl Readable for Box { fn read() -> Int { return self.value } }\nfn read(value: Readable) -> Int { return value.read() }\nlet result: Int = read(Box(2))\nfn size(value: Sized) -> Int { return value.length() }\nlet count: Int = size([1, 2])"));
    dk3::check(parse("class Bag { let values: List[Int] = [] }\nimpl Collections.Sized for Bag { fn length() -> Int { return self.values.length() } }\nlet bag: Collections.Sized = Bag()\nlet count: Int = bag.length()"));
    dk3::check(parse("from Collections import Sized as Size\ntrait Inspect { fn length(value: Collections.Sized) -> Int }\nclass Inspector {}\nimpl Inspect for Inspector { fn length(value: Size) -> Int { return value.length() } }"));
    dk3::check(parse("from Collections import Sized\ntrait Readable { fn read() -> String }\nclass Bag implements Sized, Readable { let values: List[Int] = []; fn length() -> Int { return self.values.length() }; fn read() -> String { return 'bag' }; fn asSized() -> Sized { return self } }\nfn size(value: Sized) -> Int { return value.length() }\nlet bag: Readable = Bag()\nlet count: Int = size(Bag())"));
    dk3::check(parse("class Counter { var value: Int = 0; let name: String = 'counter'; fn add(amount: Int = 1) -> Int { self.value += amount; return self.value } }\nlet counter: Counter = Counter(value: 2)\ncounter.value = 3\nlet value: Int = counter.add(amount: 4)\nlet callback: Function = counter.add"));
    for (const std::string &text : {
        "for (var index = 0; index < 3; index++) {}; System.print(index)",
        "for (var index = 0; 1; index++) {}",
        "for (let index = 0; index < 3; index++) {}",
        "Sockets.listen(1, 0)",
        "Requests.get(1)",
        "Requests.get('https://x.test', 'bad')",
        "Requests.request('GET', 1)",
        "Requests.json('bad')",
        "Requests.get(url: 'https://x.test')",
        "Sockets.connect('127.0.0.1', false)",
        "IO.Sockets.accept([])",
        "IO.Http.readRequest(null)",
        "Sockets.close()",
        "Sockets.listen(host: '127.0.0.1', port: 0)",
        "from Sockets import connect; connect('127.0.0.1', 'bad')",
        "from Http import writeRequest; fn send(peer: Resource) { writeRequest(peer, 'GET', '/', 1) }",
        "fn send(peer: Resource) { Http.writeResponse(peer, 200, true) }",
        "fn read(peer: Resource) { Http.readResponse(peer, false) }",
        "fn send(peer: Resource) { IO.Http.writeResponse(peer, 200, '', 1) }",
        "IO.Terminal.readKey('bad')",
        "IO.Terminal.open(1)",
        "IO.Terminal.readKey(timeout: 0)",
        "IO.Files.readText(1)",
        "let invalid: Object = null",
        "let result = f'{missing}'",
        "class C { fn __getitem__(key: String) -> Int { return 1 } }; C()[1]",
        "class C { fn __setitem__(key: String, value: Int) -> Null {} }; C()['x'] = 'bad'",
        "class C { fn toString() -> Int { return 1 } }",
        "class C { private fn __add__(other) {} }",
        "class C { fn __add__() {} }",
        "class C { fn __lt__(other) -> Int { return 1 } }",
        "class C { fn __neg__() { yield 1 } }",
        "class C { fn __mul__(amount: Int) -> Int { return amount } }; C() * 'bad'",
        "class C { private var value: Int = 1 }; C().value",
        "class C { protected var value: Int = 1 }; C().value = 2",
        "class C { private static let value: Int = 1 }; C.value",
        "class B { private var value: Int = 1 }; class C extends B { fn reveal() -> Int { return self.value } }",
        "class B { fn read() -> Int { return 1 } }; class C extends B { private fn read() -> Int { return 2 } }",
        "trait T { fn read() -> Int }; class C implements T { private fn read() -> Int { return 1 } }",
        "class C extends C {}",
        "class C extends Int {}",
        "class B { var value: Int }\nclass C extends B { var value: Int }",
        "class B { fn read() -> Int { return 1 } }\nclass C extends B { fn read() -> String { return 'bad' } }",
        "class B { var value: Int }\nclass C extends B {}\nC('bad')",
        "class C { static fn create(value: Int) -> Int { return value } }\nC.create('bad')",
        "class C { static let value: Int = 1 }\nC.value = 2",
        "class C { static var value: Int = 1 }\nC.value = 'bad'",
        "class C { static fn read() -> Int { return self.value } }",
        "class C { static fn read() -> Int { return 1 } }\nC().read()",
        "trait NamedBoth { fn name() -> String }\nfn describe(value: Collections.Sized & NamedBoth) {}\ndescribe([])",
        "trait NamedBoth { fn name() -> String }\nlet value: Int & NamedBoth = 1",
        "trait First { fn read() -> Int }\ntrait Second { fn read() -> String }\nfn describe(value: First & Second) {}",
        "let values: Map[String, Int] = {}\nvalues.compute('x', fn(key: String, old: Int) -> Int => old)",
        "let values: Map[String, Int] = {}\nvalues.computeIfAbsent('x', fn(key: Int) -> Int => key)",
        "let values: Map[String, Int] = {}\nvalues.computeIfPresent('x', fn(key: String, old: Int) -> String => key)",
        "let values: Map[String, Int] = {}\nvalues.putIfAbsent('x', 'bad')",
        "let values: Map[String, Int?] = {}\nvalues.merge('x', null, fn(old, added) => added)",
        "[1].map(1)",
        "[1].filter(fn(value: Int) -> Int => value)",
        "[1].map(fn() => 1)",
        "let values: List[Int] = []\nvalues.map(fn(value: String) => value)",
        "[1].all(fn(value) { yield true })",
        "[1].reduce(fn(total: Int, value: Int) -> String => 'bad', 0)",
        "let value: Int = [].reduce(fn(total: Int?, value: Int) -> Int => (total ?? 0) + value, null)",
        "let values: List[Int] = []\nlet found: Int = values.find(fn(value) -> Bool => true)",
        "Random.int('bad')",
        "Random.seed(true)",
        "Random.shuffle((1,2))",
        "Random.choice({})",
        "from Random import int as draw\ndraw(stop: 5)",
        "fn change(value: Resource) { value.closed = true }",
        "fn change(value: Resource) { value.type_name = 'Other' }",
        "fn inspect(value: Resource) -> Int { return value.closed }",
        "fn inspect(value: Resource) { value.payload }",
        "Json.parse(1)",
        "Json.parse(b'null')",
        "from Json import parse\nparse(source: 'null')",
        "Json.stringify()",
        "Json.parse = fn() {}",
        "Files.readText(1)",
        "Files.readBytes(b'path')",
        "Files.size()",
        "Files.exists(path: 'data.txt')",
        "from Files import readText as read\nread(1)",
        "Files.readText = fn() {}",
        "let word = 'abc'\nword.contains(1)",
        "'abc'.startsWith(b'a')",
        "'abc'.endsWith(1)",
        "let result: String = 'abc'.find('a')",
        "'a,b'.split(',').append(1)",
        "','.join(['a', 1])",
        "','.join(b'ab')",
        "','.join(values: ['a'])",
        "let word = 'abc'\nword.contains(needle: 'a')",
        "let add = fn(value: Int) -> Int => value\nadd('bad')",
        "let add: Function = fn(value: Int) -> Int => value\nadd()",
        "let add = fn(value: Int) -> Int => value\nadd(unknown: 1)",
        "let add = fn(value: Int) -> Int => value\nlet result: String = add(1)",
        "fn(value: Int) -> Int { return value }('bad')",
        "let data: List[Int] = []\ndata.contains('bad')",
        "let data: List[Int] = []\ndata.indexOf('bad')",
        "let data: List[Int] = []\ndata.clear(1)",
        "let data: List[Int] = []\ndata.reverse(1)",
        "let data: Map[String, Int] = {}\ndata.clear(1)",
        "let data: Map[String, Int] = {}\ndata.get(1)",
        "let data: Map[String, Int] = {}\nlet value: Int = data.get('x')",
        "let data: Map[String, Int] = {}\ndata.get(key: 'x')",
        "let data: List[Int] = []\ndata.insert('bad', 1)",
        "let data: List[Int] = []\ndata.insert(0, 'bad')",
        "let data: List[Int] = []\ndata.insert(0)",
        "let data: List[Int] = []\ndata.insert(index: 0, value: 1)",
        "let data: List[Int] = []\ndata.removeAt('bad')",
        "let data: List[Int] = []\ndata.removeAt()",
        "let data: List[Int] = []\nlet removed: String = data.removeAt(0)",
        "let data: List[Int] = []\ndata.extend([1, 'bad'])",
        "let data: List[Int] = []\ndata.extend('abc')",
        "let data: List[Int] = []\ndata.extend(1)",
        "let data: List[Int] = []\ndata.extend(values: [1])",
        "let data: Map[String, Int] = {}\ndata.contains(1)",
        "let data = {}\ndata.contains([])",
        "let data = {}\ndata.contains()",
        "let data = {}\ndata.contains(key: 'x')",
        "let data: Map[String, Int] = {}\ndata.remove(1)",
        "let data = {}\ndata.remove()",
        "let data = {}\ndata.remove(key: 'x')",
        "let data: Map[String, Int] = {}\ndata.keys().append(1)",
        "let data: Map[String, Int] = {}\ndata.values().append('wrong')",
        "let data = {}\ndata.keys(1)",
        "record R { x: Int; fn write() -> Null { self.x = 1 } }",
        "record R implements Collections.Sized {}",
        "record R implements Collections.Sized { fn length() -> String { return '' } }",
        "trait T { fn read() -> Int { return 'bad' } }",
        "trait T { fn read() -> Int { yield 1 } }",
        "trait T { fn read() -> Int { return 1 } }\ntrait U { fn read() -> Int { return 2 } }\nclass C implements T, U {}",
        "trait T { fn read() -> Int { return 1 } }\nclass C implements T { let read: Int = 1 }",
        "class C { fn init(value: Int) {} }; C('wrong')",
        "class C { fn init() { yield 1 } }",
        "class C { fn init() -> Int { return 1 } }",
        "class C { fn init() { return 1 } }",
        "class C implements Collections.Sized {}",
        "class C implements Collections.Sized { fn length() -> String { return '' } }",
        "from Collections import Sized as S\nclass C implements S, Collections.Sized { fn length() -> Int { return 0 } }",
        "class C implements Missing { fn length() -> Int { return 0 } }",
        "class C implements Int {}",
        "class C implements T { fn read() -> Int { return 1 } }\ntrait T { fn read() -> Int }",
        "class C implements Collections.Sized { fn length() -> Int { yield 1 } }",
        "class C implements Collections.Sized { fn length() -> Int { return 0 } }\nimpl Collections.Sized for C {}",
        "trait T { fn read() -> Int }\nclass C {}\nimpl T for C {}",
        "trait T { fn read() -> Int }\nclass C {}\nimpl T for C { fn read() -> String { return 'bad' } }",
        "trait T { fn read(value: Int) -> Int }\nclass C {}\nimpl T for C { fn read(other: Int) -> Int { return other } }",
        "trait T { fn read() -> Int }\nclass C { fn read() -> Int { return 1 } }\nfn take(value: T) {}\ntake(C())",
        "trait T { fn read() -> Int }\nclass C {}\nimpl T for C { fn read() -> Int { return 1 } }\nimpl T for C {}",
        "trait T { fn read() -> Int }\nclass C {}\nimpl T for C { fn read() -> Int { yield 1 } }",
        "trait T { fn read() -> Int }\nT()",
        "trait T { fn read() -> Int }\nfn mutate(value: T) { value.read = fn() -> Int { return 1 } }",
        "from Collections import Sized\nSized()",
        "fn size(value: Collections.Sized) -> Int { return value.length() }\nsize(1)",
        "class C { var value: Int = 'bad' }",
        "class C { var value: Int }\nC()",
        "class C { var value: Int }\nC('bad')",
        "class C { var value: Int = 0 }\nlet c = C()\nc.value = 'bad'",
        "class C { let value: Int = 0 }\nlet c = C()\nc.value = 1",
        "class C { fn f() {} }\nlet c = C()\nc.f = 1",
        "class C { fn f(value: Int) -> Int { return value } }\nC().f('bad')",
        "class C { fn f(value: Int) {} }\nC().f()",
        "class C { fn f() -> Int { return 'bad' } }",
        "class C { fn f() { self = 1 } }",
        "class C { var value: Int = 0; fn f() { self.missing = 1 } }",
        "class C { fn f() {} }\nC.f()",
        "record Point { x: Int }\nlet point = Point('bad')",
        "enum Event { Move(Int), Quit }\nlet event: Event = Event.Quit()\nlet result = match event { Event.Move(number) => number }",
        "record Point { x: Int }\nlet point = Point()",
        "record Point { x: Int }\nlet point = Point(z: 1)",
        "record Point { x: Int }\nlet point = Point(1, x: 2)",
        "record Point { x: Int = 'bad' }",
        "record Point { x: Int }\nlet point = Point(1)\npoint.x = 2",
        "record Point { x: Int }\nlet point = Point(1)\nSystem.print(point.missing)",
        "enum Event { Move(Int), Quit }\nlet event = Event.Move('bad')",
        "enum Event { Move(Int), Quit }\nlet event = Event.Move()",
        "enum Event { Move(Int), Quit }\nlet event = Event.Move(value: 1)",
        "enum Event { Move(Int), Quit }\nlet event = Event.Unknown()",
        "enum Event { Quit }\nlet event = Event()",
        "let namespace = 1\nlet value: namespace.Point = 1",
        "let value: missing.Point = 1",
        "import model as model\nlet value: model.Point = 1",
        "let value: List[Unknown] = []",
        "let value: Map[String] = {}",
        "let value: Map[List[Int], Int] = {}",
        "let value: Map[String, Int] = {'Ada': 'bad'}",
        "let value: Map[String, Int] = {1: 2}",
        "let value: Map[String, Int] = {}\nvalue['Ada'] = 'bad'",
        "let value: Map[String, Int] = {}\nvalue[1] = 2",
        "let value: Map[String, List[Int]] = {'group': [1, 'bad']}",
        "fn takes(value: Map[String, Int]) {}\ntakes({'Ada': 'bad'})",
        "record Box { values: Map[String, Int] = {'Ada': 'bad'} }",
        "fn bad() -> Map[String, Int] { return {'Ada': 'bad'} }",
        "let value: Map[String, List[Int]] = {}\nvalue['group'].append('bad')",
        "enum Event { Move(Int), Quit }\nlet event = Event.Move(1)\nlet result = match event { Event.Move() => 1, _ => 0 }",
        "enum Event { Move(Int), Quit }\nlet event = Event.Move(1)\nlet result = match event { Event.Move(value) => value, Event.Move(other) => other, Event.Quit => 0 }",
        "enum Event { Move(Int), Quit }\nlet event = Event.Quit()\nlet result = match event { _ => 0, Event.Quit => 1 }",
        "enum Event { Pair(Int, Int) }\nlet event = Event.Pair(1, 2)\nlet result = match event { Event.Pair(value, value) => value }",
        "enum First { Done }\nenum Second { Done }\nlet event: First = First.Done()\nlet result = match event { Second.Done => 0 }",
        "enum Event { Move(Int), Quit }\nfn bad(event: Event) -> String { return match event { Event.Move(value) => value, Event.Quit => 0 } }",
        "{ record Local { x: Int } }\nlet value: Local = 1",
        "fn typed(value: Int) -> Int { return value }\nlet result = typed('bad')"
    }) {
        bool rejected = false;
        try { dk3::check(parse(text)); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    bool namedMissing = false;
    try { dk3::check(parse("enum Event { Move(Int), Quit }\nlet event = Event.Quit()\nlet result = match event { Event.Move(value) => value }")); }
    catch (const dk3::Error &error) { namedMissing = error.category == "match" && std::string(error.what()).find("Event.Quit") != std::string::npos; }
    assert(namedMissing);
    for (const std::string &text : {"let name: Int = 'bad'", "let count = 0\ncount = 1", "missing(1)", "if 1 {}", "return 1", "break", "fn bad() { continue }", "let typed: Unknown = 1"}) {
        bool rejected = false;
        try { dk3::check(parse(text)); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    std::cout << "checker: PASS\n";
}