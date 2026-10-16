#include "syntax.hpp"
#include <algorithm>

namespace dk3 {
std::vector<std::string> typeIntersections(const std::string &type) {
    std::string text = type;
    if (!text.empty() && text.back() == '?') text.pop_back();
    while (text.size() > 1 && text.front() == '(' && text.back() == ')') {
        int depth = 0; bool wrapped = true;
        for (size_t position = 0; position + 1 < text.size(); position++) {
            if (text[position] == '(') depth++;
            if (text[position] == ')') depth--;
            if (!depth) { wrapped = false; break; }
        }
        if (!wrapped) break;
        text = text.substr(1, text.size() - 2);
    }
    std::vector<std::string> parts; size_t start = 0; int depth = 0;
    for (size_t position = 0; position < text.size(); position++) {
        if (text[position] == '[' || text[position] == '(') depth++;
        else if (text[position] == ']' || text[position] == ')') depth--;
        else if (text[position] == '&' && !depth) { parts.push_back(text.substr(start, position - start)); start = position + 1; }
    }
    if (parts.empty()) return {};
    parts.push_back(text.substr(start)); return parts;
}
std::vector<std::string> typeArguments(const std::string &type) {
    size_t open = type.find('['), close = type.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close <= open) return {};
    std::vector<std::string> arguments; size_t start = open + 1; int depth = 0;
    for (size_t position = start; position < close; position++) {
        if (type[position] == '[' || type[position] == '(') depth++;
        else if (type[position] == ']' || type[position] == ')') depth--;
        else if (type[position] == ',' && !depth) { arguments.push_back(type.substr(start, position - start)); start = position + 1; }
    }
    arguments.push_back(type.substr(start, close - start)); return arguments;
}
std::string typeText(const TypeAst &type) {
    if (!type) return {};
    auto join = [](const std::vector<TypeAst> &items, const char *separator, bool wrapAlternatives) {
        std::string result;
        for (size_t index = 0; index < items.size(); index++) {
            if (index) result += separator;
            bool wrap = wrapAlternatives && (items[index]->kind == TypeKind::Union || items[index]->kind == TypeKind::Function);
            result += wrap ? '(' + typeText(items[index]) + ')' : typeText(items[index]);
        }
        return result;
    };
    switch (type->kind) {
    case TypeKind::Named: return type->items.empty() ? type->name : type->name + '[' + join(type->items, ",", false) + ']';
    case TypeKind::Nullable: {
        bool wrap = type->inner->kind == TypeKind::Union || type->inner->kind == TypeKind::Function;
        return (wrap ? '(' + typeText(type->inner) + ')' : typeText(type->inner)) + '?';
    }
    case TypeKind::Union: return join(type->items, "|", false);
    case TypeKind::Intersection: return '(' + join(type->items, "&", true) + ')';
    case TypeKind::Function: return "fn(" + join(type->items, ",", false) + ")->" + typeText(type->inner);
    case TypeKind::Wildcard:
        if (type->bound == WildcardBound::None) return "?";
        return std::string("? ") + (type->bound == WildcardBound::Extends ? "extends " : "super ") + typeText(type->inner);
    }
    return {};
}
namespace {
class Parser {
    std::vector<Token> tokens;
    size_t position = 0;
    bool speculativeTypes = false;
    const Token &peek(size_t distance = 0) const { return tokens.at(std::min(position + distance, tokens.size() - 1)); }
    bool at(const std::string &text) const { return (peek().kind == TokenKind::Name || peek().kind == TokenKind::Symbol) && peek().text == text; }
    bool take(const std::string &text) { if (!at(text)) return false; position++; return true; }
    Token consume(const std::string &text) {
        if (!at(text)) throw Error(peek().span, "Expected '" + text + "'");
        return tokens[position++];
    }
    Token name() {
        if (peek().kind != TokenKind::Name) throw Error(peek().span, "Expected identifier");
        for (const char *keyword : {"let", "var", "fn", "if", "else", "while", "for", "in", "return", "yield", "throw", "defer",
                        "break", "continue", "try", "catch", "finally", "import", "from", "as", "export", "record", "struct", "class", "trait", "impl", "implements", "enum", "match",
                        "true", "false", "null", "and", "or", "not", "is", "static", "extends", "super", "constructor", "public", "protected", "private"})
            if (peek().text == keyword) throw Error(peek().span, "Reserved word cannot be an identifier");
        return tokens[position++];
    }
    void newlines() { while (peek().kind == TokenKind::Newline) position++; }
    void separators() { while (peek().kind == TokenKind::Newline || at(";")) position++; }
    std::string docstring() {
        if (peek().kind != TokenKind::String) return {};
        std::string text = tokens[position++].text; separators(); return text;
    }
    Ast node(std::string kind, Span span, std::string text = "") {
        auto result = std::make_shared<Node>();
        result->kind = std::move(kind); result->span = std::move(span); result->text = std::move(text);
        return result;
    }
    std::string typeName(TypeAst *out = nullptr) {
        TypeAst type = typeExpr(); if (out) *out = type;
        return typeText(type);
    }
    static std::shared_ptr<TypeExpr> typeNode(TypeKind kind, const Span &span) {
        auto result = std::make_shared<TypeExpr>(); result->kind = kind; result->span = span; return result;
    }
    void closeAngle() {
        if (take(">")) return;
        if (!speculativeTypes && at(">=")) { tokens[position].text = "="; tokens[position].span.offset++; tokens[position].span.column++; return; }
        throw Error(peek().span, "Expected '>'");
    }
    TypeAst typeArgument() {
        if (!at("?")) return typeExpr();
        auto wildcard = typeNode(TypeKind::Wildcard, peek().span); position++;
        if (take("extends")) { wildcard->bound = WildcardBound::Extends; wildcard->inner = typeExpr(); }
        else if (take("super")) { wildcard->bound = WildcardBound::Super; wildcard->inner = typeExpr(); }
        return wildcard;
    }
    std::vector<TypeAst> typeArgumentList() {
        std::vector<TypeAst> arguments;
        do { arguments.push_back(typeArgument()); } while (take(","));
        closeAngle(); return arguments;
    }
    TypeAst typeAtom() {
        Span span = peek().span; TypeAst base;
        if (take("(")) { base = typeExpr(); consume(")"); }
        else if (at("fn") && peek(1).text == "(") {
            position += 2; auto function = typeNode(TypeKind::Function, span);
            if (!at(")")) do { function->items.push_back(typeExpr()); } while (take(","));
            consume(")"); consume("->"); function->inner = typeExpr(); base = function;
        } else {
            auto named = typeNode(TypeKind::Named, span); named->name = name().text;
            while (take(".")) named->name += '.' + name().text;
            if (take("[")) {
                do { named->items.push_back(typeExpr()); } while (take(","));
                consume("]");
            } else if (take("<")) named->items = typeArgumentList();
            base = named;
        }
        if (!take("?")) return base;
        auto nullable = typeNode(TypeKind::Nullable, span); nullable->inner = base; return nullable;
    }
    TypeAst typeIntersection() {
        Span span = peek().span; std::vector<TypeAst> members{typeAtom()};
        while (take("&")) { newlines(); members.push_back(typeAtom()); }
        if (members.size() == 1) return members[0];
        auto result = typeNode(TypeKind::Intersection, span); result->items = std::move(members); return result;
    }
    TypeAst typeExpr() {
        Span span = peek().span; std::vector<TypeAst> members{typeIntersection()};
        while (take("|")) { newlines(); members.push_back(typeIntersection()); }
        if (members.size() == 1) return members[0];
        auto result = typeNode(TypeKind::Union, span); result->items = std::move(members); return result;
    }
    std::vector<TypeParam> typeParameters() {
        std::vector<TypeParam> result;
        if (!take("<")) return result;
        do {
            Token identifier = name(); TypeParam parameter; parameter.name = identifier.text;
            if (take("extends")) parameter.bound = typeExpr();
            for (const auto &previous : result) if (previous.name == parameter.name) throw Error(identifier.span, "Duplicate type parameter");
            result.push_back(std::move(parameter));
        } while (take(","));
        closeAngle(); return result;
    }
    std::vector<Parameter> parameters() {
        std::vector<Parameter> result;
        consume("("); newlines();
        if (!at(")")) do {
            newlines(); Parameter parameter;
            parameter.rest = take("*");
            Token identifier = name(); parameter.name = identifier.text;
            if (take(":")) parameter.type = typeName(&parameter.typeExpr);
            if (take("=")) parameter.defaultValue = expression();
            for (const auto &previous : result)
                if (previous.name == parameter.name) throw Error(identifier.span, "Duplicate parameter");
            if (!result.empty() && result.back().rest) throw Error(identifier.span, "Rest parameter must be last");
            if (parameter.rest && parameter.defaultValue) throw Error(identifier.span, "Rest parameter cannot have a default");
            result.push_back(std::move(parameter)); newlines();
            if (at(")")) break;
        } while (take(","));
        consume(")"); return result;
    }
    Ast function(const Span &span, std::string identifier = "") {
        Ast result = node("function", span, std::move(identifier));
        result->typeParams = typeParameters();
        result->parameters = parameters();
        if (take("->")) result->type = typeName(&result->typeExpr);
        if (take("=>")) {
            Ast body = node("block", span), returned = node("return", span);
            body->needsScope = false;
            returned->children.push_back(expression()); body->children.push_back(returned);
            result->children.push_back(body);
        } else {
            Ast body = block();
            if (!body->children.empty()) {
                Ast first = body->children.front();
                if (first->kind == "expression" && first->children[0]->kind == "literal" && first->children[0]->literalKind == TokenKind::String) {
                    result->documentation = first->children[0]->text; body->children.erase(body->children.begin());
                }
            }
            result->children.push_back(body);
        }
        const auto &last = tokens[position - 1]; result->endOffset = last.span.offset + last.span.length;
        return result;
    }
    Ast block() {
        newlines(); Token start = consume("{");
        Ast result = node("block", start.span); result->needsScope = false; separators();
        while (!at("}")) {
            if (peek().kind == TokenKind::End) throw Error(peek().span, "Unclosed block");
            result->children.push_back(statement());
            const std::string &kind = result->children.back()->kind;
            if (kind == "binding" || kind == "function" || kind == "class" || kind == "record" || kind == "trait" || kind == "enum" || kind == "import" || kind == "from_import") result->needsScope = true;
            if (!at("}") && peek().kind != TokenKind::Newline && !at(";"))
                throw Error(peek().span, "Expected newline or ';' after statement");
            separators();
        }
        consume("}"); return result;
    }
    Ast statement() {
        Token start = peek(); bool exported = take("export"); Ast result;
        if (take("let") || take("var")) {
            bool mutableBinding = tokens[position - 1].text == "var";
            result = node("binding", start.span, name().text); result->mutableBinding = mutableBinding;
            if (take(":")) result->type = typeName(&result->typeExpr);
            consume("="); result->children.push_back(expression());
        } else if (at("fn") && peek(1).kind == TokenKind::Name) {
            consume("fn"); result = function(start.span, name().text);
        } else if (at("type") && peek(1).kind == TokenKind::Name && (peek(2).text == "<" || peek(2).text == "=")) {
            consume("type"); result = node("type_alias", start.span, name().text);
            result->typeParams = typeParameters(); consume("=");
            result->type = typeName(&result->typeExpr);
        }
        else if (take("record") || take("class")) {
            bool classDeclaration = tokens[position - 1].text == "class";
            result = node(classDeclaration ? "class" : "record", start.span, name().text);
            result->typeParams = typeParameters();
            if (take("extends")) {
                if (!classDeclaration) throw Error(start.span, "Only classes support inheritance");
                result->baseClass = typeName(&result->typeExpr);
                if (result->typeExpr->kind != TypeKind::Named) throw Error(start.span, "extends requires one qualified class name");
            }
            if (take("implements")) do {
                newlines(); TypeAst traitType; std::string trait = typeName(&traitType);
                if (traitType->kind != TypeKind::Named) throw Error(start.span, "implements requires plain qualified trait names");
                if (std::find(result->names.begin(), result->names.end(), trait) != result->names.end())
                    throw Error(start.span, "Duplicate implemented trait '" + trait + "'");
                result->names.push_back(trait); result->typeExprs.push_back(traitType); newlines();
            } while (take(","));
            consume("{"); separators();
            result->documentation = docstring();
            std::vector<std::string> members;
            while (!at("}")) {
                Token location = peek(); std::string identifier;
                Visibility visibility = Visibility::Public;
                bool explicitAccess = at("public") || at("protected") || at("private");
                if (take("private")) visibility = Visibility::Private;
                else if (take("protected")) visibility = Visibility::Protected;
                else take("public");
                if (explicitAccess && !classDeclaration) throw Error(location.span, "Access modifiers require a class");
                bool staticMember = take("static");
                if (staticMember && !classDeclaration) throw Error(location.span, "Static members require a class");
                if (take("fn") || take("constructor")) {
                    bool constructor = tokens[position - 1].text == "constructor";
                    if (constructor && (!classDeclaration || staticMember)) throw Error(location.span, "Constructors require an instance class");
                    identifier = constructor ? "init" : name().text;
                    Ast method = function(location.span, identifier);
                    method->constructor = constructor;
                    method->staticMember = staticMember;
                    method->visibility = visibility;
                    if (staticMember && identifier == "init") throw Error(location.span, "init is an instance lifecycle hook");
                    for (const auto &parameter : method->parameters)
                        if (parameter.name == "self") throw Error(location.span, "self is an implicit method binding");
                    result->children.push_back(method);
                } else if (staticMember) {
                    bool mutableMember = take("var"); if (!mutableMember) consume("let");
                    identifier = name().text;
                    Ast field = node("binding", location.span, identifier); field->staticMember = true; field->mutableBinding = mutableMember;
                    field->visibility = visibility;
                    consume(":"); field->type = typeName(&field->typeExpr); consume("="); field->children.push_back(expression());
                    result->children.push_back(field);
                } else {
                    Parameter field;
                    field.visibility = visibility;
                    if (classDeclaration) {
                        if (take("var")) field.mutableBinding = true;
                        else consume("let");
                    }
                    identifier = field.name = name().text; consume(":"); field.type = typeName(&field.typeExpr);
                    if (take("=")) field.defaultValue = expression();
                    result->parameters.push_back(field);
                }
                if (identifier == "self" || std::find(members.begin(), members.end(), identifier) != members.end())
                    throw Error(location.span, "Duplicate or reserved class member '" + identifier + "'");
                members.push_back(identifier);
                if (!take(",") && !at("}") && peek().kind != TokenKind::Newline && !at(";"))
                    throw Error(peek().span, "Expected class member separator");
                separators();
            }
            consume("}");
        } else if (take("trait")) {
            result = node("trait", start.span, name().text); result->typeParams = typeParameters(); consume("{"); separators();
            result->documentation = docstring();
            while (!at("}")) {
                Token location = consume("fn"); Ast signature = node("signature", location.span, name().text);
                if (signature->text == "self" || signature->text == "init") throw Error(location.span, "Reserved trait lifecycle member");
                signature->typeParams = typeParameters();
                signature->parameters = parameters(); consume("->"); signature->type = typeName(&signature->typeExpr);
                for (const auto &parameter : signature->parameters)
                    if (parameter.name == "self" || parameter.type.empty() || parameter.rest || parameter.defaultValue)
                        throw Error(location.span, "Trait parameters require explicit types, without self, defaults or rest");
                if (at("{")) {
                    signature->kind = "function"; Ast body = block();
                    if (!body->children.empty()) {
                        Ast first = body->children[0];
                        if (first->kind == "expression" && first->children[0]->kind == "literal" && first->children[0]->literalKind == TokenKind::String) {
                            signature->documentation = first->children[0]->text; body->children.erase(body->children.begin());
                        }
                    }
                    signature->children.push_back(body);
                }
                for (const auto &previous : result->children) if (previous->text == signature->text)
                    throw Error(location.span, "Duplicate trait method");
                result->children.push_back(signature);
                if (!at("}") && peek().kind != TokenKind::Newline && !at(";")) throw Error(peek().span, "Expected trait signature separator");
                separators();
            }
            if (result->children.empty()) throw Error(start.span, "Trait requires at least one method");
            consume("}");
        } else if (take("impl")) {
            result = node("impl", start.span, typeName()); consume("for"); result->names.push_back(typeName());
            if (result->text.find_first_of("[?&(") != std::string::npos || result->names[0].find_first_of("[?&(") != std::string::npos)
                throw Error(start.span, "Trait implementations require plain qualified type names");
            consume("{"); separators();
            while (!at("}")) {
                Token location = consume("fn"); Ast method = function(location.span, name().text);
                for (const auto &parameter : method->parameters) if (parameter.name == "self")
                    throw Error(location.span, "self is an implicit method binding");
                if (method->text == "self") throw Error(location.span, "Reserved implementation member self");
                for (const auto &previous : result->children) if (previous->text == method->text)
                    throw Error(location.span, "Duplicate implementation method");
                result->children.push_back(method);
                if (!at("}") && peek().kind != TokenKind::Newline && !at(";")) throw Error(peek().span, "Expected implementation method separator");
                separators();
            }
            consume("}");
        } else if (take("enum")) {
            result = node("enum", start.span, name().text); result->typeParams = typeParameters(); consume("{"); separators();
            result->documentation = docstring();
            while (!at("}")) {
                Ast variant = node("variant", peek().span, name().text);
                if (take("(")) {
                    if (!at(")")) do { TypeAst payload; variant->names.push_back(typeName(&payload)); variant->typeExprs.push_back(payload); } while (take(","));
                    consume(")");
                }
                for (const auto &previous : result->children)
                    if (previous->text == variant->text) throw Error(variant->span, "Duplicate enum variant");
                result->children.push_back(variant);
                if (!take(",") && !at("}") && peek().kind != TokenKind::Newline) throw Error(peek().span, "Expected variant separator");
                separators();
            }
            consume("}");
        } else if (take("if")) {
            result = node("if", start.span); result->children.push_back(expression()); result->children.push_back(block());
            size_t saved = position; newlines();
            if (take("else")) result->children.push_back(at("if") ? statement() : block()); else position = saved;
        } else if (take("while")) {
            result = node("while", start.span); result->children.push_back(expression()); result->children.push_back(block());
        } else if (take("for")) {
            if (take("(")) {
                result = node("for_c", start.span); result->children.resize(4);
                if (!at(";")) {
                    result->children[0] = statement();
                    if (result->children[0]->kind != "binding" && result->children[0]->kind != "expression") throw Error(start.span, "for initializer must be a binding or expression");
                }
                consume(";");
                if (!at(";")) result->children[1] = expression();
                else { result->children[1] = node("literal", peek().span, "true"); result->children[1]->literalKind = TokenKind::Name; }
                consume(";");
                if (!at(")")) {
                    result->children[2] = statement();
                    if (result->children[2]->kind != "expression") throw Error(start.span, "for increment must be an expression");
                }
                consume(")"); result->children[3] = block();
            } else {
                result = node("for", start.span, name().text); consume("in");
                result->children.push_back(expression()); result->children.push_back(block());
            }
        } else if (take("return") || take("yield")) {
            result = node(tokens[position - 1].text, start.span);
            if (peek().kind != TokenKind::Newline && !at(";") && !at("}") && peek().kind != TokenKind::End)
                result->children.push_back(expression());
        } else if (take("throw") || take("defer")) {
            result = node(tokens[position - 1].text, start.span); result->children.push_back(expression());
        } else if (take("break") || take("continue")) result = node(tokens[position - 1].text, start.span);
        else if (take("try")) {
            result = node("try", start.span); result->children.push_back(block()); newlines(); consume("catch");
            if (take("(")) { result->text = name().text; consume(")"); } else result->text = name().text;
            result->children.push_back(block()); size_t saved = position; newlines();
            if (take("finally")) result->children.push_back(block()); else position = saved;
        } else if (take("from")) {
            result = node("from_import", start.span);
            if (peek().kind == TokenKind::String) result->text = tokens[position++].text;
            else { result->text = name().text; while (take(".")) result->text += '.' + name().text; }
            consume("import");
            do {
                std::string imported = name().text, alias = imported;
                if (take("as")) alias = name().text;
                result->names.push_back(imported); result->names.push_back(alias);
            } while (take(","));
        } else if (take("import")) {
            result = node("import", start.span);
            if (peek().kind == TokenKind::String) result->text = tokens[position++].text;
            else { result->text = name().text; while (take(".")) result->text += '.' + name().text; }
            std::string alias = result->text.substr(result->text.find_last_of("./") + 1);
            if (take("as")) alias = name().text;
            result->names.push_back(alias);
        } else if (at("{")) result = block();
        else {
            result = node("expression", start.span); Ast left = expression();
            if (at("++") || at("--")) {
                Token operation = tokens[position++];
                if (left->kind != "name" && left->kind != "index" && left->kind != "member") throw Error(left->span, "Invalid increment target");
                Ast assignment = node("assignment", operation.span, operation.text == "++" ? "+=" : "-=");
                Ast one = node("literal", operation.span, "1"); one->literalKind = TokenKind::Integer;
                assignment->children = {left, one}; left = assignment;
            } else if (at("=") || at("+=") || at("-=") || at("*=") || at("/=")) {
                Token operation = tokens[position++]; Ast assignment = node("assignment", operation.span, operation.text);
                if (left->kind != "name" && left->kind != "index" && left->kind != "member") throw Error(left->span, "Invalid assignment target");
                assignment->children = {left, expression()}; left = assignment;
            }
            result->children.push_back(left);
        }
        if (exported && result->kind != "binding" && result->kind != "function" && result->kind != "record" && result->kind != "class" && result->kind != "trait" && result->kind != "enum" && result->kind != "type_alias")
            throw Error(start.span, "Only declarations can be exported");
        result->exported = exported; return result;
    }
    int precedence(const std::string &operation) const {
        if (operation == "??") return 1;
        if (operation == "||" || operation == "or") return 2;
        if (operation == "&&" || operation == "and") return 3;
        if (operation == "==" || operation == "!=") return 4;
        if (operation == "<" || operation == ">" || operation == "<=" || operation == ">=" || operation == "is") return 5;
        if (operation == "+" || operation == "-") return 6;
        if (operation == "*" || operation == "/" || operation == "//" || operation == "%") return 7;
        if (operation == "**") return 8;
        return 0;
    }
    Ast expression(int minimum = 1) {
        Ast left = prefix();
        for (;;) {
            int priority = peek().kind == TokenKind::Name || peek().kind == TokenKind::Symbol ? precedence(peek().text) : 0;
            if (priority < minimum) break;
            Token operation = tokens[position++];
            if (operation.text == "is") {
                Ast test = node("is", operation.span, "is"); test->children = {left};
                test->type = typeName(&test->typeExpr); left = test; continue;
            }
            newlines();
            Ast binary = node("binary", operation.span, operation.text);
            binary->children = {left, expression(operation.text == "**" ? priority : priority + 1)}; left = binary;
        }
        return left;
    }
    Ast prefix() {
        Token token = peek(); Ast result;
        if (take("-") || take("+") || take("!") || take("not")) {
            result = node("unary", token.span, token.text); result->children.push_back(expression(8)); return result;
        }
        if (token.kind == TokenKind::FormatString) {
            position++; result = node("format", token.span); std::string literal;
            auto flush = [&] { Ast part = node("literal", token.span, literal); part->literalKind = TokenKind::String; result->children.push_back(part); literal.clear(); };
            for (size_t offset = 0; offset < token.text.size();) {
                char byte = token.text[offset++];
                if ((byte == '{' || byte == '}') && offset < token.text.size() && token.text[offset] == byte) { literal += byte; offset++; continue; }
                if (byte == '}') throw Error(token.span, "Unmatched formatted-string brace");
                if (byte != '{') { literal += byte; continue; }
                flush(); size_t start = offset, depth = 1; char quote = 0; bool escaped = false;
                for (; offset < token.text.size() && depth; offset++) {
                    char current = token.text[offset];
                    if (quote) { if (escaped) escaped = false; else if (current == '\\') escaped = true; else if (current == quote) quote = 0; }
                    else if (current == '\'' || current == '"') quote = current;
                    else if (current == '{') depth++;
                    else if (current == '}') depth--;
                }
                if (depth || offset == start + 1) throw Error(token.span, "Empty or unclosed formatted-string expression");
                Parser embedded(std::make_shared<Source>(Source{token.span.source->file, token.text.substr(start, offset - start - 1)}));
                for (auto &entry : embedded.tokens) entry.span = token.span;
                Ast expression = embedded.expression(); embedded.newlines();
                if (embedded.peek().kind != TokenKind::End) throw Error(token.span, "Formatted-string replacement must be one expression");
                result->children.push_back(expression);
            }
            flush();
        } else if (token.kind == TokenKind::Integer || token.kind == TokenKind::Float || token.kind == TokenKind::String || token.kind == TokenKind::Bytes ||
            at("true") || at("false") || at("null")) {
            position++; result = node("literal", token.span, token.text); result->literalKind = token.kind;
        } else if (take("fn")) result = function(token.span);
        else if (take("match")) {
            result = node("match", token.span); result->children.push_back(expression()); consume("{"); separators();
            while (!at("}")) {
                Ast arm = node("arm", peek().span, name().text);
                while (take(".")) arm->text += '.' + name().text;
                if (take("(")) { if (!at(")")) do { arm->names.push_back(name().text); } while (take(",")); consume(")"); }
                consume("=>"); arm->children.push_back(expression()); result->children.push_back(arm);
                if (!take(",") && !at("}") && peek().kind != TokenKind::Newline) throw Error(peek().span, "Expected match arm separator");
                separators();
            }
            consume("}");
        } else if (take("[")) {
            result = node("list", token.span); newlines();
            if (!at("]")) do { newlines(); if (at("]")) break; result->children.push_back(expression()); newlines(); } while (take(","));
            consume("]");
        } else if (take("{")) {
            result = node("map", token.span); newlines();
            if (!at("}")) do {
                newlines(); if (at("}")) break;
                result->children.push_back(expression()); consume(":"); result->children.push_back(expression()); newlines();
            } while (take(","));
            consume("}");
        } else if (take("(")) {
            newlines();
            if (take(")")) result = node("tuple", token.span);
            else {
                Ast first = expression(); newlines();
                if (take(",")) {
                    result = node("tuple", token.span); result->children.push_back(first);
                    while (!at(")")) { newlines(); result->children.push_back(expression()); newlines(); if (!take(",")) break; }
                } else result = first;
                consume(")");
            }
        } else if (token.kind == TokenKind::Name) {
            position++; result = node("name", token.span, token.text);
            if (at("<")) {
                size_t saved = position; position++; speculativeTypes = true;
                try {
                    auto arguments = typeArgumentList(); speculativeTypes = false;
                    if (at("(") || at(".")) result->typeArgs = std::move(arguments); else position = saved;
                } catch (const Error &) { speculativeTypes = false; position = saved; }
            }
        }
        else throw Error(token.span, "Expected expression");
        for (;;) {
            if (take("(")) {
                Ast call = node("call", result->span); call->children.push_back(result); newlines(); bool named = false;
                if (!at(")")) do {
                    newlines(); if (at(")")) break; std::string argument;
                    if (peek().kind == TokenKind::Name && peek(1).text == ":") {
                        argument = name().text; consume(":"); named = true;
                        if (std::find(call->names.begin(), call->names.end(), argument) != call->names.end()) throw Error(peek().span, "Duplicate named argument");
                    } else if (named) throw Error(peek().span, "Positional argument after named arguments");
                    call->names.push_back(argument); call->children.push_back(expression()); newlines();
                } while (take(","));
                consume(")"); result = call;
            } else if (take(".")) {
                Token member = name(); Ast access = node("member", member.span, member.text); access->children.push_back(result); result = access;
            } else if (take("[")) {
                Ast first; if (!at(":")) first = expression();
                if (take(":")) {
                    Ast slice = node("slice", result->span); slice->children = {result, first, nullptr, nullptr};
                    if (!at(":") && !at("]")) slice->children[2] = expression();
                    if (take(":")) { if (!at("]")) slice->children[3] = expression(); }
                    result = slice;
                } else { Ast index = node("index", result->span); index->children = {result, first}; result = index; }
                consume("]");
            } else if (take("?.")) {
                Token member = name(); Ast access = node("member", member.span, member.text);
                access->safe = true; access->children.push_back(result); result = access;
            } else if (at("!") && peek().kind == TokenKind::Symbol) {
                Token operation = tokens[position++]; Ast assertion = node("nonnull", operation.span, "!");
                assertion->children.push_back(result); result = assertion;
            } else if (take("as")) {
                Ast cast = node("cast", tokens[position - 1].span, "as"); cast->children.push_back(result);
                cast->type = typeName(&cast->typeExpr); result = cast;
            } else break;
        }
        return result;
    }
public:
    explicit Parser(std::shared_ptr<const Source> source) : tokens(lex(std::move(source))) {}
    Ast run() {
        Ast result = node("block", peek().span); separators();
        while (peek().kind != TokenKind::End) {
            result->children.push_back(statement());
            if (peek().kind != TokenKind::End && peek().kind != TokenKind::Newline && !at(";")) throw Error(peek().span, "Expected newline or ';'");
            separators();
        }
        return result;
    }
};
}
Ast parse(std::shared_ptr<const Source> source) { return Parser(std::move(source)).run(); }
}