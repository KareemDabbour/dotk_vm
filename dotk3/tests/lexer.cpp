#include "syntax.hpp"
#include <cassert>
#include <iostream>

int main() {
    auto source = std::make_shared<dk3::Source>(dk3::Source{"lexer.k3", "let answer = 9_007_199_254_740_993\nvar bytes = b\"a\\0b\"\n"});
    auto tokens = dk3::lex(source);
    assert(tokens[0].text == "let");
    assert(tokens[3].kind == dk3::TokenKind::Integer);
    assert(tokens[3].text == "9_007_199_254_740_993");
    assert(tokens[4].kind == dk3::TokenKind::Newline);
    assert(tokens[8].kind == dk3::TokenKind::Bytes && tokens[8].text.size() == 3);
    assert(tokens[5].span.line == 2);
    assert(dk3::utf8Offsets("a\xc3\xa9", tokens[0].span).size() == 3);
    bool rejected = false;
    try { dk3::utf8Offsets("\xff", tokens[0].span); } catch (const dk3::Error &) { rejected = true; }
    assert(rejected);
    auto diagnosticSource = std::make_shared<dk3::Source>(dk3::Source{"context.k3", "fn fail() {\r\n\tSystem.print(missing)\r\n}\r\n"});
    size_t offset = diagnosticSource->text.find("missing");
    dk3::Error error({diagnosticSource, offset, 7, 2, 15}, "Undeclared name 'missing'", "name");
    error.trace.push_back({"fail", error.span});
    std::string plain = dk3::diagnose(error), colored = dk3::diagnose(error, true);
    assert(plain.find("NameError: Undeclared name 'missing'") != std::string::npos);
    assert(plain.find("1 | fn fail() {") != std::string::npos && plain.find("2 |     System.print(missing)") != std::string::npos && plain.find("3 | }") != std::string::npos);
    assert(plain.find("|                  ^~~~~~~") != std::string::npos && plain.find("Call stack:\n  fail()") != std::string::npos);
    assert(plain.find('\x1b') == std::string::npos && colored.find("\x1b[1;31m") != std::string::npos);
    auto empty = std::make_shared<dk3::Source>(dk3::Source{"empty.k3", ""});
    assert(dk3::diagnose(dk3::Error({empty, 0, 0, 1, 1}, "Expected expression")).find("1 |") != std::string::npos);
    assert(dk3::diagnose(dk3::Error({}, "Host failure", "native")).find("<host>") != std::string::npos);
    auto longLine = std::make_shared<dk3::Source>(dk3::Source{"long.k3", std::string(512, ' ') + "missing"});
    assert(dk3::diagnose(dk3::Error({longLine, 512, 7, 1, 513}, "Missing name", "name")).find(longLine->text) != std::string::npos);
    auto unicode = std::make_shared<dk3::Source>(dk3::Source{"unicode.k3", "let label = '\xc3\xa9'; missing"});
    size_t unicodeOffset = unicode->text.find("missing");
    std::string unicodeDiagnostic = dk3::diagnose(dk3::Error({unicode, unicodeOffset, 7, 1, 19}, "Missing name", "name"));
    assert(unicodeDiagnostic.find("| " + std::string(unicodeOffset - 1, ' ') + "^~~~~~~") != std::string::npos);
    std::cout << "lexer: PASS\n";
}