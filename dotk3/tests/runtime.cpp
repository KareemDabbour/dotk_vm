#include "runtime.hpp"
#include <cassert>
#include <iostream>

class MockSocket final : public dk3::SocketHandle {
public:
    std::deque<std::optional<std::string>> incoming;
    std::shared_ptr<MockSocket> accepted;
    std::string sent;
    size_t maximumSeen = 0;
    uint64_t waitSeen = 0;
    bool failSend = false;
    void close() override { closed = true; }
    uint16_t port() const override { return 4321; }
    std::shared_ptr<dk3::SocketHandle> accept(uint64_t timeout) override { waitSeen = timeout; return accepted; }
    std::optional<std::string> receive(size_t maximum, uint64_t timeout) override {
        maximumSeen = maximum; waitSeen = timeout;
        if (incoming.empty()) return {};
        auto value = incoming.front(); incoming.pop_front(); return value;
    }
    void send(const std::string &bytes, uint64_t timeout) override {
        waitSeen = timeout;
        if (failSend) throw std::runtime_error("Partial send failed");
        sent += bytes;
    }
};

static void networkTests() {
    auto listener = std::make_shared<MockSocket>(), peer = std::make_shared<MockSocket>(); listener->accepted = peer;
    dk3::Config config;
    config.listenSocket = [listener](const std::string &host, uint16_t port) { assert(host == "127.0.0.1" && port == 0); return listener; };
    config.connectSocket = [peer](const std::string &host, uint16_t port, uint64_t wait) { assert(host == "127.0.0.1" && port == 4321 && wait == 10); return peer; };
    dk3::Runtime runtime(config);
    runtime.run("import Sockets as Tcp; let listener = Tcp.listen('127.0.0.1', 0); let peer = IO.Sockets.accept(listener, 7); let client = Tcp.connect('127.0.0.1', 4321, 10); let port = Tcp.port(listener); let timeout = Tcp.receive(peer, 8, 0)", "socket-mock.k3");
    assert(std::get<int64_t>(runtime.get("port")) == 4321 && std::holds_alternative<std::monostate>(runtime.get("timeout")));
    assert(peer->maximumSeen == 8 && peer->waitSeen == 0);
    peer->incoming.push_back(std::string());
    runtime.run("let eof = Sockets.receive(peer, 8, 5); let sent = Sockets.send(peer, b'A\\x00B', 3)", "socket-bytes.k3");
    assert(std::get<dk3::Object *>(runtime.get("eof"))->text.empty() && peer->sent == std::string("A\0B", 3) && peer->waitSeen == 3);
    for (const std::string &code : {"Sockets.listen('', 0)", "Sockets.listen('127.0.0.1', 65536)", "Sockets.connect('127.0.0.1', 0)", "Sockets.receive(peer, 0)", "Sockets.receive(peer, 1048577)", "Sockets.receive(peer, 1, -1)", "Sockets.receive(peer, 1, 1001)", "Sockets.send(peer, 1)", "Sockets.port([])", "Sockets.close(peer, 1)", "Http.writeResponse(peer, 200, '', 'bad\\r\\nInjected: yes')", "Http.writeRequest(peer, 'GET', '/bad\\npath')", "Http.writeRequest(peer, 'HEAD', '/')", "Http.writeResponse(peer, 204, 'body')"}) {
        bool rejected = false; try { runtime.run(code, "network-boundaries.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected && !peer->closed);
    }
    peer->incoming.push_back("too much");
    bool oversized = false; try { runtime.run("Sockets.receive(peer, 1)", "host-receive-limit.k3"); } catch (const dk3::Error &error) { oversized = error.category == "limit"; }
    assert(oversized);
    dk3::Object *foreign = runtime.allocate(dk3::Kind::Resource, {}); foreign->external = std::make_shared<dk3::ResourceInfo>("Other");
    runtime.set("other", foreign);
    bool foreignRejected = false; try { runtime.run("Sockets.port(other)"); } catch (const dk3::Error &error) { foreignRejected = error.category == "type"; }
    assert(foreignRejected);
    peer->failSend = true;
    bool failedSend = false; try { runtime.run("Sockets.send(peer, 'failed')"); } catch (const dk3::Error &error) { failedSend = error.category == "io"; }
    assert(failedSend && peer->closed);
    runtime.run("Sockets.close(peer); Sockets.close(peer); let closed = peer.closed"); assert(std::get<bool>(runtime.get("closed")));
    bool closedRejected = false; try { runtime.run("Sockets.port(peer)"); } catch (const dk3::Error &error) { closedRejected = error.category == "state"; }
    assert(closedRejected);
    dk3::Runtime denied;
    for (const std::string &code : {"Sockets.listen('127.0.0.1', 0)", "Sockets.connect('127.0.0.1', 80)"}) {
        bool rejected = false; try { denied.run(code); } catch (const dk3::Error &error) { rejected = error.category == "capability"; } assert(rejected);
    }
    auto http = std::make_shared<MockSocket>(); config.listenSocket = [http](const std::string &, uint16_t) { return http; };
    dk3::Runtime httpRuntime(config);
    std::string header = "POST /echo HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\n";
    http->incoming = {header.substr(0, 9), header.substr(9), std::string("A\0", 2), "BC"};
    httpRuntime.run("let socket = Sockets.listen('127.0.0.1', 0); let message = Http.readRequest(socket, 100); let method = message['method']; let body = message['body']; let host = message['headers']['host']; System.gc(); Http.writeResponse(socket, 200, body);", "http-fragments.k3");
    assert(std::get<dk3::Object *>(httpRuntime.get("body"))->text == std::string("A\0BC", 4));
    assert(std::get<dk3::Object *>(httpRuntime.get("method"))->text == "POST" && std::get<dk3::Object *>(httpRuntime.get("host"))->text == "local");
    assert(http->sent == "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\nContent-Length: 4\r\n\r\n" + std::string("A\0BC", 4));
    http->incoming = {"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n", "yes"};
    httpRuntime.run("let response = IO.Http.readResponse(socket, 100); let status = response['status']; let responseBody = response['body']", "http-response.k3");
    assert(std::get<int64_t>(httpRuntime.get("status")) == 200 && std::get<dk3::Object *>(httpRuntime.get("responseBody"))->text == "yes");
    httpRuntime.run("let emptyRequest = Http.readRequest(socket, 0)"); assert(std::holds_alternative<std::monostate>(httpRuntime.get("emptyRequest")));
    http->incoming = {"HTTP/1.1 204 No Content\r\n\r\n"}; httpRuntime.run("Http.readResponse(socket, 100)");
    for (const std::string &malformed : {"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na",
            "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
            "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999999\r\n\r\n",
            "GET / HTTP/1.1\r\n\r\n", "GET / HTTP/1.0\r\nHost: x\r\n\r\n", "GET / HTTP/1.1\nHost: x\n\n",
            "GET / HTTP/1.1\r\nHost: x\r\n continued\r\n\r\n", "GET / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n",
            "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nno", "GET / HTTP/1.1\r\nHost: x\r\n\r\nextra"}) {
        http->closed = false; http->incoming = {malformed, std::string()};
        bool rejected = false; try { httpRuntime.run("Http.readRequest(socket, 100)"); } catch (const dk3::Error &error) { rejected = error.category == "io"; }
        assert(rejected && http->closed);
    }
    for (const std::string &malformed : {"HTTP/1.1 200 OK\r\n\r\n", "HTTP/1.1 100 Continue\r\n\r\n", "HTTP/1.1 204 No Content\r\nContent-Length: 2\r\n\r\nno"}) {
        http->closed = false; http->incoming = {malformed}; bool rejected = false;
        try { httpRuntime.run("Http.readResponse(socket, 100)"); } catch (const dk3::Error &error) { rejected = error.category == "io"; }
        assert(rejected && http->closed);
    }
    http->closed = false; http->incoming = {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n", std::nullopt};
    bool incomplete = false; try { httpRuntime.run("Http.readRequest(socket, 100)"); } catch (const dk3::Error &error) { incomplete = error.category == "io"; } assert(incomplete && http->closed);
    http->closed = false; http->incoming.clear();
    std::string excessiveHeader = "GET / HTTP/1.1\r\nHost: " + std::string(16384, 'a');
    for (size_t offset = 0; offset < excessiveHeader.size(); offset += 4096) http->incoming.push_back(excessiveHeader.substr(offset, 4096));
    bool headerLimit = false; try { httpRuntime.run("Http.readRequest(socket, 100)"); } catch (const dk3::Error &error) { headerLimit = error.category == "io"; } assert(headerLimit && http->closed);
    http->closed = false; http->incoming = {"GET / HTTP/1.1\r\nHost: x\r\n\r\n"}; config.maxNetworkBytes = 8;
    dk3::Runtime bounded(config); bool boundedRequest = false;
    try { bounded.run("Http.readRequest(Sockets.listen('127.0.0.1', 0), 100)"); } catch (const dk3::Error &error) { boundedRequest = error.category == "limit"; }
    assert(boundedRequest);
    dk3::Config missing; missing.listenSocket = [](const std::string &, uint16_t) { return std::shared_ptr<dk3::SocketHandle>{}; };
    dk3::Runtime missingRuntime(missing); bool missingRejected = false;
    try { missingRuntime.run("Sockets.listen('127.0.0.1', 0)"); } catch (const dk3::Error &error) { missingRejected = error.category == "io"; }
    assert(missingRejected);
    dk3::Config quota; quota.objectLimit = 256; dk3::Runtime *quotaOwner = nullptr; auto opened = std::make_shared<MockSocket>();
    quota.listenSocket = [&quotaOwner, opened](const std::string &, uint16_t) {
        while (quotaOwner->liveObjects() < 256) quotaOwner->allocate(dk3::Kind::String, {});
        return opened;
    };
    dk3::Runtime quotaRuntime(quota); quotaOwner = &quotaRuntime; bool quotaRejected = false;
    try { quotaRuntime.run("Sockets.listen('127.0.0.1', 0)"); } catch (const dk3::Error &error) { quotaRejected = error.category == "limit"; }
    assert(quotaRejected && opened->closed);

    dk3::Config requests; auto seen = std::make_shared<dk3::HttpRequest>();
    requests.httpRequest = [seen](const dk3::HttpRequest &request) {
        *seen = request;
        if (request.url.find("/big") != std::string::npos) throw std::length_error("too big");
        dk3::HttpResponse response; response.status = 200; response.reason = "OK"; response.url = request.url;
        response.headers = {{"Content-Type", "application/json"}, {"X-Dup", "a"}, {"x-dup", "b"}};
        response.body = "{\"echo\": " + (request.body.empty() ? std::string("null") : request.body) + "}"; return response;
    };
    dk3::Runtime requestRuntime(requests);
    requestRuntime.run("let response = Requests.post('https://example.test/api?x=1#top', {'json': {'a': [1, 'b']}, 'params': {'q': 'a b&c', 'n': 2, 'tags': ['x', 'y']}, 'headers': {'X-Token': 'secret'}, 'timeoutMs': 500, 'redirects': 2}); let status = response['status']; let ok = response['ok']; let dup = response['headers']['x-dup']; let echoed = Requests.json(response)['echo']['a'][1]; let final = response['url']", "requests.k3");
    assert(std::get<int64_t>(requestRuntime.get("status")) == 200 && std::get<bool>(requestRuntime.get("ok")));
    assert(seen->method == "POST" && seen->body == "{\"a\":[1,\"b\"]}" && seen->timeoutMillis == 500 && seen->maxRedirects == 2);
    assert(seen->url == "https://example.test/api?x=1&q=a%20b%26c&n=2&tags=x&tags=y#top");
    bool typed = false, token = false; for (const auto &header : seen->headers) { typed |= header.first == "Content-Type" && header.second == "application/json"; token |= header.first == "X-Token"; }
    assert(typed && token);
    requestRuntime.run("let lowered = Requests.request('get', 'HTTP://example.test/')");
    assert(seen->method == "GET");
    for (const std::string &code : {"Requests.get('ftp://example.test/')", "Requests.get('https://example.test/a b')", "Requests.get(1)", "Requests.request('TRACE', 'https://example.test/')",
            "Requests.get('https://example.test/', {'unknown': 1})", "Requests.get('https://example.test/', {'headers': {'X': 'a\\r\\nInjected: yes'}})",
            "Requests.get('https://example.test/', {'headers': {'Content-Length': '1'}})", "Requests.post('https://example.test/', {'body': 'a', 'json': 1})",
            "Requests.get('https://example.test/', {'timeoutMs': 30001})", "Requests.head('https://example.test/', {'body': 'a'})", "Requests.json({'body': 1})",
            "Requests.get('https://example.test/', {'params': {'x': null}})", "Requests.get('https://example.test/big')"}) {
        bool rejected = false; try { requestRuntime.run(code); } catch (const dk3::Error &) { rejected = true; } assert(rejected);
    }
    dk3::Config disabled; dk3::Runtime disabledRuntime(disabled);
    bool requestsDenied = false; try { disabledRuntime.run("Requests.get('https://example.test/')"); } catch (const dk3::Error &error) { requestsDenied = error.category == "capability"; } assert(requestsDenied);
}

static void nativeRegistrationTests() {
    for (unsigned level : {0u, 1u, 2u}) {
        dk3::Config config; config.optimizationLevel = level;
        dk3::Runtime runtime(config);
        dk3::NativeMethod definition{dk3::Kind::List, "probe", "Int", 1, 2,
            [](dk3::Runtime &runtime, dk3::Object *receiver, const auto &arguments, const dk3::Span &) -> dk3::Value {
                runtime.collect();
                return static_cast<int64_t>(receiver->items.size() + std::get<dk3::Object *>(arguments[0].value)->text.size() + arguments.size());
            }};
        runtime.registerNativeMethod(definition);
        definition.name = "changed"; definition.minimumArguments = 0;
        runtime.run("let detachedProbe = [1,2].probe; let immediateProbe = [1,2,3].probe('a'); let optionalProbe = [1].probe('ab', 0)", "native-registration.k3");
        assert(std::get<int64_t>(runtime.get("immediateProbe")) == 5 && std::get<int64_t>(runtime.get("optionalProbe")) == 5);
        auto bound = std::get<dk3::Object *>(runtime.get("detachedProbe"));
        bound->origin->code->text = "length"; bound->origin->code->parameters.clear();
        for (size_t index = 0; index < 128; index++) {
            runtime.registerNativeMethod({dk3::Kind::Bytes, "extension" + std::to_string(index), "Null", 0, 0,
                [](dk3::Runtime &, dk3::Object *, const auto &, const dk3::Span &) -> dk3::Value { return {}; }});
        }
        runtime.run("System.gc(); let detachedProbeResult = detachedProbe('a'); let directProbeResult = [1,2].probe('a', 0); class ExtendedList extends List {}; let inheritedProbe = ExtendedList([1]).probe('a')", "native-registration-lifetime.k3");
        assert(std::get<int64_t>(runtime.get("detachedProbeResult")) == 4 && std::get<int64_t>(runtime.get("directProbeResult")) == 5 && std::get<int64_t>(runtime.get("inheritedProbe")) == 3);
        auto original = *bound->origin->nativeMethod;
        auto invalidArity = definition; invalidArity.minimumArguments = 3;
        auto missingBody = definition; missingBody.body = nullptr;
        auto missingName = definition; missingName.name.clear();
        for (const auto &invalid : {original, invalidArity, missingBody, missingName}) {
            bool rejected = false;
            try { runtime.registerNativeMethod(invalid); } catch (const dk3::Error &error) { rejected = error.category == "native"; }
            assert(rejected);
        }
        for (const auto &failure : std::vector<std::pair<std::string, std::string>>{
            {"[].probe()", "call"}, {"[].probe('a', 0, 1)", "call"}, {"[].probe(value: 'a')", "call"},
            {"[].init()", "state"}, {"[].init(1, 2)", "state"}, {"[].init(value: 1)", "state"}}) {
            bool rejected = false;
            try { runtime.run(failure.first, "native-registration-errors.k3"); } catch (const dk3::Error &error) { rejected = error.category == failure.second; }
            assert(rejected);
        }
        auto wrong = runtime.allocate(dk3::Kind::BoundMethod, {}); wrong->parent = std::get<dk3::Object *>(runtime.text("wrong", false, {})); wrong->origin = bound->origin;
        bool wrongReceiver = false;
        try { runtime.invoke(wrong, {{"", runtime.text("a", false, {})}}); } catch (const dk3::Error &error) { wrongReceiver = error.category == "type"; }
        assert(wrongReceiver);
    }
}

static void methodDispatchTests() {
    for (unsigned level : {0u, 1u, 2u}) {
        dk3::Config config; config.optimizationLevel = level;
        dk3::Runtime runtime(config);
        runtime.run(R"(
            var receiver = [3];
            fn dropReceiver() { receiver = null; System.gc(); return 3 }
            let retainedNative = receiver.contains(dropReceiver());
            class Parent { fn read(value: Int) -> Int { System.gc(); return value + 3 } }
            class Child extends Parent { fn read(value: Int) -> Int { return super.read(value) + 10 } }
            var child = Child();
            fn dropChild() { child = null; System.gc(); return 2 }
            let retainedScript = child.read(dropChild());
            class Holder { var action: Any = fn(value) => value + 1 }
            let holder = Holder();
            fn replaceAction() { holder.action = fn(value) => 100; System.gc(); return 3 }
            let selectedField = holder.action(replaceAction());
            let replacedField = holder.action(3);
            holder.action = [7].contains;
            let boundField = holder.action(7);
            let nested = '-'.join(['one', 'two']).contains(''.join(['tw', 'o']));
            fn detachedList() { return [4, 5].contains }
            fn detachedString() { return 'alpha'.contains }
            fn detachedBytes() { return b'abc'.length }
            let listMethod = detachedList(); let stringMethod = detachedString(); let bytesMethod = detachedBytes();
            System.gc();
            let retainedDetached = listMethod(5) and stringMethod('ph') and bytesMethod() == 3;
            var effects = 0;
            fn effect() { effects += 1; return 1 }
            class Guarded { private fn hidden(value) {}; var scalar: Int = 1 }
            let guarded = Guarded();
        )", "method-dispatch.k3");
        assert(std::get<bool>(runtime.get("retainedNative")) && std::get<int64_t>(runtime.get("retainedScript")) == 15);
        assert(std::get<int64_t>(runtime.get("selectedField")) == 4 && std::get<int64_t>(runtime.get("replacedField")) == 100);
        assert(std::get<bool>(runtime.get("boundField")) && std::get<bool>(runtime.get("nested")) && std::get<bool>(runtime.get("retainedDetached")));
        for (const auto &failure : std::vector<std::pair<std::string, std::string>>{
            {"guarded.hidden(effect())", "access"}, {"guarded.missing(effect())", "name"},
            {"[1].append(value: 2)", "call"}, {"'abc'.contains(value: 'a')", "call"},
            {"b'abc'.length(1)", "call"}, {"(1,).length(1)", "call"},
            {"Set().add()", "call"}, {"({}).get()", "call"}}) {
            bool rejected = false;
            try { runtime.run(failure.first, "method-errors.k3"); } catch (const dk3::Error &error) { rejected = error.category == failure.second; }
            assert(rejected && std::get<int64_t>(runtime.get("effects")) == 0);
        }
        bool noncallable = false;
        try { runtime.run("guarded.scalar(effect())", "field-call-order.k3"); } catch (const dk3::Error &error) { noncallable = error.category == "type"; }
        assert(noncallable && std::get<int64_t>(runtime.get("effects")) == 1);
        runtime.run("let recovered = [1,2].length()", "method-error-recovery.k3");
        assert(std::get<int64_t>(runtime.get("recovered")) == 2);
    }
}

int main() {
    nativeRegistrationTests();
    methodDispatchTests();
    for (int level : {0, 1, 2}) {
        dk3::Config parityConfig; parityConfig.optimizationLevel = static_cast<unsigned>(level); parityConfig.objectLimit = 10000; std::string parityOutput;
        parityConfig.write = [&](const std::string &value) { parityOutput += value; };
        dk3::Runtime parity(parityConfig);
        parity.run("var regularTotal = 0; for (var regularIndex = 0; regularIndex < 8; regularIndex += 1) { if regularIndex == 2 { continue }; if regularIndex == 6 { break }; regularTotal += regularIndex }; var omittedIndex = 0; for (; omittedIndex < 3; omittedIndex += 1) {}; var openIndex = 0; for (;;) { openIndex += 1; if openIndex < 4 { continue }; break }", "regular-for.k3");
        assert(std::get<int64_t>(parity.get("regularTotal")) == 13 && std::get<int64_t>(parity.get("omittedIndex")) == 3 && std::get<int64_t>(parity.get("openIndex")) == 4);
        parity.run("var cleanupCount = 0; fn counted() { for (var step = 0; step < 5; step++) { try { if step == 1 { continue }; if step == 4 { break }; yield step } catch error {} finally { cleanupCount += 1 } } }; let countedValues = List(counted()); fn callbacksFromLoop() { let saved = []; for (var step = 0; step < 3; step++) { let copy = step; saved.append(fn() => copy) }; return saved }; let loopCallbacks = callbacksFromLoop(); System.gc(); let firstCallback = loopCallbacks[0](); let lastCallback = loopCallbacks[2](); var nestedCount = 0; for (var outer = 0; outer < 3; outer++) { for (var inner = 0; inner < 3; inner++) { if inner == 1 { continue }; nestedCount += 1 } }", "bytecode-control-coverage.k3");
        assert(std::get<int64_t>(parity.get("cleanupCount")) == 5 && std::get<int64_t>(parity.get("firstCallback")) == 0 && std::get<int64_t>(parity.get("lastCallback")) == 2 && std::get<int64_t>(parity.get("nestedCount")) == 6);
        auto countedValues = std::get<dk3::Object *>(parity.get("countedValues")); assert(countedValues->items.size() == 3 && std::get<int64_t>(countedValues->items[1]) == 2);
        parity.registerNative("publishShadow", [&](const auto &, const dk3::Span &) -> dk3::Value { parity.set("Text", int64_t{42}); return {}; });
        parity.run("fn compute(limit: Int) -> Int { var total = 0; var index = 0; while index < limit { index += 1; if index == 2 { continue }; if index == 5 { break }; let item = index * 2; total += item }; return total }; System.print(compute(limit: 9)); let values = [1,2]; values[0] = 3; System.print(values[0], false and missing, true or missing, null ?? 4); fn shadow() { let value = 1; if true { let value = 2; return value }; return value }; System.print(shadow())", "backend-parity.k3");
        assert(parityOutput == "16\n3 false true 4\n2\n"); parityOutput.clear();
        parity.run("class TypeA {}; class TypeB {}; fn checkType(TypeA, value: TypeA) { return 1 }; System.print(checkType(TypeB, TypeA())); var effects = 0; fn effect() { effects += 1; return 0 }; fn immutable() { let fixed = 0; fixed = effect() }; try { immutable() } catch error {}; System.print(effects)", "boundary-parity.k3");
        assert(parityOutput == "1\n0\n");
        parity.run("var garbageIterations = 0; while garbageIterations < 12000 { let garbage = ['temporary', garbageIterations]; garbageIterations += 1 }; let completedIterations = garbageIterations", "automatic-gc.k3");
        assert(std::get<int64_t>(parity.get("completedIterations")) == 12000);
        parityOutput.clear();
        parity.run("fn arithmeticCheck() { System.print(-7 // 3, -7 % 3, 7 % -3, 5 == 5, 2 < 3) }; arithmeticCheck(); fn cachedBinding() { let initial = Reflection.type(Text); publishShadow(); return initial + ':' + Reflection.type(Text) }; System.print(cachedBinding())", "optimization-parity.k3");
        assert(parityOutput == "-3 2 -2 true true\nModule:Int\n");
        parityOutput.clear();
        parity.run("System.print(9223372036854775807 + 1)", "wrapping-optimization.k3");
        assert(parityOutput == "-9223372036854775808\n");
        parityOutput.clear();
        parity.run("var sliceEffects = 0; fn sliceBound() { sliceEffects += 1; return 0 }; let nonsliceable = {}; try { nonsliceable[sliceBound():] } catch error {}; System.print(sliceEffects)", "slice-order.k3");
        assert(parityOutput == "0\n");
    }
    {
        dk3::Runtime simple;
        simple.run("fn choose(value: Int) -> List[Int] { if value == 0 { return [1] } else if value == 1 { let local = [2]; System.gc(); return local } else { return [3] } }; let chosen = choose(1); System.gc(); let chosenValue = chosen[0]; fn emptyResult() {}; let empty = emptyResult(); fn nestedReturn() { { return 4 } }; let nested = nestedReturn()", "simple-returns.k3");
        assert(std::get<int64_t>(simple.get("chosenValue")) == 2 && std::get<int64_t>(simple.get("nested")) == 4 && std::holds_alternative<std::monostate>(simple.get("empty")));
    }
    {
        dk3::Config compactConfig; compactConfig.objectLimit = 256;
        dk3::Runtime compact(compactConfig);
        compact.run("var loops = 0; while loops < 2000 { loops += 1; if loops > 0 {} }; let result = loops", "scope-elision.k3");
        assert(std::get<int64_t>(compact.get("result")) == 2000);
        compact.run("var callbacks = []; var scopeIndex = 0; while scopeIndex < 3 { let captured = scopeIndex; callbacks.append(fn() => captured); scopeIndex += 1 }; let firstScope = callbacks[0](); let lastScope = callbacks[2]()", "scope-captures.k3");
        assert(std::get<int64_t>(compact.get("firstScope")) == 0 && std::get<int64_t>(compact.get("lastScope")) == 2);
    }
    {
        dk3::Runtime traced;
        bool failed = false;
        try { traced.run("fn leaf() { missing }\nfn middle() { leaf() }\nfn entry() { middle() }\nentry()", "trace.k3"); }
        catch (const dk3::Error &error) {
            failed = true;
            assert(error.category == "name" && error.trace.size() == 4);
            assert(error.trace[0].name == "leaf" && error.trace[0].span.line == 1);
            assert(error.trace[1].name == "middle" && error.trace[1].span.line == 2);
            assert(error.trace[2].name == "entry" && error.trace[2].span.line == 3);
            assert(error.trace[3].name == "<script>" && error.trace[3].span.line == 4);
        }
        assert(failed);
        failed = false;
        try { traced.run("try { fn caughtLeaf() { missing }; caughtLeaf() } catch error {}\nfn cleanLeaf() { otherMissing }\ncleanLeaf()", "fresh-trace.k3"); }
        catch (const dk3::Error &error) { failed = true; assert(error.trace.size() == 2 && error.trace[0].name == "cleanLeaf" && error.trace[1].name == "<script>"); }
        assert(failed);
        failed = false;
        try { traced.run("fn typedFailure() -> Int { return [1][5] }\ntypedFailure()", "return-trace.k3"); }
        catch (const dk3::Error &error) { failed = true; assert(error.category == "index" && error.trace.size() == 2 && error.trace[0].name == "typedFailure"); }
        assert(failed);
        failed = false;
        try { traced.run("fn throwing() { throw 'explicit failure' }\nthrowing()", "throw-trace.k3"); }
        catch (const dk3::Error &error) { failed = true; assert(error.category == "throw" && error.trace.size() == 2 && error.trace[0].name == "throwing"); }
        assert(failed);
        failed = false;
        try { traced.run("fn primary() { defer fn() { throw 'cleanup failure' }(); missingPrimary }\nprimary()", "defer-trace.k3"); }
        catch (const dk3::Error &error) { failed = true; assert(error.category == "name" && error.trace.size() == 2 && error.trace[0].name == "primary"); }
        assert(failed);
        failed = false;
        try { traced.run("fn resumed() { yield 1; missingGenerator }\nfor value in resumed() {}", "generator-trace.k3"); }
        catch (const dk3::Error &error) { failed = true; assert(error.trace.size() == 2 && error.trace[0].name == "resumed"); }
        assert(failed);
        failed = false;
        try { traced.run("fn workerFailure() { yield 1; missingWorker }\nlet failedWork = Tasks.spawn(workerFailure()); Tasks.pump(3)", "task-trace.k3"); }
        catch (const dk3::Error &error) { failed = true; assert(error.trace.size() >= 2 && error.trace[0].name == "workerFailure"); }
        assert(failed);
    }
    networkTests();
    std::string output;
    dk3::Config config; config.write = [&](const std::string &text) { output += text; };
    dk3::Runtime runtime(config);
    dk3::Config terminalConfig; bool rawEnabled = false; size_t terminalChanges = 0;
    terminalConfig.terminalMode = [&](bool enabled) { rawEnabled = enabled; terminalChanges++; };
    terminalConfig.readKey = [&](uint64_t timeout) -> std::optional<uint8_t> { assert(rawEnabled); if (timeout == 10) return uint8_t{65}; return {}; };
    dk3::Runtime terminalRuntime(terminalConfig);
    terminalRuntime.run("IO.Terminal.open(); IO.Terminal.open(); let key: Int? = IO.Terminal.readKey(10); let empty: Int? = IO.Terminal.readKey(); let open: Bool = IO.Terminal.isOpen(); IO.Terminal.close(); IO.Terminal.close()", "terminal-capability.k3");
    assert(!rawEnabled && terminalChanges == 2 && std::get<int64_t>(terminalRuntime.get("key")) == 65 && std::holds_alternative<std::monostate>(terminalRuntime.get("empty")));
    for (const std::string &code : {"IO.Terminal.readKey()", "IO.Terminal.readKey(-1)", "IO.Terminal.readKey(1001)"}) {
        bool rejected = false; try { terminalRuntime.run(code, "terminal-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    bool terminalDenied = false; try { runtime.run("IO.Terminal.open()", "terminal-disabled.k3"); } catch (const dk3::Error &error) { terminalDenied = error.category == "capability"; }
    assert(terminalDenied);
    size_t polls = 0;
    terminalConfig.readKey = [&](uint64_t timeout) -> std::optional<uint8_t> { assert(timeout == 0); if (++polls < 4) return {}; return uint8_t{113}; };
    dk3::Runtime polledRuntime(terminalConfig);
    polledRuntime.run("IO.Terminal.open(); var ticks: Int = 0; fn worker() { while true { ticks += 1; yield null } }; let work = Tasks.spawn(worker()); var done = false; while not done { let key = IO.Terminal.readKey(0); if key == 113 { done = true } else { Tasks.pump(1) } }; Tasks.cancel(work); IO.Terminal.close()", "polled-tasks.k3");
    assert(polls == 4 && std::get<int64_t>(polledRuntime.get("ticks")) == 3 && !rawEnabled);
    runtime.run("let objectInt: Object = 3\nlet objectText: Object = 'text'\nlet objectNull: Object? = null\nclass RootChild extends Object {}\nlet objectInstance: Object = RootChild()\nSystem.print(Reflection.type(Object), Reflection.type(objectInt), Reflection.type(objectInstance))", "object-root.k3");
    assert(output == "Type Int RootChild\n"); output.clear();
    runtime.run("from System import Object as ValueObject\nlet aliasedObject: ValueObject = 1\nlet qualifiedObject: System.Object = 'value'", "object-aliases.k3");
    dk3::Config ioConfig; std::string ioOutput; size_t inputCount = 0; uint64_t delayed = 0;
    ioConfig.write = [&](const std::string &value) { ioOutput += value; };
    ioConfig.readLine = [&]() -> std::optional<std::string> { if (inputCount++) return {}; return "Ada"; };
    ioConfig.monotonicMillis = [] { return uint64_t{42}; }; ioConfig.delay = [&](uint64_t value) { delayed = value; };
    dk3::Runtime ioRuntime(ioConfig);
    ioRuntime.run("let line: String? = IO.readLine(); IO.write(f'Hello {line}'); System.print(IO.readLine(), System.monotonicMillis()); System.delay(3)", "io-capabilities.k3");
    assert(ioOutput == "Hello Adanull 42\n" && delayed == 3);
    for (const std::string &code : {"IO.readLine()", "System.monotonicMillis()", "System.delay(0)"}) {
        bool denied = false; try { runtime.run(code, "io-disabled.k3"); } catch (const dk3::Error &error) { denied = error.category == "capability"; }
        assert(denied);
    }
    runtime.run("try { [1][9] } catch error { let kind: Error.Kind = error.kind; System.print(kind == Error.Kind.Index, Reflection.type(kind)) }\nlet enumError = Error(Error.Kind.Application, 'message')\nSystem.print(enumError.kind == Error.Kind.Application)", "error-kinds.k3");
    assert(output == "true Error.Kind\ntrue\n"); output.clear();
    runtime.run("let kindMessage = match Error.Kind.Index { Error.Kind.Index => 'index matched', _ => 'other' }\nSystem.print(kindMessage)", "error-kind-match.k3");
    assert(output == "index matched\n"); output.clear();
    runtime.run("let formattedName = 'Ada'\nvar formattedCount = 0\nfn formattedNext() -> Int { formattedCount += 1; System.gc(); return formattedCount }\nSystem.print(f'Hello {formattedName}: {1 + 2}, {{literal}}, {formattedNext()} {formattedNext()}')\nSystem.print(f\"{ {'value': 4}['value'] }\")", "formatted-strings.k3");
    assert(output == "Hello Ada: 3, {literal}, 1 2\n4\n"); output.clear();
    runtime.run("System.print(f'\\x7bescaped\\x7d')", "format-escapes.k3");
    assert(output == "{escaped}\n"); output.clear();
    runtime.run("class Indexed { private let values: Map[String, Int] = {}; fn __getitem__(key: String) -> Int { System.gc(); return self.values.get(key) ?? 0 }; fn __setitem__(key: String, value: Int) -> Null { self.values[key] = value } }\nlet indexed = Indexed()\nindexed['score'] = 4\nindexed['score'] += 3\nSystem.print(indexed['score'])", "index-hooks.k3");
    assert(output == "7\n"); output.clear();
    runtime.run("var indexedKeyCalls = 0\nvar indexedReceiverCalls = 0\nfn onceReceiver() -> Indexed { indexedReceiverCalls += 1; return indexed }\nfn onceKey() -> String { indexedKeyCalls += 1; return 'score' }\nonceReceiver()[onceKey()] += 1\nSystem.print(indexedReceiverCalls, indexedKeyCalls, indexed['score'])\nlet namedError = Error(message: 'legacy', kind: 'type')\nSystem.print(namedError.kind == Error.Kind.Type)", "foundation-boundaries.k3");
    assert(output == "1 1 8\ntrue\n"); output.clear();
    for (const std::string &code : {"class WrongSetter { fn __setitem__(key, value) { return 1 } }; WrongSetter()[0] = 1"}) {
        bool rejected = false; try { runtime.run(code, "index-hook-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("class Printable { private let label: String; constructor(label: String) -> Null { self.label = label }; public fn toString() -> String { System.gc(); return 'item:' + self.label } }\nlet printable = Printable('Ada')\nlet converted = Text.str(printable)\nSystem.print(converted, printable, printable.toString())\nclass PrintableChild extends Printable {}\nSystem.print(PrintableChild('Grace'))", "to-string.k3");
    assert(output == "item:Ada item:Ada item:Ada\nitem:Grace\n"); output.clear();
    runtime.run("record VectorOperator { x: Int, y: Int; fn toString() -> String { return Text.str(self.x) + ',' + Text.str(self.y) }; fn __add__(other: VectorOperator) -> VectorOperator { System.gc(); return VectorOperator(self.x + other.x, self.y + other.y) }; fn __mul__(amount: Int) -> VectorOperator { return VectorOperator(self.x * amount, self.y * amount) }; fn __neg__() -> VectorOperator { return VectorOperator(-self.x, -self.y) }; fn __lt__(other: VectorOperator) -> Bool { return self.x < other.x } }\nlet firstVector = VectorOperator(1,2)\nlet secondVector = VectorOperator(3,4)\nSystem.print(firstVector + secondVector, firstVector * 3, -firstVector, firstVector < secondVector)\nvar compoundVector = firstVector\ncompoundVector += secondVector\nSystem.print(compoundVector)\nclass OperatorBase { let value: Int; fn __add__(other: Int) -> Int { return self.value + other } }\nclass OperatorChild extends OperatorBase {}\nSystem.print(OperatorChild(5) + 2)", "operators.k3");
    assert(output == "4,6 3,6 -1,-2 true\n4,6\n7\n"); output.clear();
    for (const std::string &code : {"class WrongPredicate { fn __lt__(other) { return 1 } }; WrongPredicate() < WrongPredicate()", "class PrivateOperator { private fn __add__(other) {} }", "class GeneratorOperator { fn __add__(other) { yield 1 } }", "firstVector + 1", "2 * firstVector"}) {
        bool rejected = false; try { runtime.run(code, "operator-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"class WrongString { fn toString() { return 1 } }; Text.str(WrongString())", "class RecursiveString { fn toString() -> String { return Text.str(self) } }; Text.str(RecursiveString())"}) {
        bool rejected = false; try { runtime.run(code, "to-string-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("class VaultAccess { private var secret: Int = 3; protected var shared: Int = 2; private static let code: Int = 7; public fn read() -> Int { return self.secret + VaultAccess.code }; public fn callback(callback: Function) { callback(self) }; public fn closure() { return fn() -> Int { System.gc(); return self.secret } } }\nclass ChildAccess extends VaultAccess { fn readShared() -> Int { self.shared += 1; return self.shared } }\nlet vaultAccess = ChildAccess()\nlet privateClosure = vaultAccess.closure()\nSystem.gc()\nSystem.print(vaultAccess.read(), vaultAccess.readShared(), privateClosure())", "access-runtime.k3");
    assert(output == "10 3 3\n"); output.clear();
    runtime.run("System.print(Reflection.members(vaultAccess).contains('secret'), Reflection.members(vaultAccess).contains('shared'))\nclass FactoryAccess { private let value: Int; private constructor(value: Int) -> Null { self.value = value }; public static fn create(value: Int) -> FactoryAccess { return FactoryAccess(value) }; public fn read() -> Int { return self.value } }\nlet factoryAccess = FactoryAccess.create(8)\nSystem.gc()\nSystem.print(factoryAccess.read(), Text.str(factoryAccess))", "access-reflection-factory.k3");
    assert(output == "false false\n8 FactoryAccess()\n"); output.clear();
    runtime.run("class AccessDefaults { private static let seed: Int = 5; private let value: Int = AccessDefaults.seed; fn read() -> Int { return self.value }; fn stream() { yield self.value; System.gc(); yield self.value + 1 } }\nlet accessDefaults = AccessDefaults()\nclass AccessDefaultChild extends AccessDefaults {}\nlet accessDefaultChild = AccessDefaultChild()\nSystem.print(accessDefaults.read(), accessDefaultChild.read())\nfor item in accessDefaults.stream() { System.print(item) }\ntrait AccessImpl { fn read() -> Int }\nclass ImplAccess { private let value: Int = 11 }\nimpl AccessImpl for ImplAccess { fn read() -> Int { return self.value } }\nSystem.print(ImplAccess().read())", "access-defaults-generators.k3");
    assert(output == "5 5\n5\n6\n11\n"); output.clear();
    for (const std::string &code : {"vaultAccess.secret", "vaultAccess.shared = 1", "VaultAccess.code", "FactoryAccess(1)", "vaultAccess.callback(fn(value) { return value.secret })", "class BadPrivateChild extends VaultAccess { fn reveal() { return self.secret } }; BadPrivateChild().reveal()"}) {
        bool rejected = false; try { runtime.run(code, "access-errors.k3"); } catch (const dk3::Error &error) { rejected = error.category == "access"; }
        assert(rejected);
    }
    runtime.run("class CustomPlayer { let name: String; var level: Int = 1; fn init(name: String, level: Int = 1) -> Null { self.name = name; self.level = level; System.gc() } }\nclass CustomHero extends CustomPlayer { let title: String; fn init(name: String, title: String) -> Null { super.init(name, 5); self.title = title } }\nlet custom = CustomHero('Ada', 'Explorer')\nSystem.print(custom.name, custom.level, custom.title)", "custom-init.k3");
    assert(output == "Ada 5 Explorer\n"); output.clear();
    runtime.run("class DefaultConstructed { let value: Int; constructor() -> Null { self.value = 42 } }\nSystem.print(DefaultConstructed().value)", "zero-constructor.k3");
    assert(output == "42\n"); output.clear();
    runtime.run("class TaggedList extends Collections.List { var tag: String = 'items'; fn append(value: Int) -> Null { super.append(value * 2) } }\nlet tagged = TaggedList([1,2])\ntagged.append(3)\nlet typedTagged: List[Int] = tagged\nSystem.gc()\nSystem.print(tagged.length(), tagged[2], tagged.tag, Reflection.type(Collections.List))\nclass CountMap extends Map { fn count(key: String) -> Int { return self.compute(key, fn(key, old) => (old ?? 0) + 1) ?? 0 } }\nlet counted = CountMap()\nSystem.print(counted.count('x'), counted['x'])", "collection-subclasses.k3");
    assert(output == "3 6 items Type\n1 1\n"); output.clear();
    runtime.run("let unique: Set[Int] = Set([1,2,2,3])\nSystem.print(unique.length(), unique.add(2), unique.add(4), unique.remove(1))\nSystem.print(unique.values(), unique.union(Set([4,5])).values(), unique.intersection(Set([3,4])).values(), unique.difference(Set([3])).values())\nSystem.print(Set([2]).isSubsetOf(unique))\nclass NamedSet extends Set { let label: String = 'unique' }\nlet namedSet = NamedSet([1,1,2])\nfor value in namedSet { System.print(value) }\nSystem.print(namedSet.label, Reflection.type(Collections.Set))", "sets.k3");
    assert(output == "3 false true true\n[2, 3, 4] [2, 3, 4, 5] [3, 4] [2, 4]\ntrue\n1\n2\nunique Type\n"); output.clear();
    runtime.run("from Collections import List as Sequence\nlet qualified: Collections.List[Int] = Sequence([1,2])\nlet aliasCollection: Sequence[Int] = qualified\nclass InitializedList extends List { let label: String; fn init(label: String, source: List[Int]) -> Null { super.init(source); self.label = label } }\nlet initializedList = InitializedList('numbers', qualified)\nSystem.gc()\nSystem.print(initializedList.label, initializedList[1])", "collection-qualified-init.k3");
    assert(output == "numbers 2\n"); output.clear();
    runtime.run("class MeasuredList extends List { fn length() -> Int { return super.length() + 10 } }\nlet measuredList = MeasuredList([1,2])\nSystem.print(measuredList.length(), Collections.len(measuredList), measuredList[1])\nSystem.print(Reflection.members(initializedList).contains('label'))", "collection-reflection.k3");
    assert(output == "12 12 2\ntrue\n"); output.clear();
    runtime.run("let convertedSet = List(unique)\nlet convertedTuple = Tuple(unique)\nlet convertedBack = Set(convertedTuple)", "collection-conversions.k3");
    for (const std::string &code : {"class UnreadBase { let value: Int }; class UnreadChild extends UnreadBase { constructor() -> Null { self.value = super.value } }; UnreadChild()"}) {
        bool rejected = false; try { runtime.run(code, "collection-final-safety.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"Tuple([1]).init([2])"}) {
        bool rejected = false; try { runtime.run(code, "set-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"class MissingInit { let value: Int; fn init(input: Int) -> Null {} }; MissingInit(1)", "class TwiceInit { let value: Int; fn init(input: Int) -> Null { self.value = input; self.value = input } }; TwiceInit(1)"}) {
        bool rejected = false; try { runtime.run(code, "custom-init-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("class StaticCounter { static var count: Int = 0; static let label: String = 'counter'; var value: Int = 0; static fn create(value: Int) -> StaticCounter { StaticCounter.count += 1; return StaticCounter(value) }; fn read() -> Int { return self.value } }\nlet staticFactory = StaticCounter.create\nSystem.gc()\nlet staticFirst = staticFactory(3)\nlet staticSecond = StaticCounter.create(5)\nSystem.print(StaticCounter.count, StaticCounter.label, staticFirst.read(), staticSecond.read())", "static-members.k3");
    assert(output == "2 counter 3 5\n"); output.clear();
    runtime.run("class OpenBase { static var shared: Int = 0; var value: Int; fn read() -> Int { return self.value } }\nclass OpenChild extends OpenBase { let label: String = 'child'; fn read() -> Int { System.gc(); return self.value * 2 } }\nlet openChild: OpenBase = OpenChild(3)\nlet inheritedRead = openChild.read\nSystem.gc()\nOpenChild.shared += 1\nSystem.print(inheritedRead(), OpenBase.shared, openChild.value)\nopenChild.value = 4\nSystem.print(openChild.read())", "class-inheritance.k3");
    assert(output == "6 1 3\n8\n"); output.clear();
    runtime.run("class SuperChild extends OpenBase { fn read() -> Int { System.gc(); return super.read() + 10 } }\nlet superChild = SuperChild(2)\nSystem.print(superChild.read())\nclass SuperGrandchild extends SuperChild { fn read() -> Int { return super.read() + 100 } }\nSystem.print(SuperGrandchild(2).read())\nclass BaseInit { var value: Int = 0; fn init() -> Null { self.value += 1 } }\nclass ChildInit extends BaseInit { fn init() -> Null { super.init(); self.value += 10 } }\nSystem.print(ChildInit().value)", "super-dispatch.k3");
    assert(output == "12\n112\n11\n"); output.clear();
    runtime.run("class SuperFields extends OpenBase { fn read() -> Int { super.value += 1; return super.value } }\nlet superFields = SuperFields(4)\nSystem.print(superFields.read(), superFields.value)", "super-fields.k3");
    assert(output == "5 5\n"); output.clear();
    runtime.run("trait InheritedReadable { fn read() -> Int }\nclass TraitParent implements InheritedReadable { var value: Int = 2; fn read() -> Int { return self.value } }\nclass TraitChild extends TraitParent {}\nlet inheritedTrait: InheritedReadable = TraitChild()\nlet baseDefault: Int = 4\nclass LexicalParent { var value: Int = baseDefault }\n{ let baseDefault: String = 'different scope'; class LexicalChild extends LexicalParent {}; System.print(LexicalChild().value) }\nSystem.print(inheritedTrait.read())\nfn localFactory() { class LocalBase { static var count: Int = 0; static fn next() -> Int { LocalBase.count += 1; return LocalBase.count }; fn read() -> Int { return 3 } }; class LocalChild extends LocalBase {}; return (LocalChild.next, LocalChild().read) }\nlet localMethods = localFactory()\nSystem.gc()\nSystem.print(localMethods[0](), localMethods[1]())", "inheritance-lifetimes.k3");
    assert(output == "4\n2\n1 3\n"); output.clear();
    dk3::Config inheritedConfig; std::string inheritedOutput;
    inheritedConfig.write = [&](const std::string &text) { inheritedOutput += text; };
    inheritedConfig.resolveModule = [](const std::string &name) {
        return name == "base_model" ? "record Token { value: Int }; let seed: Int = 7; export class Parent { var token: Token = Token(seed); fn read() -> Int { return self.token.value } }" : "";
    };
    dk3::Runtime inheritedRuntime(inheritedConfig);
    inheritedRuntime.run("import base_model\nclass ImportedChild extends base_model.Parent {}\nlet imported: base_model.Parent = ImportedChild()\nSystem.gc()\nSystem.print(imported.read())", "imported-inheritance.k3");
    assert(inheritedOutput == "7\n");
    runtime.run("trait LateTrait { fn read() -> Int }\nclass LateBase { fn read() -> Int { return 9 } }\nclass LateChild extends LateBase {}\nimpl LateTrait for LateBase {}\nlet late: LateTrait = LateChild()\nSystem.print(late.read())", "late-inherited-trait.k3");
    assert(output == "9\n"); output.clear();
    for (const std::string &code : {"class BadBase extends Int {}", "class BadField extends OpenBase { var value: Int }"}) {
        bool rejected = false; try { runtime.run(code, "inheritance-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"StaticCounter.create = fn() {}", "staticFirst.create(1)", "StaticCounter.read()", "class BadStatic { static fn read() { return self } }\nBadStatic.read()"}) {
        bool rejected = false; try { runtime.run(code, "static-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("trait BothNamed { fn name() -> String }\nclass BothBag implements BothNamed, Collections.Sized { fn name() -> String { return 'bag' }; fn length() -> Int { return 3 } }\nfn describeBoth(value: Collections.Sized & BothNamed) -> String { System.gc(); return value.name() + Text.str(value.length()) }\nlet both: Collections.Sized & BothNamed = BothBag()\nlet bothValues: List[Collections.Sized & BothNamed] = [both]\nlet bothAlias: List[BothNamed & Collections.Sized] = bothValues\nlet optionalBoth: (Collections.Sized & BothNamed)? = null\nSystem.print(describeBoth(both), bothValues[0].length())", "intersection-runtime.k3");
    assert(output == "bag3 3\n"); output.clear();
    for (const std::string &code : {"describeBoth([1])"}) {
        bool rejected = false; try { runtime.run(code, "intersection-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let computedMap: Map[String, Int?] = {'empty': null, 'old': 2}\nvar computeCalls = 0\nfn supply(key: String) -> Int { computeCalls += 1; System.gc(); return 7 }\nSystem.print(computedMap.computeIfAbsent('old', supply), computedMap.computeIfAbsent('empty', supply), computeCalls)\nSystem.print(computedMap.computeIfPresent('missing', fn(key, value) => value + 1))\nSystem.print(computedMap.compute('new', fn(key, value) => (value ?? 0) + 3))\nSystem.print(computedMap.computeIfPresent('old', fn(key, value) => value * 2))\nSystem.print(computedMap.putIfAbsent('old', 99), computedMap.putIfAbsent('added', 5))\nSystem.print(computedMap.merge('old', 3, fn(old, added) => old + added))\ncomputedMap.compute('old', fn(key, value) => null)\nSystem.print(computedMap.contains('old'))", "map-compute.k3");
    assert(output == "2 7 1\nnull\n3\n4\n4 null\n7\nfalse\n"); output.clear();
    runtime.run("let mutatingCompute: Map[String, List[Int]] = {'x': [1]}\nlet replaced = mutatingCompute.compute('x', fn(key: String, old: List[Int]?) -> List[Int] { mutatingCompute.clear(); System.gc(); return (old ?? []) + [2] })\nSystem.print(replaced, mutatingCompute['x'])\nvar failedComputed = 0\nlet stableCompute: Map[String, Int] = {'x': 3}\ntry { stableCompute.compute('x', fn(key, old) { throw 'no result' }) } catch error { failedComputed += 1 }\nSystem.print(stableCompute['x'], failedComputed)\nlet nullCompute: Map[String, Int?] = {'x': null}\nSystem.print(nullCompute.computeIfPresent('x', fn(key, old) => 5), nullCompute.contains('x'))\nnullCompute.computeIfAbsent('x', fn(key) => null)\nSystem.print(nullCompute.contains('x'))", "map-compute-lifetimes.k3");
    assert(output == "[1, 2] [1, 2]\n3 1\nnull true\ntrue\n"); output.clear();
    for (const std::string &code : {"stableCompute.merge('x', null, fn(old, added) => added)", "stableCompute.computeIfAbsent('x', 1)"}) {
        bool rejected = false; try { runtime.run(code, "map-intersection-boundaries.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("Random.seed(42)\nlet firstRandom = Random.int(-10, 10)\nlet firstFloat = Random.float()\nRandom.seed(42)\nSystem.print(firstRandom == Random.int(-10, 10), firstFloat == Random.float())\nlet deck: List[Int] = [1, 2, 3, 4]\nRandom.seed(7)\nRandom.shuffle(deck)\nlet shuffledCopy = List(deck)\nlet secondDeck: List[Int] = [1, 2, 3, 4]\nRandom.seed(7)\nRandom.shuffle(secondDeck)\nSystem.print(Json.stringify(shuffledCopy) == Json.stringify(secondDeck), deck.length())\nSystem.print(Random.int(1), Random.int(-2, -1), Random.choice(('only',)), Random.choice('x'), Random.choice(b'A'))\nlet chosenFloat = Random.float()\nSystem.print(chosenFloat >= 0.0 and chosenFloat < 1.0)", "random.k3");
    assert(output == "true true\ntrue 4\n0 -2 only x 65\ntrue\n"); output.clear();
    for (const std::string &code : {"Random.int(0)", "Random.int(2, 1)", "Random.int(1.0)", "Random.choice([])", "Random.choice({})", "Random.shuffle((1, 2))", "Random.seed(true)"}) {
        bool rejected = false; try { runtime.run(code, "random-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    dk3::Runtime randomFirst, randomSecond;
    randomFirst.run("let draw = Random.int(100000)"); randomSecond.run("let draw = Random.int(100000)");
    assert(dk3::Runtime::equal(randomFirst.get("draw"), randomSecond.get("draw")));
    runtime.run("Random.seed(-1)\nlet wideRandom = Random.int(-9223372036854775808, 9223372036854775807)\nSystem.print(wideRandom >= -9223372036854775808 and wideRandom < 9223372036854775807)\nSystem.print(Random.choice('\\xc3\\xa9'))\nRandom.shuffle([])\nRandom.shuffle([1])", "random-boundaries.k3");
    assert(output == "true\n\xc3\xa9\n"); output.clear();
    dk3::Config callbackBudget; callbackBudget.stepLimit = 30;
    dk3::Runtime limitedCallbacks(callbackBudget);
    limitedCallbacks.run("let budgeted = []\nvar callbackCount = 0\nRandom.seed(9)", "callback-budget-setup.k3");
    auto budgeted = std::get<dk3::Object *>(limitedCallbacks.get("budgeted"));
    budgeted->items.assign(100, int64_t{1}); budgeted->items.front() = int64_t{2};
    for (const std::string &code : {"budgeted.map(fn(value) { callbackCount += 1; return value })", "Random.shuffle(budgeted)"}) {
        bool limited = false; try { limitedCallbacks.run(code, "callback-budget.k3"); } catch (const dk3::Error &error) { limited = error.category == "limit"; }
        assert(limited && std::get<int64_t>(budgeted->items.front()) == 2);
    }
    assert(std::get<int64_t>(limitedCallbacks.get("callbackCount")) == 0);
    limitedCallbacks.run("let nextDraw = Random.int(10000)");
    dk3::Runtime shuffleControl; shuffleControl.run("Random.seed(9)\nlet nextDraw = Random.int(10000)");
    assert(dk3::Runtime::equal(limitedCallbacks.get("nextDraw"), shuffleControl.get("nextDraw")));
    runtime.run("let mapSource: List[Int] = [1, 2, 3]\nlet mapped: List[Int] = mapSource.map(fn(value: Int) -> Int { mapSource.clear(); System.gc(); return value * 2 })\nSystem.print(mapped, mapSource)\nlet filterSource: List[Int] = [1, 2, 3, 4]\nlet filtered = filterSource.filter(fn(value: Int) -> Bool { System.gc(); return value % 2 == 0 })\nSystem.print(filtered)\nlet retainedFilter = filterSource.filter\nSystem.gc()\nSystem.print(retainedFilter(fn(value: Int) -> Bool => value > 2))", "callback-lists.k3");
    assert(output == "[2, 4, 6] []\n[2, 4]\n[3, 4]\n"); output.clear();
    runtime.run("let reduced: Int = [1, 2, 3].reduce(fn(total: Int, value: Int) -> Int { System.gc(); return total + value }, 10)\nSystem.print(reduced, [].reduce(fn(total, value) => total, 7))\nvar visited = 0\nfn positive(value: Int) -> Bool { visited += 1; return value > 1 }\nSystem.print([1, 2, 3].any(positive), visited)\nvisited = 0\nSystem.print([1, 2, 3].all(positive), visited)\nSystem.print([].any(positive), [].all(positive))\nSystem.print([1, 2, 3].find(positive), [1].find(positive))\nSystem.print([null, 2].findIndex(fn(value) -> Bool => value == null), [].findIndex(positive))\nlet folded: List[Int] = [1, 2].reduce(fn(total: List[Int], value: Int) -> List[Int] { System.gc(); return total + [value] }, [])\nSystem.print(folded)", "callback-queries.k3");
    assert(output == "16 7\ntrue 2\nfalse 1\nfalse true\n2 null\n0 -1\n[1, 2]\n"); output.clear();
    runtime.run("var partialMap = [99]\nvar callbackEffects = 0\ntry { partialMap = [1,2,3].map(fn(value: Int) -> Int { callbackEffects += 1; if value == 2 { throw 'stop' }; return value }) } catch error {}\nSystem.print(partialMap, callbackEffects)\nclass CallbackCounter { var calls: Int = 0; fn visit(value: Int) -> Int { self.calls += 1; System.gc(); return value + self.calls } }\nlet callbackCounter = CallbackCounter()\nlet detachedCallback = callbackCounter.visit\nSystem.gc()\nSystem.print([1,2].map(detachedCallback))\nlet emptyFiltered = filterSource.filter(fn(value: Int) -> Bool => false)", "callback-failure-lifetimes.k3");
    assert(output == "[99] 2\n[2, 4]\n"); output.clear();
    for (const std::string &code : {"[1].filter(fn(value) => 1)", "[1].any(fn(value) => null)", "[1].reduce(fn(value) => value, 0)", "[].map(1)", "[1].map(callback: fn(value) => value)"}) {
        bool rejected = false; try { runtime.run(code, "callback-list-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let parsedJson = Json.parse('{\"value\":9007199254740993,\"items\":[null,true,1.25],\"text\":\"a\\\\u0000b\"}')\nSystem.gc()\nSystem.print(parsedJson['value'], parsedJson['items'][1], parsedJson['text'].length())\nlet encodedJson = Json.stringify(parsedJson)\nlet jsonRoundTrip = Json.parse(encodedJson)\nSystem.print(jsonRoundTrip['value'], Json.stringify((1, 2)))", "json.k3");
    assert(output == "9007199254740993 true 3\n9007199254740993 [1,2]\n"); output.clear();
    runtime.run("System.print(Json.parse('-9223372036854775808'), Json.parse('9223372036854775807'))\nlet unicodeJson = Json.parse('\"\\\\uD83D\\\\uDE00\"')\nSystem.print(unicodeJson.length())\nlet aliasJson = [1]\nSystem.print(Json.stringify([aliasJson, aliasJson]))\nlet typedJson: Map[String, List[Int]] = Json.parse('{\"values\":[1,2]}')\ntypedJson['values'].append(3)\nSystem.gc()", "json-boundaries.k3");
    assert(output == "-9223372036854775808 9223372036854775807\n1\n[[1],[1]]\n"); output.clear();
    std::string deepJson(65, '['); deepJson += '0'; deepJson += std::string(65, ']');
    runtime.set("deepJson", runtime.text(deepJson, false, dk3::Span{}));
    for (const std::string &code : {"Json.parse(deepJson)", "Json.parse('-9223372036854775809')", "Json.parse('1e9999')", "Json.parse('[1,]')", "Json.parse('null true')", "Json.parse('{\"a\":1,\"\\\\u0061\":2}')", "{ record String {}; Json.stringify({String(): 1}) }"}) {
        bool rejected = false; try { runtime.run(code, "json-boundary-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    dk3::Config jsonBudget; jsonBudget.stepLimit = 25;
    dk3::Runtime limitedJson(jsonBudget);
    limitedJson.run("let input = '[]'", "json-budget-setup.k3");
    std::get<dk3::Object *>(limitedJson.get("input"))->text = "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]";
    bool jsonLimited = false;
    try { limitedJson.run("Json.parse(input)", "json-budget.k3"); } catch (const dk3::Error &error) { jsonLimited = error.category == "limit"; }
    assert(jsonLimited);
    runtime.set("oversizedJson", runtime.text(std::string(4 * 1024 * 1024 + 1, 'x'), false, dk3::Span{}));
    for (const std::string &code : {"Json.parse(oversizedJson)", "Json.stringify(oversizedJson)", "{ var nested = []; for index in Collections.range(65) { nested = [nested] }; Json.stringify(nested) }", "{ class JsonRecord {}; Json.stringify(JsonRecord()) }"}) {
        bool rejected = false; try { runtime.run(code, "json-resource-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"Json.parse('{')", "Json.parse('{\"x\":1,\"x\":2}')", "Json.parse('9223372036854775808')", "Json.parse('18446744073709551616')", "Json.parse(b'null')", "Json.stringify(b'bytes')", "Json.stringify({1: 2})", "Json.stringify(fn() {})", "{ let cycle = []; cycle.append(cycle); Json.stringify(cycle) }"}) {
        bool rejected = false; try { runtime.run(code, "json-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    bool filesDisabled = false;
    try { runtime.run("Files.readText('test.txt')", "files-disabled.k3"); }
    catch (const dk3::Error &error) { filesDisabled = error.category == "capability"; }
    assert(filesDisabled);
    dk3::Config fileConfig; fileConfig.maxFileBytes = 8;
    fileConfig.readFile = [](const std::string &path, size_t limit) {
        assert(limit == 8);
        if (path == "text") return std::string("a\xc3\xa9\0b", 5);
        if (path == "binary") return std::string("\xff\0", 2);
        if (path == "large") return std::string(9, 'x');
        throw std::runtime_error("Cannot read requested file");
    };
    fileConfig.fileSize = [](const std::string &path) -> std::optional<uint64_t> {
        if (path == "text") return 5;
        if (path == "huge") return UINT64_MAX;
        return {};
    };
    dk3::Runtime fileRuntime(fileConfig);
    fileRuntime.run("let text = Files.readText('text')\nlet bytes = Files.readBytes('binary')\nlet present = Files.exists('text')\nlet absent = Files.exists('missing')\nlet byteSize = Files.size('text')\nSystem.gc()", "files-provider.k3");
    assert(dk3::Runtime::display(fileRuntime.get("text")) == std::string("a\xc3\xa9\0b", 5));
    assert(dk3::Runtime::typeOf(fileRuntime.get("bytes")) == "Bytes");
    assert(std::get<bool>(fileRuntime.get("present")) && !std::get<bool>(fileRuntime.get("absent")));
    assert(std::get<int64_t>(fileRuntime.get("byteSize")) == 5);
    for (const std::string &code : {"Files.readText('binary')", "Files.readBytes('large')", "Files.readText('missing')", "Files.size('missing')", "Files.size('huge')", "Files.readText('')", "Files.readText('a\\0b')", "Files.readText(b'text')"}) {
        bool rejected = false; try { fileRuntime.run(code, "files-provider-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let searchText = 'a\\xc3\\xa9b'\nlet textContains = searchText.contains\nSystem.gc()\nSystem.print(textContains('\\xc3\\xa9'), textContains(''), textContains('missing'))", "string-contains.k3");
    assert(output == "true true false\n"); output.clear();
    runtime.run("let prefix = searchText.startsWith\nSystem.gc()\nSystem.print(prefix('a'), prefix('a\\xc3\\xa9'), prefix(''), prefix('longer than source'), prefix('b'))", "string-prefix.k3");
    assert(output == "true true true false false\n"); output.clear();
    runtime.run("let suffix = searchText.endsWith\nSystem.gc()\nSystem.print(suffix('b'), suffix('\\xc3\\xa9b'), suffix(''), suffix('longer than source'), suffix('a'))\nSystem.print(''.startsWith(''), ''.endsWith(''))", "string-suffix.k3");
    assert(output == "true true true false false\ntrue true\n"); output.clear();
    runtime.run("let find = searchText.find\nSystem.gc()\nSystem.print(find('b'), find('\\xc3\\xa9'), find(''), find('missing'))\nSystem.print('\\xf0\\x9f\\x98\\x80x'.find('x'), 'aba'.find('a'))", "string-find.k3");
    assert(output == "2 1 0 -1\n1 0\n"); output.clear();
    runtime.run("let split = 'a,,b,'.split\nSystem.gc()\nlet splitParts: List[String] = split(',')\nSystem.print(splitParts.length(), splitParts[1].length(), splitParts[3].length())\nSystem.print('x\\xc3\\xa9y\\xc3\\xa9'.split('\\xc3\\xa9'))\nSystem.print(''.split(',').length(), 'abc'.split('missing').length())", "string-split.k3");
    assert(output == "4 0 0\n[x, y, ]\n1 1\n"); output.clear();
    runtime.run("let join = '|'.join\nSystem.gc()\nSystem.print(join(['a', '', 'b']), ','.join(('x', 'y')), '-'.join('ab'))\nSystem.print(''.join(['a', 'b']), 'separator'.join([]).length())\nfn joinedSource() { yield 'first'; System.gc(); yield 'second' }\nSystem.print(join(joinedSource()))\nSystem.print(','.join('a,,b,'.split(',')))", "string-join.k3");
    assert(output == "a||b x,y a-b\nab 0\nfirst|second\na,,b,\n"); output.clear();
    runtime.run("var failedJoinResult = 'unchanged'\nfn failingStrings() { defer System.print('join cleanup'); yield 'a'; throw 'join failed' }\ntry { failedJoinResult = ','.join(failingStrings()) } catch error { System.print(failedJoinResult) }\nSystem.print('a\\0b'.find('\\0'), '|'.join('a\\0b'.split('\\0')))\nlet retainedParts = 'first|second'.split('|')\nSystem.gc()\nSystem.print(retainedParts[1])", "string-lifetimes.k3");
    assert(output == "join cleanup\nunchanged\n1 a|b\nsecond\n"); output.clear();
    dk3::Config stringBudget; stringBudget.stepLimit = 100;
    dk3::Runtime limitedStrings(stringBudget);
    limitedStrings.run("let source = 'x'\nfn endlessStrings() { while true { yield 'x' } }", "string-budget-setup.k3");
    std::get<dk3::Object *>(limitedStrings.get("source"))->text.assign(200, 'x');
    for (const std::string &code : {"source.contains('z')", "source.find('z')", "source.split(',')", "','.join(endlessStrings())"}) {
        bool limited = false; try { limitedStrings.run(code, "string-budget.k3"); } catch (const dk3::Error &error) { limited = error.category == "limit"; }
        assert(limited);
    }
    for (const std::string &code : {"','.join([1])", "','.join(b'ab')", "','.join({})", "','.join()", "','.join(values: ['a'])", "{ record String {}; ','.join([String()]) }"}) {
        bool rejected = false; try { runtime.run(code, "join-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"'abc'.split('')", "'abc'.split(b',')", "'abc'.split()"}) {
        bool rejected = false; try { runtime.run(code, "split-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"searchText.contains(b'a')", "searchText.contains(1)", "searchText.contains()", "searchText.contains(needle: 'a')", "{ record String {}; searchText.contains(String()) }"}) {
        bool rejected = false; try { runtime.run(code, "string-contains-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("fn() { System.print('immediate') }()\nlet anonAdd: Function = fn(first: Int, second: Int = 2) -> Int => first + second\nSystem.print(anonAdd(second: 4, first: 3))\nlet anonRest = fn(first: Int, *rest) -> Int { return first + rest.length() }\nSystem.print(anonRest(1, 2, 3))\nfn anonFactory(value: Int) { return fn(addition: Int) -> Int => value + addition }\nlet anonFive = anonFactory(5)\nSystem.gc()\nSystem.print(anonFive(10))\nlet anonGenerator = fn() { defer System.print('anonymous cleanup'); yield null; yield 2 }\nfor value in anonGenerator() { System.print(value) }\nlet anonDocumented = fn() -> Int { \"\"\"Anonymous documentation.\"\"\"\nreturn 1 }\nSystem.print(Reflection.doc(anonDocumented))", "anonymous-functions.k3");
    assert(output == "immediate\n7\n3\n15\nnull\n2\nanonymous cleanup\nAnonymous documentation.\n"); output.clear();
    runtime.run("let searchable: List[Int?] = [1, null, 1]\nlet listContains = searchable.contains\nSystem.gc()\nSystem.print(listContains(1), listContains(null), listContains(2))", "list-contains.k3");
    assert(output == "true true false\n"); output.clear();
    runtime.run("let indexOf = searchable.indexOf\nSystem.gc()\nSystem.print(indexOf(1), indexOf(null), indexOf(2))\nrecord SearchPoint { value: Int }\nlet searchPoints: List[SearchPoint] = [SearchPoint(2)]\nSystem.print(searchPoints.indexOf(SearchPoint(2)))", "list-indexof.k3");
    assert(output == "0 1 -1\n0\n"); output.clear();
    runtime.run("let clearable: List[Int] = [1, 2]\nlet listAlias = clearable\nlet listCopy = List(clearable)\nlet clearList = clearable.clear\nSystem.gc()\nclearList()\nclearList()\nSystem.print(listAlias.length(), listCopy.length())\nclearable.append(3)", "list-clear.k3");
    assert(output == "0 2\n"); output.clear();
    runtime.run("let reversible: List[Int] = [1, 2, 3]\nlet reverse = reversible.reverse\nlet reverseAlias = reversible\nSystem.gc()\nreverse()\nSystem.print(reverseAlias)\nreverse()\nSystem.print(reversible)\nlet reverseEmpty: List[Int] = []\nreverseEmpty.reverse()", "list-reverse.k3");
    assert(output == "[3, 2, 1]\n[1, 2, 3]\n"); output.clear();
    dk3::Config reverseBudget; reverseBudget.stepLimit = 30;
    dk3::Runtime limitedReverse(reverseBudget);
    limitedReverse.run("let data = Collections.range(8)", "reverse-budget-setup.k3");
    auto reverseData = std::get<dk3::Object *>(limitedReverse.get("data"));
    reverseData->items.assign(100, int64_t{1}); reverseData->items.front() = int64_t{2};
    bool reverseLimited = false;
    try { limitedReverse.run("data.reverse()", "reverse-budget.k3"); } catch (const dk3::Error &error) { reverseLimited = error.category == "limit"; }
    assert(reverseLimited && std::get<int64_t>(reverseData->items.front()) == 2);
    runtime.run("let clearableMap: Map[String, Int] = {'x': 1}\nlet clearMapAlias = clearableMap\nlet clearMapSnapshot = clearableMap.values()\nlet clearMap = clearableMap.clear\nSystem.gc()\nclearMap()\nclearMap()\nSystem.print(clearMapAlias.length(), clearMapSnapshot)\nclearableMap['next'] = 2", "map-clear.k3");
    assert(output == "0 [1]\n"); output.clear();
    runtime.run("let safeMap: Map[String, Int?] = {'value': 7, 'empty': null}\nlet get = safeMap.get\nSystem.gc()\nSystem.print(get('value'), get('empty'), get('missing'), safeMap.contains('empty'))\nlet absent: Int? = get('missing')", "map-get.k3");
    assert(output == "7 null null true\n"); output.clear();
    runtime.run("class SearchIdentity {}\nlet identity = SearchIdentity()\nlet identities: List[SearchIdentity] = [identity]\nSystem.print(identities.contains(identity), identities.contains(SearchIdentity()))\nlet nestedGet: Map[String, List[Int]] = {'x': [1]}\nlet retrieved = nestedGet.get('x')\nnestedGet.clear()\nSystem.gc()\nSystem.print(retrieved)", "collection-ownership.k3");
    assert(output == "true false\n[1]\n"); output.clear();
    dk3::Config searchBudget; searchBudget.stepLimit = 30;
    dk3::Runtime limitedSearch(searchBudget);
    limitedSearch.run("let data = [1]", "search-budget-setup.k3");
    std::get<dk3::Object *>(limitedSearch.get("data"))->items.assign(100, int64_t{1});
    bool searchLimited = false;
    try { limitedSearch.run("data.indexOf(2)", "search-budget.k3"); } catch (const dk3::Error &error) { searchLimited = error.category == "limit"; }
    assert(searchLimited);
    for (const std::string &code : {"safeMap.get([])", "safeMap.get()", "safeMap.get(key: 'value')", "safeMap['missing']"}) {
        bool rejected = false; try { runtime.run(code, "map-get-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"clearableMap.clear(1)"}) {
        bool rejected = false; try { runtime.run(code, "map-clear-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"clearable.clear(1)"}) {
        bool rejected = false; try { runtime.run(code, "list-clear-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let inserted: List[Int] = [1, 3]\nlet insert = inserted.insert\nSystem.gc()\ninsert(1, 2)\ninserted.insert(-1, 8)\ninserted.insert(inserted.length(), 4)\nSystem.print(inserted)\nlet emptyInsertion: List[Int] = []\nemptyInsertion.insert(0, 1)", "list-insertion.k3");
    assert(output == "[1, 2, 8, 3, 4]\n"); output.clear();
    for (const std::string &code : {"inserted.insert(-99, 1)", "inserted.insert(99, 1)", "inserted.insert(1.0, 1)", "inserted.insert(1)", "inserted.insert(index: 1, value: 2)"}) {
        bool rejected = false; try { runtime.run(code, "insertion-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(std::get<dk3::Object *>(runtime.get("inserted"))->items.size() == 5);
    runtime.run("let removedItems: List[Int?] = [1, null, 3]\nlet removeAt = removedItems.removeAt\nSystem.gc()\nSystem.print(removeAt(1), removeAt(-1), removedItems)\nlet removedLast: Int? = removedItems.removeAt(0)\nSystem.print(removedLast, removedItems.length())", "list-removal.k3");
    assert(output == "null 3 [1]\n1 0\n"); output.clear();
    runtime.run("let extended: List[Int] = [1]\nlet extend = extended.extend\nSystem.gc()\nextend((2, 3))\nextended.extend(extended)\nfn extensionSource() { yield 4; System.gc(); yield 5 }\nextended.extend(extensionSource())\nSystem.print(extended)\nlet letters: List[String] = []\nletters.extend('abc')\nlet byteValues: List[Int] = []\nbyteValues.extend(b'ab')\nSystem.print(letters, byteValues)", "list-extension.k3");
    assert(output == "[1, 2, 3, 1, 2, 3, 4, 5]\n[a, b, c] [97, 98]\n"); output.clear();
    for (const std::string &code : {"extended.extend(1)", "extended.extend({})", "extended.extend()", "extended.extend(values: [6])"}) {
        bool rejected = false; try { runtime.run(code, "extension-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(std::get<dk3::Object *>(runtime.get("extended"))->items.size() == 8);
    runtime.run("fn failingExtension() { yield 9; throw 'failure' }\ntry { extended.extend(failingExtension()) } catch error {}", "atomic-extension.k3");
    assert(std::get<dk3::Object *>(runtime.get("extended"))->items.size() == 8);
    dk3::Config extensionBudget; extensionBudget.stepLimit = 120;
    dk3::Runtime limitedExtension(extensionBudget);
    limitedExtension.run("let destination: List[Int] = [1]\nfn unbounded() { while true { yield 2 } }", "extension-budget-setup.k3");
    bool boundedExtension = false;
    try { limitedExtension.run("destination.extend(unbounded())", "extension-budget.k3"); }
    catch (const dk3::Error &error) { boundedExtension = error.category == "limit"; }
    assert(boundedExtension && std::get<dk3::Object *>(limitedExtension.get("destination"))->items.size() == 1);
    runtime.run("fn removedRecord() { record Entry { value: Int; fn read() -> Int { return self.value } }\nlet entries: List[Entry] = []\nentries.insert(0, Entry(7))\nentries.extend((Entry(8),))\nlet removed = entries.removeAt(0)\nreturn removed.read }\nlet removedReader = removedRecord()\nSystem.gc()\nSystem.print(removedReader(), Reflection.members(inserted))", "removed-record-gc.k3");
    assert(output == "7 [all, any, append, clear, contains, extend, filter, find, findIndex, indexOf, insert, length, map, reduce, removeAt, reverse]\n"); output.clear();
    for (const std::string &code : {"removedItems.removeAt(0)", "inserted.removeAt(-99)", "inserted.removeAt(99)", "inserted.removeAt('bad')", "inserted.removeAt()", "inserted.removeAt(index: 0)"}) {
        bool rejected = false; try { runtime.run(code, "indexed-removal-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let membership: Map[String, Int?] = {'present': null, 'number': 3}\nlet contains = membership.contains\nSystem.gc()\nSystem.print(contains('present'), contains('missing'), membership.contains('number'))", "map-membership.k3");
    assert(output == "true false true\n"); output.clear();
    runtime.run("let removable: Map[String, Int?] = {'first': 1, 'null': null, 'last': 3}\nlet remove = removable.remove\nSystem.gc()\nSystem.print(remove('null'), remove('null'), removable.length(), removable.contains('last'))\nSystem.print(removable)\nremovable['new'] = 4", "map-removal.k3");
    assert(output == "true false 2 true\n{first: 1, last: 3}\n"); output.clear();
    for (const std::string &code : {"removable.remove([])", "removable.remove()", "removable.remove(key: 'first')"}) {
        bool rejected = false; try { runtime.run(code, "removal-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(std::get<dk3::Object *>(runtime.get("removable"))->entries.size() == 3);
    runtime.run("let snapshots: Map[String, List[Int]] = {'first': [1], 'second': [2]}\nlet snapshotKeys = snapshots.keys()\nlet snapshotValues = snapshots.values()\nlet detachedValues = snapshots.values\nSystem.gc()\nsnapshots.remove('first')\nsnapshots['third'] = [3]\nsnapshotValues[0].append(4)\nsnapshotKeys.append('independent')\nSystem.print(snapshotKeys, snapshotValues, snapshots.keys(), detachedValues())", "map-snapshots.k3");
    assert(output == "[first, second, independent] [[1, 4], [2]] [second, third] [[2], [3]]\n"); output.clear();
    for (const std::string &code : {"snapshots.keys(1)", "snapshots.values(key: 'first')"}) {
        bool rejected = false; try { runtime.run(code, "snapshot-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let emptySnapshotSource: Map[String, Int] = {}\nlet emptySnapshot = emptySnapshotSource.values()\nSystem.gc()", "empty-snapshot.k3");
    runtime.run("fn escapedSnapshot() { record SnapshotEntry { value: Int }\nlet source: Map[String, SnapshotEntry] = {'x': SnapshotEntry(7)}\nlet snapshot = source.values()\nsource.remove('x')\nreturn snapshot }\nlet escapedSnapshotValues = escapedSnapshot()\nSystem.gc()\nSystem.print(escapedSnapshotValues[0].value, Reflection.members(membership))", "snapshot-nominal-gc.k3");
    assert(output == "7 [clear, compute, computeIfAbsent, computeIfPresent, contains, get, keys, length, merge, putIfAbsent, remove, values]\n"); output.clear();
    for (const std::string &code : {"membership.contains([])", "membership.contains()", "membership.contains('present', 'other')", "membership.contains(key: 'present')"}) {
        bool rejected = false; try { runtime.run(code, "membership-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("trait RecordNamed { fn name() -> String { return 'record' } }\nrecord NamedPoint implements RecordNamed, Collections.Sized { x: Int, y: Int = x; fn length() -> Int { return 2 }; fn sum() -> Int { return self.x + self.y }; fn init() -> Int { return 9 } }\nlet namedPoint = NamedPoint(3)\nlet pointSum = namedPoint.sum\nSystem.gc()\nfn recordSize(value: Collections.Sized) -> Int { return value.length() }\nSystem.print(namedPoint.name(), pointSum(), namedPoint == NamedPoint(3), recordSize(namedPoint), Collections.len(namedPoint), namedPoint.init())\nrecord MutableInside { items: List[Int] = []; fn append(value: Int) -> Null { self.items.append(value) } }\nlet mutableInside = MutableInside()\nmutableInside.append(2)\nSystem.print(mutableInside.items)", "record-methods.k3");
    assert(output == "record 6 true 2 2 9\n[2]\n"); output.clear();
    for (const std::string &code : {"namedPoint.x = 4", "record WrongRecord implements Collections.Sized {}", "record RecordWrite { x: Int; fn write() -> Null { self.x = 1 } }\nRecordWrite(2).write()"}) {
        bool rejected = false; try { runtime.run(code, "record-method-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("trait DefaultNamed { fn name() -> String { \"\"\"Default method documentation.\"\"\"\nreturn 'default' } }\nclass Named implements DefaultNamed {}\nclass OverrideNamed implements DefaultNamed { fn name() -> String { return 'override' } }\nlet named = Named()\nlet defaultName = named.name\nSystem.gc()\nSystem.print(defaultName(), OverrideNamed().name(), Reflection.doc(defaultName))\ntrait CompositeDefault { fn read() -> Int; fn twice() -> Int { return self.read() * 2 } }\nclass DefaultReader implements CompositeDefault { fn read() -> Int { return 3 } }\nSystem.print(DefaultReader().twice())\ntrait OtherDefault { fn name() -> String { return 'other' } }\nclass ExplicitDefaults implements DefaultNamed, OtherDefault { fn name() -> String { return 'explicit' } }\nSystem.print(ExplicitDefaults().name())", "trait-defaults.k3");
    assert(output == "default override Default method documentation.\n6\nexplicit\n"); output.clear();
    for (const std::string &code : {"class AmbiguousDefaults implements DefaultNamed, OtherDefault {}", "class DefaultFieldConflict implements DefaultNamed { let name: String = '' }", "trait BadDefaultYield { fn read() -> Int { yield 1 } }"}) {
        bool rejected = false; try { runtime.run(code, "default-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("var initializedCount = 0\nclass Initialized { var value: Int = 0; fn init() -> Null { System.gc(); self.value += 2; initializedCount += 1 } }\nlet initialized = Initialized(value: 3)\nSystem.print(initialized.value, initializedCount)\nclass FailedInitialization { var value: Int = 0; fn init() { defer System.print('init cleanup'); throw 'init failed' } }\ntry { FailedInitialization() } catch error { System.print(error) }", "initializers.k3");
    assert(output == "5 1\ninit cleanup\ninit failed\n"); output.clear();
    for (const std::string &code : {"class BadInitYield { fn init() { yield 1 } }", "class BadInitType { fn init() -> Int { return 1 } }", "class BadInitReturn { fn init() { return 1 } }\nBadInitReturn()"}) {
        bool rejected = false; try { runtime.run(code, "initializer-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("from Collections import Sized\ntrait Measured { \"\"\"A nominal measurement.\"\"\"\nfn length() -> Int }\nclass Bag { let items: List[Int] = [] }\nimpl Measured for Bag { fn length() -> Int { return self.items.length() } }\nimpl Sized for Bag {}\nlet bag = Bag([1, 2])\nfn measure(value: Measured) -> Int { return value.length() }\nfn count(value: Sized) -> Int { return value.length() }\nSystem.gc()\nSystem.print(measure(bag), count(bag), Collections.len(bag), count([1]), count((1, 2)), count({'x': 1}), count('abc'), count(b'ab'))\nlet measured: List[Measured] = [bag]\nSystem.print(Reflection.doc(Measured), Reflection.members(Measured))", "traits.k3");
    assert(output == "2 2 2 1 2 1 3 2\nA nominal measurement. [length]\n"); output.clear();
    runtime.run("class EmbeddedBag implements Sized, Measured { let items: List[Int] = []; fn length() -> Int { return self.items.length() }; fn asSized() -> Sized { return self } }\nlet embedded: Measured = EmbeddedBag([1, 2, 3])\nlet embeddedLength = embedded.length\nSystem.gc()\nSystem.print(measure(embedded), count(embedded), Collections.len(embedded), embeddedLength())\nlet embeddedItems: List[Sized] = [embedded]", "embedded-traits.k3");
    assert(output == "3 3 3 3\n"); output.clear();
    runtime.run("from Collections import Sized as AlsoSized\nlet PreservedName = 9\ntrait SecondRequirement { fn other() -> Int }", "embedded-alias.k3");
    for (const std::string &code : {"class MissingEmbedded implements Sized {}", "class DuplicateEmbedded implements Sized, AlsoSized { fn length() -> Int { return 0 } }", "class UnknownEmbedded implements UnknownTrait {}", "class GeneratorEmbedded implements Sized { fn length() -> Int { yield 1 } }", "class PartialEmbedded implements Sized, SecondRequirement { fn length() -> Int { return 0 } }", "class PreservedName implements Sized { fn length() -> Int { return 1 } }", "impl Sized for EmbeddedBag {}"}) {
        bool rejected = false; try { runtime.run(code, "invalid-embedded-traits.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &name : {"MissingEmbedded", "WrongEmbedded", "DuplicateEmbedded", "UnknownEmbedded", "GeneratorEmbedded", "PartialEmbedded"}) {
    }
    assert(std::get<int64_t>(runtime.get("PreservedName")) == 9);
    runtime.run("class MissingEmbedded implements Sized { fn length() -> Int { return 0 } }\nlet recoveredEmbedded: Sized = MissingEmbedded()", "embedded-recovery.k3");
    runtime.run("class Duck { fn length() -> Int { return 1 } }\ntrait Pair { fn first() -> Int; fn second() -> Int }\nclass Incomplete {}", "trait-errors-setup.k3");
    for (const std::string &code : {"count(1)", "Measured()", "impl Measured for Bag {}", "impl Measured for Duck { fn length() -> Int { return 2 } }", "impl Pair for Incomplete { fn first() -> Int { return 1 } }", "impl Sized for Duck { fn extra() -> Int { return 1 } }", "impl Measured for Duck { fn length() -> Int { yield 1 } }"}) {
        bool rejected = false; try { runtime.run(code, "trait-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(runtime.inspectMembers(runtime.get("Incomplete")).empty());
    runtime.run("impl Measured for Duck {}\nSystem.print(measure(Duck()))\nfn escapedTrait() { trait LocalTrait { fn read() -> Int }\nclass LocalValue {}\nimpl LocalTrait for LocalValue { fn read() -> Int { return 7 } }\nreturn LocalValue().read }\nlet traitCallback = escapedTrait()\nSystem.gc()\nSystem.print(traitCallback())", "trait-lifetimes.k3");
    assert(output == "1\n7\n"); output.clear();
    runtime.run("class Unmarked {}\nclass TraitHolder { var value: Measured }\nlet holder = TraitHolder(bag)\nholder.value = Duck()\nlet traitMap: Map[String, Measured] = {'bag': bag}\nvar TraitAlias = Measured\nvar pinnedTrait: TraitAlias? = bag\nTraitAlias = Sized\nclass GeneratorDuck { fn length() -> Int { yield 1 } }\ntrait Scaler { fn scale(value: Int) -> Int }\nclass BadScaler {}", "trait-contracts.k3");
    for (const std::string &code : {"impl Measured for GeneratorDuck {}", "impl Scaler for BadScaler { fn scale(other: Int) -> Int { return 1 } }"}) {
        bool rejected = false; try { runtime.run(code, "trait-contract-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(dk3::Runtime::typeOf(runtime.getPath("holder.value")) == "Duck");
    dk3::Config traitModules; traitModules.resolveModule = [](const std::string &) {
        return "export trait Readable { fn read() -> Int }\nexport class Box implements Readable { fn read() -> Int { return 3 } }";
    };
    dk3::Runtime importedTraits(traitModules);
    importedTraits.run("import first as first\nimport second as second\nfn accept(value: first.Readable) -> Int { return value.read() }\nlet result = accept(first.Box())\nSystem.gc()", "trait-modules.k3");
    assert(std::get<int64_t>(importedTraits.get("result")) == 3);
    runtime.run("class Counter { \"\"\"A mutable counter.\"\"\"\nvar value: Int = 0\nlet label: String = 'counter'\nfn add(amount: Int = 1) -> Int { \"\"\"Increment the counter.\"\"\"\nself.value += amount; return self.value }\nfn snapshot() -> Counter { return self }\nfn stream() { yield self.value; self.value += 1; yield self.value }\n}\nlet classCounter: Counter = Counter(value: 2)\nlet detached = classCounter.add\nSystem.gc()\nSystem.print(detached(amount: 3), classCounter.snapshot() == classCounter, classCounter == Counter(5))\nclassCounter.value = 8\nfor item in classCounter.stream() { System.print(item) }\nSystem.print(Reflection.doc(classCounter), Reflection.doc(detached))", "classes.k3");
    assert(output == "5 true false\n8\n9\nA mutable counter. Increment the counter.\n"); output.clear();
    for (const std::string &code : {"classCounter.label = 'bad'", "classCounter.unknown = 1", "classCounter.add = 1", "Counter(1, value: 2)"}) {
        bool rejected = false; try { runtime.run(code, "class-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(std::get<int64_t>(runtime.getPath("classCounter.value")) == 9);
    runtime.run("fn escapedMethod() { class Local { var value: Int = 1; fn add() -> Int { self.value += 1; return self.value } }\nreturn Local().add }\nlet escapedAdd = escapedMethod()\nSystem.gc()\nSystem.print(escapedAdd())", "escaped-method.k3");
    assert(output == "2\n"); output.clear();
    size_t classHeap = runtime.liveObjects();
    for (int iteration = 0; iteration < 12; iteration++)
        runtime.run("{ class Cycle { var callback: Function? = null; fn value() -> Int { return 1 } }\nlet cycle = Cycle()\ncycle.callback = cycle.value }", "class-cycles.k3");
    assert(runtime.liveObjects() == classHeap);
    bool mutableField = false, boundMember = false;
    for (const auto &member : runtime.inspectMembers(runtime.get("classCounter"))) {
        if (member.name == "value") mutableField = member.mutableBinding && member.type == "Int";
        if (member.name == "add") boundMember = member.type == "Function" && !member.mutableBinding;
    }
    assert(mutableField && boundMember);
    runtime.run("from System import print as say\nlet constructed = List((1, 2))\nlet table = Map({'x': 3})\nsay(constructed.length(), table.length(), 'abc'.length(), b'abc'.length(), (1, 2).length())", "constructors.k3");
    assert(output == "2 1 3 3 2\n"); output.clear();
    runtime.run("record Documented { \"\"\"Stored record documentation.\"\"\"\nx: Int }\nfn documented() { \"\"\"Stored function documentation.\"\"\"\nreturn 1 }\nSystem.print(Reflection.doc(Documented(1)), Reflection.doc(documented))", "docstrings.k3");
    assert(output == "Stored record documentation. Stored function documentation.\n"); output.clear();
    runtime.run("let sourceList: List[Int] = [1]\nlet copiedList = List(sourceList)\ncopiedList.append(2)\nlet sourceMap: Map[String, Int] = {'x': 1}\nlet copiedMap = Map(sourceMap)\ncopiedMap['y'] = 2\nSystem.print(sourceList.length(), copiedList.length(), sourceMap.length(), copiedMap.length())", "copies-aliases.k3");
    assert(output == "1 2 1 2\n"); output.clear();
    for (const std::string &code : {"List(1, 2)", "Map([])", "sourceList.length(1)", "from System import missing", "from System import print as duplicated, gc as duplicated"}) {
        bool rejected = false; try { runtime.run(code, "constructor-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &name : {"print", "gc", "members", "type", "len", "range", "args"}) {
        bool absent = false; try { runtime.get(name); } catch (const dk3::Error &) { absent = true; }
        assert(absent);
    }
    runtime.run("from System import print\nprint('explicit alias')", "explicit-import.k3");
    assert(output == "explicit alias\n"); output.clear();
    runtime.registerNative("host.add", [](const auto &, const dk3::Span &) -> dk3::Value { return int64_t{9}; });
    runtime.run("let reflected = host.add()\nSystem.print(Reflection.binding('reflected'))\nSystem.gc()", "core-api.k3");
    assert(output == "9\n"); output.clear();
    runtime.run("let xs = [1] + [2, 3]\nxs.append(4)\nSystem.print(xs)\nSystem.print(xs[::-1])\nSystem.print(xs[-100:2])\nSystem.print(9007199254740993)\nSystem.print(-7 // 3)\nSystem.print(-7 % 3)\n", "core.k3");
    assert(output == "[1, 2, 3, 4]\n[4, 3, 2, 1]\n[1, 2]\n9007199254740993\n-3\n2\n");
    output.clear();
    runtime.run("fn add(first: Int, second: Int = 2) -> Int { return first + second }\nSystem.print(add(second: 4, first: 3))\nlet word = 'a\\xc3\\xa9'\nSystem.print(Collections.len(word))\nSystem.print(word[1])\nSystem.print(Reflection.type(Text.encode(word)))\n", "calls.k3");
    assert(output == "7\n2\n\xc3\xa9\nBytes\n");
    for (const std::string &source : {"let immutable = 1\nimmutable = 2", "unknown = 2", "if 1 { System.print(0) }", "add(1, first: 2)", "System.print(xs[::0])"}) {
        bool rejected = false; try { runtime.run(source, "invalid.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    output.clear();
    runtime.run("record Vec2 { x: Float, y: Float }\nenum Event { Move(Vec2), Quit }\nlet event = Event.Move(Vec2(1.0, 2.0))\nSystem.print(match event { Event.Move(point) => point.x, Event.Quit => null })\n", "data.k3");
    assert(output == "1\n");
    output.clear();
    runtime.run("record Profile { name: String, labels: List[String] = [] }\nrecord Point { x: Int, y: Int = x + 1 }\nenum Lookup { Found(Profile), Missing }\nlet firstProfile = Profile(name: 'Ada')\nlet secondProfile = Profile('Grace')\nfirstProfile.labels.append('active')\nSystem.gc()\nSystem.print(Collections.len(firstProfile.labels), Collections.len(secondProfile.labels))\nlet point = Point(x: 3)\nSystem.print(point.y, point == Point(3, y: 4))\nlet lookup = Lookup.Found(firstProfile)\nSystem.print(match lookup { Lookup.Found(profile) => profile.name, Lookup.Missing => 'none' })", "record-contracts.k3");
    assert(output == "1 0\n4 true\nAda\n");
    for (const std::string &source : {"firstProfile.name = 'changed'", "Profile()", "Profile(name: 'A', unknown: 2)", "Lookup.Found()"}) {
        bool rejected = false;
        try { runtime.run(source, "invalid-data.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    output.clear();
    runtime.run("fn values() { yield null\nyield 2 }\nfor item in values() { System.print(item) }\n", "generator.k3");
    assert(output == "null\n2\n");
    output.clear();
    runtime.run("fn cleanup() { defer System.print('closed')\nreturn 9 }\nSystem.print(cleanup())\ntry { throw 'problem' } catch error { System.print(error) } finally { System.print('finally') }\n", "errors.k3");
    assert(output == "closed\n9\nproblem\nfinally\n");
    output.clear();
    runtime.run("fn capture() { class Box { var count: Int = 0 }\nlet box = Box()\nreturn fn() { box.count += 1\nreturn box.count } }\nlet counter = capture()\nSystem.gc()\nSystem.print(counter())\nSystem.print(counter())\n", "closures.k3");
    assert(output == "1\n2\n");
    output.clear();
    runtime.run("fn defaults(values = []) { values.append(1)\nreturn Collections.len(values) }\nSystem.print(defaults())\nSystem.print(defaults())\n", "defaults.k3");
    assert(output == "1\n1\n");
    size_t before = runtime.liveObjects();
    for (int batch = 0; batch < 20; batch++) {
        runtime.run("{ let discard = [fn() { return 1 }, 'temporary'] }\n", "garbage.k3");
    }
    assert(runtime.liveObjects() < before + 10);
    dk3::Config modules;
    modules.write = [&](const std::string &text) { output += text; };
    modules.resolveModule = [](const std::string &name) {
        return name == "library" ? "let privateValue = 99\nexport let answer = 42\n" : "";
    };
    dk3::Runtime first(modules), second(modules);
    output.clear();
    first.run("import library as lib\nSystem.print(lib.answer)\n", "imports.k3");
    assert(output == "42\n");
    bool privateRejected = false;
    try { first.run("System.print(lib.privateValue)", "private.k3"); } catch (const dk3::Error &) { privateRejected = true; }
    assert(privateRejected);
    first.run("let isolated = 11", "first.k3");
    second.run("let isolated = 22", "second.k3");
    assert(std::get<int64_t>(first.get("isolated")) == 11 && std::get<int64_t>(second.get("isolated")) == 22);
    dk3::Config typeModules;
    typeModules.write = [&](const std::string &text) { output += text; };
    typeModules.resolveModule = [](const std::string &name) {
        return name == "model" || name == "other" ? "export record Point { x: Int }\nexport enum Event { Move(Point), Quit }\nexport class Box { var value: Int; fn set(value: Int) -> Int { self.value = value; return self.value } }\n" : "";
    };
    dk3::Runtime typedModules(typeModules);
    output.clear();
    typedModules.run("import model as model\nimport other as other\nlet imported: model.Point = model.Point(3)\nfn identity(value: model.Point) -> model.Point { return value }\nSystem.print(identity(imported).x)", "qualified-types.k3");
    assert(output == "3\n");
    output.clear();
    typedModules.run("let importedBox: model.Box = model.Box(3)\nfn sameBox(value: model.Box) -> model.Box { return value }\nSystem.gc()\nSystem.print(sameBox(importedBox).set(7))", "qualified-classes.k3");
    assert(output == "7\n");
    output.clear();
    typedModules.run("let event: model.Event = model.Event.Move(imported)\nSystem.print(match event { model.Event.Move(point) => point.x, model.Event.Quit => 0 })\nlet optional: model.Point? = null\nvar points: List[model.Point] = [imported]\npoints.append(model.Point(4))\nSystem.gc()\nSystem.print(points[1].x)", "qualified-enums.k3");
    assert(output == "3\n4\n");
    for (const std::string &source : {"let invalid: model.Event = other.Event.Quit()"}) {
        bool rejected = false;
        try { typedModules.run(source, "invalid-qualified.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    typedModules.run("var Alias = model.Point\nvar pinned: Alias? = null\nAlias = other.Point\npinned = imported\nSystem.gc()\nlet aliasedList: List[model.Point] = points", "pinned-types.k3");
    runtime.run("fn escapedContract() { record PrivateShape { x: Int }\nlet values: List[PrivateShape?] = [null]\nreturn values }\nlet privateValues = escapedContract()\nSystem.gc()", "escaped-contract.k3");
    runtime.run("privateValues.append(null)", "nullable-elements.k3");
    output.clear();
    runtime.run("var order = ''\nfn bound(label: String, value: Int) { order += label\nreturn value }\nlet ordered = xs[bound('A', 0):bound('B', 2):bound('C', 1)]\nSystem.print(order)\n", "order.k3");
    assert(output == "ABC\n");
    runtime.run("let typed: List[Int] = [1, 2]", "typed.k3");
    output.clear();
    runtime.run("let scores: Map[String, Int] = {'Ada': 1}\nscores['Grace'] = 2\nscores['Ada'] += 3\nSystem.print(scores['Ada'], scores['Grace'])", "typed-map.k3");
    assert(output == "4 2\n");
    for (const std::string &source : {"scores[[]] = 1"}) {
        bool rejected = false;
        try { runtime.run(source, "invalid-map.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let looseList = []\nlet looseMap = {'first': looseList, 'bad': 'wrong'}\ntry { let annotated: Map[String, List[Int]] = looseMap } catch error {}\nlooseList.append('still untyped')\nlet looseOuter = [[], 'bad']\ntry { let annotated: List[List[Int]] = looseOuter } catch error {}\nlooseOuter[0].append('not partially pinned')\nSystem.gc()", "atomic-contracts.k3");
    output.clear();
    typedModules.run("let mapOfPoints: Map[String, model.Point?] = {'origin': imported, 'empty': null}\nmapOfPoints['other'] = model.Point(8)\nSystem.gc()\nSystem.print(mapOfPoints['other'].x)", "nominal-map.k3");
    assert(output == "8\n");
    runtime.run("fn privateMap() { record Hidden {}\nlet values: Map[String, Hidden?] = {'none': null}\nreturn values }\nlet hiddenMap = privateMap()\nSystem.gc()", "map-contract-gc.k3");
    dk3::Span span{std::make_shared<dk3::Source>(dk3::Source{"host", ""}), 0, 0, 1, 1};
    size_t handle = runtime.retain(runtime.text("persistent", false, span));
    runtime.run("System.gc()", "handles.k3");
    assert(dk3::Runtime::display(runtime.retained(handle)) == "persistent");
    runtime.release(handle);
    runtime.collect();
    output.clear();
    runtime.run("try { System.print(xs[::0]) } catch error { System.print(Reflection.type(error), error.kind, error.message) }\n", "structured-errors.k3");
    assert(output == "Error value Slice step cannot be zero\n");
    bool boundaryRejected = false;
    try { runtime.run("fn badControl() { break }\nwhile true { badControl() }", "boundaries.k3"); }
    catch (const dk3::Error &) { boundaryRejected = true; }
    assert(boundaryRejected);
    output.clear();
    runtime.run("System.print(+9007199254740993)\nSystem.print(9007199254740993 == 9007199254740992.0)\nfn failingStream() { yield 1\nmissing() }\nlet failedGenerator = failingStream()\ntry { for value in failedGenerator { System.print(value) } } catch error { System.print(error.kind) }\nfor value in failedGenerator { System.print('must not run') }\n", "boundaries-2.k3");
    assert(output == "9007199254740993\nfalse\n1\nname\n");
    size_t baseline = runtime.liveObjects();
    dk3::Value graph;
    for (int depth = 0; depth < 10000; depth++) {
        dk3::Object *layer = runtime.allocate(dk3::Kind::List, span); layer->items.push_back(graph); graph = layer;
    }
    size_t graphHandle = runtime.retain(graph);
    runtime.collect();
    assert(runtime.liveObjects() >= baseline + 10000);
    runtime.release(graphHandle); runtime.collect();
    assert(runtime.liveObjects() <= baseline);
    std::cout << "runtime: PASS\n";
}