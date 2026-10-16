#include "runtime.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::string output;
    dk3::Config config;
    config.write = [&](const std::string &text) { output += text; };
    dk3::Runtime runtime(config);
    runtime.run("fn worker() { defer System.print('cleanup')\nyield null\nyield 2 }\nlet job = Tasks.spawn(worker())\nSystem.print(job.status)\nSystem.print(Tasks.pump(1))\nSystem.print(job.status)\nSystem.gc()\nTasks.cancel(job)\nSystem.print(job.status)\nTasks.cancel(job)\nSystem.print(Tasks.pump(1))", "task-lifecycle.k3");
    assert(output == "ready\n1\nready\ncleanup\ncancelled\n0\n");
    output.clear();
    runtime.run("fn child(label) { defer System.print(label)\nwhile true { yield null } }\nfn parentWork() { defer System.print('parent')\nTasks.spawn(child('first'))\nTasks.spawn(child('second'))\nwhile true { yield null } }\nlet family = Tasks.spawn(parentWork())\nTasks.pump(3)\nSystem.gc()\nTasks.cancel(family)\nSystem.print(family.status)", "task-family.k3");
    assert(output == "second\nfirst\nparent\ncancelled\n"); output.clear();
    runtime.run("fn completes() { defer System.print('parent')\nTasks.spawn(child('child'))\nyield null }\nlet completion = Tasks.spawn(completes())\nTasks.pump(3)\nSystem.print(completion.status)", "task-completion.k3");
    assert(output == "child\nparent\ndone\n"); output.clear();
    runtime.run("fn explicitReturn() { defer System.print('parent')\nTasks.spawn(child('child'))\nyield null\nreturn }\nlet returning = Tasks.spawn(explicitReturn())\nTasks.pump(3)\nSystem.print(returning.done)", "task-return.k3");
    assert(output == "child\nparent\ntrue\n"); output.clear();
    runtime.run("fn badChild() { defer System.print('child')\nyield null\nthrow ['failure'] }\nfn failingParent() { defer System.print('parent')\nTasks.spawn(badChild())\nwhile true { yield null } }\nlet failing = Tasks.spawn(failingParent())\nTasks.pump(2)\ntry { Tasks.pump(2) } catch error { System.gc()\nSystem.print(error[0]) }\nSystem.print(failing.status)\nSystem.print(failing.error[0])", "task-failure.k3");
    assert(output == "child\nparent\nfailure\nfailed\nfailure\n"); output.clear();
    runtime.run("fn selfWork() { defer System.print('self cleanup')\nyield null\nTasks.cancel(selfTask)\nSystem.print('unexpected')\nyield 2 }\nlet selfTask = Tasks.spawn(selfWork())\nTasks.pump(2)\nSystem.print(selfTask.status)", "self-cancel.k3");
    assert(output == "self cleanup\ncancelled\n"); output.clear();
    runtime.run("var ancestor = null\nfn cancellingChild() { defer System.print('child')\nyield null\nTasks.cancel(ancestor)\nSystem.print('unexpected')\nyield 2 }\nfn ancestorWork() { defer System.print('parent')\nTasks.spawn(cancellingChild())\nwhile true { yield null } }\nancestor = Tasks.spawn(ancestorWork())\nTasks.pump(5)\nSystem.print(ancestor.status)", "ancestor-cancel.k3");
    assert(output == "child\nparent\ncancelled\n"); output.clear();
    runtime.run("fn fair(label) { for number in Collections.range(2) { System.print(label, number)\nyield number } }\nlet first = Tasks.spawn(fair('a'))\nlet second = Tasks.spawn(fair('b'))\nSystem.print(first.has_value)\nSystem.print(Tasks.pump(6))\nSystem.print(first.done, second.done)\nSystem.print(first.value, second.value)", "round-robin.k3");
    assert(output == "false\na 0\nb 0\na 1\nb 1\n6\ntrue true\n1 1\n"); output.clear();
    runtime.run("let ownedGenerator = child('unused')\nlet exclusive = Tasks.spawn(ownedGenerator)\ntry { Tasks.spawn(ownedGenerator) } catch error { System.print(error.kind) }\ntry { Tasks.close(ownedGenerator) } catch error { System.print(error.kind) }\ntry { for value in ownedGenerator { break } } catch error { System.print(error.kind) }\nTasks.cancel(exclusive)\nSystem.print(exclusive.status)", "exclusive-task.k3");
    assert(output == "state\nstate\nstate\ncancelled\n"); output.clear();
    runtime.run("fn forbiddenPump() { yield null\nTasks.pump(1) }\nlet recursive = Tasks.spawn(forbiddenPump())\nTasks.pump(1)\ntry { Tasks.pump(1) } catch error { System.print(error.kind) }\nSystem.print(recursive.status)", "task-reentry.k3");
    assert(output == "state\nfailed\n"); output.clear();
    runtime.run("fn badCleanup() { defer System.print('child outer')\ndefer fn() { throw ['cleanup failure'] }()\nyield null }\nfn cleanupParent() { defer System.print('parent')\nTasks.spawn(badCleanup())\nwhile true { yield null } }\nlet cleanupFamily = Tasks.spawn(cleanupParent())\nTasks.pump(2)\ntry { Tasks.cancel(cleanupFamily) } catch error { System.gc()\nSystem.print(error[0]) }\nSystem.print(cleanupFamily.status)\nTasks.cancel(cleanupFamily)", "task-cleanup-error.k3");
    assert(output == "child outer\nparent\ncleanup failure\nfailed\n"); output.clear();
    runtime.run("fn transient() { defer System.print('transient cleanup')\nyield null }\nTasks.spawn(transient())", "rooted-task.k3");
    runtime.collect(); assert(runtime.pump(2) == 2);
    assert(output == "transient cleanup\n"); output.clear();
    dk3::Config limitedConfig; limitedConfig.stepLimit = 60;
    limitedConfig.write = [&](const std::string &text) { output += text; };
    dk3::Runtime limited(limitedConfig);
    limited.run("fn limitedWork() { defer System.print('limit cleanup')\nyield null\nwhile true {} }\nlet limitedTask = Tasks.spawn(limitedWork())", "task-budget.k3");
    assert(limited.pump(1) == 1); bool failed = false;
    try { limited.pump(1); } catch (const dk3::Error &error) { failed = error.category == "limit"; }
    assert(failed && output == "limit cleanup\n"); assert(limited.pump(1) == 0);
    output.clear();
    runtime.run("fn shutdownBad() { defer System.print('bad cleanup')\ndefer fn() { throw 'shutdown failure' }()\nyield null }\nfn shutdownGood() { defer System.print('good cleanup')\nyield null }\nTasks.spawn(shutdownBad())\nTasks.spawn(shutdownGood())", "shutdown.k3");
    assert(runtime.pump(2) == 2); failed = false;
    try { runtime.cancelAll(); } catch (const dk3::Error &error) { failed = std::string(error.what()) == "shutdown failure"; }
    assert(failed && output == "bad cleanup\ngood cleanup\n"); assert(runtime.pump(1) == 0); runtime.cancelAll();
    std::cout << "tasks: PASS\n";
}