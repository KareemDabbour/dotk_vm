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
    httpRuntime.run("let socket = Sockets.listen('127.0.0.1', 0); let message = Http.readRequest(socket, 100); let method = message['method']; let body = message['body']; let host = message['headers']['host']; GC.collect(); Http.writeResponse(socket, 200, body);", "http-fragments.k3");
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
        runtime.run("GC.collect(); let detachedProbeResult = detachedProbe('a'); let directProbeResult = [1,2].probe('a', 0); class ExtendedList extends List {}; let inheritedProbe = ExtendedList([1]).probe('a')", "native-registration-lifetime.k3");
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
            fn dropReceiver() { receiver = null; GC.collect(); return 3 }
            let retainedNative = receiver.contains(dropReceiver());
            class Parent { fn read(value: Int) -> Int { GC.collect(); return value + 3 } }
            class Child extends Parent { fn read(value: Int) -> Int { return super.read(value) + 10 } }
            var child = Child();
            fn dropChild() { child = null; GC.collect(); return 2 }
            let retainedScript = child.read(dropChild());
            class Holder { var action: Any = fn(value) => value + 1 }
            let holder = Holder();
            fn replaceAction() { holder.action = fn(value) => 100; GC.collect(); return 3 }
            let selectedField = holder.action(replaceAction());
            let replacedField = holder.action(3);
            holder.action = [7].contains;
            let boundField = holder.action(7);
            let nested = '-'.join(['one', 'two']).contains(''.join(['tw', 'o']));
            fn detachedList() { return [4, 5].contains }
            fn detachedString() { return 'alpha'.contains }
            fn detachedBytes() { return b'abc'.length }
            let listMethod = detachedList(); let stringMethod = detachedString(); let bytesMethod = detachedBytes();
            GC.collect();
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

static void inlineCacheTests() {
    for (unsigned level : {0u, 1u, 2u}) {
        dk3::Config config; config.optimizationLevel = level;
        dk3::Runtime runtime(config);
        runtime.run(R"(
            trait Named { fn name() -> String }
            class Base { fn name() -> String { return 'base' } }
            class Kid extends Base {}
            fn label(value) { return value.name() }
            let kid = Kid();
            var before = ''; var after = '';
            before = label(kid);
            impl Named for Kid { fn name() -> String { return 'kid' } }
            after = label(kid);
            class Wide { var a: Int = 1; var b: Int = 2 }
            class Narrow { var b: Int = 20 }
            fn readB(value) { return value.b }
            fn bump(value) { value.b += 5; return value.b }
            let wide = Wide(); let narrow = Narrow();
            let mixed = readB(wide) + readB(narrow) + readB(wide) + bump(narrow) + bump(wide) + readB(narrow);
            class Secret { private var hidden: Int = 7; fn peek() { return self.hidden } }
            fn reach(value) { return value.hidden }
            let secret = Secret();
            let peeked = secret.peek() + secret.peek();
            class Fixed { let value: Int; constructor(value: Int) { self.assign(value) }; fn assign(value: Int) { self.value = value } }
            let fixed = Fixed(3);
            var churn = 0;
            while churn < 200 { let temporary = [Wide(), Narrow()]; churn += readB(temporary[churn % 2]) - readB(temporary[churn % 2]) + 1 }
            let afterChurn = readB(wide) + readB(narrow);
        )", "inline-cache.k3");
        assert(std::get<dk3::Object *>(runtime.get("before"))->text == "base" && std::get<dk3::Object *>(runtime.get("after"))->text == "kid");
        assert(std::get<int64_t>(runtime.get("mixed")) == 2 + 20 + 2 + 25 + 7 + 25);
        assert(std::get<int64_t>(runtime.get("peeked")) == 14 && std::get<int64_t>(runtime.get("afterChurn")) == 7 + 25);
        for (const auto &failure : std::vector<std::pair<std::string, std::string>>{
            {"reach(secret)", "access"}, {"secret.hidden = 1", "access"}, {"fixed.assign(4)", "mutability"}, {"readB(secret)", "name"}}) {
            bool rejected = false;
            try { runtime.run(failure.first, "inline-cache-errors.k3"); } catch (const dk3::Error &error) { rejected = error.category == failure.second; }
            assert(rejected);
        }
    }
}

static void mapIndexTests() {
    for (unsigned level : {0u, 1u, 2u}) {
        dk3::Config config; config.optimizationLevel = level; std::string output;
        config.write = [&](const std::string &value) { output += value; };
        dk3::Runtime runtime(config);
        runtime.run(R"(
            let m = {};
            for (var i = 0; i < 40; i++) { m[i] = i * 2 }
            m[1.0] = 100; m[-0.0] = 7; m['k'] = 1; m[b'k'] = 2;
            System.print(m[1], m[0], m.length(), m['k'], m[b'k'], m.contains(2.5), m.get(39.0));
            m.remove(5); m[5] = 55; m.remove(10);
            System.print(m.keys()[38], m[5], m.contains(10), m.length());
            m.clear(); m['x'] = 1;
            System.print(m.length(), m['x'], m.contains(3));
            let big = {};
            for (var i = 0; i < 20; i++) { big[i] = i }
            big.compute(4, fn(k, v) => null);
            big.merge(30, 1, fn(a, b) => a + b); big.merge(30, 1, fn(a, b) => a + b);
            let copied = Map(big); copied.remove(0);
            System.print(big.get(19) + big.get(3), big.contains(4), big[30], big.length(), copied.length(), copied.contains(0), big.contains(0));
        )", "map-index.k3");
        assert(output == "100 7 42 1 2 false 78\nk 55 false 41\n1 1 false\n22 false 2 20 19 false true\n");
    }
}

static void annotationTests() {
    for (unsigned level : {0u, 1u, 2u}) {
        dk3::Config config; config.optimizationLevel = level; std::string output;
        config.write = [&](const std::string &value) { output += value; };
        dk3::Runtime runtime(config);
        runtime.run(R"(
            var log = [];
            fn trace(label) { log.append('make ' + label); return fn(f) { log.append('apply ' + label); return fn(x) => f(x) * 10 } }
            fn register(t) { log.append('register'); return 0 }
            @trace('outer') @trace('inner') fn base(x) { return x + 1 }
            @register record Point { x: Int, y: Int }
            @trace('rec') fn countdown(n) { if n <= 0 { return 0 }; return countdown(n - 1) + 1 }
            fn column(name, size) { return size }
            class User { @column('user_name', 32) var name: String = ''; @deprecated fn legacy(@column('id', -1.5) id: Int) { return id } }
            @pure @test fn square(x = 2) { return x * x }
            System.print(base(1), Point(1, 2).x, log, countdown(2));
            System.print(Reflection.annotations(base), Reflection.annotations(Point), Reflection.annotations(User, 'name'), Reflection.annotation(User(), 'column', 'name'));
            System.print(Reflection.annotation(User().legacy, 'column', 'id'), Reflection.annotations(User, 'legacy'), Reflection.annotations(square), Reflection.annotation(square, 'missing'), Reflection.annotations(1));
        )", "annotations.k3");
        assert(output == "200 1 [make outer, make inner, apply inner, apply outer, register, make rec, apply rec] 110\n"
                         "[] [register] [column] [user_name, 32]\n"
                         "[id, -1.5] [deprecated] [pure, test] null []\n");
    }
}

int main() {
    nativeRegistrationTests();
    methodDispatchTests();
    inlineCacheTests();
    mapIndexTests();
    annotationTests();
    for (int level : {0, 1, 2}) {
        dk3::Config parityConfig; parityConfig.optimizationLevel = static_cast<unsigned>(level); parityConfig.objectLimit = 10000; std::string parityOutput;
        parityConfig.write = [&](const std::string &value) { parityOutput += value; };
        dk3::Runtime parity(parityConfig);
        parity.run("var regularTotal = 0; for (var regularIndex = 0; regularIndex < 8; regularIndex += 1) { if regularIndex == 2 { continue }; if regularIndex == 6 { break }; regularTotal += regularIndex }; var omittedIndex = 0; for (; omittedIndex < 3; omittedIndex += 1) {}; var openIndex = 0; for (;;) { openIndex += 1; if openIndex < 4 { continue }; break }", "regular-for.k3");
        assert(std::get<int64_t>(parity.get("regularTotal")) == 13 && std::get<int64_t>(parity.get("omittedIndex")) == 3 && std::get<int64_t>(parity.get("openIndex")) == 4);
        parity.run("var cleanupCount = 0; fn counted() { for (var step = 0; step < 5; step++) { try { if step == 1 { continue }; if step == 4 { break }; yield step } catch error {} finally { cleanupCount += 1 } } }; let countedValues = List(counted()); fn callbacksFromLoop() { let saved = []; for (var step = 0; step < 3; step++) { let copy = step; saved.append(fn() => copy) }; return saved }; let loopCallbacks = callbacksFromLoop(); GC.collect(); let firstCallback = loopCallbacks[0](); let lastCallback = loopCallbacks[2](); var nestedCount = 0; for (var outer = 0; outer < 3; outer++) { for (var inner = 0; inner < 3; inner++) { if inner == 1 { continue }; nestedCount += 1 } }", "bytecode-control-coverage.k3");
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
        parityOutput.clear();
        parity.run("var fusedText = 'a'; var fusedCount = 0; while fusedText != 'aaaa' { fusedText = fusedText + 'a'; fusedCount += 1 }; fn fusedFloat() { var f = 1.5; f += 1; f -= 3; return f }; fn fusedWrap() { var w = 9223372036854775807; w += 1; return w }; var fusedMaybe = null; var fusedHits = 0; if (fusedMaybe ?? 3) < 5 { fusedHits += 1 }; if 1 < (fusedMaybe ?? 3) { fusedHits += 10 }; for (var i = 0; i < 4; i++) { if i != 2 { fusedHits += 100 } }; class FusedOdd { fn __lt__(other) { return 1 } }; var fusedOdd = 'none'; try { if FusedOdd() < FusedOdd() { fusedOdd = 'ran' } } catch error { fusedOdd = error.message }; System.print(fusedCount, fusedFloat(), fusedWrap(), fusedHits, fusedOdd)", "fused-ops.k3");
        assert(parityOutput == "3 -0.5 -9223372036854775808 311 Condition requires Bool, got Int\n");
    }
    {
        dk3::Runtime simple;
        simple.run("fn choose(value: Int) -> List[Int] { if value == 0 { return [1] } else if value == 1 { let local = [2]; GC.collect(); return local } else { return [3] } }; let chosen = choose(1); GC.collect(); let chosenValue = chosen[0]; fn emptyResult() {}; let empty = emptyResult(); fn nestedReturn() { { return 4 } }; let nested = nestedReturn()", "simple-returns.k3");
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
    runtime.run("let aliasedObject: Object = 1\nlet qualifiedObject: Object = 'value'", "object-aliases.k3");
    dk3::Config ioConfig; std::string ioOutput; size_t inputCount = 0; uint64_t delayed = 0;
    ioConfig.write = [&](const std::string &value) { ioOutput += value; };
    ioConfig.readLine = [&]() -> std::optional<std::string> { if (inputCount++) return {}; return "Ada"; };
    ioConfig.monotonicMillis = [] { return uint64_t{42}; }; ioConfig.delay = [&](uint64_t value) { delayed = value; };
    dk3::Runtime ioRuntime(ioConfig);
    ioRuntime.run("let line: String? = IO.readLine(); IO.write(f'Hello {line}'); System.print(IO.readLine(), Time.monotonicMillis()); Time.sleep(3)", "io-capabilities.k3");
    assert(ioOutput == "Hello Adanull 42\n" && delayed == 3);
    for (const std::string &code : {"IO.readLine()", "Time.monotonicMillis()", "Time.sleep(0)"}) {
        bool denied = false; try { runtime.run(code, "io-disabled.k3"); } catch (const dk3::Error &error) { denied = error.category == "capability"; }
        assert(denied);
    }
    runtime.run("try { [1][9] } catch error { let kind: Error.Kind = error.kind; System.print(kind == Error.Kind.Index, Reflection.type(kind)) }\nlet enumError = Error(Error.Kind.Application, 'message')\nSystem.print(enumError.kind == Error.Kind.Application)", "error-kinds.k3");
    assert(output == "true Error.Kind\ntrue\n"); output.clear();
    runtime.run("let kindMessage = match Error.Kind.Index { Error.Kind.Index => 'index matched', _ => 'other' }\nSystem.print(kindMessage)", "error-kind-match.k3");
    assert(output == "index matched\n"); output.clear();
    runtime.run("let formattedName = 'Ada'\nvar formattedCount = 0\nfn formattedNext() -> Int { formattedCount += 1; GC.collect(); return formattedCount }\nSystem.print(f'Hello {formattedName}: {1 + 2}, {{literal}}, {formattedNext()} {formattedNext()}')\nSystem.print(f\"{ {'value': 4}['value'] }\")", "formatted-strings.k3");
    assert(output == "Hello Ada: 3, {literal}, 1 2\n4\n"); output.clear();
    runtime.run("System.print(f'\\x7bescaped\\x7d')", "format-escapes.k3");
    assert(output == "{escaped}\n"); output.clear();
    runtime.run("class Indexed { private let values: Map[String, Int] = {}; fn __getitem__(key: String) -> Int { GC.collect(); return self.values.get(key) ?? 0 }; fn __setitem__(key: String, value: Int) -> Null { self.values[key] = value } }\nlet indexed = Indexed()\nindexed['score'] = 4\nindexed['score'] += 3\nSystem.print(indexed['score'])", "index-hooks.k3");
    assert(output == "7\n"); output.clear();
    runtime.run("var indexedKeyCalls = 0\nvar indexedReceiverCalls = 0\nfn onceReceiver() -> Indexed { indexedReceiverCalls += 1; return indexed }\nfn onceKey() -> String { indexedKeyCalls += 1; return 'score' }\nonceReceiver()[onceKey()] += 1\nSystem.print(indexedReceiverCalls, indexedKeyCalls, indexed['score'])\nlet namedError = Error(message: 'legacy', kind: 'type')\nSystem.print(namedError.kind == Error.Kind.Type)", "foundation-boundaries.k3");
    assert(output == "1 1 8\ntrue\n"); output.clear();
    for (const std::string &code : {"class WrongSetter { fn __setitem__(key, value) { return 1 } }; WrongSetter()[0] = 1"}) {
        bool rejected = false; try { runtime.run(code, "index-hook-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("class Printable { private let label: String; constructor(label: String) -> Null { self.label = label }; public fn toString() -> String { GC.collect(); return 'item:' + self.label } }\nlet printable = Printable('Ada')\nlet converted = Text.str(printable)\nSystem.print(converted, printable, printable.toString())\nclass PrintableChild extends Printable {}\nSystem.print(PrintableChild('Grace'))", "to-string.k3");
    assert(output == "item:Ada item:Ada item:Ada\nitem:Grace\n"); output.clear();
    runtime.run("record VectorOperator { x: Int, y: Int; fn toString() -> String { return Text.str(self.x) + ',' + Text.str(self.y) }; fn __add__(other: VectorOperator) -> VectorOperator { GC.collect(); return VectorOperator(self.x + other.x, self.y + other.y) }; fn __mul__(amount: Int) -> VectorOperator { return VectorOperator(self.x * amount, self.y * amount) }; fn __neg__() -> VectorOperator { return VectorOperator(-self.x, -self.y) }; fn __lt__(other: VectorOperator) -> Bool { return self.x < other.x } }\nlet firstVector = VectorOperator(1,2)\nlet secondVector = VectorOperator(3,4)\nSystem.print(firstVector + secondVector, firstVector * 3, -firstVector, firstVector < secondVector)\nvar compoundVector = firstVector\ncompoundVector += secondVector\nSystem.print(compoundVector)\nclass OperatorBase { let value: Int; fn __add__(other: Int) -> Int { return self.value + other } }\nclass OperatorChild extends OperatorBase {}\nSystem.print(OperatorChild(5) + 2)", "operators.k3");
    assert(output == "4,6 3,6 -1,-2 true\n4,6\n7\n"); output.clear();
    for (const std::string &code : {"class WrongPredicate { fn __lt__(other) { return 1 } }; if WrongPredicate() < WrongPredicate() {}", "class PrivateOperator { private fn __add__(other) {} }", "class GeneratorOperator { fn __add__(other) { yield 1 } }", "firstVector + 1", "2 * firstVector"}) {
        bool rejected = false; try { runtime.run(code, "operator-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    for (const std::string &code : {"class WrongString { fn toString() { return 1 } }; Text.str(WrongString())", "class RecursiveString { fn toString() -> String { return Text.str(self) } }; Text.str(RecursiveString())"}) {
        bool rejected = false; try { runtime.run(code, "to-string-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("class VaultAccess { private var secret: Int = 3; protected var shared: Int = 2; private static let code: Int = 7; public fn read() -> Int { return self.secret + VaultAccess.code }; public fn callback(callback: Function) { callback(self) }; public fn closure() { return fn() -> Int { GC.collect(); return self.secret } } }\nclass ChildAccess extends VaultAccess { fn readShared() -> Int { self.shared += 1; return self.shared } }\nlet vaultAccess = ChildAccess()\nlet privateClosure = vaultAccess.closure()\nGC.collect()\nSystem.print(vaultAccess.read(), vaultAccess.readShared(), privateClosure())", "access-runtime.k3");
    assert(output == "10 3 3\n"); output.clear();
    runtime.run("System.print(Reflection.members(vaultAccess).contains('secret'), Reflection.members(vaultAccess).contains('shared'))\nclass FactoryAccess { private let value: Int; private constructor(value: Int) -> Null { self.value = value }; public static fn create(value: Int) -> FactoryAccess { return FactoryAccess(value) }; public fn read() -> Int { return self.value } }\nlet factoryAccess = FactoryAccess.create(8)\nGC.collect()\nSystem.print(factoryAccess.read(), Text.str(factoryAccess))", "access-reflection-factory.k3");
    assert(output == "false false\n8 FactoryAccess()\n"); output.clear();
    runtime.run("class AccessDefaults { private static let seed: Int = 5; private let value: Int = AccessDefaults.seed; fn read() -> Int { return self.value }; fn stream() { yield self.value; GC.collect(); yield self.value + 1 } }\nlet accessDefaults = AccessDefaults()\nclass AccessDefaultChild extends AccessDefaults {}\nlet accessDefaultChild = AccessDefaultChild()\nSystem.print(accessDefaults.read(), accessDefaultChild.read())\nfor item in accessDefaults.stream() { System.print(item) }\ntrait AccessImpl { fn read() -> Int }\nclass ImplAccess { private let value: Int = 11 }\nimpl AccessImpl for ImplAccess { fn read() -> Int { return self.value } }\nSystem.print(ImplAccess().read())", "access-defaults-generators.k3");
    assert(output == "5 5\n5\n6\n11\n"); output.clear();
    for (const std::string &code : {"vaultAccess.secret", "vaultAccess.shared = 1", "VaultAccess.code", "FactoryAccess(1)", "vaultAccess.callback(fn(value) { return value.secret })", "class BadPrivateChild extends VaultAccess { fn reveal() { return self.secret } }; BadPrivateChild().reveal()"}) {
        bool rejected = false; try { runtime.run(code, "access-errors.k3"); } catch (const dk3::Error &error) { rejected = error.category == "access"; }
        assert(rejected);
    }
    runtime.run("class CustomPlayer { let name: String; var level: Int = 1; fn init(name: String, level: Int = 1) -> Null { self.name = name; self.level = level; GC.collect() } }\nclass CustomHero extends CustomPlayer { let title: String; fn init(name: String, title: String) -> Null { super.init(name, 5); self.title = title } }\nlet custom = CustomHero('Ada', 'Explorer')\nSystem.print(custom.name, custom.level, custom.title)", "custom-init.k3");
    assert(output == "Ada 5 Explorer\n"); output.clear();
    runtime.run("class DefaultConstructed { let value: Int; constructor() -> Null { self.value = 42 } }\nSystem.print(DefaultConstructed().value)", "zero-constructor.k3");
    assert(output == "42\n"); output.clear();
    runtime.run("class TaggedList extends Collections.List { var tag: String = 'items'; fn append(value: Int) -> Null { super.append(value * 2) } }\nlet tagged = TaggedList([1,2])\ntagged.append(3)\nlet typedTagged: List[Int] = tagged\nGC.collect()\nSystem.print(tagged.length(), tagged[2], tagged.tag, Reflection.type(Collections.List))\nclass CountMap extends Map { fn count(key: String) -> Int { return self.compute(key, fn(key, old) => (old ?? 0) + 1) ?? 0 } }\nlet counted = CountMap()\nSystem.print(counted.count('x'), counted['x'])", "collection-subclasses.k3");
    assert(output == "3 6 items Type\n1 1\n"); output.clear();
    runtime.run("let unique: Set[Int] = Set([1,2,2,3])\nSystem.print(unique.length(), unique.add(2), unique.add(4), unique.remove(1))\nSystem.print(unique.values(), unique.union(Set([4,5])).values(), unique.intersection(Set([3,4])).values(), unique.difference(Set([3])).values())\nSystem.print(Set([2]).isSubsetOf(unique))\nclass NamedSet extends Set { let label: String = 'unique' }\nlet namedSet = NamedSet([1,1,2])\nfor value in namedSet { System.print(value) }\nSystem.print(namedSet.label, Reflection.type(Collections.Set))", "sets.k3");
    assert(output == "3 false true true\n[2, 3, 4] [2, 3, 4, 5] [3, 4] [2, 4]\ntrue\n1\n2\nunique Type\n"); output.clear();
    runtime.run("from Collections import List as Sequence\nlet qualified: Collections.List[Int] = Sequence([1,2])\nlet aliasCollection: Sequence[Int] = qualified\nclass InitializedList extends List { let label: String; fn init(label: String, source: List[Int]) -> Null { super.init(source); self.label = label } }\nlet initializedList = InitializedList('numbers', qualified)\nGC.collect()\nSystem.print(initializedList.label, initializedList[1])", "collection-qualified-init.k3");
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
    runtime.run("class StaticCounter { static var count: Int = 0; static let label: String = 'counter'; var value: Int = 0; static fn create(value: Int) -> StaticCounter { StaticCounter.count += 1; return StaticCounter(value) }; fn read() -> Int { return self.value } }\nlet staticFactory = StaticCounter.create\nGC.collect()\nlet staticFirst = staticFactory(3)\nlet staticSecond = StaticCounter.create(5)\nSystem.print(StaticCounter.count, StaticCounter.label, staticFirst.read(), staticSecond.read())", "static-members.k3");
    assert(output == "2 counter 3 5\n"); output.clear();
    runtime.run("class OpenBase { static var shared: Int = 0; var value: Int; fn read() -> Int { return self.value } }\nclass OpenChild extends OpenBase { let label: String = 'child'; fn read() -> Int { GC.collect(); return self.value * 2 } }\nlet openChild: OpenBase = OpenChild(3)\nlet inheritedRead = openChild.read\nGC.collect()\nOpenChild.shared += 1\nSystem.print(inheritedRead(), OpenBase.shared, openChild.value)\nopenChild.value = 4\nSystem.print(openChild.read())", "class-inheritance.k3");
    assert(output == "6 1 3\n8\n"); output.clear();
    runtime.run("class SuperChild extends OpenBase { fn read() -> Int { GC.collect(); return super.read() + 10 } }\nlet superChild = SuperChild(2)\nSystem.print(superChild.read())\nclass SuperGrandchild extends SuperChild { fn read() -> Int { return super.read() + 100 } }\nSystem.print(SuperGrandchild(2).read())\nclass BaseInit { var value: Int = 0; fn init() -> Null { self.value += 1 } }\nclass ChildInit extends BaseInit { fn init() -> Null { super.init(); self.value += 10 } }\nSystem.print(ChildInit().value)", "super-dispatch.k3");
    assert(output == "12\n112\n11\n"); output.clear();
    runtime.run("class SuperFields extends OpenBase { fn read() -> Int { super.value += 1; return super.value } }\nlet superFields = SuperFields(4)\nSystem.print(superFields.read(), superFields.value)", "super-fields.k3");
    assert(output == "5 5\n"); output.clear();
    runtime.run("trait InheritedReadable { fn read() -> Int }\nclass TraitParent implements InheritedReadable { var value: Int = 2; fn read() -> Int { return self.value } }\nclass TraitChild extends TraitParent {}\nlet inheritedTrait: InheritedReadable = TraitChild()\nlet baseDefault: Int = 4\nclass LexicalParent { var value: Int = baseDefault }\n{ let baseDefault: String = 'different scope'; class LexicalChild extends LexicalParent {}; System.print(LexicalChild().value) }\nSystem.print(inheritedTrait.read())\nfn localFactory() { class LocalBase { static var count: Int = 0; static fn next() -> Int { LocalBase.count += 1; return LocalBase.count }; fn read() -> Int { return 3 } }; class LocalChild extends LocalBase {}; return (LocalChild.next, LocalChild().read) }\nlet localMethods = localFactory()\nGC.collect()\nSystem.print(localMethods[0](), localMethods[1]())", "inheritance-lifetimes.k3");
    assert(output == "4\n2\n1 3\n"); output.clear();
    dk3::Config inheritedConfig; std::string inheritedOutput;
    inheritedConfig.write = [&](const std::string &text) { inheritedOutput += text; };
    inheritedConfig.resolveModule = [](const std::string &name) {
        return name == "base_model" ? "record Token { value: Int }; let seed: Int = 7; export class Parent { var token: Token = Token(seed); fn read() -> Int { return self.token.value } }" : "";
    };
    dk3::Runtime inheritedRuntime(inheritedConfig);
    inheritedRuntime.run("import base_model\nclass ImportedChild extends base_model.Parent {}\nlet imported: base_model.Parent = ImportedChild()\nGC.collect()\nSystem.print(imported.read())", "imported-inheritance.k3");
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
    runtime.run("trait BothNamed { fn name() -> String }\nclass BothBag implements BothNamed, Collections.Sized { fn name() -> String { return 'bag' }; fn length() -> Int { return 3 } }\nfn describeBoth(value: Collections.Sized & BothNamed) -> String { GC.collect(); return value.name() + Text.str(value.length()) }\nlet both: Collections.Sized & BothNamed = BothBag()\nlet bothValues: List[Collections.Sized & BothNamed] = [both]\nlet bothAlias: List[BothNamed & Collections.Sized] = bothValues\nlet optionalBoth: (Collections.Sized & BothNamed)? = null\nSystem.print(describeBoth(both), bothValues[0].length())", "intersection-runtime.k3");
    assert(output == "bag3 3\n"); output.clear();
    for (const std::string &code : {"describeBoth([1])"}) {
        bool rejected = false; try { runtime.run(code, "intersection-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let computedMap: Map[String, Int?] = {'empty': null, 'old': 2}\nvar computeCalls = 0\nfn supply(key: String) -> Int { computeCalls += 1; GC.collect(); return 7 }\nSystem.print(computedMap.computeIfAbsent('old', supply), computedMap.computeIfAbsent('empty', supply), computeCalls)\nSystem.print(computedMap.computeIfPresent('missing', fn(key, value) => value + 1))\nSystem.print(computedMap.compute('new', fn(key, value) => (value ?? 0) + 3))\nSystem.print(computedMap.computeIfPresent('old', fn(key, value) => value * 2))\nSystem.print(computedMap.putIfAbsent('old', 99), computedMap.putIfAbsent('added', 5))\nSystem.print(computedMap.merge('old', 3, fn(old, added) => old + added))\ncomputedMap.compute('old', fn(key, value) => null)\nSystem.print(computedMap.contains('old'))", "map-compute.k3");
    assert(output == "2 7 1\nnull\n3\n4\n4 null\n7\nfalse\n"); output.clear();
    runtime.run("let mutatingCompute: Map[String, List[Int]] = {'x': [1]}\nlet replaced = mutatingCompute.compute('x', fn(key: String, old: List[Int]?) -> List[Int] { mutatingCompute.clear(); GC.collect(); return (old ?? []) + [2] })\nSystem.print(replaced, mutatingCompute['x'])\nvar failedComputed = 0\nlet stableCompute: Map[String, Int] = {'x': 3}\ntry { stableCompute.compute('x', fn(key, old) { throw 'no result' }) } catch error { failedComputed += 1 }\nSystem.print(stableCompute['x'], failedComputed)\nlet nullCompute: Map[String, Int?] = {'x': null}\nSystem.print(nullCompute.computeIfPresent('x', fn(key, old) => 5), nullCompute.contains('x'))\nnullCompute.computeIfAbsent('x', fn(key) => null)\nSystem.print(nullCompute.contains('x'))", "map-compute-lifetimes.k3");
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
    runtime.run("let mapSource: List[Int] = [1, 2, 3]\nlet mapped: List[Int] = mapSource.map(fn(value: Int) -> Int { mapSource.clear(); GC.collect(); return value * 2 })\nSystem.print(mapped, mapSource)\nlet filterSource: List[Int] = [1, 2, 3, 4]\nlet filtered = filterSource.filter(fn(value: Int) -> Bool { GC.collect(); return value % 2 == 0 })\nSystem.print(filtered)\nlet retainedFilter = filterSource.filter\nGC.collect()\nSystem.print(retainedFilter(fn(value: Int) -> Bool => value > 2))", "callback-lists.k3");
    assert(output == "[2, 4, 6] []\n[2, 4]\n[3, 4]\n"); output.clear();
    runtime.run("let reduced: Int = [1, 2, 3].reduce(fn(total: Int, value: Int) -> Int { GC.collect(); return total + value }, 10)\nSystem.print(reduced, [].reduce(fn(total, value) => total, 7))\nvar visited = 0\nfn positive(value: Int) -> Bool { visited += 1; return value > 1 }\nSystem.print([1, 2, 3].any(positive), visited)\nvisited = 0\nSystem.print([1, 2, 3].all(positive), visited)\nSystem.print([].any(positive), [].all(positive))\nSystem.print([1, 2, 3].find(positive), [1].find(positive))\nSystem.print([null, 2].findIndex(fn(value) -> Bool => value == null), [].findIndex(positive))\nlet folded: List[Int] = [1, 2].reduce(fn(total: List[Int], value: Int) -> List[Int] { GC.collect(); return total + [value] }, [])\nSystem.print(folded)", "callback-queries.k3");
    assert(output == "16 7\ntrue 2\nfalse 1\nfalse true\n2 null\n0 -1\n[1, 2]\n"); output.clear();
    runtime.run("var partialMap = [99]\nvar callbackEffects = 0\ntry { partialMap = [1,2,3].map(fn(value: Int) -> Int { callbackEffects += 1; if value == 2 { throw 'stop' }; return value }) } catch error {}\nSystem.print(partialMap, callbackEffects)\nclass CallbackCounter { var calls: Int = 0; fn visit(value: Int) -> Int { self.calls += 1; GC.collect(); return value + self.calls } }\nlet callbackCounter = CallbackCounter()\nlet detachedCallback = callbackCounter.visit\nGC.collect()\nSystem.print([1,2].map(detachedCallback))\nlet emptyFiltered = filterSource.filter(fn(value: Int) -> Bool => false)", "callback-failure-lifetimes.k3");
    assert(output == "[99] 2\n[2, 4]\n"); output.clear();
    for (const std::string &code : {"[1].filter(fn(value) => 1)", "[1].any(fn(value) => null)", "[1].reduce(fn(value) => value, 0)", "[].map(1)", "[1].map(callback: fn(value) => value)"}) {
        bool rejected = false; try { runtime.run(code, "callback-list-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let parsedJson = Json.parse('{\"value\":9007199254740993,\"items\":[null,true,1.25],\"text\":\"a\\\\u0000b\"}')\nGC.collect()\nSystem.print(parsedJson['value'], parsedJson['items'][1], parsedJson['text'].length())\nlet encodedJson = Json.stringify(parsedJson)\nlet jsonRoundTrip = Json.parse(encodedJson)\nSystem.print(jsonRoundTrip['value'], Json.stringify((1, 2)))", "json.k3");
    assert(output == "9007199254740993 true 3\n9007199254740993 [1,2]\n"); output.clear();
    runtime.run("System.print(Json.parse('-9223372036854775808'), Json.parse('9223372036854775807'))\nlet unicodeJson = Json.parse('\"\\\\uD83D\\\\uDE00\"')\nSystem.print(unicodeJson.length())\nlet aliasJson = [1]\nSystem.print(Json.stringify([aliasJson, aliasJson]))\nlet typedJson: Map[String, List[Int]] = Json.parse('{\"values\":[1,2]}')\ntypedJson['values'].append(3)\nGC.collect()", "json-boundaries.k3");
    assert(output == "-9223372036854775808 9223372036854775807\n1\n[[1],[1]]\n"); output.clear();
    std::string deepJson(65, '['); deepJson += '0'; deepJson += std::string(65, ']');
    runtime.set("deepJson", runtime.text(deepJson, false, dk3::Span{}));
    runtime.run("Json.parse(deepJson)", "json-deep-parse.k3");
    for (const std::string &code : {"Json.parse('-9223372036854775809')", "Json.parse('1e9999')", "Json.parse('[1,]')", "Json.parse('null true')", "Json.parse('{\"a\":1,\"\\\\u0061\":2}')", "{ record String {}; Json.stringify({String(): 1}) }"}) {
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
    runtime.set("largeJson", runtime.text(std::string(65 * 1024 * 1024, ' ') + std::string(200, '[') + std::string(200, ']'), false, dk3::Span{}));
    runtime.run("Json.parse(largeJson)", "json-unbounded-parse.k3");
    runtime.set("oversizedJson", runtime.text(std::string(65 * 1024 * 1024, 'x'), false, dk3::Span{}));
    runtime.set("specialJson", runtime.text(std::string("\x01\t\"\\\xc3\xa9", 6), false, dk3::Span{}));
    runtime.run(R"(let largeEncoded = Json.stringify(oversizedJson)
var nested = []
for index in Collections.range(20000) { nested = [nested] }
let deepEncoded = Json.stringify(nested)
Json.parse(deepEncoded)
System.print(largeEncoded.length(), deepEncoded.length(), Json.stringify([specialJson, 1.0, 0.1, -0.0, 100000000000000000000.0, 9223372036854775807, null, true, (1,), {'k': []}])))", "json-unbounded.k3");
    assert(output == "68157442 40002 " R"(["\u0001\t\"\\)" "\xc3\xa9" R"(",1.0,0.1,-0.0,1e+20,9223372036854775807,null,true,[1],{"k":[]}])" "\n"); output.clear();
    for (const std::string &code : {"Json.parse(oversizedJson)", "{ class JsonRecord {}; Json.stringify(JsonRecord()) }"}) {
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
    fileRuntime.run("let text = Files.readText('text')\nlet bytes = Files.readBytes('binary')\nlet present = Files.exists('text')\nlet absent = Files.exists('missing')\nlet byteSize = Files.size('text')\nGC.collect()", "files-provider.k3");
    assert(dk3::Runtime::display(fileRuntime.get("text")) == std::string("a\xc3\xa9\0b", 5));
    assert(dk3::Runtime::typeOf(fileRuntime.get("bytes")) == "Bytes");
    assert(std::get<bool>(fileRuntime.get("present")) && !std::get<bool>(fileRuntime.get("absent")));
    assert(std::get<int64_t>(fileRuntime.get("byteSize")) == 5);
    for (const std::string &code : {"Files.readText('binary')", "Files.readBytes('large')", "Files.readText('missing')", "Files.size('missing')", "Files.size('huge')", "Files.readText('')", "Files.readText('a\\0b')", "Files.readText(b'text')"}) {
        bool rejected = false; try { fileRuntime.run(code, "files-provider-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let searchText = 'a\\xc3\\xa9b'\nlet textContains = searchText.contains\nGC.collect()\nSystem.print(textContains('\\xc3\\xa9'), textContains(''), textContains('missing'))", "string-contains.k3");
    assert(output == "true true false\n"); output.clear();
    runtime.run("let prefix = searchText.startsWith\nGC.collect()\nSystem.print(prefix('a'), prefix('a\\xc3\\xa9'), prefix(''), prefix('longer than source'), prefix('b'))", "string-prefix.k3");
    assert(output == "true true true false false\n"); output.clear();
    runtime.run("let suffix = searchText.endsWith\nGC.collect()\nSystem.print(suffix('b'), suffix('\\xc3\\xa9b'), suffix(''), suffix('longer than source'), suffix('a'))\nSystem.print(''.startsWith(''), ''.endsWith(''))", "string-suffix.k3");
    assert(output == "true true true false false\ntrue true\n"); output.clear();
    runtime.run("let find = searchText.find\nGC.collect()\nSystem.print(find('b'), find('\\xc3\\xa9'), find(''), find('missing'))\nSystem.print('\\xf0\\x9f\\x98\\x80x'.find('x'), 'aba'.find('a'))", "string-find.k3");
    assert(output == "2 1 0 -1\n1 0\n"); output.clear();
    runtime.run("let split = 'a,,b,'.split\nGC.collect()\nlet splitParts: List[String] = split(',')\nSystem.print(splitParts.length(), splitParts[1].length(), splitParts[3].length())\nSystem.print('x\\xc3\\xa9y\\xc3\\xa9'.split('\\xc3\\xa9'))\nSystem.print(''.split(',').length(), 'abc'.split('missing').length())", "string-split.k3");
    assert(output == "4 0 0\n[x, y, ]\n1 1\n"); output.clear();
    runtime.run("let join = '|'.join\nGC.collect()\nSystem.print(join(['a', '', 'b']), ','.join(('x', 'y')), '-'.join('ab'))\nSystem.print(''.join(['a', 'b']), 'separator'.join([]).length())\nfn joinedSource() { yield 'first'; GC.collect(); yield 'second' }\nSystem.print(join(joinedSource()))\nSystem.print(','.join('a,,b,'.split(',')))", "string-join.k3");
    assert(output == "a||b x,y a-b\nab 0\nfirst|second\na,,b,\n"); output.clear();
    runtime.run("var failedJoinResult = 'unchanged'\nfn failingStrings() { defer System.print('join cleanup'); yield 'a'; throw 'join failed' }\ntry { failedJoinResult = ','.join(failingStrings()) } catch error { System.print(failedJoinResult) }\nSystem.print('a\\0b'.find('\\0'), '|'.join('a\\0b'.split('\\0')))\nlet retainedParts = 'first|second'.split('|')\nGC.collect()\nSystem.print(retainedParts[1])", "string-lifetimes.k3");
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
    runtime.run("fn() { System.print('immediate') }()\nlet anonAdd: Function = fn(first: Int, second: Int = 2) -> Int => first + second\nSystem.print(anonAdd(second: 4, first: 3))\nlet anonRest = fn(first: Int, *rest) -> Int { return first + rest.length() }\nSystem.print(anonRest(1, 2, 3))\nfn anonFactory(value: Int) { return fn(addition: Int) -> Int => value + addition }\nlet anonFive = anonFactory(5)\nGC.collect()\nSystem.print(anonFive(10))\nlet anonGenerator = fn() { defer System.print('anonymous cleanup'); yield null; yield 2 }\nfor value in anonGenerator() { System.print(value) }\nlet anonDocumented = fn() -> Int { \"\"\"Anonymous documentation.\"\"\"\nreturn 1 }\nSystem.print(Reflection.doc(anonDocumented))", "anonymous-functions.k3");
    assert(output == "immediate\n7\n3\n15\nnull\n2\nanonymous cleanup\nAnonymous documentation.\n"); output.clear();
    runtime.run("let searchable: List[Int?] = [1, null, 1]\nlet listContains = searchable.contains\nGC.collect()\nSystem.print(listContains(1), listContains(null), listContains(2))", "list-contains.k3");
    assert(output == "true true false\n"); output.clear();
    runtime.run("let indexOf = searchable.indexOf\nGC.collect()\nSystem.print(indexOf(1), indexOf(null), indexOf(2))\nrecord SearchPoint { value: Int }\nlet searchPoints: List[SearchPoint] = [SearchPoint(2)]\nSystem.print(searchPoints.indexOf(SearchPoint(2)))", "list-indexof.k3");
    assert(output == "0 1 -1\n0\n"); output.clear();
    runtime.run("let clearable: List[Int] = [1, 2]\nlet listAlias = clearable\nlet listCopy = List(clearable)\nlet clearList = clearable.clear\nGC.collect()\nclearList()\nclearList()\nSystem.print(listAlias.length(), listCopy.length())\nclearable.append(3)", "list-clear.k3");
    assert(output == "0 2\n"); output.clear();
    runtime.run("let reversible: List[Int] = [1, 2, 3]\nlet reverse = reversible.reverse\nlet reverseAlias = reversible\nGC.collect()\nreverse()\nSystem.print(reverseAlias)\nreverse()\nSystem.print(reversible)\nlet reverseEmpty: List[Int] = []\nreverseEmpty.reverse()", "list-reverse.k3");
    assert(output == "[3, 2, 1]\n[1, 2, 3]\n"); output.clear();
    dk3::Config reverseBudget; reverseBudget.stepLimit = 30;
    dk3::Runtime limitedReverse(reverseBudget);
    limitedReverse.run("let data = Collections.range(8)", "reverse-budget-setup.k3");
    auto reverseData = std::get<dk3::Object *>(limitedReverse.get("data"));
    reverseData->items.assign(100, int64_t{1}); reverseData->items.front() = int64_t{2};
    bool reverseLimited = false;
    try { limitedReverse.run("data.reverse()", "reverse-budget.k3"); } catch (const dk3::Error &error) { reverseLimited = error.category == "limit"; }
    assert(reverseLimited && std::get<int64_t>(reverseData->items.front()) == 2);
    runtime.run("let clearableMap: Map[String, Int] = {'x': 1}\nlet clearMapAlias = clearableMap\nlet clearMapSnapshot = clearableMap.values()\nlet clearMap = clearableMap.clear\nGC.collect()\nclearMap()\nclearMap()\nSystem.print(clearMapAlias.length(), clearMapSnapshot)\nclearableMap['next'] = 2", "map-clear.k3");
    assert(output == "0 [1]\n"); output.clear();
    runtime.run("let safeMap: Map[String, Int?] = {'value': 7, 'empty': null}\nlet get = safeMap.get\nGC.collect()\nSystem.print(get('value'), get('empty'), get('missing'), safeMap.contains('empty'))\nlet absent: Int? = get('missing')", "map-get.k3");
    assert(output == "7 null null true\n"); output.clear();
    runtime.run("class SearchIdentity {}\nlet identity = SearchIdentity()\nlet identities: List[SearchIdentity] = [identity]\nSystem.print(identities.contains(identity), identities.contains(SearchIdentity()))\nlet nestedGet: Map[String, List[Int]] = {'x': [1]}\nlet retrieved = nestedGet.get('x')\nnestedGet.clear()\nGC.collect()\nSystem.print(retrieved)", "collection-ownership.k3");
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
    runtime.run("let inserted: List[Int] = [1, 3]\nlet insert = inserted.insert\nGC.collect()\ninsert(1, 2)\ninserted.insert(-1, 8)\ninserted.insert(inserted.length(), 4)\nSystem.print(inserted)\nlet emptyInsertion: List[Int] = []\nemptyInsertion.insert(0, 1)", "list-insertion.k3");
    assert(output == "[1, 2, 8, 3, 4]\n"); output.clear();
    for (const std::string &code : {"inserted.insert(-99, 1)", "inserted.insert(99, 1)", "inserted.insert(1.0, 1)", "inserted.insert(1)", "inserted.insert(index: 1, value: 2)"}) {
        bool rejected = false; try { runtime.run(code, "insertion-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(std::get<dk3::Object *>(runtime.get("inserted"))->items.size() == 5);
    runtime.run("let removedItems: List[Int?] = [1, null, 3]\nlet removeAt = removedItems.removeAt\nGC.collect()\nSystem.print(removeAt(1), removeAt(-1), removedItems)\nlet removedLast: Int? = removedItems.removeAt(0)\nSystem.print(removedLast, removedItems.length())", "list-removal.k3");
    assert(output == "null 3 [1]\n1 0\n"); output.clear();
    runtime.run("let extended: List[Int] = [1]\nlet extend = extended.extend\nGC.collect()\nextend((2, 3))\nextended.extend(extended)\nfn extensionSource() { yield 4; GC.collect(); yield 5 }\nextended.extend(extensionSource())\nSystem.print(extended)\nlet letters: List[String] = []\nletters.extend('abc')\nlet byteValues: List[Int] = []\nbyteValues.extend(b'ab')\nSystem.print(letters, byteValues)", "list-extension.k3");
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
    runtime.run("fn removedRecord() { record Entry { value: Int; fn read() -> Int { return self.value } }\nlet entries: List[Entry] = []\nentries.insert(0, Entry(7))\nentries.extend((Entry(8),))\nlet removed = entries.removeAt(0)\nreturn removed.read }\nlet removedReader = removedRecord()\nGC.collect()\nSystem.print(removedReader(), Reflection.members(inserted))", "removed-record-gc.k3");
    assert(output == "7 [all, any, append, clear, contains, extend, filter, find, findIndex, indexOf, insert, length, map, reduce, removeAt, reverse]\n"); output.clear();
    for (const std::string &code : {"removedItems.removeAt(0)", "inserted.removeAt(-99)", "inserted.removeAt(99)", "inserted.removeAt('bad')", "inserted.removeAt()", "inserted.removeAt(index: 0)"}) {
        bool rejected = false; try { runtime.run(code, "indexed-removal-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let membership: Map[String, Int?] = {'present': null, 'number': 3}\nlet contains = membership.contains\nGC.collect()\nSystem.print(contains('present'), contains('missing'), membership.contains('number'))", "map-membership.k3");
    assert(output == "true false true\n"); output.clear();
    runtime.run("let removable: Map[String, Int?] = {'first': 1, 'null': null, 'last': 3}\nlet remove = removable.remove\nGC.collect()\nSystem.print(remove('null'), remove('null'), removable.length(), removable.contains('last'))\nSystem.print(removable)\nremovable['new'] = 4", "map-removal.k3");
    assert(output == "true false 2 true\n{first: 1, last: 3}\n"); output.clear();
    for (const std::string &code : {"removable.remove([])", "removable.remove()", "removable.remove(key: 'first')"}) {
        bool rejected = false; try { runtime.run(code, "removal-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(std::get<dk3::Object *>(runtime.get("removable"))->entries.size() == 3);
    runtime.run("let snapshots: Map[String, List[Int]] = {'first': [1], 'second': [2]}\nlet snapshotKeys = snapshots.keys()\nlet snapshotValues = snapshots.values()\nlet detachedValues = snapshots.values\nGC.collect()\nsnapshots.remove('first')\nsnapshots['third'] = [3]\nsnapshotValues[0].append(4)\nsnapshotKeys.append('independent')\nSystem.print(snapshotKeys, snapshotValues, snapshots.keys(), detachedValues())", "map-snapshots.k3");
    assert(output == "[first, second, independent] [[1, 4], [2]] [second, third] [[2], [3]]\n"); output.clear();
    for (const std::string &code : {"snapshots.keys(1)", "snapshots.values(key: 'first')"}) {
        bool rejected = false; try { runtime.run(code, "snapshot-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("let emptySnapshotSource: Map[String, Int] = {}\nlet emptySnapshot = emptySnapshotSource.values()\nGC.collect()", "empty-snapshot.k3");
    runtime.run("fn escapedSnapshot() { record SnapshotEntry { value: Int }\nlet source: Map[String, SnapshotEntry] = {'x': SnapshotEntry(7)}\nlet snapshot = source.values()\nsource.remove('x')\nreturn snapshot }\nlet escapedSnapshotValues = escapedSnapshot()\nGC.collect()\nSystem.print(escapedSnapshotValues[0].value, Reflection.members(membership))", "snapshot-nominal-gc.k3");
    assert(output == "7 [clear, compute, computeIfAbsent, computeIfPresent, contains, get, keys, length, merge, putIfAbsent, remove, values]\n"); output.clear();
    for (const std::string &code : {"membership.contains([])", "membership.contains()", "membership.contains('present', 'other')", "membership.contains(key: 'present')"}) {
        bool rejected = false; try { runtime.run(code, "membership-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("trait RecordNamed { fn name() -> String { return 'record' } }\nrecord NamedPoint implements RecordNamed, Collections.Sized { x: Int, y: Int = x; fn length() -> Int { return 2 }; fn sum() -> Int { return self.x + self.y }; fn init() -> Int { return 9 } }\nlet namedPoint = NamedPoint(3)\nlet pointSum = namedPoint.sum\nGC.collect()\nfn recordSize(value: Collections.Sized) -> Int { return value.length() }\nSystem.print(namedPoint.name(), pointSum(), namedPoint == NamedPoint(3), recordSize(namedPoint), Collections.len(namedPoint), namedPoint.init())\nrecord MutableInside { items: List[Int] = []; fn append(value: Int) -> Null { self.items.append(value) } }\nlet mutableInside = MutableInside()\nmutableInside.append(2)\nSystem.print(mutableInside.items)", "record-methods.k3");
    assert(output == "record 6 true 2 2 9\n[2]\n"); output.clear();
    for (const std::string &code : {"namedPoint.x = 4", "record WrongRecord implements Collections.Sized {}", "record RecordWrite { x: Int; fn write() -> Null { self.x = 1 } }\nRecordWrite(2).write()"}) {
        bool rejected = false; try { runtime.run(code, "record-method-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("trait DefaultNamed { fn name() -> String { \"\"\"Default method documentation.\"\"\"\nreturn 'default' } }\nclass Named implements DefaultNamed {}\nclass OverrideNamed implements DefaultNamed { fn name() -> String { return 'override' } }\nlet named = Named()\nlet defaultName = named.name\nGC.collect()\nSystem.print(defaultName(), OverrideNamed().name(), Reflection.doc(defaultName))\ntrait CompositeDefault { fn read() -> Int; fn twice() -> Int { return self.read() * 2 } }\nclass DefaultReader implements CompositeDefault { fn read() -> Int { return 3 } }\nSystem.print(DefaultReader().twice())\ntrait OtherDefault { fn name() -> String { return 'other' } }\nclass ExplicitDefaults implements DefaultNamed, OtherDefault { fn name() -> String { return 'explicit' } }\nSystem.print(ExplicitDefaults().name())", "trait-defaults.k3");
    assert(output == "default override Default method documentation.\n6\nexplicit\n"); output.clear();
    for (const std::string &code : {"class AmbiguousDefaults implements DefaultNamed, OtherDefault {}", "class DefaultFieldConflict implements DefaultNamed { let name: String = '' }", "trait BadDefaultYield { fn read() -> Int { yield 1 } }"}) {
        bool rejected = false; try { runtime.run(code, "default-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("var initializedCount = 0\nclass Initialized { var value: Int = 0; fn init() -> Null { GC.collect(); self.value += 2; initializedCount += 1 } }\nlet initialized = Initialized(value: 3)\nSystem.print(initialized.value, initializedCount)\nclass FailedInitialization { var value: Int = 0; fn init() { defer System.print('init cleanup'); throw 'init failed' } }\ntry { FailedInitialization() } catch error { System.print(error) }", "initializers.k3");
    assert(output == "5 1\ninit cleanup\ninit failed\n"); output.clear();
    for (const std::string &code : {"class BadInitYield { fn init() { yield 1 } }", "class BadInitType { fn init() -> Int { return 1 } }", "class BadInitReturn { fn init() { return 1 } }\nBadInitReturn()"}) {
        bool rejected = false; try { runtime.run(code, "initializer-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    runtime.run("from Collections import Sized\ntrait Measured { \"\"\"A nominal measurement.\"\"\"\nfn length() -> Int }\nclass Bag { let items: List[Int] = [] }\nimpl Measured for Bag { fn length() -> Int { return self.items.length() } }\nimpl Sized for Bag {}\nlet bag = Bag([1, 2])\nfn measure(value: Measured) -> Int { return value.length() }\nfn count(value: Sized) -> Int { return value.length() }\nGC.collect()\nSystem.print(measure(bag), count(bag), Collections.len(bag), count([1]), count((1, 2)), count({'x': 1}), count('abc'), count(b'ab'))\nlet measured: List[Measured] = [bag]\nSystem.print(Reflection.doc(Measured), Reflection.members(Measured))", "traits.k3");
    assert(output == "2 2 2 1 2 1 3 2\nA nominal measurement. [length]\n"); output.clear();
    runtime.run("class EmbeddedBag implements Sized, Measured { let items: List[Int] = []; fn length() -> Int { return self.items.length() }; fn asSized() -> Sized { return self } }\nlet embedded: Measured = EmbeddedBag([1, 2, 3])\nlet embeddedLength = embedded.length\nGC.collect()\nSystem.print(measure(embedded), count(embedded), Collections.len(embedded), embeddedLength())\nlet embeddedItems: List[Sized] = [embedded]", "embedded-traits.k3");
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
    runtime.run("impl Measured for Duck {}\nSystem.print(measure(Duck()))\nfn escapedTrait() { trait LocalTrait { fn read() -> Int }\nclass LocalValue {}\nimpl LocalTrait for LocalValue { fn read() -> Int { return 7 } }\nreturn LocalValue().read }\nlet traitCallback = escapedTrait()\nGC.collect()\nSystem.print(traitCallback())", "trait-lifetimes.k3");
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
    importedTraits.run("import first as first\nimport second as second\nfn accept(value: first.Readable) -> Int { return value.read() }\nlet result = accept(first.Box())\nGC.collect()", "trait-modules.k3");
    assert(std::get<int64_t>(importedTraits.get("result")) == 3);
    runtime.run("class Counter { \"\"\"A mutable counter.\"\"\"\nvar value: Int = 0\nlet label: String = 'counter'\nfn add(amount: Int = 1) -> Int { \"\"\"Increment the counter.\"\"\"\nself.value += amount; return self.value }\nfn snapshot() -> Counter { return self }\nfn stream() { yield self.value; self.value += 1; yield self.value }\n}\nlet classCounter: Counter = Counter(value: 2)\nlet detached = classCounter.add\nGC.collect()\nSystem.print(detached(amount: 3), classCounter.snapshot() == classCounter, classCounter == Counter(5))\nclassCounter.value = 8\nfor item in classCounter.stream() { System.print(item) }\nSystem.print(Reflection.doc(classCounter), Reflection.doc(detached))", "classes.k3");
    assert(output == "5 true false\n8\n9\nA mutable counter. Increment the counter.\n"); output.clear();
    for (const std::string &code : {"classCounter.label = 'bad'", "classCounter.unknown = 1", "classCounter.add = 1", "Counter(1, value: 2)"}) {
        bool rejected = false; try { runtime.run(code, "class-errors.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    assert(std::get<int64_t>(runtime.getPath("classCounter.value")) == 9);
    runtime.run("fn escapedMethod() { class Local { var value: Int = 1; fn add() -> Int { self.value += 1; return self.value } }\nreturn Local().add }\nlet escapedAdd = escapedMethod()\nGC.collect()\nSystem.print(escapedAdd())", "escaped-method.k3");
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
    runtime.run("let reflected = host.add()\nSystem.print(Reflection.binding('reflected'))\nGC.collect()", "core-api.k3");
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
    runtime.run("record Profile { name: String, labels: List[String] = [] }\nrecord Point { x: Int, y: Int = x + 1 }\nenum Lookup { Found(Profile), Missing }\nlet firstProfile = Profile(name: 'Ada')\nlet secondProfile = Profile('Grace')\nfirstProfile.labels.append('active')\nGC.collect()\nSystem.print(Collections.len(firstProfile.labels), Collections.len(secondProfile.labels))\nlet point = Point(x: 3)\nSystem.print(point.y, point == Point(3, y: 4))\nlet lookup = Lookup.Found(firstProfile)\nSystem.print(match lookup { Lookup.Found(profile) => profile.name, Lookup.Missing => 'none' })", "record-contracts.k3");
    assert(output == "1 0\n4 true\nAda\n");
    output.clear();
    runtime.run("enum Marker: Bytes { Start = b'\\xFF\\xD8', End = b'\\xFF\\xD9' }\nlet scale = 10\nenum Level { Low = scale, High = Level.Low.value * 2 }\nGC.collect()\nlet header = b'\\xFF\\xD9\\x00'\nSystem.print(Marker.fromValue(header[0:2]) == Marker.End, Marker.fromValue(b'\\x00'), Marker.Start.value)\nSystem.print(Level.High.value, Level.fromValue(10) == Level.Low, match Level.High { Level.Low => 'low', Level.High => 'high' })\nSystem.print(Level.Low, [Marker.End])", "enum-values.k3");
    assert(output == "true null b\"\\xff\\xd8\"\n20 true high\nLevel.Low [Marker.End]\n");
    for (const std::string &source : {"enum Twice { A = 1, B = 1 }", "enum Wrong { A = true }"}) {
        bool rejected = false;
        try { runtime.run(source, "invalid-enum.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    output.clear();
    runtime.run("enum Twice { A = 1, B = 2 }\nSystem.print(Twice.B.value)", "enum-rollback.k3");
    assert(output == "2\n");
    output.clear();
    runtime.run("var bits = 0b1100\nbits |= 1\nbits &= ~4\nbits ^= 0xF0\nbits <<= 1\nbits >>= 2\nSystem.print(bits, 5 & 3, 5 | 3, 5 ^ 3, ~0, 1 << 63, -8 >> 1, 1 << 64, -1 >> 70, 3 & 1 == 1)", "bitwise.k3");
    assert(output == "124 1 7 6 -1 -9223372036854775808 -4 0 -1 true\n");
    for (const std::string &source : {"System.print(1 << -1)", "System.print(8 >> -2)"}) {
        bool rejected = false;
        try { runtime.run(source, "invalid-shift.k3"); } catch (const dk3::Error &error) { rejected = error.category == "value"; }
        assert(rejected);
    }
    output.clear();
    runtime.run("let packet = b'\\xFF\\xD8\\x00\\x10JFIF\\x00\\xFE\\xFF'\nGC.collect()\n"
                "System.print(packet[2:4].toInt(), packet.readInt(0, 2), packet.readSignedInt(0, 2), packet.readInt(2, 2, 'little'), packet.readInt(2, 2, order: 'little'), packet[9:11].toSignedInt(order: 'little'))\n"
                "System.print(4096.toBytes(2), 4096.toBytes(4, 'little'), (-2).toBytes(2), Bytes.of([74, 70]), b'-'.join([b'a', b'b']), packet[4:9])\n"
                "System.print(packet.find(b'JFIF'), packet.find(b'\\xFF', 1), packet.find(b'Z'), packet.contains(b'IF'), packet.startsWith(b'\\xFF\\xD8'), packet.endsWith(b'\\xFF'))", "bytes-api.k3");
    assert(output == "16 65496 -40 4096 4096 -2\nb\"\\x10\\x00\" b\"\\x00\\x10\\x00\\x00\" b\"\\xff\\xfe\" b\"JF\" b\"a-b\" b\"JFIF\\x00\"\n4 10 -1 true true true\n");
    for (const std::string &source : {"packet.readInt(9, 4)", "packet.readInt(0, 9)", "packet.toInt()", "packet[0:2].toInt('middle')", "256.toBytes(1)", "Bytes.of([256])", "packet.find(b'x', 99)", "packet.readInt(0, size: 2, offset: 1)", "packet.readInt(size: 2)"}) {
        bool rejected = false;
        try { runtime.run(source, "invalid-bytes.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    output.clear();
    runtime.run("var pulled = 0\nfn source() { for x in [1, 2, 3, 4, 5, 6] { pulled += 1; yield x } }\nlet pipeline = source().map(fn(x: Int) -> Int => x * x).filter(fn(x: Int) -> Bool => x % 2 == 0)\nGC.collect()\n"
                "System.print(pulled, pipeline.take(2).toList(), pulled)\n"
                "System.print(Collections.stream([b'\\x01\\x02', b'\\x03']).flatMap(fn(b: Bytes) -> Bytes => b).toList(), Collections.stream(Collections.range(10)).skip(7).count(), source().dropWhile(fn(x: Int) -> Bool => x < 5).first())\n"
                "System.print(source().takeWhile(fn(x: Int) -> Bool => x < 3).reduce(callback: fn(a: Int, b: Int) -> Int => a + b, initial: 0), Collections.stream([2, 2]).toSet(), Collections.stream([1]).any(fn(x: Int) -> Bool => x > 0), Collections.stream([1]).all(fn(x: Int) -> Bool => x > 1))\n"
                "System.print(Collectors.joining(['a', 'b'], '/'), Collectors.groupBy([1, 2, 3], fn(x: Int) -> Int => x % 2), Collectors.partitionBy([1, 2], fn(x: Int) -> Bool => x > 1), Collectors.toMap([1], fn(x: Int) -> Int => x, fn(x: Int) -> Int => -x))", "streams.k3");
    assert(output == "0 [4, 16] 4\n[1, 2, 3] 3 5\n3 Set(2) true false\na/b {1: [1, 3], 0: [2]} {true: [2], false: [1]} {1: -1}\n");
    output.clear();
    runtime.run("let png = Encoding.fromHex('89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001349444154789c63f8cfc0f01f0c8134083400004949097828a0db770000000049454e44ae426082')\nGC.collect()\n"
                "let info = ImageCodec.info(png)\nlet rgba = ImageCodec.decode(png)\n"
                "System.print(info, rgba.length(), rgba.readInt(0, 4), rgba[12:16], ImageCodec.resize(rgba, 2, 2, 1, 1), ImageCodec.resize(rgba, 2, 2, 4, 4).length(), ImageCodec.resize(rgba, 2, 2, 2, 1)[0:4])", "image-codec.k3");
    assert(output == "(2, 2, 4) 16 4278190335 b\"\\xff\\xff\\xff\\x80\" b\"mmm\\xdf\" 64 b\"\\x7f\\x00\\x7f\\xff\"\n");
    // Untrusted input fails with catchable value errors: oversized headers, and stb reasons embedding corrupt chunk-name bytes.
    output.clear();
    runtime.run("let huge = Encoding.fromHex('89504e470d0a1a0a0000000d494844520000271000002710' + '0802000000' + '00000000' + '0000000049444154')\n"
                "try { ImageCodec.info(huge) } catch error { System.print(error.kind, error.message) }\n"
                "let corrupt = Encoding.fromHex('89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001349444154789c63f8cfc0f01f0c8134083400004949097828a0db770000000049454e86ae426082')\n"
                "try { ImageCodec.decode(corrupt) } catch error { System.print(error.kind, error.message) }", "image-errors.k3");
    assert(output == "value Image is larger than 67108864 pixels\nvalue Cannot decode image: IEN? PNG chunk not known\n");
    for (const std::string &source : {"ImageCodec.info(b'')", "ImageCodec.decode(b'not an image')", "ImageCodec.decode('text')", "ImageCodec.resize(b'abcd', 2, 2, 1, 1)", "ImageCodec.resize(b'abcd', 1, 1, 0, 1)", "ImageCodec.resize(b'abcd', 1, 1, 100000, 100000)", "ImageCodec.resize(b'abcd', 1, 1, 1)"}) {
        bool rejected = false;
        try { runtime.run(source, "invalid-image.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    // a[i, j] indexes with the tuple (i, j); subscripts containing a slice go to __getslice__/__setslice__ as Collections.Slice records.
    output.clear();
    runtime.run("class Grid {\n  var log: List<Object?> = []\n"
                "  fn __getitem__(key: Int | Tuple<Int>) -> Object? { return key }\n"
                "  fn __setitem__(key: Int | Tuple<Int>, value: Int) -> Null { self.log.append((key, value)) }\n"
                "  fn __getslice__(key: Tuple<Int | Collections.Slice>) -> Object? { return key }\n"
                "  fn __setslice__(key: Tuple<Int | Collections.Slice>, value: Int) -> Null { self.log.append((key, value)) }\n"
                "  fn __rmul__(other: Int) -> String { return 'rmul ' + Text.str(other) }\n"
                "  fn __rsub__(other: Int) -> String { return 'rsub ' + Text.str(other) }\n"
                "  fn __gt__(other: Int) -> String { return 'gt ' + Text.str(other) }\n}\n"
                "let grid = Grid()\nGC.collect()\n"
                "System.print(grid[1], grid[1, -2], grid[1:2], grid[::-1, 0])\nSystem.print(grid[:, 1:])\n"
                "grid[3, 4] = 5\ngrid[0:1] = 7\ngrid[1, :] = 8\nSystem.print(grid.log)\n"
                "System.print(2 * grid, 2 - grid, 1 < grid, grid > 3, Collections.Slice(1, step: -1))", "subscripts.k3");
    assert(output == "1 (1, -2) (Slice(start: 1, stop: 2, step: null),) (Slice(start: null, stop: null, step: -1), 0)\n"
                     "(Slice(start: null, stop: null, step: null), Slice(start: 1, stop: null, step: null))\n"
                     "[((3, 4), 5), ((Slice(start: 0, stop: 1, step: null),), 7), ((1, Slice(start: null, stop: null, step: null)), 8)]\n"
                     "rmul 2 rsub 2 gt 1 gt 3 Slice(start: 1, stop: null, step: -1)\n");
    for (const std::string &source : {"[1, 2][0:1, 0]", "let fixed = [1, 2]\nfixed[0:1] = 3", "grid[0:1] += 1", "grid[0, 1:] -= 1", "grid[1:]++", "grid['a':]",
                                      "class BadSlice { fn __setslice__(key: Object, value: Object) -> Int { return 1 } }"}) {
        bool rejected = false;
        try { runtime.run(source, "invalid-subscripts.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    // NDKernel: layouts are (offset, dimensions..., strides...); stride 0 broadcasts and negative strides walk backwards.
    output.clear();
    runtime.run("let a = [0.0, 1.0, 2.0, 3.0, 4.0, 5.0]\nGC.collect()\n"
                "System.print(NDKernel.binary('add', a, (0, 2, 3, 3, 1), [10.0, 20.0], (0, 2, 3, 1, 0)), NDKernel.copy(a, (5, 3, -2)), NDKernel.unary('sqrt', [4, 9], (0, 2, 1)))\n"
                "System.print(NDKernel.reduce('sum', a, (0, 2, 3, 3, 1), 0), NDKernel.reduce('argmax', a, (0, 2, 3, 3, 1), -1), NDKernel.reduce('mean', a, (0, 2, 3, 3, 1), 1), NDKernel.reduce('sum', [], (0, 0, 1), -1))\n"
                "System.print(NDKernel.matmul(a, (0, 2, 3, 3, 1), a, (0, 3, 2, 1, 3)), NDKernel.where([1.0, 0.0], (0, 2, 1), a, (0, 2, 1), [-1.0], (0, 2, 0)), NDKernel.binary('mod', [-7.0, 7.0], (0, 2, 1), [3.0], (0, 2, 0)))\n"
                "let shifted = [0.0, 1.0, 2.0, 3.0]\nNDKernel.assign(shifted, (1, 3, 1), shifted, (0, 3, 1))\n"
                "System.print(shifted, NDKernel.toBytes([0.5, 2.0, -1.0], (0, 3, 1), 255.0), NDKernel.fromBytes(b'\\x00\\xff', 1.0), NDKernel.arange(0, 1, 0.25), NDKernel.linspace(0, 1, 3))\n"
                "System.print(NDKernel.format(a, (0, 2, 3, 3, 1), 8))\nSystem.print(NDKernel.format([0.5, 2.25, 3.0, 0.1], (0, 4, 1), 8), NDKernel.format([0.00001, 1.0], (0, 2, 1), 8), NDKernel.format([7.0], (0,), 8))", "ndkernel.k3");
    assert(output == "[10, 11, 12, 23, 24, 25] [5, 3, 1] [2, 3]\n[3, 5, 7] [5] [1, 4] [0]\n[5, 14, 14, 50] [0, -1] [2, 1]\n"
                     "[0, 0, 1, 2] b\"\\x80\\xff\\x00\" [0, 255] [0, 0.25, 0.5, 0.75] [0, 0.5, 1]\n"
                     "[[0. 1. 2.]\n [3. 4. 5.]]\n[0.5  2.25 3.   0.1 ] [1.e-05 1.e+00] 7.0\n");
    // Layouts are validated before any element is touched; failures are catchable.
    for (const auto &[source, category] : std::vector<std::pair<std::string, std::string>>{
             {"NDKernel.copy([1.0], (0, 2, 1))", "index"}, {"NDKernel.copy([1.0], (-1, 1, 1))", "index"}, {"NDKernel.copy([1.0], (0, 3, 4611686018427387904))", "index"},
             {"NDKernel.copy([1.0], (0, 1))", "value"}, {"NDKernel.copy([1.0], (0, -1, 1))", "value"}, {"NDKernel.copy([1.0], (0, 6000, 6000, 0, 0))", "value"},
             {"NDKernel.copy(['x'], (0, 1, 1))", "type"}, {"NDKernel.copy((1.0,), (0, 1, 1))", "type"}, {"NDKernel.binary('nope', [1.0], (0,), [1.0], (0,))", "value"},
             {"NDKernel.binary('add', [1.0], (0, 1, 1), [1.0], (0, 2, 0))", "value"}, {"NDKernel.reduce('min', [], (0, 0, 1), -1)", "value"},
             {"NDKernel.reduce('sum', [1.0], (0, 1, 1), 1)", "index"}, {"NDKernel.fill(-1, 0.0)", "value"}, {"NDKernel.fill(40000000, 0.0)", "value"},
             {"NDKernel.matmul([1.0], (0, 1, 1), [1.0], (0, 1, 1))", "value"}, {"NDKernel.arange(0, 1, 0)", "value"}, {"NDKernel.format([1.0], (0,), 30)", "value"}}) {
        std::string caught;
        try { runtime.run(source, "invalid-ndkernel.k3"); } catch (const dk3::Error &error) { caught = error.category; }
        assert(caught == category);
    }
    {
        dk3::Config kernelBudget; kernelBudget.stepLimit = 1000;
        dk3::Runtime limitedKernel(kernelBudget);
        bool limited = false;
        try { limitedKernel.run("NDKernel.copy(NDKernel.fill(5000, 1.0), (0, 5000, 1))", "kernel-budget.k3"); } catch (const dk3::Error &error) { limited = error.category == "limit"; }
        assert(limited);
    }
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
    runtime.run("fn capture() { class Box { var count: Int = 0 }\nlet box = Box()\nreturn fn() { box.count += 1\nreturn box.count } }\nlet counter = capture()\nGC.collect()\nSystem.print(counter())\nSystem.print(counter())\n", "closures.k3");
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
    typedModules.run("let importedBox: model.Box = model.Box(3)\nfn sameBox(value: model.Box) -> model.Box { return value }\nGC.collect()\nSystem.print(sameBox(importedBox).set(7))", "qualified-classes.k3");
    assert(output == "7\n");
    output.clear();
    typedModules.run("let event: model.Event = model.Event.Move(imported)\nSystem.print(match event { model.Event.Move(point) => point.x, model.Event.Quit => 0 })\nlet optional: model.Point? = null\nvar points: List[model.Point] = [imported]\npoints.append(model.Point(4))\nGC.collect()\nSystem.print(points[1].x)", "qualified-enums.k3");
    assert(output == "3\n4\n");
    for (const std::string &source : {"let invalid: model.Event = other.Event.Quit()"}) {
        bool rejected = false;
        try { typedModules.run(source, "invalid-qualified.k3"); } catch (const dk3::Error &) { rejected = true; }
        assert(rejected);
    }
    typedModules.run("var Alias = model.Point\nvar pinned: Alias? = null\nAlias = other.Point\npinned = imported\nGC.collect()\nlet aliasedList: List[model.Point] = points", "pinned-types.k3");
    runtime.run("fn escapedContract() { record PrivateShape { x: Int }\nlet values: List[PrivateShape?] = [null]\nreturn values }\nlet privateValues = escapedContract()\nGC.collect()", "escaped-contract.k3");
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
    runtime.run("let looseList = []\nlet looseMap = {'first': looseList, 'bad': 'wrong'}\ntry { let annotated: Map[String, List[Int]] = looseMap } catch error {}\nlooseList.append('still untyped')\nlet looseOuter = [[], 'bad']\ntry { let annotated: List[List[Int]] = looseOuter } catch error {}\nlooseOuter[0].append('not partially pinned')\nGC.collect()", "atomic-contracts.k3");
    output.clear();
    typedModules.run("let mapOfPoints: Map[String, model.Point?] = {'origin': imported, 'empty': null}\nmapOfPoints['other'] = model.Point(8)\nGC.collect()\nSystem.print(mapOfPoints['other'].x)", "nominal-map.k3");
    assert(output == "8\n");
    runtime.run("fn privateMap() { record Hidden {}\nlet values: Map[String, Hidden?] = {'none': null}\nreturn values }\nlet hiddenMap = privateMap()\nGC.collect()", "map-contract-gc.k3");
    dk3::Span span{std::make_shared<dk3::Source>(dk3::Source{"host", ""}), 0, 0, 1, 1};
    size_t handle = runtime.retain(runtime.text("persistent", false, span));
    runtime.run("GC.collect()", "handles.k3");
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