#include "native_jit.hpp"
#include "bytecode.hpp"
#include "types.hpp"

#include <cstddef>
#ifdef DK3_LLVM
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace dk3
{
#ifndef DK3_LLVM
    struct NativeTier::State
    {
    };
    NativeTier::NativeTier() = default;
    NativeTier::~NativeTier() = default;
    bool NativeTier::available() { return false; }
    NativeReport NativeTier::compile(const Ast &, bool) { return {}; }
    NativeReport NativeTier::emitObject(const Ast &, const EmbeddedSource &, const std::string &)
    {
        throw std::runtime_error("Building native executables requires the LLVM native tier (make LLVM=1)");
    }
    void NativeTier::setIRPath(const std::string &) {}
#else
    namespace
    {
        struct Unsupported
        {
            std::string reason;
        };

        // How values travel through generated code: unboxed scalars, or a pointer to a rooted dk3::Value slot.
        enum class Rep
        {
            Int,
            Float,
            Bool,
            Null,
            Value
        };

        Rep declaredRep(const std::string &type)
        {
            if (type == "Int")
                return Rep::Int;
            if (type == "Float")
                return Rep::Float;
            if (type == "Bool")
                return Rep::Bool;
            return Rep::Value;
        }

        // Representation of a checked expression; nullopt when the checker established no usable type.
        std::optional<Rep> checkedRep(Ty type)
        {
            if (!type || type->tag == TypeTag::Error || type->tag == TypeTag::Never)
                return std::nullopt;
            if (type->tag == TypeTag::Int)
                return Rep::Int;
            if (type->tag == TypeTag::Float)
                return Rep::Float;
            if (type->tag == TypeTag::Bool)
                return Rep::Bool;
            return Rep::Value;
        }

        bool scalar(Rep rep) { return rep == Rep::Int || rep == Rep::Float || rep == Rep::Bool; }

        char kindCode(Rep rep)
        {
            switch (rep)
            {
            case Rep::Int:
                return 'i';
            case Rep::Float:
                return 'f';
            case Rep::Bool:
                return 'b';
            case Rep::Null:
                return 'n';
            default:
                return 'v';
            }
        }

        bool decorated(const Ast &node)
        {
            return std::any_of(node->annotations.begin(), node->annotations.end(), [](const Annotation &annotation)
                               { return !annotation.builtin; });
        }

        bool returnsValue(const Ast &node)
        {
            if (!node)
                return false;
            if (node->kind == "return" && !node->children.empty())
                return true;
            if (node->kind == "function")
                return false;
            return std::any_of(node->children.begin(), node->children.end(), returnsValue);
        }

        struct Target
        {
            Ast node;
            std::string owner; // declaring type of a method
            bool method = false;
            bool closure = false;   // lambda: captures load into locals at entry
            bool script = false;    // top-level loop run in place of its bytecode
            bool requested = false; // --jit, @jit, or a lambda inside an @jit function
            std::vector<std::string> captures;
            std::vector<Rep> parameters;
            Rep result = Rep::Null;
            llvm::Function *function = nullptr;
            std::set<const Node *> callees;
            std::string label() const
            {
                std::string at = std::to_string(node->span.line) + ":" + std::to_string(node->span.column);
                if (closure || script)
                    return (closure ? "lambda@" : "loop@") + at;
                return owner.empty() ? node->text : owner + "." + node->text;
            }
        };

        // Same name collection as the bytecode compiler's closure capture analysis.
        void collectNames(const Ast &node, std::vector<std::string> &referenced, std::set<std::string> &declared)
        {
            if (!node)
                return;
            if (node->kind == "name")
                referenced.push_back(node->text);
            if (node->kind == "binding" || node->kind == "for" || node->kind == "try")
                declared.insert(node->text);
            if (node->kind == "arm")
                for (const auto &name : node->names)
                    declared.insert(name);
            for (const auto &parameter : node->parameters)
            {
                declared.insert(parameter.name);
                collectNames(parameter.defaultValue, referenced, declared);
            }
            for (const auto &child : node->children)
                collectNames(child, referenced, declared);
        }

        struct Typed
        {
            llvm::Value *value; // scalar, or a slot pointer for Rep::Value
            Rep rep;
        };

        llvm::Type *parameterType(llvm::LLVMContext &context, Rep rep)
        {
            switch (rep)
            {
            case Rep::Float:
                return llvm::Type::getDoubleTy(context);
            case Rep::Bool:
                return llvm::Type::getInt1Ty(context);
            case Rep::Value:
                return llvm::PointerType::getUnqual(context);
            default:
                return llvm::Type::getInt64Ty(context);
            }
        }

        // (context, environment, boxed result, [self], parameters...); Null and boxed results return 0.
        llvm::FunctionType *signatureOf(llvm::LLVMContext &context, const Target &target)
        {
            llvm::Type *pointer = llvm::PointerType::getUnqual(context);
            std::vector<llvm::Type *> parameters{pointer, pointer, pointer};
            if (target.method)
                parameters.push_back(pointer);
            for (Rep rep : target.parameters)
                parameters.push_back(parameterType(context, rep));
            return llvm::FunctionType::get(target.result == Rep::Value ? llvm::Type::getInt64Ty(context) : parameterType(context, target.result), parameters, false);
        }

        llvm::StructType *contextType(llvm::LLVMContext &context)
        {
            llvm::Type *pointer = llvm::PointerType::getUnqual(context), *word = llvm::Type::getInt64Ty(context);
            return llvm::StructType::get(context, {pointer, word, word, pointer, pointer, pointer, pointer, pointer, pointer, pointer, pointer, pointer, pointer});
        }
        enum ContextField : unsigned
        {
            DepthField,
            LimitField,
            FailureField,
            FramesField,
            RuntimeField,
            HelpersField,
            SitesField,
            EnvironmentField,
            SelfField,
            ResultField,
            FunctionField,
            EpochField,
            CollectField
        };

        class Lowering
        {
            llvm::LLVMContext &context;
            llvm::Module &module;
            llvm::IRBuilder<> builder, prologue;
            Target &target;
            const std::map<std::string, Target *> &callable;
            const NativeValueLayout &layout;
            std::vector<NativeSite> &sites;
            llvm::Value *nativeContext = nullptr, *environment = nullptr, *result = nullptr, *helpers = nullptr;
            llvm::AllocaInst *frame = nullptr, *returned = nullptr;
            llvm::BasicBlock *bail = nullptr, *exit = nullptr;
            uint64_t slots = 0;
            const Span *statementSpan = nullptr;
            // Runtime helper and native calls emitted so far; loops that make any get a GC safe point.
            size_t runtimeCalls = 0;
            struct Local
            {
                llvm::Value *storage; // alloca for scalars, frame slot for boxed values
                Rep rep;
                bool mutableBinding;
            };
            std::vector<std::map<std::string, Local>> scopes;
            struct Loop
            {
                llvm::BasicBlock *next, *exit;
            };
            std::vector<Loop> loops;

            llvm::Type *storageType(Rep rep) { return rep == Rep::Float ? builder.getDoubleTy() : rep == Rep::Bool ? builder.getInt1Ty() : builder.getInt64Ty(); }
            llvm::Value *zero(Rep rep) { return llvm::Constant::getNullValue(storageType(rep)); }
            llvm::Value *null() { return llvm::ConstantPointerNull::get(builder.getPtrTy()); }
            llvm::BasicBlock *block(const char *name) { return llvm::BasicBlock::Create(context, name, target.function); }
            bool open() const
            {
                const llvm::BasicBlock *current = builder.GetInsertBlock();
                return current->empty() || !current->back().isTerminator();
            }
            // Code after return/break/continue still has to be emitted somewhere; it lands in an unreachable block.
            void reopen() { builder.SetInsertPoint(block("dead")); }
            llvm::Value *contextField(ContextField field) { return builder.CreateStructGEP(contextType(context), nativeContext, field); }
            llvm::Value *site(const Span &span)
            {
                sites.push_back({span, "", ""});
                return builder.getInt64(sites.size());
            }
            // Records the failure site and leaves the function; the runtime boundary raises it.
            void failIf(llvm::Value *condition, const Span &span, const std::string &message, const std::string &category)
            {
                sites.push_back({span, message, category});
                llvm::BasicBlock *failed = block("fail"), *continued = block("ok");
                builder.CreateCondBr(condition, failed, continued);
                builder.SetInsertPoint(failed);
                builder.CreateStore(builder.getInt64(sites.size()), contextField(FailureField));
                builder.CreateBr(bail);
                builder.SetInsertPoint(continued);
            }
            void checkFailure()
            {
                llvm::Value *failure = builder.CreateLoad(builder.getInt64Ty(), contextField(FailureField));
                llvm::BasicBlock *continued = block("ok");
                builder.CreateCondBr(builder.CreateICmpNE(failure, builder.getInt64(0)), bail, continued);
                builder.SetInsertPoint(continued);
            }
            llvm::Value *callHelper(llvm::IRBuilder<> &at, NativeHelper id, llvm::Type *resultType, std::vector<llvm::Value *> arguments)
            {
                arguments.insert(arguments.begin(), nativeContext);
                std::vector<llvm::Type *> types;
                for (llvm::Value *argument : arguments)
                    types.push_back(argument->getType());
                llvm::Value *address = at.CreateLoad(at.getPtrTy(), at.CreateConstGEP1_64(at.getPtrTy(), helpers, static_cast<uint64_t>(id)));
                return at.CreateCall(llvm::FunctionType::get(resultType, types, false), address, arguments);
            }
            llvm::Value *helper(NativeHelper id, std::vector<llvm::Value *> arguments, llvm::Type *resultType = nullptr)
            {
                runtimeCalls++;
                llvm::Value *value = callHelper(builder, id, resultType ? resultType : builder.getVoidTy(), std::move(arguments));
                checkFailure();
                return value;
            }
            // A GC safe point at the current position, taken only when allocation crossed the collection threshold.
            void safePoint()
            {
                llvm::Value *requested = builder.CreateLoad(builder.getInt8Ty(), builder.CreateLoad(builder.getPtrTy(), contextField(CollectField)));
                llvm::BasicBlock *collect = block("gc"), *continued = block("gc.done");
                builder.CreateCondBr(builder.CreateICmpNE(requested, builder.getInt8(0)), collect, continued);
                builder.SetInsertPoint(collect);
                callHelper(builder, NativeHelper::SafePoint, builder.getVoidTy(), {});
                builder.CreateBr(continued);
                builder.SetInsertPoint(continued);
            }
            llvm::Constant *text(const std::string &value)
            {
                llvm::Constant *bytes = llvm::ConstantDataArray::getString(context, value, true);
                return new llvm::GlobalVariable(module, bytes->getType(), true, llvm::GlobalValue::PrivateLinkage, bytes, "dk3.text");
            }
            // Zeroed per-site inline cache, shaped like ByteSite.
            llvm::Value *cache()
            {
                auto *type = llvm::ArrayType::get(builder.getInt64Ty(), 5);
                return new llvm::GlobalVariable(module, type, false, llvm::GlobalValue::InternalLinkage, llvm::ConstantAggregateZero::get(type), "dk3.cache");
            }
            std::map<std::string, llvm::Value *> globalCaches;
            // The cached global Binding when its (environment, epoch) still match and it is initialized (and mutable
            // for writes); otherwise branches to miss. Same validation as the bytecode's name cache.
            llvm::Value *cachedBinding(const std::string &name, bool write, llvm::BasicBlock *miss)
            {
                llvm::Value *&slot = globalCaches[name];
                if (!slot)
                    slot = cache();
                llvm::Type *word = builder.getInt64Ty(), *pointer = builder.getPtrTy(), *byte = builder.getInt8Ty();
                llvm::Value *cachedEnvironment = builder.CreateLoad(pointer, slot);
                llvm::Value *cachedEpoch = builder.CreateLoad(word, builder.CreateConstGEP1_64(byte, slot, 8));
                llvm::Value *binding = builder.CreateLoad(pointer, builder.CreateConstGEP1_64(byte, slot, 16));
                llvm::Value *epoch = builder.CreateLoad(word, builder.CreateLoad(pointer, contextField(EpochField)));
                llvm::Value *valid = builder.CreateAnd(builder.CreateAnd(builder.CreateICmpEQ(cachedEnvironment, environment), builder.CreateICmpEQ(cachedEpoch, epoch)), builder.CreateIsNotNull(binding));
                llvm::BasicBlock *cached = block("global.cached"), *hit = block("global.hit");
                builder.CreateCondBr(valid, cached, miss);
                builder.SetInsertPoint(cached);
                llvm::Value *ready = builder.CreateICmpNE(builder.CreateLoad(byte, builder.CreateConstGEP1_64(byte, binding, layout.bindingInitialized)), builder.getInt8(0));
                if (write)
                    ready = builder.CreateAnd(ready, builder.CreateICmpNE(builder.CreateLoad(byte, builder.CreateConstGEP1_64(byte, binding, layout.bindingMutable)), builder.getInt8(0)));
                builder.CreateCondBr(ready, hit, miss);
                builder.SetInsertPoint(hit);
                return binding;
            }
            // Field Binding from a ByteSite cache filled by the Member/PrepareMember/SetMember helpers: the receiver must be
            // an object whose class matches the cached type under the current epoch. Otherwise branches to miss.
            llvm::Value *cachedField(llvm::Value *receiver, llvm::Value *site, llvm::BasicBlock *miss)
            {
                llvm::Type *word = builder.getInt64Ty(), *pointer = builder.getPtrTy(), *byte = builder.getInt8Ty();
                auto at = [&](llvm::Value *base, uint64_t offset)
                { return builder.CreateConstGEP1_64(byte, base, offset); };
                branchUnless(builder.CreateICmpEQ(builder.CreateLoad(byte, tagOf(receiver)), builder.getInt8(layout.object)), miss);
                llvm::Value *instance = builder.CreateLoad(pointer, receiver);
                llvm::Value *type = builder.CreateLoad(pointer, site);
                llvm::Value *epoch = builder.CreateLoad(word, builder.CreateLoad(pointer, contextField(EpochField)));
                llvm::Value *matches = builder.CreateAnd(builder.CreateIsNotNull(type), builder.CreateICmpEQ(builder.CreateLoad(pointer, at(instance, layout.objectOrigin)), type));
                branchUnless(builder.CreateAnd(matches, builder.CreateICmpEQ(builder.CreateLoad(word, at(site, 8)), epoch)), miss);
                llvm::Value *begin = builder.CreateLoad(pointer, at(instance, layout.objectSlots));
                llvm::Value *end = builder.CreateLoad(pointer, at(instance, layout.objectSlots + 8));
                llvm::Value *count = builder.CreateLShr(builder.CreateSub(builder.CreatePtrToInt(end, word), builder.CreatePtrToInt(begin, word)), 3);
                llvm::Value *index = builder.CreateLoad(word, at(site, 32));
                branchUnless(builder.CreateICmpULT(index, count), miss);
                return builder.CreateLoad(pointer, builder.CreateGEP(pointer, begin, index));
            }
            llvm::Value *flag(llvm::Value *binding, unsigned offset)
            {
                return builder.CreateICmpNE(builder.CreateLoad(builder.getInt8Ty(), builder.CreateConstGEP1_64(builder.getInt8Ty(), binding, offset)), builder.getInt8(0));
            }
            void branchUnless(llvm::Value *condition, llvm::BasicBlock *miss)
            {
                llvm::BasicBlock *continued = block("cache.hit");
                builder.CreateCondBr(condition, continued, miss);
                builder.SetInsertPoint(continued);
            }
            Typed loadMember(const Ast &node, llvm::Value *receiver)
            {
                llvm::Value *out = newSlot(), *memberCache = cache();
                llvm::BasicBlock *miss = block("member.miss"), *done = block("member.done");
                llvm::Value *binding = cachedField(receiver, memberCache, miss);
                branchUnless(flag(binding, layout.bindingInitialized), miss);
                box({binding, Rep::Value}, out);
                builder.CreateBr(done);
                builder.SetInsertPoint(miss);
                helper(NativeHelper::Member, {out, receiver, text(node->text), memberCache, site(node->span)});
                rejoin(done);
                return {out, Rep::Value};
            }
            // PrepareMember + SetMember; plain writes and Int/Float + - * updates of cached fields run inline.
            void storeMember(const Ast &node)
            {
                const Ast &destination = node->children[0];
                bool compound = node->text != "=";
                std::string operation = compound ? compoundOperator(node->text) : "";
                llvm::Value *receiver = boxed(expression(destination->children[0])), *container = newSlot(), *memberCache = cache();
                llvm::BasicBlock *prepareMiss = block("prepare.miss"), *prepared = block("prepare.done");
                llvm::Value *field = cachedField(receiver, memberCache, prepareMiss);
                llvm::Value *writable = flag(field, layout.bindingMutable);
                if (compound)
                    writable = builder.CreateAnd(writable, flag(field, layout.bindingInitialized));
                branchUnless(writable, prepareMiss);
                box({receiver, Rep::Value}, container);
                builder.CreateBr(prepared);
                builder.SetInsertPoint(prepareMiss);
                helper(NativeHelper::PrepareMember, {container, receiver, text(destination->text), builder.getInt64(compound), memberCache, site(destination->span)});
                rejoin(prepared);
                Typed value = typed(node->children[1]);
                llvm::Value *stored = boxed(value);
                std::optional<Rep> fieldRep = checkedRep(destination->resolved);
                bool arithmetic = (value.rep == Rep::Int || value.rep == Rep::Float) && (operation == "+" || operation == "-" || operation == "*");
                bool inlined = !compound || (fieldRep && *fieldRep == value.rep && arithmetic);
                llvm::BasicBlock *setMiss = block("set.miss"), *done = block("set.done");
                if (inlined)
                {
                    llvm::Value *binding = cachedField(container, memberCache, setMiss);
                    if (compound)
                    {
                        unsigned char expected = value.rep == Rep::Int ? layout.integer : layout.floating;
                        branchUnless(builder.CreateICmpEQ(builder.CreateLoad(builder.getInt8Ty(), tagOf(binding)), builder.getInt8(expected)), setMiss);
                        Typed current{builder.CreateLoad(storageType(value.rep), binding), value.rep};
                        box(*scalarBinary(operation, current, value, node->span), binding);
                    }
                    else
                        box({stored, Rep::Value}, binding);
                    builder.CreateStore(builder.getInt8(1), builder.CreateConstGEP1_64(builder.getInt8Ty(), binding, layout.bindingInitialized));
                    builder.CreateBr(done);
                }
                else
                    builder.CreateBr(setMiss);
                builder.SetInsertPoint(setMiss);
                helper(NativeHelper::SetMember, {container, text(destination->text), compound ? text(operation) : null(), stored, memberCache, site(node->span)});
                rejoin(done);
            }
            Typed loadGlobal(const std::string &name, const Span &span)
            {
                llvm::Value *out = newSlot();
                llvm::BasicBlock *miss = block("global.miss"), *done = block("global.done");
                box({cachedBinding(name, false, miss), Rep::Value}, out);
                builder.CreateBr(done);
                builder.SetInsertPoint(miss);
                helper(NativeHelper::Global, {environment, out, text(name), globalCaches[name], site(span)});
                rejoin(done);
                return {out, Rep::Value};
            }
            void storeGlobal(const std::string &name, llvm::Value *value, const Span &span)
            {
                llvm::BasicBlock *miss = block("global.miss"), *done = block("global.done");
                box({value, Rep::Value}, cachedBinding(name, true, miss));
                builder.CreateBr(done);
                builder.SetInsertPoint(miss);
                helper(NativeHelper::StoreGlobal, {environment, value, text(name), globalCaches[name], site(span)});
                rejoin(done);
            }
            llvm::Value *argumentNames(const Ast &call)
            {
                size_t count = call->children.size() - 1;
                if (std::all_of(call->names.begin(), call->names.end(), [](const std::string &name)
                                { return name.empty(); }))
                    return null();
                std::vector<llvm::Constant *> items;
                for (size_t index = 0; index < count; index++)
                {
                    const std::string name = index < call->names.size() ? call->names[index] : "";
                    items.push_back(name.empty() ? llvm::ConstantPointerNull::get(builder.getPtrTy()) : text(name));
                }
                auto *type = llvm::ArrayType::get(builder.getPtrTy(), items.size());
                return new llvm::GlobalVariable(module, type, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(type, items), "dk3.names");
            }

            llvm::Value *slotAt(uint64_t index)
            {
                if (!layout.usable)
                    throw Unsupported{"uses object values, which this platform's runtime layout does not support"};
                return prologue.CreateConstGEP1_64(prologue.getInt8Ty(), frame, sizeof(NativeFrame) + uint64_t{layout.size} * index);
            }
            llvm::Value *newSlot() { return slotAt(slots++); }
            llvm::Value *tagOf(llvm::Value *slot) { return builder.CreateConstGEP1_64(builder.getInt8Ty(), slot, layout.tag); }
            llvm::Value *isNull(llvm::Value *slot) { return builder.CreateICmpEQ(builder.CreateLoad(builder.getInt8Ty(), tagOf(slot)), builder.getInt8(layout.null)); }
            // `?.`: stores Null into out and jumps past the access when the receiver is null; rejoin() closes it.
            llvm::BasicBlock *skipIfNull(llvm::Value *receiver, llvm::Value *out)
            {
                box({nullptr, Rep::Null}, out);
                llvm::BasicBlock *present = block("safe.present"), *merge = block("safe.end");
                builder.CreateCondBr(isNull(receiver), merge, present);
                builder.SetInsertPoint(present);
                return merge;
            }
            void rejoin(llvm::BasicBlock *merge)
            {
                builder.CreateBr(merge);
                builder.SetInsertPoint(merge);
            }
            void box(Typed value, llvm::Value *slot)
            {
                unsigned char tag = layout.null;
                switch (value.rep)
                {
                case Rep::Value:
                    if (slot != value.value)
                        builder.CreateMemCpy(slot, llvm::MaybeAlign(8), value.value, llvm::MaybeAlign(8), layout.size);
                    return;
                case Rep::Int:
                    builder.CreateStore(value.value, slot);
                    tag = layout.integer;
                    break;
                case Rep::Float:
                    builder.CreateStore(value.value, slot);
                    tag = layout.floating;
                    break;
                case Rep::Bool:
                    builder.CreateStore(builder.CreateZExt(value.value, builder.getInt8Ty()), slot);
                    tag = layout.boolean;
                    break;
                case Rep::Null:
                    break;
                }
                builder.CreateStore(builder.getInt8(tag), tagOf(slot));
            }
            llvm::Value *boxed(Typed value)
            {
                if (value.rep == Rep::Value)
                    return value.value;
                llvm::Value *slot = newSlot();
                box(value, slot);
                return slot;
            }
            Typed coerce(Typed value, Rep rep, const Span &span)
            {
                if (value.rep == rep)
                    return value;
                if (rep == Rep::Value)
                    return {boxed(value), Rep::Value};
                if (rep == Rep::Null)
                    return {zero(Rep::Null), Rep::Null};
                if (value.rep != Rep::Value)
                    throw Unsupported{"mixes Int, Float and Bool values without conversion"};
                static const char *const names[] = {"Int", "Float", "Bool"};
                unsigned char expected = rep == Rep::Int ? layout.integer : rep == Rep::Float ? layout.floating
                                                                                              : layout.boolean;
                llvm::Value *tag = builder.CreateLoad(builder.getInt8Ty(), tagOf(value.value));
                failIf(builder.CreateICmpNE(tag, builder.getInt8(expected)), span, std::string("Expected ") + names[static_cast<int>(rep)] + " value", "type");
                if (rep == Rep::Bool)
                    return {builder.CreateICmpNE(builder.CreateLoad(builder.getInt8Ty(), value.value), builder.getInt8(0)), Rep::Bool};
                return {builder.CreateLoad(storageType(rep), value.value), rep};
            }
            const Local *local(const std::string &name) const
            {
                for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
                {
                    auto found = scope->find(name);
                    if (found != scope->end())
                        return &found->second;
                }
                return nullptr;
            }

            Typed expression(const Ast &node)
            {
                if (node->widen && node->kind == "literal" && node->literalKind == TokenKind::Integer)
                    return {llvm::ConstantFP::get(builder.getDoubleTy(), static_cast<double>(integerLiteral(node->text))), Rep::Float};
                Typed value = inner(node);
                if (node->widen && value.rep != Rep::Float)
                {
                    value = coerce(value, Rep::Int, node->span);
                    value = {builder.CreateSIToFP(value.value, builder.getDoubleTy()), Rep::Float};
                }
                return value;
            }
            // Like expression, but boxed values the checker typed as Int/Float/Bool come back unboxed.
            Typed typed(const Ast &node)
            {
                Typed value = expression(node);
                if (value.rep != Rep::Value)
                    return value;
                std::optional<Rep> rep = checkedRep(node->resolved);
                return rep && scalar(*rep) ? coerce(value, *rep, node->span) : value;
            }
            static int64_t integerLiteral(const std::string &text)
            {
                auto value = integerLiteralValue(text);
                if (!value || *value > static_cast<uint64_t>(INT64_MAX))
                    throw Unsupported{"integer literal '" + text + "'"};
                return static_cast<int64_t>(*value);
            }
            // Evaluates expressions into consecutive slots and returns the first (null when there are none).
            llvm::Value *sequence(const std::vector<Ast> &items, size_t from)
            {
                if (items.size() <= from)
                    return null();
                uint64_t first = slots;
                slots += items.size() - from;
                for (size_t index = from; index < items.size(); index++)
                    box(expression(items[index]), slotAt(first + index - from));
                return slotAt(first);
            }
            Typed literal(const Ast &node)
            {
                if (node->literalKind == TokenKind::Integer)
                    return {builder.getInt64(static_cast<uint64_t>(integerLiteral(node->text))), Rep::Int};
                if (node->literalKind == TokenKind::Float)
                {
                    std::string digits = node->text;
                    digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                    double value = std::strtod(digits.c_str(), nullptr);
                    if (!std::isfinite(value))
                        throw Unsupported{"float literal '" + node->text + "'"};
                    return {llvm::ConstantFP::get(builder.getDoubleTy(), value), Rep::Float};
                }
                if (node->literalKind == TokenKind::String || node->literalKind == TokenKind::Bytes)
                {
                    llvm::Value *out = newSlot();
                    helper(NativeHelper::Text, {out, text(node->text), builder.getInt64(node->text.size()), builder.getInt64(node->literalKind == TokenKind::Bytes), site(node->span)});
                    return {out, Rep::Value};
                }
                if (node->text == "null")
                {
                    llvm::Value *out = newSlot();
                    box({nullptr, Rep::Null}, out);
                    return {out, Rep::Value};
                }
                if (node->text == "true" || node->text == "false")
                    return {builder.getInt1(node->text == "true"), Rep::Bool};
                throw Unsupported{"literal '" + node->text + "'"};
            }
            Typed name(const Ast &node)
            {
                if (const Local *found = local(node->text))
                    return found->rep == Rep::Value ? Typed{found->storage, Rep::Value} : Typed{builder.CreateLoad(storageType(found->rep), found->storage), found->rep};
                if (node->text == "super")
                    throw Unsupported{"uses super"};
                return loadGlobal(node->text, node->span);
            }
            // ByteOp::MakeClosure: captures enclosing locals by value, in first-reference order.
            Typed closure(const Ast &node)
            {
                if (!node->text.empty() || !node->typeParams.empty() || node->children.empty())
                    throw Unsupported{"declares a nested named or generic function"};
                std::vector<std::string> referenced, captured;
                std::set<std::string> declared;
                collectNames(node, referenced, declared);
                for (const auto &name : referenced)
                    if (!declared.count(name) && local(name) && std::find(captured.begin(), captured.end(), name) == captured.end())
                        captured.push_back(name);
                if (!compileClosure(node, captured))
                    throw Unsupported{"creates a lambda the bytecode compiler cannot capture"};
                llvm::Value *values = null(), *names = null();
                if (!captured.empty())
                {
                    uint64_t first = slots;
                    slots += captured.size();
                    std::vector<llvm::Constant *> items;
                    for (size_t index = 0; index < captured.size(); index++)
                    {
                        const Local *found = local(captured[index]);
                        Typed value = found->rep == Rep::Value ? Typed{found->storage, Rep::Value} : Typed{builder.CreateLoad(storageType(found->rep), found->storage), found->rep};
                        box(value, slotAt(first + index));
                        items.push_back(text(captured[index]));
                    }
                    values = slotAt(first);
                    auto *type = llvm::ArrayType::get(builder.getPtrTy(), items.size());
                    names = new llvm::GlobalVariable(module, type, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(type, items), "dk3.captures");
                }
                sites.push_back({node->span, "", "closure", node});
                llvm::Value *at = builder.getInt64(sites.size()), *out = newSlot();
                helper(NativeHelper::MakeClosure, {environment, out, values, names, builder.getInt64(captured.size()), at});
                return {out, Rep::Value};
            }
            Typed inner(const Ast &node)
            {
                const std::string &kind = node->kind;
                if (kind == "literal")
                    return literal(node);
                if (kind == "name")
                    return name(node);
                if (kind == "unary")
                    return unary(node);
                if (kind == "binary")
                    return binary(node);
                if (kind == "call")
                    return call(node);
                if (kind == "function")
                    return closure(node);
                if (kind == "member")
                {
                    llvm::Value *receiver = boxed(expression(node->children[0]));
                    if (!node->safe)
                        return loadMember(node, receiver);
                    llvm::Value *out = newSlot();
                    llvm::BasicBlock *merge = skipIfNull(receiver, out);
                    helper(NativeHelper::Member, {out, receiver, text(node->text), cache(), site(node->span)});
                    rejoin(merge);
                    return {out, Rep::Value};
                }
                if (kind == "nonnull")
                {
                    Typed value = expression(node->children[0]);
                    if (value.rep == Rep::Value)
                        failIf(isNull(value.value), node->span, "Non-null assertion failed: value is null", "value");
                    return value;
                }
                if (kind == "is" || kind == "cast")
                {
                    Typed value = expression(node->children[0]);
                    llvm::Value *slot = boxed(value);
                    if (kind == "cast")
                    {
                        helper(NativeHelper::Cast, {environment, slot, text(node->type), site(node->span)});
                        return value;
                    }
                    llvm::Value *matches = helper(NativeHelper::InstanceOf, {environment, slot, text(node->type), site(node->span)}, builder.getInt64Ty());
                    return {builder.CreateICmpNE(matches, builder.getInt64(0)), Rep::Bool};
                }
                if (kind == "index")
                {
                    llvm::Value *receiver = boxed(expression(node->children[0]));
                    llvm::Value *key = boxed(expression(node->children[1]));
                    std::string type = builtinType(node->children[0]);
                    if (type == "List" || type == "Tuple")
                        return indexList(node, receiver, key);
                    llvm::Value *out = newSlot();
                    helper(NativeHelper::Index, {out, receiver, key, site(node->span)});
                    return {out, Rep::Value};
                }
                if (kind == "list" || kind == "tuple" || kind == "map")
                {
                    llvm::Value *items = sequence(node->children, 0), *out = newSlot();
                    helper(NativeHelper::Collection, {out, builder.getInt64(kind == "list" ? 0 : kind == "tuple" ? 1 : 2), items, builder.getInt64(node->children.size()), site(node->span)});
                    return {out, Rep::Value};
                }
                if (kind == "format")
                {
                    llvm::Value *out = newSlot();
                    helper(NativeHelper::Text, {out, text(node->text), builder.getInt64(node->text.size()), builder.getInt64(0), site(node->span)});
                    for (const auto &part : node->children)
                    {
                        llvm::Value *value = boxed(expression(part));
                        helper(NativeHelper::Format, {out, value, site(part->span)});
                    }
                    return {out, Rep::Value};
                }
                if (kind == "slice")
                {
                    // Slots hold receiver, start, end, step; the receiver is checked before bounds run, as in bytecode.
                    uint64_t first = slots;
                    slots += 4;
                    box(expression(node->children[0]), slotAt(first));
                    helper(NativeHelper::Slice, {null(), slotAt(first), builder.getInt64(0), site(node->span)});
                    uint64_t mask = 0;
                    for (size_t index = 1; index < 4; index++)
                        if (node->children[index])
                        {
                            box(expression(node->children[index]), slotAt(first + index));
                            mask |= uint64_t{1} << (index - 1);
                        }
                    llvm::Value *out = newSlot();
                    helper(NativeHelper::Slice, {out, slotAt(first), builder.getInt64(mask), site(node->span)});
                    return {out, Rep::Value};
                }
                throw Unsupported{"uses " + kind + " expressions"};
            }
            // Bool conditions; boxed values convert like the interpreter's boolean().
            Typed condition(const Ast &node)
            {
                Typed value = typed(node);
                if (value.rep == Rep::Bool)
                    return value;
                if (value.rep != Rep::Value)
                    throw Unsupported{"uses a non-Bool condition"};
                llvm::Value *truthy = helper(NativeHelper::Truthy, {value.value, site(node->span)}, builder.getInt64Ty());
                return {builder.CreateICmpNE(truthy, builder.getInt64(0)), Rep::Bool};
            }
            Typed unary(const Ast &node)
            {
                const Ast &operand = node->children[0];
                if (node->text == "-" && operand->kind == "literal" && operand->literalKind == TokenKind::Integer && !node->widen)
                {
                    auto value = integerLiteralValue(operand->text);
                    if (!value)
                        throw Unsupported{"integer literal '-" + operand->text + "'"};
                    return {builder.getInt64(0 - *value), Rep::Int};
                }
                if (node->text == "!" || node->text == "not")
                    return {builder.CreateNot(condition(operand).value), Rep::Bool};
                Typed value = typed(operand);
                if (node->text == "+" && (value.rep == Rep::Int || value.rep == Rep::Float))
                    return value;
                if (node->text == "-" && value.rep == Rep::Int)
                    return {builder.CreateSub(builder.getInt64(0), value.value), Rep::Int};
                if (node->text == "-" && value.rep == Rep::Float)
                    return {builder.CreateFNeg(value.value), Rep::Float};
                if (node->text == "~" && value.rep == Rep::Int)
                    return {builder.CreateNot(value.value), Rep::Int};
                throw Unsupported{"uses unary '" + node->text + "'"};
            }
            Typed logical(const Ast &node, bool conjunction)
            {
                Typed left = condition(node->children[0]);
                llvm::BasicBlock *start = builder.GetInsertBlock(), *right = block("logic.right"), *merge = block("logic.merge");
                if (conjunction)
                    builder.CreateCondBr(left.value, right, merge);
                else
                    builder.CreateCondBr(left.value, merge, right);
                builder.SetInsertPoint(right);
                Typed second = condition(node->children[1]);
                llvm::BasicBlock *end = builder.GetInsertBlock();
                builder.CreateBr(merge);
                builder.SetInsertPoint(merge);
                llvm::PHINode *phi = builder.CreatePHI(builder.getInt1Ty(), 2);
                phi->addIncoming(builder.getInt1(!conjunction), start);
                phi->addIncoming(second.value, end);
                return {phi, Rep::Bool};
            }
            // Floor division/modulo with the interpreter's zero check and INT64_MIN / -1 results.
            llvm::Value *floorDivide(llvm::Value *first, llvm::Value *second, bool modulo, const Span &span)
            {
                failIf(builder.CreateICmpEQ(second, builder.getInt64(0)), span, "Division by zero", "value");
                llvm::Value *overflow = builder.CreateAnd(builder.CreateICmpEQ(first, builder.getInt64(static_cast<uint64_t>(INT64_MIN))), builder.CreateICmpEQ(second, builder.getInt64(static_cast<uint64_t>(-1))));
                llvm::BasicBlock *start = builder.GetInsertBlock(), *normal = block("div.normal"), *merge = block("div.merge");
                builder.CreateCondBr(overflow, merge, normal);
                builder.SetInsertPoint(normal);
                llvm::Value *quotient = builder.CreateSDiv(first, second), *remainder = builder.CreateSRem(first, second);
                llvm::Value *adjust = builder.CreateAnd(builder.CreateICmpNE(remainder, builder.getInt64(0)), builder.CreateICmpSLT(builder.CreateXor(remainder, second), builder.getInt64(0)));
                llvm::Value *value = modulo ? builder.CreateSelect(adjust, builder.CreateAdd(remainder, second), remainder) : builder.CreateSub(quotient, builder.CreateZExt(adjust, builder.getInt64Ty()));
                llvm::BasicBlock *normalEnd = builder.GetInsertBlock();
                builder.CreateBr(merge);
                builder.SetInsertPoint(merge);
                llvm::PHINode *phi = builder.CreatePHI(builder.getInt64Ty(), 2);
                phi->addIncoming(builder.getInt64(modulo ? 0 : static_cast<uint64_t>(INT64_MIN)), start);
                phi->addIncoming(value, normalEnd);
                return phi;
            }
            // Shifts with the interpreter's rules: negative counts fail; counts of 64 or more shift every bit out.
            llvm::Value *shift(llvm::Value *value, llvm::Value *count, bool left, const Span &span)
            {
                failIf(builder.CreateICmpSLT(count, builder.getInt64(0)), span, "Negative shift count", "value");
                llvm::Value *wide = builder.CreateICmpUGE(count, builder.getInt64(64));
                llvm::Value *bounded = builder.CreateSelect(wide, builder.getInt64(63), count);
                if (left)
                    return builder.CreateSelect(wide, builder.getInt64(0), builder.CreateShl(value, bounded));
                return builder.CreateAShr(value, bounded);
            }
            // Same-kind scalar operations generated inline; nullopt leaves the operation to the runtime.
            std::optional<Typed> scalarBinary(const std::string &operation, Typed left, Typed right, const Span &span)
            {
                if (left.rep != right.rep || !scalar(left.rep))
                    return std::nullopt;
                static const std::map<std::string, std::pair<llvm::CmpInst::Predicate, llvm::CmpInst::Predicate>> comparisons{
                    {"==", {llvm::CmpInst::ICMP_EQ, llvm::CmpInst::FCMP_OEQ}}, {"!=", {llvm::CmpInst::ICMP_NE, llvm::CmpInst::FCMP_UNE}},
                    {"<", {llvm::CmpInst::ICMP_SLT, llvm::CmpInst::FCMP_OLT}}, {">", {llvm::CmpInst::ICMP_SGT, llvm::CmpInst::FCMP_OGT}},
                    {"<=", {llvm::CmpInst::ICMP_SLE, llvm::CmpInst::FCMP_OLE}}, {">=", {llvm::CmpInst::ICMP_SGE, llvm::CmpInst::FCMP_OGE}}};
                auto comparison = comparisons.find(operation);
                if (comparison != comparisons.end())
                {
                    if (left.rep == Rep::Float)
                        return Typed{builder.CreateFCmp(comparison->second.second, left.value, right.value), Rep::Bool};
                    if (left.rep == Rep::Int || operation == "==" || operation == "!=")
                        return Typed{builder.CreateICmp(comparison->second.first, left.value, right.value), Rep::Bool};
                    return std::nullopt;
                }
                if (left.rep == Rep::Int)
                {
                    if (operation == "+")
                        return Typed{builder.CreateAdd(left.value, right.value), Rep::Int};
                    if (operation == "-")
                        return Typed{builder.CreateSub(left.value, right.value), Rep::Int};
                    if (operation == "*")
                        return Typed{builder.CreateMul(left.value, right.value), Rep::Int};
                    if (operation == "//" || operation == "%")
                        return Typed{floorDivide(left.value, right.value, operation == "%", span), Rep::Int};
                    if (operation == "&")
                        return Typed{builder.CreateAnd(left.value, right.value), Rep::Int};
                    if (operation == "|")
                        return Typed{builder.CreateOr(left.value, right.value), Rep::Int};
                    if (operation == "^")
                        return Typed{builder.CreateXor(left.value, right.value), Rep::Int};
                    if (operation == "<<" || operation == ">>")
                        return Typed{shift(left.value, right.value, operation == "<<", span), Rep::Int};
                }
                else if (left.rep == Rep::Float)
                {
                    if (operation == "+")
                        return Typed{builder.CreateFAdd(left.value, right.value), Rep::Float};
                    if (operation == "-")
                        return Typed{builder.CreateFSub(left.value, right.value), Rep::Float};
                    if (operation == "*")
                        return Typed{builder.CreateFMul(left.value, right.value), Rep::Float};
                    if (operation == "/")
                    {
                        failIf(builder.CreateFCmpOEQ(right.value, llvm::ConstantFP::get(builder.getDoubleTy(), 0.0)), span, "Division by zero", "value");
                        return Typed{builder.CreateFDiv(left.value, right.value), Rep::Float};
                    }
                }
                return std::nullopt;
            }
            Typed runtimeBinary(const std::string &operation, Typed left, Typed right, const Span &span, bool strings = false)
            {
                llvm::Value *first = boxed(left), *second = boxed(right), *out = newSlot();
                if (strings && operation == "+")
                    helper(NativeHelper::Concat, {out, first, second, site(span)});
                else
                    helper(NativeHelper::Binary, {out, text(operation), first, second, site(span)});
                return {out, Rep::Value};
            }
            // Builtin class name of a checked expression's type ("List", "String", ...), or empty.
            static std::string builtinType(const Ast &node)
            {
                Ty type = node ? node->resolved : nullptr;
                return type && type->tag == TypeTag::Named && type->info && type->info->builtin ? type->info->name : "";
            }
            static uint64_t kindMask(std::initializer_list<unsigned> kinds)
            {
                uint64_t mask = 0;
                for (unsigned kind : kinds)
                    mask |= uint64_t{1} << kind;
                return mask;
            }
            void classify(llvm::Value *value, llvm::Value *collection, uint64_t mask)
            {
                callHelper(builder, NativeHelper::Classify, builder.getVoidTy(), {value, collection, builder.getInt64(mask)});
            }
            llvm::Value *byteAt(llvm::Value *base, uint64_t offset) { return builder.CreateConstGEP1_64(builder.getInt8Ty(), base, offset); }
            // The instance when value is an object of the builtin collection type classified into this cache under the
            // current epoch (so its class has no operator hooks); otherwise branches to miss.
            llvm::Value *cachedCollection(llvm::Value *value, llvm::Value *collection, llvm::BasicBlock *miss)
            {
                branchUnless(builder.CreateICmpEQ(builder.CreateLoad(builder.getInt8Ty(), tagOf(value)), builder.getInt8(layout.object)), miss);
                llvm::Value *instance = builder.CreateLoad(builder.getPtrTy(), value);
                llvm::Value *type = builder.CreateLoad(builder.getPtrTy(), collection);
                llvm::Value *epoch = builder.CreateLoad(builder.getInt64Ty(), builder.CreateLoad(builder.getPtrTy(), contextField(EpochField)));
                llvm::Value *same = builder.CreateAnd(builder.CreateIsNotNull(type), builder.CreateICmpEQ(builder.CreateLoad(builder.getPtrTy(), byteAt(instance, layout.objectOrigin)), type));
                branchUnless(builder.CreateAnd(same, builder.CreateICmpEQ(builder.CreateLoad(builder.getInt64Ty(), byteAt(collection, 8)), epoch)), miss);
                return instance;
            }
            // {begin, element count} of an Object vector member holding `size`-byte elements.
            std::pair<llvm::Value *, llvm::Value *> storage(llvm::Value *instance, unsigned offset, unsigned size)
            {
                llvm::Value *begin = builder.CreateLoad(builder.getPtrTy(), byteAt(instance, offset));
                llvm::Value *end = builder.CreateLoad(builder.getPtrTy(), byteAt(instance, offset + 8));
                llvm::Value *bytes = builder.CreateSub(builder.CreatePtrToInt(end, builder.getInt64Ty()), builder.CreatePtrToInt(begin, builder.getInt64Ty()));
                return {begin, builder.CreateUDiv(bytes, builder.getInt64(size))};
            }
            llvm::Value *element(llvm::Value *begin, llvm::Value *index) { return builder.CreateGEP(builder.getInt8Ty(), begin, builder.CreateMul(index, builder.getInt64(layout.size))); }
            // Element index for an Int key (negative counts from the end); branches to miss unless it is in range.
            llvm::Value *position(llvm::Value *key, llvm::Value *count, llvm::BasicBlock *miss)
            {
                branchUnless(builder.CreateICmpEQ(builder.CreateLoad(builder.getInt8Ty(), tagOf(key)), builder.getInt8(layout.integer)), miss);
                llvm::Value *index = builder.CreateLoad(builder.getInt64Ty(), key);
                llvm::Value *adjusted = builder.CreateSelect(builder.CreateICmpSLT(index, builder.getInt64(0)), builder.CreateAdd(index, count), index);
                branchUnless(builder.CreateICmpULT(adjusted, count), miss);
                return adjusted;
            }
            Typed indexList(const Ast &node, llvm::Value *receiver, llvm::Value *key)
            {
                llvm::Value *out = newSlot(), *collection = cache();
                llvm::BasicBlock *miss = block("index.miss"), *done = block("index.done");
                auto [begin, count] = storage(cachedCollection(receiver, collection, miss), layout.objectItems, layout.size);
                box({element(begin, position(key, count, miss)), Rep::Value}, out);
                builder.CreateBr(done);
                builder.SetInsertPoint(miss);
                helper(NativeHelper::Index, {out, receiver, key, site(node->span)});
                classify(receiver, collection, kindMask({layout.kindList, layout.kindTuple}));
                rejoin(done);
                return {out, Rep::Value};
            }
            void storeList(const Ast &node, llvm::Value *receiver, llvm::Value *key, llvm::Value *value, const Span &target)
            {
                llvm::Value *collection = cache();
                llvm::BasicBlock *miss = block("store.miss"), *done = block("store.done");
                auto [begin, count] = storage(cachedCollection(receiver, collection, miss), layout.objectItems, layout.size);
                box({value, Rep::Value}, element(begin, position(key, count, miss)));
                builder.CreateBr(done);
                builder.SetInsertPoint(miss);
                helper(NativeHelper::SetIndex, {receiver, key, value, null(), site(node->span), site(target)});
                classify(receiver, collection, kindMask({layout.kindList}));
                rejoin(done);
            }
            Typed lengthOf(const Ast &node, llvm::Value *receiver)
            {
                llvm::Value *out = newSlot(), *collection = cache();
                llvm::BasicBlock *miss = block("length.miss"), *done = block("length.done");
                llvm::Value *instance = cachedCollection(receiver, collection, miss);
                llvm::Value *kind = builder.CreateLoad(builder.getInt64Ty(), byteAt(collection, 32));
                llvm::Value *items = storage(instance, layout.objectItems, layout.size).second;
                llvm::Value *entries = storage(instance, layout.objectEntries, 2 * layout.size).second;
                box({builder.CreateSelect(builder.CreateICmpEQ(kind, builder.getInt64(layout.kindMap)), entries, items), Rep::Int}, out);
                builder.CreateBr(done);
                builder.SetInsertPoint(miss);
                helper(NativeHelper::CallBuiltin, {out, receiver, text("length"), null(), builder.getInt64(0), cache(), site(node->span)});
                classify(receiver, collection, kindMask({layout.kindList, layout.kindTuple, layout.kindSet, layout.kindMap}));
                rejoin(done);
                return {out, Rep::Value};
            }
            Typed binary(const Ast &node)
            {
                const std::string &operation = node->text;
                if (operation == "and" || operation == "&&")
                    return logical(node, true);
                if (operation == "or" || operation == "||")
                    return logical(node, false);
                if (operation == "??")
                {
                    llvm::Value *out = newSlot();
                    box(expression(node->children[0]), out);
                    llvm::BasicBlock *fallback = block("coalesce.null"), *merge = block("coalesce.end");
                    builder.CreateCondBr(isNull(out), fallback, merge);
                    builder.SetInsertPoint(fallback);
                    box(expression(node->children[1]), out);
                    rejoin(merge);
                    return {out, Rep::Value};
                }
                Typed left = typed(node->children[0]), right = typed(node->children[1]);
                if (std::optional<Typed> value = scalarBinary(operation, left, right, node->span))
                    return *value;
                bool strings = builtinType(node->children[0]) == "String" && builtinType(node->children[1]) == "String";
                return runtimeBinary(operation, left, right, node->span, strings);
            }
            // Direct calls between compiled top-level functions skip the runtime entirely.
            Typed direct(const Ast &node, Target &called)
            {
                llvm::Value *out = called.result == Rep::Value ? newSlot() : null();
                std::vector<llvm::Value *> arguments{nativeContext, environment, out};
                for (size_t index = 1; index < node->children.size(); index++)
                {
                    Rep rep = called.parameters[index - 1];
                    const Ast &argument = node->children[index];
                    arguments.push_back(rep == Rep::Value ? boxed(expression(argument)) : coerce(typed(argument), rep, argument->span).value);
                }
                target.callees.insert(called.node.get());
                runtimeCalls++;
                llvm::Value *depthSlot = builder.CreateLoad(builder.getPtrTy(), contextField(DepthField));
                llvm::Value *depth = builder.CreateLoad(builder.getInt64Ty(), depthSlot);
                llvm::Value *deeper = builder.CreateAdd(depth, builder.getInt64(1));
                failIf(builder.CreateICmpUGT(deeper, builder.CreateLoad(builder.getInt64Ty(), contextField(LimitField))), node->span, "Call depth exceeded", "limit");
                builder.CreateStore(deeper, depthSlot);
                llvm::Value *value = builder.CreateCall(called.function, arguments);
                builder.CreateStore(depth, depthSlot);
                // On failure, record the callee frame for the error trace before leaving.
                llvm::Value *failure = builder.CreateLoad(builder.getInt64Ty(), contextField(FailureField));
                llvm::BasicBlock *unwound = block("call.unwind"), *continued = block("ok");
                builder.CreateCondBr(builder.CreateICmpNE(failure, builder.getInt64(0)), unwound, continued);
                builder.SetInsertPoint(unwound);
                callHelper(builder, NativeHelper::Unwind, builder.getVoidTy(), {text(called.node->text), site(statementSpan ? *statementSpan : node->span)});
                builder.CreateBr(bail);
                builder.SetInsertPoint(continued);
                return called.result == Rep::Value ? Typed{out, Rep::Value} : Typed{value, called.result};
            }
            Typed call(const Ast &node)
            {
                const Ast &callee = node->children[0];
                size_t count = node->children.size() - 1;
                bool positional = std::all_of(node->names.begin(), node->names.end(), [](const std::string &name)
                                              { return name.empty(); });
                // Methods, lambdas and their environments differ from top-level code, so only top-level callers call directly.
                if (target.owner.empty() && !target.closure && callee->kind == "name" && !local(callee->text) && positional && callee->typeArgs.empty())
                {
                    auto found = callable.find(callee->text);
                    if (found != callable.end() && found->second->parameters.size() == count)
                        return direct(node, *found->second);
                }
                // Checker-resolved core functions and builtin methods use the bytecode's cached fast paths.
                if (!node->target.empty() && positional && !(callee->kind == "member" && callee->safe))
                {
                    llvm::Value *out = newSlot();
                    if (node->target[0] == '.')
                    {
                        llvm::Value *receiver = boxed(expression(callee->children[0]));
                        std::string type = builtinType(callee->children[0]);
                        if (node->target == ".length" && count == 0 && (type == "List" || type == "Tuple" || type == "Set" || type == "Map"))
                            return lengthOf(node, receiver);
                        if (node->target == ".append" && count == 1 && type == "List")
                        {
                            llvm::Value *value = boxed(expression(node->children[1]));
                            helper(NativeHelper::Append, {out, receiver, value, cache(), site(node->span)});
                            return {out, Rep::Value};
                        }
                        llvm::Value *arguments = sequence(node->children, 1);
                        helper(NativeHelper::CallBuiltin, {out, receiver, text(callee->text), arguments, builder.getInt64(count), cache(), site(node->span)});
                    }
                    else
                    {
                        llvm::Value *arguments = sequence(node->children, 1);
                        helper(NativeHelper::CallNative, {out, text(node->target), arguments, builder.getInt64(count), cache(), site(node->span)});
                    }
                    return {out, Rep::Value};
                }
                if (callee->kind == "member")
                {
                    llvm::Value *receiver = boxed(expression(callee->children[0])), *out = newSlot();
                    llvm::BasicBlock *merge = callee->safe ? skipIfNull(receiver, out) : nullptr;
                    llvm::Value *arguments = sequence(node->children, 1);
                    helper(NativeHelper::CallMethod, {out, receiver, text(callee->text), arguments, builder.getInt64(count), argumentNames(node), cache(), site(callee->span), site(node->span)});
                    if (merge)
                        rejoin(merge);
                    return {out, Rep::Value};
                }
                llvm::Value *function = boxed(expression(callee));
                llvm::Value *arguments = sequence(node->children, 1), *out = newSlot();
                helper(NativeHelper::Call, {out, function, arguments, builder.getInt64(count), argumentNames(node), site(node->span)});
                return {out, Rep::Value};
            }

            void assignLocal(const Ast &node, const Local &found)
            {
                if (!found.mutableBinding)
                    throw Unsupported{"assigns to a let binding"};
                bool compound = node->text != "=";
                if (!scalar(found.rep))
                {
                    Typed value = expression(node->children[1]);
                    if (compound)
                    {
                        bool strings = builtinType(node->children[0]) == "String" && builtinType(node->children[1]) == "String";
                        value = runtimeBinary(compoundOperator(node->text), {found.storage, Rep::Value}, value, node->span, strings);
                    }
                    box(value, found.storage);
                    return;
                }
                Typed value = typed(node->children[1]);
                if (compound)
                {
                    Typed current{builder.CreateLoad(storageType(found.rep), found.storage), found.rep};
                    std::optional<Typed> combined = scalarBinary(compoundOperator(node->text), current, value, node->span);
                    value = combined ? *combined : runtimeBinary(compoundOperator(node->text), current, value, node->span);
                }
                builder.CreateStore(coerce(value, found.rep, node->span).value, found.storage);
            }
            void assignment(const Ast &node)
            {
                const Ast &destination = node->children[0];
                bool compound = node->text != "=";
                llvm::Value *operation = compound ? text(compoundOperator(node->text)) : null();
                if (destination->kind == "name")
                {
                    if (const Local *found = local(destination->text))
                        return assignLocal(node, *found);
                    if (destination->text == "self" || destination->text == "super" || target.closure)
                        throw Unsupported{"assigns to " + (target.closure ? "a captured or outer variable" : destination->text)};
                    Typed value = expression(node->children[1]);
                    if (compound)
                        value = runtimeBinary(compoundOperator(node->text), loadGlobal(destination->text, destination->span), value, node->span);
                    storeGlobal(destination->text, boxed(value), destination->span);
                }
                else if (destination->kind == "member" && !destination->safe)
                    storeMember(node);
                else if (destination->kind == "index")
                {
                    llvm::Value *receiver = boxed(expression(destination->children[0]));
                    llvm::Value *key = boxed(expression(destination->children[1]));
                    llvm::Value *value = boxed(expression(node->children[1]));
                    if (!compound && builtinType(destination->children[0]) == "List")
                        storeList(node, receiver, key, value, destination->span);
                    else
                        helper(NativeHelper::SetIndex, {receiver, key, value, operation, site(node->span), site(destination->span)});
                }
                else
                    throw Unsupported{"assigns to " + destination->kind + " targets"};
            }
            void body(const Ast &node)
            {
                scopes.emplace_back();
                for (const auto &child : node->children)
                    statement(child);
                scopes.pop_back();
            }
            void branchTo(llvm::BasicBlock *destination)
            {
                if (open())
                    builder.CreateBr(destination);
            }
            void binding(const Ast &node)
            {
                if (node->children.empty() || node->text.empty())
                    throw Unsupported{"declares a binding without a value"};
                Typed value = typed(node->children[0]);
                Rep rep = checkedRep(node->resolved).value_or(value.rep);
                if (scalar(rep))
                {
                    value = coerce(value, rep, node->span);
                    llvm::AllocaInst *storage = prologue.CreateAlloca(storageType(rep), nullptr, node->text);
                    builder.CreateStore(value.value, storage);
                    scopes.back()[node->text] = {storage, rep, node->mutableBinding};
                    return;
                }
                llvm::Value *storage = newSlot();
                box(value, storage);
                scopes.back()[node->text] = {storage, Rep::Value, node->mutableBinding};
            }
            void returnStatement(const Ast &node)
            {
                if (node->children.empty())
                {
                    if (target.result != Rep::Null)
                        throw Unsupported{"returns without a value"};
                }
                else
                {
                    Typed value = typed(node->children[0]);
                    if (target.result == Rep::Value)
                        box(value, result);
                    else if (target.result != Rep::Null)
                        builder.CreateStore(coerce(value, target.result, node->children[0]->span).value, returned);
                }
                builder.CreateBr(exit);
                reopen();
            }
            void forIn(const Ast &node)
            {
                scopes.emplace_back();
                llvm::Value *iterable = newSlot(), *item = newSlot(), *collection = cache();
                box(expression(node->children[0]), iterable);
                helper(NativeHelper::IterInit, {iterable, site(node->span)});
                llvm::AllocaInst *cursor = prologue.CreateAlloca(builder.getInt64Ty(), nullptr, "cursor");
                builder.CreateStore(builder.getInt64(0), cursor);
                llvm::BasicBlock *test = block("for.next"), *loop = block("for.body"), *latch = block("for.latch"), *done = block("for.end");
                builder.CreateBr(test);
                builder.SetInsertPoint(test);
                // Builtin List/Tuple/Set storage and Range arithmetic run inline; anything else takes Runtime::next.
                llvm::BasicBlock *range = block("for.range"), *slow = block("for.slow"), *take = block("for.take");
                llvm::Value *index = builder.CreateLoad(builder.getInt64Ty(), cursor);
                auto [begin, count] = storage(cachedCollection(iterable, collection, range), layout.objectItems, layout.size);
                builder.CreateCondBr(builder.CreateICmpULT(index, count), take, done);
                builder.SetInsertPoint(take);
                box({element(begin, index), Rep::Value}, item);
                builder.CreateStore(builder.CreateAdd(index, builder.getInt64(1)), cursor);
                builder.CreateBr(loop);
                builder.SetInsertPoint(range);
                branchUnless(builder.CreateICmpEQ(builder.CreateLoad(builder.getInt8Ty(), tagOf(iterable)), builder.getInt8(layout.object)), slow);
                llvm::Value *instance = builder.CreateLoad(builder.getPtrTy(), iterable);
                branchUnless(builder.CreateICmpEQ(builder.CreateLoad(builder.getInt32Ty(), byteAt(instance, layout.objectKind)), builder.getInt32(layout.kindRange)), slow);
                llvm::Value *bounds = builder.CreateLoad(builder.getPtrTy(), byteAt(instance, layout.objectItems));
                llvm::Value *start = builder.CreateLoad(builder.getInt64Ty(), bounds);
                llvm::Value *stop = builder.CreateLoad(builder.getInt64Ty(), byteAt(bounds, layout.size));
                llvm::Value *stride = builder.CreateLoad(builder.getInt64Ty(), byteAt(bounds, 2 * layout.size));
                llvm::Value *offset = builder.CreateIntrinsic(llvm::Intrinsic::smul_with_overflow, {builder.getInt64Ty()}, {index, stride});
                llvm::Value *current = builder.CreateIntrinsic(llvm::Intrinsic::sadd_with_overflow, {builder.getInt64Ty()}, {start, builder.CreateExtractValue(offset, 0)});
                llvm::Value *value = builder.CreateExtractValue(current, 0);
                llvm::Value *ended = builder.CreateSelect(builder.CreateICmpSGT(stride, builder.getInt64(0)), builder.CreateICmpSGE(value, stop), builder.CreateICmpSLE(value, stop));
                llvm::Value *finished = builder.CreateOr(builder.CreateOr(builder.CreateExtractValue(offset, 1), builder.CreateExtractValue(current, 1)), ended);
                llvm::BasicBlock *step = block("for.range.take");
                builder.CreateCondBr(finished, done, step);
                builder.SetInsertPoint(step);
                box({value, Rep::Int}, item);
                builder.CreateStore(builder.CreateAdd(index, builder.getInt64(1)), cursor);
                builder.CreateBr(loop);
                builder.SetInsertPoint(slow);
                llvm::Value *more = helper(NativeHelper::IterNext, {item, iterable, cursor, site(node->span)}, builder.getInt64Ty());
                classify(iterable, collection, kindMask({layout.kindList, layout.kindTuple, layout.kindSet}));
                builder.CreateCondBr(builder.CreateICmpNE(more, builder.getInt64(0)), loop, done);
                builder.SetInsertPoint(loop);
                scopes.back()[node->text] = {item, Rep::Value, false};
                size_t calls = runtimeCalls;
                loops.push_back({latch, done});
                statement(node->children[1]);
                loops.pop_back();
                branchTo(latch);
                builder.SetInsertPoint(latch);
                if (runtimeCalls > calls)
                    safePoint();
                builder.CreateBr(test);
                builder.SetInsertPoint(done);
                scopes.pop_back();
            }
            void statement(const Ast &node)
            {
                // Frame positions in traces are statement-granular, as the bytecode's Point ops are.
                const Span *enclosing = statementSpan;
                statementSpan = &node->span;
                statementKind(node);
                statementSpan = enclosing;
            }
            void statementKind(const Ast &node)
            {
                const std::string &kind = node->kind;
                if (kind == "block")
                    body(node);
                else if (kind == "binding")
                    binding(node);
                else if (kind == "expression")
                {
                    if (node->children[0]->kind == "assignment")
                        assignment(node->children[0]);
                    else
                        expression(node->children[0]);
                }
                else if (kind == "return")
                    returnStatement(node);
                else if (kind == "if")
                {
                    Typed test = condition(node->children[0]);
                    llvm::BasicBlock *then = block("if.then"), *otherwise = block("if.else"), *merge = block("if.end");
                    builder.CreateCondBr(test.value, then, otherwise);
                    builder.SetInsertPoint(then);
                    statement(node->children[1]);
                    branchTo(merge);
                    builder.SetInsertPoint(otherwise);
                    if (node->children.size() > 2)
                        statement(node->children[2]);
                    branchTo(merge);
                    builder.SetInsertPoint(merge);
                }
                else if (kind == "while")
                {
                    llvm::BasicBlock *test = block("while.test"), *loop = block("while.body"), *latch = block("while.latch"), *done = block("while.end");
                    builder.CreateBr(test);
                    builder.SetInsertPoint(test);
                    builder.CreateCondBr(condition(node->children[0]).value, loop, done);
                    builder.SetInsertPoint(loop);
                    size_t calls = runtimeCalls;
                    loops.push_back({latch, done});
                    statement(node->children[1]);
                    loops.pop_back();
                    branchTo(latch);
                    builder.SetInsertPoint(latch);
                    if (runtimeCalls > calls)
                        safePoint();
                    builder.CreateBr(test);
                    builder.SetInsertPoint(done);
                }
                else if (kind == "for_c")
                {
                    scopes.emplace_back();
                    if (node->children[0])
                        statement(node->children[0]);
                    llvm::BasicBlock *test = block("for.test"), *loop = block("for.body"), *step = block("for.step"), *done = block("for.end");
                    builder.CreateBr(test);
                    builder.SetInsertPoint(test);
                    builder.CreateCondBr(condition(node->children[1]).value, loop, done);
                    builder.SetInsertPoint(loop);
                    size_t calls = runtimeCalls;
                    loops.push_back({step, done});
                    statement(node->children[3]);
                    loops.pop_back();
                    branchTo(step);
                    builder.SetInsertPoint(step);
                    if (runtimeCalls > calls)
                        safePoint();
                    if (node->children[2])
                        statement(node->children[2]);
                    branchTo(test);
                    builder.SetInsertPoint(done);
                    scopes.pop_back();
                }
                else if (kind == "for")
                    forIn(node);
                else if ((kind == "break" || kind == "continue") && !loops.empty())
                {
                    builder.CreateBr(kind == "break" ? loops.back().exit : loops.back().next);
                    reopen();
                }
                else if (kind == "throw")
                {
                    helper(NativeHelper::Throw, {boxed(expression(node->children[0])), site(node->span)});
                    builder.CreateBr(bail);
                    reopen();
                }
                else
                    throw Unsupported{"uses '" + kind + "' statements"};
            }

        public:
            Lowering(llvm::LLVMContext &context, llvm::Module &module, Target &target, const std::map<std::string, Target *> &callable, const NativeValueLayout &layout, std::vector<NativeSite> &sites)
                : context(context), module(module), builder(context), prologue(context), target(target), callable(callable), layout(layout), sites(sites) {}

            void lower()
            {
                llvm::Function *function = target.function;
                llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "prologue", function);
                prologue.SetInsertPoint(entry);
                nativeContext = function->getArg(0);
                environment = function->getArg(1);
                result = function->getArg(2);
                frame = prologue.CreateAlloca(llvm::ArrayType::get(prologue.getInt8Ty(), sizeof(NativeFrame)), nullptr, "frame");
                frame->setAlignment(llvm::Align(16));
                helpers = prologue.CreateLoad(prologue.getPtrTy(), prologue.CreateStructGEP(contextType(context), nativeContext, HelpersField), "helpers");
                returned = prologue.CreateAlloca(storageType(target.result), nullptr, "returned");
                prologue.CreateStore(zero(target.result), returned);
                llvm::BasicBlock *start = block("body");
                bail = block("bail");
                exit = block("exit");
                builder.SetInsertPoint(bail);
                builder.CreateBr(exit);
                builder.SetInsertPoint(start);
                scopes.emplace_back();
                unsigned argument = 3;
                if (target.method)
                    scopes.back()["self"] = {function->getArg(argument++), Rep::Value, false};
                const auto &parameters = target.node->parameters;
                for (size_t index = 0; index < parameters.size(); index++, argument++)
                {
                    Rep rep = target.parameters[index];
                    llvm::Value *value = function->getArg(argument);
                    if (rep == Rep::Value)
                    {
                        scopes.back()[parameters[index].name] = {value, Rep::Value, false};
                        continue;
                    }
                    llvm::AllocaInst *storage = prologue.CreateAlloca(storageType(rep), nullptr, parameters[index].name);
                    prologue.CreateStore(value, storage);
                    scopes.back()[parameters[index].name] = {storage, rep, false};
                }
                // Captured enclosing locals become this lambda's locals, as in slot-closure bytecode.
                for (const auto &name : target.captures)
                {
                    llvm::Value *storage = newSlot();
                    helper(NativeHelper::Capture, {storage, text(name), site(target.node->span)});
                    scopes.back()[name] = {storage, Rep::Value, false};
                }
                if (target.script)
                    statement(target.node);
                else
                    body(target.node->children[0]);
                if (open())
                {
                    if (target.result == Rep::Value)
                        box({nullptr, Rep::Null}, result);
                    builder.CreateBr(exit);
                }
                builder.SetInsertPoint(exit);
                if (slots)
                    builder.CreateStore(builder.CreateLoad(builder.getPtrTy(), frame), contextField(FramesField));
                builder.CreateRet(builder.CreateLoad(storageType(target.result), returned));
                // Only now is the frame size known: size it, zero the slots (Null) and link it for the GC.
                uint64_t bytes = sizeof(NativeFrame) + uint64_t{layout.size} * slots;
                frame->setAllocatedType(llvm::ArrayType::get(prologue.getInt8Ty(), bytes));
                if (slots)
                {
                    prologue.CreateMemSet(prologue.CreateConstGEP1_64(prologue.getInt8Ty(), frame, sizeof(NativeFrame)), prologue.getInt8(0), bytes - sizeof(NativeFrame), llvm::MaybeAlign(8));
                    llvm::Value *frames = prologue.CreateStructGEP(contextType(context), nativeContext, FramesField);
                    prologue.CreateStore(prologue.CreateLoad(prologue.getPtrTy(), frames), frame);
                    prologue.CreateStore(prologue.getInt64(slots), prologue.CreateConstGEP1_64(prologue.getInt8Ty(), frame, offsetof(NativeFrame, count)));
                    prologue.CreateStore(frame, frames);
                }
                prologue.CreateBr(start);
                if (frame->use_empty())
                    frame->eraseFromParent();
                if (llvm::verifyFunction(*function))
                    throw Unsupported{"produced invalid native code"};
            }
        };

        // Signature-level eligibility; the body is checked by lowering it.
        std::optional<std::string> signatureProblem(const Ast &node, Target &target)
        {
            if (!node->typeParams.empty())
                return "is generic";
            if (node->parameters.size() > 16)
                return "has more than 16 parameters";
            // Lambdas may leave types to inference; the checker recorded the inferred function type.
            Ty inferred = target.closure && node->resolved && node->resolved->tag == TypeTag::Function ? node->resolved : nullptr;
            for (size_t index = 0; index < node->parameters.size(); index++)
            {
                const Parameter &parameter = node->parameters[index];
                if (parameter.rest || parameter.defaultValue)
                    return "has a rest or default parameter";
                Rep rep = declaredRep(parameter.type);
                if (parameter.type.empty() && inferred && index < inferred->args.size())
                    rep = checkedRep(inferred->args[index]).value_or(Rep::Value);
                target.parameters.push_back(rep);
            }
            if (!node->type.empty())
                target.result = node->type == "Null" ? Rep::Null : declaredRep(node->type);
            else if (inferred && inferred->result)
                target.result = inferred->result->tag == TypeTag::Null ? Rep::Null : checkedRep(inferred->result).value_or(Rep::Value);
            else if (returnsValue(node->children.empty() ? nullptr : node->children[0]))
                return "has an inferred return type";
            else
                target.result = Rep::Null;
            if (node->children.empty())
                return "has no body";
            return std::nullopt;
        }

        struct Lowered
        {
            std::unique_ptr<llvm::LLVMContext> context;
            std::unique_ptr<llvm::Module> module;
            std::vector<NativeSite> sites;
            std::vector<Target> candidates;
            std::set<const Node *> selected;
        };

        // Returns whether anything asks for native code (`all` or an @jit candidate).
        bool collect(const Ast &program, bool all, std::vector<Target> &candidates, std::set<std::string> &decoratedNames, NativeReport &report)
        {
            auto consider = [&](const Ast &function, const std::string &owner, bool method, bool closure, bool requested, std::vector<std::string> captures = {})
            {
                if (findAnnotation(function->annotations, "nojit"))
                    return;
                Target target;
                target.node = function;
                target.owner = owner;
                target.method = method;
                target.closure = closure;
                target.captures = std::move(captures);
                target.requested = requested || findAnnotation(function->annotations, "jit");
                if (auto problem = signatureProblem(function, target))
                {
                    if (target.requested)
                        report.interpreted.push_back({function, *problem});
                    return;
                }
                candidates.push_back(std::move(target));
            };
            // Lambdas anywhere in the program; those inside an @jit function are requested with it. A lambda captures
            // its free names that enclosing functions declare (an over-approximation the runtime capture lookup tolerates).
            std::vector<std::set<std::string>> enclosing;
            std::function<void(const Ast &, bool, bool)> lambdas = [&](const Ast &node, bool requested, bool inType)
            {
                if (!node)
                    return;
                bool function = node->kind == "function";
                if (function)
                {
                    std::vector<std::string> referenced, captures;
                    std::set<std::string> declared;
                    collectNames(node, referenced, declared);
                    if (node->text.empty())
                    {
                        for (const auto &name : referenced)
                            if (!declared.count(name) && std::find(captures.begin(), captures.end(), name) == captures.end() &&
                                std::any_of(enclosing.begin(), enclosing.end(), [&](const std::set<std::string> &scope)
                                            { return scope.count(name) != 0; }))
                                captures.push_back(name);
                        consider(node, "", false, true, requested, captures);
                    }
                    if (inType && !node->staticMember)
                        declared.insert("self");
                    enclosing.push_back(std::move(declared));
                    requested = requested || findAnnotation(node->annotations, "jit");
                }
                bool type = node->kind == "class" || node->kind == "record" || node->kind == "impl" || node->kind == "trait";
                for (const auto &child : node->children)
                    lambdas(child, requested, type);
                if (function)
                    enclosing.pop_back();
            };
            for (const auto &statement : program->children)
            {
                // Bytecode gives blocks nested in top-level statements slot locals, which their lambdas capture.
                std::vector<std::string> referenced;
                std::set<std::string> declared;
                collectNames(statement, referenced, declared);
                enclosing.push_back(std::move(declared));
                lambdas(statement, all, false);
                enclosing.pop_back();
                // Top-level loops replace their bytecode through ByteOp::NativeStatement.
                if (statement->kind == "while" || statement->kind == "for_c" || statement->kind == "for")
                {
                    Target target;
                    target.node = statement;
                    target.script = true;
                    target.requested = all;
                    candidates.push_back(std::move(target));
                    continue;
                }
                bool type = statement->kind == "class" || statement->kind == "record" || statement->kind == "impl";
                if (type || statement->kind == "trait")
                {
                    for (const auto &member : statement->children)
                    {
                        if (member->kind != "function" || member->text.empty())
                            continue;
                        if (type && !member->constructor && member->text != "init")
                            consider(member, statement->text, !member->staticMember, false, all);
                        else if (findAnnotation(member->annotations, "jit"))
                            report.interpreted.push_back({member, type ? "is a constructor" : "is a trait method; the native tier compiles class, record and impl methods"});
                    }
                    continue;
                }
                if (statement->kind != "function" || statement->text.empty())
                    continue;
                if (decorated(statement))
                    decoratedNames.insert(statement->text);
                consider(statement, "", false, false, all);
            }
            return std::any_of(candidates.begin(), candidates.end(), [](const Target &candidate)
                               { return candidate.requested; });
        }

        // Lowers every candidate, dropping those with unsupported constructs until stable (callers of a dropped
        // function are relowered to reach it through the runtime), then selects requested functions plus their
        // direct native callees.
        void lowerAll(const NativeValueLayout &values, const llvm::DataLayout &layout, const llvm::Triple &triple, Lowered &lowered, NativeReport &report)
        {
            std::map<const Node *, std::string> excluded;
            auto &candidates = lowered.candidates;
            for (bool changed = true; changed;)
            {
                changed = false;
                lowered.module.reset();
                lowered.context = std::make_unique<llvm::LLVMContext>();
                lowered.module = std::make_unique<llvm::Module>("dotk3", *lowered.context);
                lowered.module->setDataLayout(layout);
                lowered.module->setTargetTriple(triple);
                lowered.sites.clear();
                std::map<std::string, Target *> callable;
                for (auto &candidate : candidates)
                {
                    candidate.function = nullptr;
                    candidate.callees.clear();
                    if (excluded.count(candidate.node.get()))
                        continue;
                    candidate.function = llvm::Function::Create(signatureOf(*lowered.context, candidate), llvm::Function::InternalLinkage, "dk3." + candidate.label(), *lowered.module);
                    candidate.function->addFnAttr(llvm::Attribute::NoUnwind);
                    // Decorated names are rebound to the decorator result, so direct calls would skip it.
                    if (candidate.owner.empty() && !decorated(candidate.node))
                        callable[candidate.node->text] = &candidate;
                }
                for (auto &candidate : candidates)
                {
                    if (!candidate.function)
                        continue;
                    try
                    {
                        Lowering(*lowered.context, *lowered.module, candidate, callable, values, lowered.sites).lower();
                    }
                    catch (const Unsupported &unsupported)
                    {
                        excluded[candidate.node.get()] = unsupported.reason;
                        changed = true;
                    }
                }
            }
            std::vector<Target *> pending;
            for (auto &candidate : candidates)
                if (candidate.function && candidate.requested)
                    pending.push_back(&candidate);
            while (!pending.empty())
            {
                Target *next = pending.back();
                pending.pop_back();
                if (!lowered.selected.insert(next->node.get()).second)
                    continue;
                for (auto &candidate : candidates)
                    if (next->callees.count(candidate.node.get()))
                        pending.push_back(&candidate);
            }
            for (auto &candidate : candidates)
                if (!candidate.function && candidate.requested)
                    report.interpreted.push_back({candidate.node, excluded[candidate.node.get()]});
        }

        // Uniform entry points convert raw 64-bit payloads so the runtime never depends on per-function signatures.
        std::map<const Node *, llvm::Function *> entryPoints(Lowered &lowered, const std::string &prefix, llvm::GlobalValue::LinkageTypes linkage)
        {
            std::map<const Node *, llvm::Function *> entries;
            llvm::LLVMContext &context = *lowered.context;
            llvm::IRBuilder<> builder(context);
            for (auto &candidate : lowered.candidates)
            {
                if (!lowered.selected.count(candidate.node.get()))
                    continue;
                auto *entry = llvm::Function::Create(llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false),
                                                     linkage, prefix + std::to_string(entries.size()) + "." + candidate.label(), *lowered.module);
                entry->addFnAttr(llvm::Attribute::NoUnwind);
                builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
                llvm::Value *nativeContext = entry->getArg(0);
                auto field = [&](ContextField index)
                { return builder.CreateLoad(builder.getPtrTy(), builder.CreateStructGEP(contextType(context), nativeContext, index)); };
                std::vector<llvm::Value *> arguments{nativeContext, field(EnvironmentField), field(ResultField)};
                if (candidate.method)
                    arguments.push_back(field(SelfField));
                for (size_t index = 0; index < candidate.parameters.size(); index++)
                {
                    llvm::Value *raw = builder.CreateLoad(builder.getInt64Ty(), builder.CreateConstGEP1_64(builder.getInt64Ty(), entry->getArg(1), index));
                    switch (candidate.parameters[index])
                    {
                    case Rep::Float:
                        arguments.push_back(builder.CreateBitCast(raw, builder.getDoubleTy()));
                        break;
                    case Rep::Bool:
                        arguments.push_back(builder.CreateICmpNE(raw, builder.getInt64(0)));
                        break;
                    case Rep::Value:
                        arguments.push_back(builder.CreateIntToPtr(raw, builder.getPtrTy()));
                        break;
                    default:
                        arguments.push_back(raw);
                    }
                }
                llvm::Value *result = builder.CreateCall(candidate.function, arguments);
                if (candidate.result == Rep::Float)
                    result = builder.CreateBitCast(result, builder.getInt64Ty());
                else if (candidate.result == Rep::Bool)
                    result = builder.CreateZExt(result, builder.getInt64Ty());
                builder.CreateRet(result);
                entries[candidate.node.get()] = entry;
            }
            return entries;
        }

        void optimize(llvm::Module &module, llvm::TargetMachine *machine, llvm::OptimizationLevel level)
        {
            llvm::LoopAnalysisManager loops;
            llvm::FunctionAnalysisManager functions;
            llvm::CGSCCAnalysisManager graphs;
            llvm::ModuleAnalysisManager modules;
            llvm::PassBuilder passes(machine);
            passes.registerModuleAnalyses(modules);
            passes.registerCGSCCAnalyses(graphs);
            passes.registerFunctionAnalyses(functions);
            passes.registerLoopAnalyses(loops);
            passes.crossRegisterProxies(loops, functions, graphs, modules);
            passes.buildPerModuleDefaultPipeline(level).run(module, modules);
        }

        std::string parametersOf(const Target &target)
        {
            std::string parameters;
            for (Rep rep : target.parameters)
                parameters += kindCode(rep);
            return parameters;
        }

        // Every top-level function reachable through direct native calls.
        std::vector<std::string> dependenciesOf(const std::vector<Target> &candidates, const Target &target)
        {
            std::vector<std::string> dependencies;
            std::set<const Node *> reached;
            std::vector<const Target *> walk{&target};
            while (!walk.empty())
            {
                const Target *next = walk.back();
                walk.pop_back();
                for (const auto &other : candidates)
                    if (next->callees.count(other.node.get()) && reached.insert(other.node.get()).second)
                    {
                        dependencies.push_back(other.node->text);
                        walk.push_back(&other);
                    }
            }
            return dependencies;
        }
    }

    struct NativeTier::State
    {
        std::unique_ptr<llvm::orc::LLLazyJIT> jit;
        std::deque<std::vector<NativeSite>> sites;
        std::deque<NativeFunction> functions;
        size_t modules = 0;
        std::string irPath;
        bool irWritten = false;

        void writeIR(const llvm::Module &module)
        {
            if (irPath.empty())
                return;
            std::error_code status;
            llvm::raw_fd_ostream output(irPath, status, irWritten ? llvm::sys::fs::OF_Append : llvm::sys::fs::OF_None);
            if (status)
                throw std::runtime_error("Cannot write " + irPath + ": " + status.message());
            module.print(output, nullptr);
            irWritten = true;
        }
    };

    NativeTier::NativeTier() : state(std::make_unique<State>()) {}
    NativeTier::~NativeTier() = default;
    bool NativeTier::available() { return true; }
    void NativeTier::setIRPath(const std::string &path)
    {
        state->irPath = path;
        state->irWritten = false;
    }

    NativeReport NativeTier::compile(const Ast &program, bool all)
    {
        NativeReport report;
        Lowered lowered;
        std::set<std::string> decoratedNames;
        if (!collect(program, all, lowered.candidates, decoratedNames, report))
            return report;
        if (!state->jit)
        {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            // Lazy: each function is code-generated on its first call, so cold code costs no machine-code time.
            auto jit = llvm::orc::LLLazyJITBuilder().create();
            if (!jit)
            {
                std::string message = llvm::toString(jit.takeError());
                for (const auto &candidate : lowered.candidates)
                    report.interpreted.push_back({candidate.node, "native tier unavailable: " + message});
                return report;
            }
            state->jit = std::move(*jit);
        }
        lowerAll(nativeValueLayout(), state->jit->getDataLayout(), state->jit->getTargetTriple(), lowered, report);
        if (lowered.selected.empty())
            return report;
        std::string prefix = "dk3.entry." + std::to_string(state->modules++) + ".";
        std::map<const Node *, std::string> symbols;
        for (const auto &[node, entry] : entryPoints(lowered, prefix, llvm::Function::ExternalLinkage))
            symbols[node] = entry->getName().str();
        // O1 matches O2 on the benchmarks at a third less compile time; AOT keeps O2.
        optimize(*lowered.module, nullptr, llvm::OptimizationLevel::O1);
        state->writeIR(*lowered.module);
        if (auto error = state->jit->addLazyIRModule(llvm::orc::ThreadSafeModule(std::move(lowered.module), std::move(lowered.context))))
        {
            std::string message = llvm::toString(std::move(error));
            for (auto &candidate : lowered.candidates)
                if (lowered.selected.count(candidate.node.get()))
                    report.interpreted.push_back({candidate.node, "native tier failed: " + message});
            return report;
        }
        state->sites.push_back(std::move(lowered.sites));
        for (auto &candidate : lowered.candidates)
        {
            if (!lowered.selected.count(candidate.node.get()))
                continue;
            auto symbol = state->jit->lookup(symbols.at(candidate.node.get()));
            if (!symbol)
            {
                report.interpreted.push_back({candidate.node, "native tier failed: " + llvm::toString(symbol.takeError())});
                continue;
            }
            state->functions.push_back({symbol->toPtr<NativeEntry>(), parametersOf(candidate), kindCode(candidate.result), &state->sites.back(),
                                        dependenciesOf(lowered.candidates, candidate), candidate.method});
            candidate.node->native = &state->functions.back();
            report.compiled.push_back(candidate.label());
        }
        return report;
    }

    NativeReport NativeTier::emitObject(const Ast &program, const EmbeddedSource &embedded, const std::string &path)
    {
        NativeReport report;
        Lowered lowered;
        std::set<std::string> decoratedNames;
        collect(program, true, lowered.candidates, decoratedNames, report);
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::Triple triple(llvm::sys::getProcessTriple());
#if defined(__APPLE__) && defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 100000
        // Match the deployment target the runtime archive was compiled for, so the linker accepts the mix.
        constexpr int minimum = __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__;
        triple.setOSName("macosx" + std::to_string(minimum / 10000) + "." + std::to_string(minimum / 100 % 100) + "." + std::to_string(minimum % 100));
#endif
        std::string problem;
        const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, problem);
        if (!target)
            throw std::runtime_error("No native code generator for " + triple.str() + ": " + problem);
        std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(triple, llvm::sys::getHostCPUName(), "", llvm::TargetOptions(), llvm::Reloc::PIC_));
        if (!machine)
            throw std::runtime_error("Cannot create a code generator for " + triple.str());
        lowerAll(nativeValueLayout(), machine->createDataLayout(), triple, lowered, report);
        auto entries = entryPoints(lowered, "dk3.entry.", llvm::Function::InternalLinkage);

        llvm::LLVMContext &context = *lowered.context;
        llvm::Module &module = *lowered.module;
        llvm::IRBuilder<> builder(context);
        llvm::Type *word = builder.getInt64Ty();
        llvm::PointerType *pointer = builder.getPtrTy();
        size_t constants = 0;
        auto text = [&](const std::string &value) -> llvm::Constant *
        {
            llvm::Constant *bytes = llvm::ConstantDataArray::getString(context, value, true);
            return new llvm::GlobalVariable(module, bytes->getType(), true, llvm::GlobalValue::PrivateLinkage, bytes, "dk3.text." + std::to_string(constants++));
        };
        auto number = [&](uint64_t value) -> llvm::Constant * { return builder.getInt64(value); };
        auto table = [&](llvm::Type *element, const std::vector<llvm::Constant *> &items) -> llvm::Constant *
        {
            if (items.empty())
                return llvm::ConstantPointerNull::get(pointer);
            auto *type = llvm::ArrayType::get(element, items.size());
            return new llvm::GlobalVariable(module, type, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(type, items), "dk3.table." + std::to_string(constants++));
        };

        llvm::StructType *siteType = llvm::StructType::get(context, {word, word, word, word, pointer, pointer});
        std::vector<llvm::Constant *> sites;
        for (const auto &site : lowered.sites)
            sites.push_back(llvm::ConstantStruct::get(siteType, {number(site.span.offset), number(site.span.length), number(static_cast<uint64_t>(site.span.line)),
                                                                 number(static_cast<uint64_t>(site.span.column)), text(site.message), text(site.category)}));
        llvm::StructType *functionType = llvm::StructType::get(context, {pointer, word, word, pointer, pointer, word, pointer, word, word});
        std::vector<llvm::Constant *> functions;
        for (auto &candidate : lowered.candidates)
        {
            if (!lowered.selected.count(candidate.node.get()))
                continue;
            std::vector<llvm::Constant *> dependencies;
            for (const auto &name : dependenciesOf(lowered.candidates, candidate))
                dependencies.push_back(text(name));
            const Span &span = candidate.node->span;
            functions.push_back(llvm::ConstantStruct::get(functionType, {text(candidate.script ? "" : candidate.node->text), number(static_cast<uint64_t>(span.line)), number(static_cast<uint64_t>(span.column)),
                                                                         entries.at(candidate.node.get()), text(parametersOf(candidate)), number(static_cast<uint64_t>(kindCode(candidate.result))),
                                                                         table(pointer, dependencies), number(dependencies.size()), number(candidate.script ? 2 : candidate.method)}));
            report.compiled.push_back(candidate.label());
        }
        std::vector<llvm::Constant *> options;
        for (const auto &option : embedded.options)
            options.push_back(text(option));
        llvm::StructType *moduleType = llvm::StructType::get(context, {pointer, pointer, word});
        std::vector<llvm::Constant *> modules;
        for (const auto &[name, source] : embedded.modules)
            modules.push_back(llvm::ConstantStruct::get(moduleType, {text(name), text(source), number(source.size())}));
        llvm::StructType *programType = llvm::StructType::get(context, {word, pointer, pointer, word, pointer, word, pointer, word, pointer, word, pointer, word});
        llvm::Constant *description = llvm::ConstantStruct::get(programType, {number(embeddedProgramVersion), text(embedded.file), text(embedded.source), number(embedded.source.size()),
                                                                                table(pointer, options), number(options.size()), table(siteType, sites), number(sites.size()),
                                                                                table(functionType, functions), number(functions.size()), table(moduleType, modules), number(modules.size())});
        auto *programTable = new llvm::GlobalVariable(module, programType, true, llvm::GlobalValue::PrivateLinkage, description, "dk3.program");
        new llvm::GlobalVariable(module, pointer, false, llvm::GlobalValue::ExternalLinkage, programTable, "dk3_embedded_program");
        if (llvm::verifyModule(module, &llvm::errs()))
            throw std::runtime_error("Generated an invalid native module");
        optimize(module, machine.get(), llvm::OptimizationLevel::O2);
        state->writeIR(module);

        std::error_code status;
        llvm::raw_fd_ostream output(path, status, llvm::sys::fs::OF_None);
        if (status)
            throw std::runtime_error("Cannot write " + path + ": " + status.message());
        llvm::legacy::PassManager emit;
        if (machine->addPassesToEmitFile(emit, output, nullptr, llvm::CodeGenFileType::ObjectFile))
            throw std::runtime_error("The host code generator cannot emit object files");
        emit.run(module);
        output.flush();
        return report;
    }
#endif
}
