#pragma once
#include "syntax.hpp"

namespace dk3 {
struct Object;
enum class ByteOp {
    Tick, Point, Null, Bool, Int, Float, Text, LoadLocal, StoreLocal, DeclareLocal, ClearLocal,
    LoadName, StoreName, CheckLocal, CheckName, CheckObject, DeclareName, DefineFunction, Member, Index, SetIndex, Binary, Unary,
    Call, List, Tuple, Map, Pop, Dup, Jump, JumpFalse, JumpTrue, JumpPresent, SafePoint, Return,
    Literal, Closure, Format, Slice, Match, PrepareMember, SetMember, Swap, DeclareType, Import, Throw, PrepareSlice, ResolveMethod, InvokeMethod,
    InstanceOf, Cast, NonNull, JumpNull, IntToFloat,
    MakeClosure, IterInit, IterNext, MatchTest, MatchBind, MatchFail, CallNative, CallMethod
};
struct ByteInstruction {
    ByteOp op;
    Ast node;
    size_t operand = 0;
    int64_t integer = 0;
    double floating = 0;
    mutable Object *cache = nullptr;
    mutable int64_t cachedKind = -1;
    enum class Math { Generic, Add, Subtract, Multiply, FloorDivide, Modulo, Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual,
        AddF, SubtractF, MultiplyF, DivideF, EqualF, NotEqualF, LessF, GreaterF, LessEqualF, GreaterEqualF } math = Math::Generic;
};
struct ByteLocal { std::string name, type; bool mutableBinding = false; };
struct Bytecode;
struct ByteClosure { Ast node; std::shared_ptr<const Bytecode> code; std::vector<size_t> slots; };
struct ByteMatchArm { std::string pattern; std::vector<std::string> bindings; Span span; std::shared_ptr<const Bytecode> code; std::string type = {}; };
struct ByteMatch { std::vector<ByteMatchArm> arms; };
struct Bytecode {
    std::vector<ByteInstruction> instructions;
    std::vector<ByteLocal> locals;
    std::vector<std::string> names;
    std::vector<ByteMatch> matches;
    std::vector<ByteClosure> closures;
    size_t parameters = 0;
    // Captured values occupy the locals right after the parameters (copied from the closure object).
    size_t captures = 0;
    // Methods keep their receiver in the local right after the parameters.
    bool hasSelf = false;
    unsigned optimization = 2;
    bool program = false;
    // Resolved global/module bindings, valid for one environment and binding epoch.
    mutable std::vector<void *> nameBindings;
    mutable Object *nameEnvironment = nullptr;
    mutable uint64_t nameEpoch = ~uint64_t{0};
};
std::shared_ptr<const Bytecode> compileBytecode(const Ast &node, unsigned optimization = 2, bool echoResult = false, bool slotClosures = false, bool method = false);
std::shared_ptr<const Bytecode> compileExpression(const Ast &node, unsigned optimization = 2);
std::shared_ptr<const Bytecode> compileStatement(const Ast &node, unsigned optimization = 2);
const char *byteOpName(ByteOp op);
}