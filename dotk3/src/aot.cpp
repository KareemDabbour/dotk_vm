#include "aot.hpp"
#include "bytecode.hpp"
#include <cstring>
#include <set>
#include <stdexcept>

namespace dk3
{
    namespace
    {

        // ---- binary writer ----
        struct Writer
        {
            std::vector<uint8_t> buf;
            void u8(uint8_t v) { buf.push_back(v); }
            void u32(uint32_t v)
            {
                for (int i = 0; i < 4; i++)
                {
                    buf.push_back(v & 0xff);
                    v >>= 8;
                }
            }
            void i64(int64_t v)
            {
                uint64_t u;
                std::memcpy(&u, &v, 8);
                for (int i = 0; i < 8; i++)
                {
                    buf.push_back(u & 0xff);
                    u >>= 8;
                }
            }
            void f64(double v)
            {
                uint64_t u;
                std::memcpy(&u, &v, 8);
                for (int i = 0; i < 8; i++)
                {
                    buf.push_back(u & 0xff);
                    u >>= 8;
                }
            }
            void str(const std::string &s)
            {
                u32(static_cast<uint32_t>(s.size()));
                for (char c : s)
                    buf.push_back(static_cast<uint8_t>(c));
            }
        };

        // ---- binary reader ----
        struct Reader
        {
            const uint8_t *p, *end;
            Reader(const std::vector<uint8_t> &data) : p(data.data()), end(data.data() + data.size()) {}
            void need(size_t n)
            {
                if (p + n > end)
                    throw std::runtime_error("AOT: truncated file");
            }
            uint8_t u8()
            {
                need(1);
                return *p++;
            }
            uint32_t u32()
            {
                need(4);
                uint32_t v = 0;
                for (int i = 0; i < 4; i++)
                    v |= uint32_t(*p++) << (8 * i);
                return v;
            }
            int64_t i64()
            {
                need(8);
                uint64_t v = 0;
                for (int i = 0; i < 8; i++)
                    v |= uint64_t(*p++) << (8 * i);
                int64_t r;
                std::memcpy(&r, &v, 8);
                return r;
            }
            double f64()
            {
                need(8);
                uint64_t v = 0;
                for (int i = 0; i < 8; i++)
                    v |= uint64_t(*p++) << (8 * i);
                double r;
                std::memcpy(&r, &v, 8);
                return r;
            }
            std::string str()
            {
                uint32_t len = u32();
                need(len);
                std::string s(reinterpret_cast<const char *>(p), len);
                p += len;
                return s;
            }
        };

        // ---- slim AST node ----
        // Everything a ByteInstruction.node is accessed for at runtime.
        struct SlimNode
        {
            uint32_t line = 1, col = 1;
            std::string file; // source file path
            std::string kind, text, type, target;
            bool mutableBinding = false, safe = false;
            uint8_t literalKind = 0; // TokenKind as int
            uint8_t numeric = 0;
            std::vector<std::string> names;
            // First child (used by CheckLocal, CheckName, SetMember, SetIndex, CallMethod)
            bool hasChild0 = false;
            uint32_t child0line = 1, child0col = 1;
            std::string child0text, child0type;
        };

        void writeSlim(Writer &w, const Ast &node)
        {
            if (!node)
            {
                w.u32(0);
                w.u32(0);
                w.str("");
                w.str("");
                w.str("");
                w.str("");
                w.str("");
                w.u8(0);
                w.u8(0);
                w.u8(0);
                w.u32(0);
                w.u8(0);
                return;
            }
            w.u32(static_cast<uint32_t>(node->span.line));
            w.u32(static_cast<uint32_t>(node->span.column));
            w.str(node->span.source ? node->span.source->file : "");
            w.str(node->kind);
            w.str(node->text);
            w.str(node->type);
            w.str(node->target);
            uint8_t flags = (node->mutableBinding ? 1 : 0) | (node->safe ? 2 : 0);
            w.u8(flags);
            w.u8(static_cast<uint8_t>(node->literalKind));
            w.u8(node->numeric);
            w.u32(static_cast<uint32_t>(node->names.size()));
            for (const auto &n : node->names)
                w.str(n);
            bool hasChild0 = !node->children.empty() && node->children[0];
            w.u8(hasChild0 ? 1 : 0);
            if (hasChild0)
            {
                const auto &c0 = node->children[0];
                w.u32(static_cast<uint32_t>(c0->span.line));
                w.u32(static_cast<uint32_t>(c0->span.column));
                w.str(c0->text);
                w.str(c0->type);
            }
        }

        SlimNode readSlim(Reader &r)
        {
            SlimNode s;
            s.line = r.u32();
            s.col = r.u32();
            s.file = r.str();
            s.kind = r.str();
            s.text = r.str();
            s.type = r.str();
            s.target = r.str();
            uint8_t flags = r.u8();
            s.mutableBinding = flags & 1;
            s.safe = flags & 2;
            s.literalKind = r.u8();
            s.numeric = r.u8();
            uint32_t nn = r.u32();
            s.names.resize(nn);
            for (auto &n : s.names)
                n = r.str();
            s.hasChild0 = r.u8() != 0;
            if (s.hasChild0)
            {
                s.child0line = r.u32();
                s.child0col = r.u32();
                s.child0text = r.str();
                s.child0type = r.str();
            }
            return s;
        }

        // Reconstruct a minimal Ast from a SlimNode; source is used for the Span.
        Ast slimToAst(const SlimNode &s, const std::shared_ptr<const Source> &src)
        {
            auto node = std::make_shared<Node>();
            // Use the provided source if the file matches, else create a stub source.
            if (src && src->file == s.file)
            {
                node->span = {src, 0, 0, static_cast<int>(s.line), static_cast<int>(s.col)};
            }
            else if (!s.file.empty())
            {
                auto stub = std::make_shared<Source>();
                const_cast<Source *>(stub.get())->file = s.file;
                node->span = {stub, 0, 0, static_cast<int>(s.line), static_cast<int>(s.col)};
            }
            node->kind = s.kind;
            node->text = s.text;
            node->type = s.type;
            node->target = s.target;
            node->mutableBinding = s.mutableBinding;
            node->safe = s.safe;
            node->literalKind = static_cast<TokenKind>(s.literalKind);
            node->numeric = s.numeric;
            node->names = s.names;
            if (s.hasChild0)
            {
                auto c0 = std::make_shared<Node>();
                if (src && src->file == s.file)
                    c0->span = {src, 0, 0, static_cast<int>(s.child0line), static_cast<int>(s.child0col)};
                c0->text = s.child0text;
                c0->type = s.child0type;
                node->children.push_back(c0);
            }
            return node;
        }

        // Forward declarations for recursive serialization.
        void writeBytecode(Writer &w, const Bytecode &bc);
        std::shared_ptr<const Bytecode> readBytecode(Reader &r, const std::shared_ptr<const Source> &src);

        void writeBytecode(Writer &w, const Bytecode &bc)
        {
            w.u32(static_cast<uint32_t>(bc.parameters));
            w.u32(static_cast<uint32_t>(bc.defaultParameters));
            w.u32(static_cast<uint32_t>(bc.captures));
            uint8_t flags = (bc.hasSelf ? 1 : 0) | (bc.program ? 2 : 0);
            w.u8(flags);
            w.u8(bc.optimization);

            w.u32(static_cast<uint32_t>(bc.instructions.size()));
            for (const auto &ins : bc.instructions)
            {
                w.u8(static_cast<uint8_t>(ins.op));
                w.u32(static_cast<uint32_t>(ins.operand));
                w.i64(ins.integer);
                w.f64(ins.floating);
                w.u8(static_cast<uint8_t>(ins.math));
                writeSlim(w, ins.node);
            }

            w.u32(static_cast<uint32_t>(bc.locals.size()));
            for (const auto &loc : bc.locals)
            {
                w.str(loc.name);
                w.str(loc.type);
                w.u8(loc.mutableBinding ? 1 : 0);
            }

            w.u32(static_cast<uint32_t>(bc.names.size()));
            for (const auto &n : bc.names)
                w.str(n);

            w.u32(static_cast<uint32_t>(bc.closures.size()));
            for (const auto &cl : bc.closures)
            {
                writeSlim(w, cl.node);
                w.u32(static_cast<uint32_t>(cl.slots.size()));
                for (size_t s : cl.slots)
                    w.u32(static_cast<uint32_t>(s));
                w.u8(cl.code ? 1 : 0);
                if (cl.code)
                    writeBytecode(w, *cl.code);
            }

            w.u32(static_cast<uint32_t>(bc.matches.size()));
            for (const auto &m : bc.matches)
            {
                w.u32(static_cast<uint32_t>(m.arms.size()));
                for (const auto &arm : m.arms)
                {
                    w.str(arm.pattern);
                    w.str(arm.type);
                    w.u32(static_cast<uint32_t>(arm.span.line));
                    w.u32(static_cast<uint32_t>(arm.span.column));
                    w.u32(static_cast<uint32_t>(arm.bindings.size()));
                    for (const auto &b : arm.bindings)
                        w.str(b);
                    w.u8(arm.code ? 1 : 0);
                    if (arm.code)
                        writeBytecode(w, *arm.code);
                }
            }

            w.u32(static_cast<uint32_t>(bc.defaultBytecodes.size()));
            for (const auto &db : bc.defaultBytecodes)
            {
                w.u8(db ? 1 : 0);
                if (db)
                    writeBytecode(w, *db);
            }
        }

        std::shared_ptr<const Bytecode> readBytecode(Reader &r, const std::shared_ptr<const Source> &src)
        {
            auto bc = std::make_shared<Bytecode>();
            bc->parameters = r.u32();
            bc->defaultParameters = r.u32();
            bc->captures = r.u32();
            uint8_t flags = r.u8();
            bc->hasSelf = flags & 1;
            bc->program = flags & 2;
            bc->optimization = r.u8();

            uint32_t numIns = r.u32();
            bc->instructions.resize(numIns);
            for (auto &ins : bc->instructions)
            {
                ins.op = static_cast<ByteOp>(r.u8());
                ins.operand = r.u32();
                ins.integer = r.i64();
                ins.floating = r.f64();
                ins.math = static_cast<ByteInstruction::Math>(r.u8());
                ins.node = slimToAst(readSlim(r), src);
            }
            bc->nameBindings.assign(bc->names.size(), nullptr);

            uint32_t numLocals = r.u32();
            bc->locals.resize(numLocals);
            for (auto &loc : bc->locals)
            {
                loc.name = r.str();
                loc.type = r.str();
                loc.mutableBinding = r.u8() != 0;
            }

            uint32_t numNames = r.u32();
            bc->names.resize(numNames);
            for (auto &n : bc->names)
                n = r.str();
            bc->nameBindings.assign(numNames, nullptr);

            uint32_t numClosures = r.u32();
            bc->closures.resize(numClosures);
            for (auto &cl : bc->closures)
            {
                cl.node = slimToAst(readSlim(r), src);
                uint32_t numSlots = r.u32();
                cl.slots.resize(numSlots);
                for (auto &s : cl.slots)
                    s = r.u32();
                if (r.u8())
                    cl.code = readBytecode(r, src);
            }

            uint32_t numMatches = r.u32();
            bc->matches.resize(numMatches);
            for (auto &m : bc->matches)
            {
                uint32_t numArms = r.u32();
                m.arms.resize(numArms);
                for (auto &arm : m.arms)
                {
                    arm.pattern = r.str();
                    arm.type = r.str();
                    int aline = r.u32(), acol = r.u32();
                    if (src)
                        arm.span = {src, 0, 0, aline, acol};
                    uint32_t nb = r.u32();
                    arm.bindings.resize(nb);
                    for (auto &b : arm.bindings)
                        b = r.str();
                    if (r.u8())
                        arm.code = readBytecode(r, src);
                }
            }

            uint32_t numDefaults = r.u32();
            bc->defaultBytecodes.resize(numDefaults);
            for (auto &db : bc->defaultBytecodes)
            {
                if (r.u8())
                    db = readBytecode(r, src);
            }
            bc->maxStack = operandStackDepth(*bc);
            prepareSites(*bc);

            return bc;
        }

        // Walk AST to find all function nodes and whether they're class methods.
        void collectFunctions(const Ast &node, bool inClass, std::vector<std::pair<Ast, bool>> &out)
        {
            if (!node)
                return;
            if (node->kind == "function" && !node->text.empty())
            {
                out.emplace_back(node, inClass);
            }
            bool nowInClass = inClass || node->kind == "class" || node->kind == "record" || node->kind == "impl";
            for (const auto &child : node->children)
                collectFunctions(child, nowInClass, out);
        }

    } // namespace

    AotBytecodes compileAllBytecodes(const Ast &program, unsigned optimization)
    {
        std::vector<std::pair<Ast, bool>> functions;
        collectFunctions(program, false, functions);

        AotBytecodes pool;
        for (const auto &[fn, isMethod] : functions)
        {
            const auto &span = fn->span;
            std::string file = span.source ? span.source->file : "";
            // Compile with the appropriate method flag.
            auto bc = compileBytecode(fn, optimization, false, true, isMethod);
            if (bc)
                pool[{file, (uint32_t)span.line, (uint32_t)span.column, isMethod}] = bc;
            // Also compile as non-method if it's in a class (some methods can be called both ways).
            if (isMethod)
            {
                auto bc2 = compileBytecode(fn, optimization, false, true, false);
                if (bc2)
                    pool[{file, (uint32_t)span.line, (uint32_t)span.column, false}] = bc2;
            }
        }
        return pool;
    }

    static constexpr char MAGIC[] = "DOTBC\x01";
    static constexpr size_t MAGIC_LEN = 6;

    std::vector<uint8_t> serializeAot(const std::string &sourceFile,
                                      const std::string &source,
                                      const AotBytecodes &pool)
    {
        Writer w;
        for (size_t i = 0; i < MAGIC_LEN; i++)
            w.u8(static_cast<uint8_t>(MAGIC[i]));
        w.str(sourceFile);
        w.str(source);
        w.u32(static_cast<uint32_t>(pool.size()));
        for (const auto &[key, bc] : pool)
        {
            w.str(key.file);
            w.u32(key.line);
            w.u32(key.col);
            w.u8(key.hasSelf ? 1 : 0);
            writeBytecode(w, *bc);
        }
        return w.buf;
    }

    AotFile deserializeAot(const std::vector<uint8_t> &data)
    {
        Reader r(data);
        for (size_t i = 0; i < MAGIC_LEN; i++)
        {
            if (r.u8() != static_cast<uint8_t>(MAGIC[i]))
                throw std::runtime_error("AOT: invalid magic or version mismatch");
        }
        AotFile result;
        result.sourceFile = r.str();
        result.source = r.str();

        // Build source object so spans reconstruct properly.
        auto src = std::make_shared<Source>();
        src->file = result.sourceFile;
        src->text = result.source;

        uint32_t numEntries = r.u32();
        for (uint32_t i = 0; i < numEntries; i++)
        {
            AotKey key;
            key.file = r.str();
            key.line = r.u32();
            key.col = r.u32();
            key.hasSelf = r.u8() != 0;
            result.pool[key] = readBytecode(r, src);
        }
        return result;
    }

} // namespace dk3
