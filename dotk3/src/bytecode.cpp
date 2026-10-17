#include "bytecode.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>

namespace dk3 {
namespace {
void collectNames(const Ast &node, std::vector<std::string> &referenced, std::set<std::string> &declared) {
    if (!node) return;
    if (node->kind == "name") referenced.push_back(node->text);
    if (node->kind == "binding" || node->kind == "for" || node->kind == "try") declared.insert(node->text);
    if (node->kind == "arm") for (const auto &name : node->names) declared.insert(name);
    for (const auto &parameter : node->parameters) { declared.insert(parameter.name); collectNames(parameter.defaultValue, referenced, declared); }
    for (const auto &child : node->children) collectNames(child, referenced, declared);
}

class Compiler {
    Bytecode code;
    bool valid = true;
    bool lexical = false;
    bool slotClosures = false;
    std::vector<std::unordered_map<std::string, size_t>> scopes{1};
    std::unordered_map<std::string, size_t> names;
    struct Loop { size_t start, depth; std::vector<size_t> breaks; std::vector<size_t> continues = {}; };
    std::vector<Loop> loops;
    size_t emit(ByteOp op, const Ast &node, size_t operand = 0) {
        code.instructions.push_back({op, node, operand}); return code.instructions.size() - 1;
    }
    size_t local(const std::string &name) {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            auto found = scope->find(name); if (found != scope->end()) return found->second;
        }
        return std::numeric_limits<size_t>::max();
    }
    size_t name(const std::string &value) {
        auto found = names.find(value); if (found != names.end()) return found->second;
        size_t index = code.names.size(); code.names.push_back(value); names.emplace(value, index); return index;
    }
    void operation(const Ast &node, bool compound = false) {
        size_t position = emit(ByteOp::Binary, node, compound);
        if (!code.optimization) return;
        const std::string symbol = compound ? node->text.substr(0, 1) : node->text;
        using Math = ByteInstruction::Math;
        if (node->numeric == 2) {
            for (const auto &entry : std::vector<std::pair<std::string, Math>>{{"+", Math::AddF}, {"-", Math::SubtractF}, {"*", Math::MultiplyF}, {"/", Math::DivideF}, {"==", Math::EqualF}, {"!=", Math::NotEqualF}, {"<", Math::LessF}, {">", Math::GreaterF}, {"<=", Math::LessEqualF}, {">=", Math::GreaterEqualF}})
                if (entry.first == symbol) { code.instructions[position].math = entry.second; return; }
        } 
        for (const auto &entry : std::vector<std::pair<std::string, Math>>{{"+", Math::Add}, {"-", Math::Subtract}, {"*", Math::Multiply}, {"//", Math::FloorDivide}, {"%", Math::Modulo}, {"==", Math::Equal}, {"!=", Math::NotEqual}, {"<", Math::Less}, {">", Math::Greater}, {"<=", Math::LessEqual}, {">=", Math::GreaterEqual}})
            if (entry.first == symbol) { code.instructions[position].math = entry.second; return; }
    }
    void load(const Ast &node) {
        size_t slot = local(node->text);
        emit(slot == std::numeric_limits<size_t>::max() ? ByteOp::LoadName : ByteOp::LoadLocal, node, slot == std::numeric_limits<size_t>::max() ? name(node->text) : slot);
    }
    void clear(size_t depth, const Ast &node) {
        for (size_t index = scopes.size(); index > depth; index--)
            for (const auto &binding : scopes[index - 1]) emit(ByteOp::ClearLocal, node, binding.second);
    }
    void expression(const Ast &node) {
        if (node->widen && node->kind == "literal" && node->literalKind == TokenKind::Integer) {
            std::string digits = node->text; digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
            try { double value = static_cast<double>(std::stoll(digits, nullptr, 0)); size_t position = emit(ByteOp::Float, node); code.instructions[position].floating = value; return; }
            catch (const std::exception &) {}
        }
        expressionInner(node);
        if (node->widen) emit(ByteOp::IntToFloat, node);
    }
    // Typed (static) code counts steps per statement and loop iteration instead of per expression.
    bool ticksPerExpression() const { return !slotClosures || lexical; }
    void expressionInner(const Ast &node) {
        if (ticksPerExpression()) emit(ByteOp::Tick, node);
        if (node->kind == "literal") {
            if (node->literalKind == TokenKind::Integer) {
                std::string digits = node->text; digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                int64_t value = 0; auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) { emit(ByteOp::Literal, node); return; }
                size_t position = emit(ByteOp::Int, node); code.instructions[position].integer = value;
            } else if (node->literalKind == TokenKind::Float) {
                std::string digits = node->text; digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                try { double value = std::stod(digits); size_t position = emit(ByteOp::Float, node); code.instructions[position].floating = value; }
                catch (const std::exception &) { emit(ByteOp::Literal, node); }
            } else if (node->literalKind == TokenKind::String || node->literalKind == TokenKind::Bytes) emit(ByteOp::Text, node);
            else if (node->text == "null") emit(ByteOp::Null, node);
            else emit(ByteOp::Bool, node, node->text == "true");
        } else if (node->kind == "name") load(node);
        else if (node->kind == "binary") {
            expression(node->children[0]);
            if (node->text == "and" || node->text == "&&" || node->text == "or" || node->text == "||" || node->text == "??") {
                size_t branch = emit(node->text == "??" ? ByteOp::JumpPresent : node->text == "or" || node->text == "||" ? ByteOp::JumpTrue : ByteOp::JumpFalse, node);
                emit(ByteOp::Pop, node); expression(node->children[1]);
                if (node->text != "??") emit(ByteOp::Unary, node, 1);
                code.instructions[branch].operand = code.instructions.size();
            } else { expression(node->children[1]); operation(node); }
        } else if (node->kind == "unary") {
            if (node->text == "-" && node->children[0]->kind == "literal" && node->children[0]->literalKind == TokenKind::Integer) {
                std::string digits = node->children[0]->text; digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                uint64_t value = 0; auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || value > uint64_t{1} << 63) { emit(ByteOp::Literal, node, 1); return; }
                size_t position = emit(ByteOp::Int, node); code.instructions[position].integer = value == uint64_t{1} << 63 ? INT64_MIN : -static_cast<int64_t>(value);
            } else { expression(node->children[0]); emit(ByteOp::Unary, node); }
        } else if (node->kind == "member") {
            expression(node->children[0]);
            size_t skip = node->safe ? emit(ByteOp::JumpNull, node) : 0;
            emit(ByteOp::Member, node);
            if (node->safe) code.instructions[skip].operand = code.instructions.size();
        }
        else if (node->kind == "is") { expression(node->children[0]); emit(ByteOp::InstanceOf, node); }
        else if (node->kind == "cast") { expression(node->children[0]); emit(ByteOp::Cast, node); }
        else if (node->kind == "nonnull") { expression(node->children[0]); emit(ByteOp::NonNull, node); }
        else if (node->kind == "index") { expression(node->children[0]); expression(node->children[1]); emit(ByteOp::Index, node); }
        else if (node->kind == "call") {
            const auto &callee = node->children[0];
            bool positionalOnly = std::all_of(node->names.begin(), node->names.end(), [](const std::string &argument) { return argument.empty(); });
            if (slotClosures && !lexical && !node->target.empty() && positionalOnly) {
                if (node->target[0] == '.') expression(callee->children[0]);
                for (size_t index = 1; index < node->children.size(); index++) expression(node->children[index]);
                emit(node->target[0] == '.' ? ByteOp::CallMethod : ByteOp::CallNative, node, node->children.size() - 1);
            } else if (callee->kind == "member") {
                if (ticksPerExpression()) emit(ByteOp::Tick, callee);
                expression(callee->children[0]);
                size_t skip = callee->safe ? emit(ByteOp::JumpNull, callee) : 0;
                emit(ByteOp::ResolveMethod, callee);
                for (size_t index = 1; index < node->children.size(); index++) expression(node->children[index]);
                emit(ByteOp::InvokeMethod, node, node->children.size() - 1);
                if (callee->safe) code.instructions[skip].operand = code.instructions.size();
            } else {
                for (const auto &child : node->children) expression(child);
                emit(ByteOp::Call, node, node->children.size() - 1);
            }
        } else if (node->kind == "list" || node->kind == "tuple" || node->kind == "map") {
            for (const auto &child : node->children) expression(child);
            emit(node->kind == "list" ? ByteOp::List : node->kind == "tuple" ? ByteOp::Tuple : ByteOp::Map, node, node->children.size());
        } else if (node->kind == "function") {
            if (lexical) emit(ByteOp::Closure, node);
            else if (!closure(node)) { valid = false; return; }
        } else if (node->kind == "format") {
            emit(ByteOp::Text, node);
            for (const auto &part : node->children) { expression(part); emit(ByteOp::Format, part); }
        } else if (node->kind == "slice") {
            expression(node->children[0]); emit(ByteOp::PrepareSlice, node);
            for (size_t index = 1; index < 4; index++) { if (node->children[index]) expression(node->children[index]); else emit(ByteOp::Null, node); }
            size_t bounds = 0; for (size_t index = 1; index < 4; index++) if (node->children[index]) bounds |= size_t{1} << (index - 1);
            emit(ByteOp::Slice, node, bounds);
        } else if (node->kind == "match") {
            if (slotClosures && !lexical) { inlineMatch(node); return; }
            for (size_t index = 1; index < node->children.size(); index++) {
                std::vector<std::string> referenced; std::set<std::string> declared;
                collectNames(node->children[index]->children[0], referenced, declared);
                const auto &bound = node->children[index]->names;
                for (const auto &name : referenced)
                    if (local(name) != std::numeric_limits<size_t>::max() && std::find(bound.begin(), bound.end(), name) == bound.end()) { valid = false; return; }
            }
            expression(node->children[0]); ByteMatch match;
            for (size_t index = 1; index < node->children.size(); index++) {
                const auto &arm = node->children[index]; auto compiled = compileExpression(arm->children[0], code.optimization);
                if (!compiled) { valid = false; return; }
                match.arms.push_back({arm->text, arm->names, arm->span, std::move(compiled)});
            }
            size_t index = code.matches.size(); code.matches.push_back(std::move(match)); emit(ByteOp::Match, node, index);
        } else if (node->kind == "assignment") {
            const auto &target = node->children[0];
            if (target->kind == "name") {
                size_t slot = local(target->text);
                if (slot == std::numeric_limits<size_t>::max()) slot = name(target->text);
                bool external = local(target->text) == std::numeric_limits<size_t>::max();
                emit(external ? ByteOp::CheckName : ByteOp::CheckLocal, node, slot);
                if (node->text != "=") { expression(node->children[1]); load(target); emit(ByteOp::Swap, node); operation(node, true); }
                else expression(node->children[1]);
                emit(external ? ByteOp::StoreName : ByteOp::StoreLocal, node, slot);
            } else if (target->kind == "index") {
                expression(target->children[0]); emit(ByteOp::CheckObject, target); expression(target->children[1]); expression(node->children[1]); emit(ByteOp::SetIndex, node);
            } else if (target->kind == "member") {
                expression(target->children[0]); emit(ByteOp::PrepareMember, target, node->text != "="); expression(node->children[1]); emit(ByteOp::SetMember, node);
            } else valid = false;
        } else valid = false;
    }
    bool closure(const Ast &node) {
        if (!slotClosures || !node->text.empty()) return false;
        for (const auto &parameter : node->parameters) if (parameter.defaultValue || parameter.rest) return false;
        std::vector<std::string> referenced; std::set<std::string> declared;
        collectNames(node, referenced, declared);
        std::vector<std::string> captured; ByteClosure info; info.node = node;
        for (const auto &name : referenced) {
            size_t slot = local(name);
            if (declared.count(name) || slot == std::numeric_limits<size_t>::max() || std::find(captured.begin(), captured.end(), name) != captured.end()) continue;
            captured.push_back(name); info.slots.push_back(slot);
        }
        Compiler inner; inner.enableSlotClosures(true);
        info.code = inner.compileClosure(node, captured, code.optimization);
        if (!info.code) return false;
        code.closures.push_back(std::move(info));
        emit(ByteOp::MakeClosure, node, code.closures.size() - 1);
        return true;
    }
    void inlineMatch(const Ast &node) {
        expression(node->children[0]);
        std::vector<size_t> ends;
        for (size_t index = 1; index < node->children.size(); index++) {
            const auto &arm = node->children[index];
            size_t test = emit(ByteOp::MatchTest, arm);
            scopes.emplace_back(); size_t first = code.locals.size();
            for (const auto &bound : arm->names) { size_t slot = code.locals.size(); code.locals.push_back({bound, "", false}); scopes.back()[bound] = slot; }
            if (!arm->names.empty()) { size_t bind = emit(ByteOp::MatchBind, arm, arm->names.size()); code.instructions[bind].integer = static_cast<int64_t>(first); }
            expression(arm->children[0]);
            clear(scopes.size() - 1, arm); scopes.pop_back();
            emit(ByteOp::Swap, node); emit(ByteOp::Pop, node);
            ends.push_back(emit(ByteOp::Jump, node));
            code.instructions[test].operand = code.instructions.size();
        }
        emit(ByteOp::MatchFail, node);
        for (size_t jump : ends) code.instructions[jump].operand = code.instructions.size();
    }
    void forIn(const Ast &node) {
        scopes.emplace_back();
        expression(node->children[0]);
        size_t hidden = code.locals.size();
        code.locals.push_back({" iterable", "", false}); code.locals.push_back({" cursor", "", true});
        scopes.back()[" iterable" + std::to_string(hidden)] = hidden; scopes.back()[" cursor" + std::to_string(hidden)] = hidden + 1;
        size_t init = emit(ByteOp::IterInit, node); code.instructions[init].integer = static_cast<int64_t>(hidden);
        size_t begin = code.instructions.size(); emit(ByteOp::Point, node);
        size_t advance = emit(ByteOp::IterNext, node); code.instructions[advance].integer = static_cast<int64_t>(hidden);
        scopes.emplace_back();
        size_t slot = code.locals.size(); code.locals.push_back({node->text, "", false}); scopes.back()[node->text] = slot;
        emit(ByteOp::DeclareLocal, node, slot);
        loops.push_back({begin, scopes.size() - 1, {}});
        block(node->children[1], true);
        clear(scopes.size() - 1, node); scopes.pop_back();
        emit(ByteOp::SafePoint, node); emit(ByteOp::Jump, node, begin);
        code.instructions[advance].operand = code.instructions.size();
        for (size_t jump : loops.back().breaks) code.instructions[jump].operand = code.instructions.size();
        loops.pop_back(); clear(scopes.size() - 1, node); scopes.pop_back();
    }
    void block(const Ast &node, bool scope) {
        if (scope) scopes.emplace_back();
        for (const auto &child : node->children) statement(child);
        if (scope) { clear(scopes.size() - 1, node); scopes.pop_back(); }
    }
    void statement(const Ast &node) {
        if (node->kind == "block") { emit(ByteOp::Tick, node); block(node, true); return; }
        if (node->kind != "while") emit(ByteOp::Point, node);
        if (node->kind == "expression") { expression(node->children[0]); emit(ByteOp::Pop, node); }
        else if (node->kind == "binding") {
            expression(node->children[0]);
            if (lexical || (code.program && scopes.size() == 1)) emit(ByteOp::DeclareName, node);
            else {
                if (scopes.back().count(node->text)) { valid = false; return; }
                size_t slot = code.locals.size(); code.locals.push_back({node->text, node->type, node->mutableBinding}); scopes.back()[node->text] = slot;
                emit(ByteOp::DeclareLocal, node, slot);
            }
        } else if (node->kind == "function" && (lexical || (code.program && scopes.size() == 1))) emit(ByteOp::DefineFunction, node);
        else if (node->kind == "type_alias") {}
        else if ((lexical || (code.program && scopes.size() == 1)) && (node->kind == "class" || node->kind == "record" || node->kind == "trait" || node->kind == "impl" || node->kind == "enum")) emit(ByteOp::DeclareType, node);
        else if ((lexical || (code.program && scopes.size() == 1)) && (node->kind == "import" || node->kind == "from_import")) emit(ByteOp::Import, node);
        else if ((lexical || slotClosures) && node->kind == "throw") { expression(node->children[0]); emit(ByteOp::Throw, node); }
        else if (slotClosures && node->kind == "for") forIn(node);
        else if (node->kind == "return" && !code.program) {
            if (node->children.empty()) emit(ByteOp::Null, node); else expression(node->children[0]);
            emit(ByteOp::Return, node);
        } else if (node->kind == "if") {
            expression(node->children[0]); size_t branch = emit(ByteOp::JumpFalse, node); emit(ByteOp::Pop, node);
            if (node->children[1]->kind == "block") block(node->children[1], true); else statement(node->children[1]);
            size_t end = emit(ByteOp::Jump, node); code.instructions[branch].operand = code.instructions.size(); emit(ByteOp::Pop, node);
            if (node->children.size() > 2) { if (node->children[2]->kind == "block") block(node->children[2], true); else statement(node->children[2]); }
            code.instructions[end].operand = code.instructions.size();
        } else if (node->kind == "while") {
            emit(ByteOp::Tick, node); size_t begin = code.instructions.size(); emit(ByteOp::Point, node); expression(node->children[0]);
            size_t branch = emit(ByteOp::JumpFalse, node); emit(ByteOp::Pop, node); loops.push_back({begin, scopes.size(), {}});
            block(node->children[1], true); emit(ByteOp::SafePoint, node); emit(ByteOp::Jump, node, begin);
            code.instructions[branch].operand = code.instructions.size(); emit(ByteOp::Pop, node);
            for (size_t jump : loops.back().breaks) code.instructions[jump].operand = code.instructions.size(); loops.pop_back();
        } else if (node->kind == "for_c") {
            scopes.emplace_back();
            if (node->children[0]) statement(node->children[0]);
            size_t begin = code.instructions.size(); emit(ByteOp::Point, node); expression(node->children[1]);
            size_t branch = emit(ByteOp::JumpFalse, node); emit(ByteOp::Pop, node); loops.push_back({begin, scopes.size(), {}});
            block(node->children[3], true);
            size_t increment = code.instructions.size();
            for (size_t jump : loops.back().continues) code.instructions[jump].operand = increment;
            if (node->children[2]) statement(node->children[2]);
            emit(ByteOp::SafePoint, node); emit(ByteOp::Jump, node, begin);
            code.instructions[branch].operand = code.instructions.size(); emit(ByteOp::Pop, node);
            for (size_t jump : loops.back().breaks) code.instructions[jump].operand = code.instructions.size();
            loops.pop_back(); clear(scopes.size() - 1, node); scopes.pop_back();
        } else if ((node->kind == "break" || node->kind == "continue") && !loops.empty()) {
            clear(loops.back().depth, node); size_t jump = emit(ByteOp::Jump, node, loops.back().start);
            if (node->kind == "break") loops.back().breaks.push_back(jump);
            else loops.back().continues.push_back(jump);
        } else valid = false;
        emit(ByteOp::SafePoint, node);
    }
public:
    void enableSlotClosures(bool value) { slotClosures = value; }
    std::shared_ptr<const Bytecode> compileClosure(const Ast &node, const std::vector<std::string> &captured, unsigned optimization) {
        code.optimization = optimization;
        for (const auto &parameter : node->parameters) {
            size_t slot = code.locals.size(); code.locals.push_back({parameter.name, parameter.type, false}); scopes.front()[parameter.name] = slot;
        }
        code.parameters = code.locals.size();
        for (const auto &name : captured) {
            size_t slot = code.locals.size(); code.locals.push_back({name, "", false}); scopes.front()[name] = slot;
        }
        code.captures = captured.size();
        block(node->children[0], false); emit(ByteOp::Null, node->children[0]); emit(ByteOp::Return, node->children[0]);
        return valid ? std::make_shared<const Bytecode>(std::move(code)) : nullptr;
    }
    std::shared_ptr<const Bytecode> expressionCode(const Ast &node, unsigned optimization) {
        lexical = true; code.optimization = optimization; expression(node); emit(ByteOp::Return, node);
        if (!valid) throw Error(node->span, "Cannot lower expression to bytecode: " + node->kind, "internal");
        return std::make_shared<const Bytecode>(std::move(code));
    }
    std::shared_ptr<const Bytecode> statementCode(const Ast &node, unsigned optimization) {
        lexical = true; code.program = true; code.optimization = optimization; statement(node); emit(ByteOp::Null, node); emit(ByteOp::Return, node);
        if (!valid) throw Error(node->span, "Cannot lower statement to bytecode: " + node->kind, "internal");
        return std::make_shared<const Bytecode>(std::move(code));
    }
    std::shared_ptr<const Bytecode> compile(const Ast &node, unsigned optimization, bool echoResult) {
        code.optimization = optimization;
        Ast body = node;
        if (node->kind == "function") {
            for (const auto &parameter : node->parameters) {
                if (parameter.defaultValue || parameter.rest) return {};
                size_t slot = code.locals.size(); code.locals.push_back({parameter.name, parameter.type, false}); scopes.front()[parameter.name] = slot;
            }
            code.parameters = code.locals.size(); body = node->children[0];
        } else if (node->kind == "block") code.program = true;
        else return {};
        if (code.program && echoResult && !body->children.empty() && body->children.back()->kind == "expression") {
            for (size_t index = 0; index + 1 < body->children.size(); index++) statement(body->children[index]);
            const auto &last = body->children.back(); emit(ByteOp::Point, last); expression(last->children[0]); emit(ByteOp::Return, last);
        } else { block(body, false); emit(ByteOp::Null, body); emit(ByteOp::Return, body); }
        for (const auto &binding : code.locals) if (!binding.type.empty()) {
            for (const auto &candidate : code.locals) {
                size_t position = binding.type.find(candidate.name);
                if (position != std::string::npos && (position == 0 || !std::isalnum(static_cast<unsigned char>(binding.type[position - 1]))) &&
                    (position + candidate.name.size() == binding.type.size() || !std::isalnum(static_cast<unsigned char>(binding.type[position + candidate.name.size()])))) valid = false;
            }
        }
        return valid ? std::make_shared<const Bytecode>(std::move(code)) : nullptr;
    }
};
}
std::shared_ptr<const Bytecode> compileBytecode(const Ast &node, unsigned optimization, bool echoResult, bool slotClosures) {
    Compiler compiler; compiler.enableSlotClosures(slotClosures);
    return compiler.compile(node, optimization, echoResult);
}
std::shared_ptr<const Bytecode> compileExpression(const Ast &node, unsigned optimization) { return Compiler{}.expressionCode(node, optimization); }
std::shared_ptr<const Bytecode> compileStatement(const Ast &node, unsigned optimization) { return Compiler{}.statementCode(node, optimization); }
const char *byteOpName(ByteOp op) {
    static const char *names[] = {"Tick", "Point", "Null", "Bool", "Int", "Float", "Text", "LoadLocal", "StoreLocal", "DeclareLocal", "ClearLocal", "LoadName", "StoreName", "CheckLocal", "CheckName", "CheckObject", "DeclareName", "DefineFunction", "Member", "Index", "SetIndex", "Binary", "Unary", "Call", "List", "Tuple", "Map", "Pop", "Dup", "Jump", "JumpFalse", "JumpTrue", "JumpPresent", "SafePoint", "Return", "Literal", "Closure", "Format", "Slice", "Match", "PrepareMember", "SetMember", "Swap", "DeclareType", "Import", "Throw", "PrepareSlice", "ResolveMethod", "InvokeMethod", "InstanceOf", "Cast", "NonNull", "JumpNull", "IntToFloat", "MakeClosure", "IterInit", "IterNext", "MatchTest", "MatchBind", "MatchFail", "CallNative", "CallMethod"};
    return names[static_cast<size_t>(op)];
}
}