#include "runtime.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::string output;
    bool interrupted = false;
    dk3::Config config; config.write = [&](const std::string &text) { output += text; };
    config.interrupted = [&] { return interrupted; };
    dk3::Runtime runtime(config);
    runtime.run("fn deferred() { var label = 'captured'\ndefer System.print(label)\nlabel = 'changed'\nyield null\nyield 2 }\nlet generator = deferred()", "defer.k3");
    dk3::Value generator = runtime.get("generator"), item; size_t cursor = 0;
    assert(runtime.resume(generator, item, cursor));
    assert(std::holds_alternative<std::monostate>(item));
    assert(output.empty()); runtime.collect();
    assert(runtime.resume(generator, item, cursor) && std::get<int64_t>(item) == 2);
    assert(output.empty());
    assert(!runtime.resume(generator, item, cursor));
    assert(output == "captured\n"); runtime.close(generator); assert(output == "captured\n");
    output.clear();
    runtime.run("fn abandoned() { defer System.print('outer')\n{ defer System.print('inner')\nyield 1 } }\nlet abandonedGenerator = abandoned()", "close.k3");
    generator = runtime.get("abandonedGenerator"); cursor = 0;
    assert(runtime.resume(generator, item, cursor)); runtime.collect(); runtime.close(generator);
    assert(output == "inner\nouter\n"); assert(!runtime.resume(generator, item, cursor));
    output.clear();
    runtime.run("fn fails() { defer System.print('cleanup')\nyield 1\nmissing() }\nlet failingGenerator = fails()", "failure.k3");
    generator = runtime.get("failingGenerator"); cursor = 0;
    assert(runtime.resume(generator, item, cursor));
    bool failed = false;
    try { runtime.resume(generator, item, cursor); } catch (const dk3::Error &) { failed = true; }
    assert(failed && output == "cleanup\n"); assert(!runtime.resume(generator, item, cursor));
    output.clear();
    runtime.run("fn transfers() { for number in Collections.range(4) { defer System.print('leave', number)\nif number == 1 { continue }\nif number == 3 { break }\nyield number }\nyield 9 }\nfor value in transfers() { System.print('item', value) }", "transfers.k3");
    assert(output == "item 0\nleave 0\nleave 1\nitem 2\nleave 2\nleave 3\nitem 9\n");
    output.clear();
    runtime.run("fn nestedLoops() { var number = 0\nwhile number < 3 { number += 1\nfor inner in Collections.range(3) { if inner == 1 { break }\nyield number }\nif number == 2 { continue }\nyield 10 } }\nfor value in nestedLoops() { System.print(value) }", "nested-loops.k3");
    assert(output == "1\n10\n2\n3\n10\n");
    output.clear();
    runtime.run("fn handled() { defer System.print('done')\ntry { defer System.print('body')\nyield 1\nthrow 'payload' } catch error { defer System.print('catch')\nyield error } finally { System.print('finally')\nyield 3\nSystem.print('finally-end') }\nyield 4 }\nlet handledGenerator = handled()", "handlers.k3");
    generator = runtime.get("handledGenerator"); cursor = 0;
    assert(runtime.resume(generator, item, cursor) && std::get<int64_t>(item) == 1);
    assert(runtime.resume(generator, item, cursor) && dk3::Runtime::display(item) == "payload");
    assert(output == "body\n"); runtime.collect();
    assert(runtime.resume(generator, item, cursor) && std::get<int64_t>(item) == 3);
    assert(runtime.resume(generator, item, cursor) && std::get<int64_t>(item) == 4);
    assert(!runtime.resume(generator, item, cursor));
    assert(output == "body\ncatch\nfinally\nfinally-end\ndone\n");
    output.clear();
    runtime.run("fn returns() { defer System.print('outer')\ntry { yield 1\nreturn } catch error { System.print('unexpected') } finally { yield 2\nSystem.print('final') } }\nlet returningGenerator = returns()", "return-finally.k3");
    generator = runtime.get("returningGenerator"); cursor = 0;
    assert(runtime.resume(generator, item, cursor));
    assert(runtime.resume(generator, item, cursor) && std::get<int64_t>(item) == 2);
    runtime.collect(); assert(!runtime.resume(generator, item, cursor)); assert(output == "final\nouter\n");
    output.clear();
    runtime.run("fn pending() { try { yield 1\nthrow ['pending'] } catch error { throw error } finally { yield 2\nSystem.gc() } }\nlet pendingGenerator = pending()", "pending-error.k3");
    generator = runtime.get("pendingGenerator"); cursor = 0;
    assert(runtime.resume(generator, item, cursor)); assert(runtime.resume(generator, item, cursor)); runtime.collect();
    failed = false;
    try { runtime.resume(generator, item, cursor); } catch (const dk3::Error &error) { failed = std::string(error.what()) == "[pending]"; }
    assert(failed && !runtime.resume(generator, item, cursor));
    runtime.run("fn closeHandled() { defer System.print('outer')\ntry { defer System.print('body')\nyield 1 } catch error { System.print('unexpected') } finally { System.print('final-start')\nyield 2\nSystem.print('final-end') } }\nlet closeGenerator = closeHandled()", "close-handlers.k3");
    generator = runtime.get("closeGenerator"); cursor = 0; assert(runtime.resume(generator, item, cursor));
    runtime.close(generator); assert(output == "body\nfinal-start\nfinal-end\nouter\n");
    runtime.close(generator); assert(!runtime.resume(generator, item, cursor));
    output.clear();
    runtime.run("fn transfersHandled() { for number in Collections.range(3) { try { yield number\nif number == 0 { continue }\nbreak } catch error { System.print('unexpected') } finally { System.print('leave', number) } }\nyield 9 }\nfor value in transfersHandled() { System.print(value) }", "transfer-handlers.k3");
    assert(output == "0\nleave 0\n1\nleave 1\n9\n");
    output.clear();
    runtime.run("fn activeFinally() { defer System.print('outer')\ntry { yield 1\nreturn } catch error {} finally { System.print('start')\nyield 2\nSystem.print('end') } }\nlet activeGenerator = activeFinally()", "active-finally.k3");
    generator = runtime.get("activeGenerator"); cursor = 0;
    assert(runtime.resume(generator, item, cursor)); assert(runtime.resume(generator, item, cursor));
    assert(output == "start\n"); runtime.close(generator); assert(output == "start\nend\nouter\n");
    output.clear();
    runtime.run("fn cancelled() { defer System.print('outer')\ntry { defer System.print('body')\nyield 1\nSystem.print('unexpected') } catch error { System.print('caught') } finally { System.print('finally')\nyield 2\nSystem.print('finished') } }\nlet cancelledGenerator = cancelled()", "cancelled.k3");
    generator = runtime.get("cancelledGenerator"); cursor = 0; assert(runtime.resume(generator, item, cursor));
    interrupted = true; failed = false;
    try { runtime.resume(generator, item, cursor); } catch (const dk3::Error &error) { failed = error.category == "cancelled"; }
    interrupted = false;
    assert(failed && output == "body\nfinally\nfinished\nouter\n"); assert(!runtime.resume(generator, item, cursor));
    output.clear();
    runtime.run("fn primaryFailure() { defer System.print('outer')\ntry { yield 1\nthrow 'primary' } catch error { throw error } finally { defer System.print('inner')\nmissing() } }\nlet primaryGenerator = primaryFailure()", "primary-error.k3");
    generator = runtime.get("primaryGenerator"); cursor = 0; assert(runtime.resume(generator, item, cursor)); failed = false;
    try { runtime.resume(generator, item, cursor); } catch (const dk3::Error &error) { failed = std::string(error.what()) == "primary"; }
    assert(failed && output == "inner\nouter\n");
    output.clear();
    runtime.run("fn neverStarted() { defer System.print('unexpected')\nyield 1 }\nlet freshGenerator = neverStarted()", "fresh.k3");
    runtime.close(runtime.get("freshGenerator")); assert(output.empty());
    runtime.run("fn scriptClose() { defer System.print('closed')\nyield 1\nyield 2 }\nlet partial = scriptClose()\nfor value in partial { break }\nTasks.close(partial)\nTasks.close(partial)\nfor value in partial { System.print('unexpected') }", "script-close.k3");
    assert(output == "closed\n"); output.clear();
    runtime.run("fn innerLoop() { try { for value in Collections.range(3) { yield value\nbreak }\nyield 5 } catch error { System.print('unexpected') } finally { System.print('finally') }\nyield 9 }\nfor value in innerLoop() { System.print(value) }", "inner-loop.k3");
    assert(output == "0\n5\nfinally\n9\n"); output.clear();
    runtime.run("fn nativeGc() { defer fn() { System.print('retained') }()\ndefer force_gc()\nyield 1 }\nlet nativeGcGenerator = nativeGc()", "native-gc.k3");
    runtime.registerNative("force_gc", [&](const auto &, const dk3::Span &) -> dk3::Value { runtime.collect(); return {}; });
    generator = runtime.get("nativeGcGenerator"); cursor = 0; assert(runtime.resume(generator, item, cursor)); runtime.close(generator);
    assert(output == "retained\n"); output.clear();
    runtime.run("fn badDeferred() { throw 'cleanup-error' }\nfn failingTransfer() { try { for number in Collections.range(1) { defer badDeferred()\nyield 1\nbreak }\nyield 'unexpected' } catch error { yield error } finally { System.print('finally') } }\nlet transferGenerator = failingTransfer()", "failing-transfer.k3");
    generator = runtime.get("transferGenerator"); cursor = 0; assert(runtime.resume(generator, item, cursor));
    assert(runtime.resume(generator, item, cursor) && dk3::Runtime::display(item) == "cleanup-error");
    assert(!runtime.resume(generator, item, cursor) && output == "finally\n"); output.clear();
    runtime.run("fn selfClosing() { defer System.print('cleanup')\nyield 1\nTasks.close(selfGenerator) }\nlet selfGenerator = selfClosing()", "reentry.k3");
    generator = runtime.get("selfGenerator"); cursor = 0; assert(runtime.resume(generator, item, cursor)); failed = false;
    try { runtime.resume(generator, item, cursor); } catch (const dk3::Error &error) { failed = error.category == "state"; }
    assert(failed && output == "cleanup\n" && !runtime.resume(generator, item, cursor)); output.clear();
    runtime.run("fn throwingClose() { defer fn() { throw ['close-error'] }()\nyield 1 }\nlet throwingResource = throwingClose()\nfor value in throwingResource { break }\ntry { Tasks.close(throwingResource) } catch error { System.gc()\nSystem.print(error[0]) }", "close-payload.k3");
    assert(output == "close-error\n"); output.clear();
    dk3::Config limitedConfig; limitedConfig.stepLimit = 80;
    limitedConfig.write = [&](const std::string &text) { output += text; };
    dk3::Runtime limited(limitedConfig);
    limited.run("fn budgeted() { defer System.print('outer')\ntry { yield 1\nwhile true {} } catch error { System.print('unexpected') } finally { System.print('finally') } }\nlet limitedGenerator = budgeted()", "budget.k3");
    generator = limited.get("limitedGenerator"); cursor = 0; assert(limited.resume(generator, item, cursor)); failed = false;
    try { limited.resume(generator, item, cursor); } catch (const dk3::Error &error) { failed = error.category == "limit"; }
    assert(failed && output == "finally\nouter\n"); assert(!limited.resume(generator, item, cursor));
    output.clear();
    limited.run("fn endlessCleanup() { defer System.print('outer')\ntry { yield 1 } catch error {} finally { while true {} } }\nlet endlessGenerator = endlessCleanup()", "cleanup-budget.k3");
    generator = limited.get("endlessGenerator"); cursor = 0; assert(limited.resume(generator, item, cursor)); failed = false;
    try { limited.close(generator); } catch (const dk3::Error &error) { failed = error.category == "limit"; }
    assert(failed && !limited.resume(generator, item, cursor));
    limited.run("fn fatalPrecedence() { try { try { yield 1\nthrow 'earlier' } catch error { throw error } finally { while true {} } } catch error { yield 'swallowed' } }\nlet fatalGenerator = fatalPrecedence()", "fatal-precedence.k3");
    generator = limited.get("fatalGenerator"); cursor = 0; assert(limited.resume(generator, item, cursor)); failed = false;
    try { limited.resume(generator, item, cursor); } catch (const dk3::Error &error) { failed = error.category == "limit"; }
    assert(failed && !limited.resume(generator, item, cursor));
    std::cout << "generators: PASS\n";
}