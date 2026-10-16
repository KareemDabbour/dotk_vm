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
    for (const char *invalid : {"trait Bad { fn length() }", "trait Bad { fn f(value) -> Int }", "trait Bad { fn f(self: Int) -> Int }", "trait Bad { fn f(value: Int = 0) -> Int }", "impl T for C { fn f(self) {} }", "export impl T for C {}", "class C implements {}", "class C implements T, T {}", "class C implements T? {}", "class C implements T[Int] {}", "class C implements T, {}", "let implements = 1"}) {
        rejected = false;
        try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"invalid-trait.k3", invalid})); }
        catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const char *unsupported : {"struct Point { x: Int }", "let struct = 1", "trait Length {}", "impl Length {}", "class C { var self: Int }", "class C { fn f(self) {} }", "class C { var x: Int; fn x() {} }", "let value = 1 '+' 2", "let value = true 'and' false"}) {
        rejected = false;
        try { dk3::parse(std::make_shared<dk3::Source>(dk3::Source{"unsupported.k3", unsupported})); }
        catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    std::cout << "parser: PASS\n";
}