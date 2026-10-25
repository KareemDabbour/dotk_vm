#include "syntax.hpp"
#include <cassert>
#include <iostream>

int main() {
    auto formatted = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"formatted.k3", "let value = f'hello {{name}} {1 + 2}'"}));
    assert(formatted->children[0]->children[0]->kind == "format");
    for (const char *invalid : {"let value = f'{}'", "let value = f'{1'", "let value = f'}'", "let value = f'{1; 2}'"}) {
        bool rejected = false; try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"format-invalid.k3", invalid})); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    auto access = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"access.k3", "class Access { private var value: Int = 0; protected static let shared: Int = 1; public fn read() -> Int { return self.value } }"}))->children[0];
    assert(access->parameters[0].visibility == dk3::Visibility::Private);
    assert(access->children[0]->visibility == dk3::Visibility::Protected && access->children[1]->visibility == dk3::Visibility::Public);
    for (const char *invalid : {"record R { private value: Int }", "class C { private public var value: Int }", "let private = 1"}) {
        bool rejected = false; try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"access-invalid.k3", invalid})); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    auto oop = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"oop.k3", "class Child extends model.Parent { static var count: Int = 0; static fn create() -> Child { return Child() }; var value: Int = 1 }"}))->children[0];
    assert(oop->baseClass == "model.Parent" && oop->parameters.size() == 1);
    assert(oop->children.size() == 2 && oop->children[0]->staticMember && oop->children[1]->staticMember);
    for (const char *invalid : {"class C { static var value: Int }", "class C { static fn init() {} }", "record R { static fn create() {} }", "class C extends Base? {}", "class C extends Base, Other {}"}) {
        bool rejected = false; try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"oop-errors.k3", invalid})); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    auto intersections = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"intersection.k3", "fn inspect(value: Collections.Sized & Named) -> String { return value.name() }\nlet optional: (Collections.Sized & Named)? = null\nlet values: List[Collections.Sized & Named] = []"}));
    assert(intersections->children[0]->parameters[0].type == "(Collections.Sized&Named)");
    assert(intersections->children[1]->type == "(Collections.Sized&Named)?");
    assert(dk3::typeIntersections(intersections->children[0]->parameters[0].type).size() == 2);
    auto source = std::make_shared<dk3::Source>(dk3::Source{"parser.k3",
        "let xs = [1, 2, 3]\nvar table = {\"key\": xs[::-1]}\n"
        "fn add(first: Int, second: Int = 2) -> Int { return first + second }\n"
        "let value = add(second: 3, first: 1)\nrecord Point { x: Float, y: Float }\n"
        "enum Event { Move(Point), Quit }\nlet position = match event { Event.Move(point) => point, Event.Quit => null }\n"});
    auto program = dk3::parse(source);
    assert(program->children.size() == 7);
    assert(program->children[0]->children[0]->kind == "list");
    assert(program->children[2]->parameters[1].defaultValue);
    assert(program->children[3]->children[0]->names[0] == "second");
    assert(program->children[6]->children[0]->kind == "match");
    auto literalTokens = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"literal-tokens.k3", "let minus = '-'\nlet keyword = 'fn'\nlet brace = '}'\nlet boolean = 'true'\nlet joined = '-'.join(['a', 'b'])"}));
    assert(literalTokens->children.size() == 5);
    for (size_t index = 0; index < 4; index++) assert(literalTokens->children[index]->children[0]->literalKind == dk3::TokenKind::String);
    auto anonymous = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"anonymous.k3", "fn(value: Int) -> Int { return value + 1 }(2)\nlet double = fn(value: Int) -> Int => value * 2\nfn() { return null }"}));
    assert(anonymous->children[0]->kind == "expression" && anonymous->children[0]->children[0]->kind == "call");
    assert(anonymous->children[1]->children[0]->kind == "function");
    assert(anonymous->children[2]->children[0]->kind == "function");
    auto modern = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"modern.k3", "from System import print as say\nrecord Point { x: Int }"}));
    assert(modern->children[0]->kind == "from_import" && modern->children[0]->names[1] == "say");
    assert(modern->children[1]->kind == "record");
    // Bitwise precedence binds tighter than comparison; `>>` is joined from adjacent `>` tokens so nested generics still close.
    auto bits = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"bits.k3", "let a = x & 1 == 0\nlet b = 1 | 2 ^ 3 & 4 << 1 + 2\nlet c: List<List<Int>> = [[y >> 2]]\nz >>= 1\nz <<= 2\nlet d = ~-x"}));
    assert(bits->children[0]->children[0]->text == "==" && bits->children[0]->children[0]->children[0]->text == "&");
    const auto &bitChain = bits->children[1]->children[0];
    assert(bitChain->text == "|" && bitChain->children[1]->text == "^" && bitChain->children[1]->children[1]->text == "&" && bitChain->children[1]->children[1]->children[1]->text == "<<" && bitChain->children[1]->children[1]->children[1]->children[1]->text == "+");
    assert(bits->children[2]->children[0]->children[0]->children[0]->text == ">>");
    assert(bits->children[3]->children[0]->text == ">>=" && bits->children[4]->children[0]->text == "<<=");
    assert(bits->children[5]->children[0]->text == "~" && bits->children[5]->children[0]->children[0]->text == "-");
    // a[i, j] indexes with a tuple; a slice among several subscripts, or any slice assignment, calls a slice hook.
    auto subscripts = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"subscripts.k3", "a[i, j]\na[1:2]\na[::2, 0]\na[1:] = v\na[0, :] = v\na[i, j] += 1"}));
    const auto &pair = subscripts->children[0]->children[0];
    assert(pair->kind == "index" && pair->children[1]->kind == "tuple" && pair->children[1]->children.size() == 2);
    assert(subscripts->children[1]->children[0]->kind == "slice");
    const auto &mixed = subscripts->children[2]->children[0];
    assert(mixed->kind == "call" && mixed->children[0]->text == "__getslice__" && mixed->children[1]->kind == "tuple");
    const auto &stepKey = mixed->children[1]->children[0];
    assert(stepKey->kind == "call" && stepKey->children[0]->text == "Slice" && stepKey->children[0]->children[0]->text == "Collections");
    assert(stepKey->children[1]->text == "null" && stepKey->children[3]->text == "2" && mixed->children[1]->children[1]->text == "0");
    const auto &single = subscripts->children[3]->children[0];
    assert(single->kind == "call" && single->children[0]->text == "__setslice__" && single->children.size() == 3 && single->children[1]->children.size() == 1);
    assert(subscripts->children[4]->children[0]->children[0]->text == "__setslice__");
    assert(subscripts->children[5]->children[0]->kind == "assignment" && subscripts->children[5]->children[0]->children[0]->children[1]->kind == "tuple");
    for (const char *invalid : {"a[1:] += 1", "a[0, :] *= 2", "a[1:]++", "a[]", "a[1,]", "a[1, 2"}) {
        bool rejected = false; try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"subscripts-invalid.k3", invalid})); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    auto documented = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"docs.k3", "record Point {\n\"\"\"A point.\nImmutable data.\"\"\"\nx: Int\n}\nfn name() { \"\"\"A function.\"\"\"\nreturn 1 }"}));
    assert(documented->children[0]->documentation == "A point.\nImmutable data.");
    assert(documented->children[1]->documentation == "A function.");
    bool rejected = false;
    try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"bad.k3", "let x = f(name: 1, name: 2)"})); }
    catch (const dk3::Error &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"reserved.k3", "let true = 1"})); }
    catch (const dk3::Error &) { rejected = true; }
    assert(rejected);
    auto counter = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"class.k3", "export class Counter {\n\"\"\"Mutable counter.\"\"\"\nvar value: Int = 0\nlet name: String\nfn add(amount: Int) -> Int { self.value += amount; return self.value }\n}"}))->children[0];
    assert(counter->kind == "class" && counter->exported && counter->documentation == "Mutable counter.");
    assert(counter->parameters[0].mutableBinding && !counter->parameters[1].mutableBinding);
    assert(counter->children[0]->text == "add");
    auto embedded = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"implements.k3", "export class Bag implements Collections.Sized,\nReadable { fn length() -> Int { return 0 }; fn read() -> String { return '' } }"}))->children[0];
    assert(embedded->kind == "class" && embedded->exported);
    assert((embedded->names == std::vector<std::string>{"Collections.Sized", "Readable"}));
    assert(embedded->children.size() == 2);
    auto traits = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"traits.k3", "export trait Sized { \"\"\"A length contract.\"\"\"\nfn length() -> Int }\nimpl Sized for Bag { fn length() -> Int { return self.items.length() } }"}));
    assert(traits->children[0]->kind == "trait" && traits->children[0]->exported);
    assert(traits->children[0]->children[0]->kind == "signature");
    assert(traits->children[1]->kind == "impl" && traits->children[1]->names[0] == "Bag");
    auto defaults = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"defaults.k3", "trait Named { fn name() -> String { \"\"\"A default name.\"\"\"\nreturn 'default' } }"}))->children[0];
    assert(defaults->children[0]->kind == "function" && defaults->children[0]->documentation == "A default name.");
    auto recordMethods = dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"record-methods.k3", "record Point implements Named { x: Int, y: Int = x; fn name() -> String { return 'point' } }"}))->children[0];
    assert(recordMethods->kind == "record" && recordMethods->names[0] == "Named" && recordMethods->children[0]->text == "name");
    for (const char *invalid : {"trait Bad { fn length() }", "trait Bad { fn f(value) -> Int }", "trait Bad { fn f(self: Int) -> Int }", "trait Bad { fn f(value: Int = 0) -> Int }", "impl T for C { fn f(self) {} }", "export impl T for C {}", "class C implements {}", "class C implements T, T {}", "class C implements T? {}", "class C implements T, {}", "let implements = 1"}) {
        rejected = false;
        try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"invalid-trait.k3", invalid})); }
        catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const char *unsupported : {"struct Point { x: Int }", "let struct = 1", "trait Length {}", "impl Length {}", "class C { var self: Int }", "class C { fn f(self) {} }", "class C { var x: Int; fn x() {} }", "let value = 1 '+' 2", "let value = true 'and' false",
                                    "enum E { A = 1, B }", "enum E: Int { A }", "enum E { A = 1, B(Int) }", "enum E<T> { A = 1 }"}) {
        rejected = false;
        try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"unsupported.k3", unsupported})); }
        catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    auto parseSource = [](const char *text) { return dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"static.k3", text})); };
    auto rejects = [&](const char *text) { try { parseSource(text); } catch (const dk3::Error &) { return true; } return false; };

    auto generics = parseSource("fn first<T extends Comparable, U>(items: List<T>, pick: fn(T, Int) -> U?) -> T? { return null }");
    auto genericFunction = generics->children[0];
    assert(genericFunction->typeParams.size() == 2 && genericFunction->typeParams[0].name == "T" && genericFunction->typeParams[0].bound->name == "Comparable");
    assert(genericFunction->parameters[0].typeExpr->kind == dk3::TypeKind::Named && genericFunction->parameters[0].typeExpr->items[0]->name == "T");
    assert(genericFunction->parameters[0].type == "List[T]");
    auto functionType = genericFunction->parameters[1].typeExpr;
    assert(functionType->kind == dk3::TypeKind::Function && functionType->items.size() == 2 && functionType->inner->kind == dk3::TypeKind::Nullable);
    assert(genericFunction->typeExpr->kind == dk3::TypeKind::Nullable && genericFunction->type == "T?");

    auto unions = parseSource("let value: Int | String | Null = 1\nlet nested: List<Map<String, Int | Float>>= []\nlet either: (Int | String)? = null");
    assert(unions->children[0]->typeExpr->kind == dk3::TypeKind::Union && unions->children[0]->typeExpr->items.size() == 3);
    assert(unions->children[1]->typeExpr->items[0]->items[1]->kind == dk3::TypeKind::Union);
    assert(unions->children[2]->typeExpr->kind == dk3::TypeKind::Nullable && unions->children[2]->typeExpr->inner->kind == dk3::TypeKind::Union);
    assert(unions->children[2]->type == "(Int|String)?");

    auto wildcards = parseSource("fn total(values: List<? extends Number>, sink: Map<?, ? super Int>, any: List<?>) -> Int { return 0 }");
    auto bounded = wildcards->children[0]->parameters[0].typeExpr->items[0];
    assert(bounded->kind == dk3::TypeKind::Wildcard && bounded->bound == dk3::WildcardBound::Extends && bounded->inner->name == "Number");
    assert(wildcards->children[0]->parameters[1].typeExpr->items[1]->bound == dk3::WildcardBound::Super);
    assert(wildcards->children[0]->parameters[2].typeExpr->items[0]->bound == dk3::WildcardBound::None);

    auto declarations = parseSource("class Stack<T> implements Iterable<T> { var items: List<T> = [] }\nrecord Pair<A, B> { first: A, second: B }\nenum Option<T> { Some(T), None }\ntrait Source<T> { fn next() -> T? }\ntype Point = Tuple<Int, Int>\nexport type Pair2<T> = Tuple<T, T>\nclass Node extends Base<Int> {}");
    assert(declarations->children[0]->typeParams.size() == 1 && declarations->children[0]->typeExprs[0]->items[0]->name == "T");
    assert(declarations->children[1]->typeParams.size() == 2 && declarations->children[2]->typeParams[0].name == "T" && declarations->children[3]->typeParams[0].name == "T");
    assert(declarations->children[4]->kind == "type_alias" && declarations->children[4]->text == "Point" && declarations->children[4]->typeExpr->items.size() == 2);
    assert(declarations->children[5]->kind == "type_alias" && declarations->children[5]->exported && declarations->children[5]->typeParams.size() == 1);
    assert(declarations->children[6]->baseClass == "Base" && declarations->children[6]->typeExpr->items[0]->name == "Int");

    auto instantiation = parseSource("let a = first<Int>(items)\nlet b = Stack<Int>()\nlet c = List<Int>.of(1)\nlet d = x < y\nlet e = i < n && n > (j)\nlet f = (a < b) == (c > d)");
    assert(instantiation->children[0]->children[0]->kind == "call" && instantiation->children[0]->children[0]->children[0]->typeArgs.size() == 1);
    assert(instantiation->children[1]->children[0]->children[0]->typeArgs[0]->name == "Int");
    assert(instantiation->children[2]->children[0]->kind == "call" && instantiation->children[2]->children[0]->children[0]->children[0]->typeArgs.size() == 1);
    assert(instantiation->children[3]->children[0]->kind == "binary" && instantiation->children[3]->children[0]->text == "<");
    assert(instantiation->children[4]->children[0]->kind == "binary" && instantiation->children[4]->children[0]->text == "&&");
    assert(instantiation->children[5]->children[0]->kind == "binary" && instantiation->children[5]->children[0]->text == "==");

    auto operators = parseSource("let a = value is String && other is List<Int>?\nlet b = (value as Int) + 1\nlet c = value as Map<String, Int>\nlet d = user?.name\nlet e = maybe!\nlet f = user?.address?.city!.length()\nlet g = x != y");
    auto conjunction = operators->children[0]->children[0];
    assert(conjunction->kind == "binary" && conjunction->text == "&&" && conjunction->children[0]->kind == "is" && conjunction->children[0]->typeExpr->name == "String");
    assert(conjunction->children[1]->kind == "is" && conjunction->children[1]->typeExpr->kind == dk3::TypeKind::Nullable);
    assert(operators->children[1]->children[0]->children[0]->kind == "cast" && operators->children[1]->children[0]->children[0]->typeExpr->name == "Int");
    assert(operators->children[2]->children[0]->kind == "cast" && operators->children[2]->children[0]->typeExpr->items.size() == 2);
    assert(operators->children[3]->children[0]->kind == "member" && operators->children[3]->children[0]->safe);
    assert(operators->children[4]->children[0]->kind == "nonnull");
    auto chain = operators->children[5]->children[0];
    assert(chain->kind == "call" && chain->children[0]->kind == "member" && chain->children[0]->children[0]->kind == "nonnull");
    assert(chain->children[0]->children[0]->children[0]->safe && operators->children[6]->children[0]->text == "!=");

    auto functionTypeAlias = parseSource("type Callback = fn(Int) -> Int\nlet type = 1\nlet is_ok = type\nSystem.print(Reflection.type(1))");
    assert(functionTypeAlias->children[0]->typeExpr->kind == dk3::TypeKind::Function && functionTypeAlias->children[1]->kind == "binding");
    for (const char *invalid : {"let x: List<Int = []", "let x: Int | = 1", "fn f<T, T>() {}", "fn f<>() {}", "let x: List<? extends> = []", "let x = y as", "let is = 1", "class C extends Base<Int>? {}", "type = 1 2", "type Alias<T> Tuple<T, T>"})
        assert(rejects(invalid));

    auto annotated = parseSource("@route('/x', verb: 'GET')\n@deprecated\nexport fn handler(@meta(1) id: Int) -> Null {}\n@model class User { @column('n', -2) var name: String = ''; @override fn show() -> Null {}; @key static let shared: Int = 1 }\ntrait T { @pure fn read() -> Int }\nimpl T for User { @inline fn read() -> Int { return 1 } }");
    const auto &handler = annotated->children[0];
    assert(handler->exported && handler->annotations.size() == 2 && handler->annotations[0].name == "route" && !handler->annotations[0].builtin);
    assert(handler->annotations[0].expression->kind == "call" && handler->annotations[0].expression->names[1] == "verb");
    assert(handler->annotations[1].builtin && handler->annotations[1].expression->kind == "name" && handler->parameters[0].annotations[0].name == "meta");
    const auto &user = annotated->children[1];
    assert(user->annotations[0].name == "model" && user->parameters[0].annotations[0].name == "column");
    assert(user->children[0]->annotations[0].name == "override" && user->children[1]->annotations[0].name == "key");
    assert(annotated->children[2]->children[0]->annotations[0].name == "pure" && annotated->children[3]->children[0]->annotations[0].name == "inline");
    assert(parseSource("@a.b.c fn f() -> Null {}")->children[0]->annotations[0].name == "a.b.c");
    assert(parseSource("@trace('a') @trace('b') fn f() -> Null {}")->children[0]->annotations.size() == 2);
    auto generic = parseSource("@memo<Int, Int>\n@cache<String>('k') fn f(x: Int) -> Int { return x }")->children[0];
    assert(generic->annotations[0].expression->kind == "name" && generic->annotations[0].expression->typeArgs.size() == 2);
    assert(generic->annotations[1].expression->kind == "call" && generic->annotations[1].expression->children[0]->typeArgs.size() == 1);
    for (const char *invalid : {"@x let y = 1", "@x System.print(1)", "@x fn(a) => a", "@test @test fn f() -> Null {}", "@ fn f() -> Null {}", "@1 fn f() -> Null {}", "@x"})
        assert(rejects(invalid));

    std::cout << "parser: PASS\n";
}