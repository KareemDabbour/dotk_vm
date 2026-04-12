# Test coroutine scaffold natives through modules.coroutines

from modules.coroutines import spawn, yieldNow, sleepFor, awaitTimeout;

fn add(a, b) {
    return a + b
}

fn sleepy() {
    sleepFor(0.01)
    yieldNow()
    return 7
}

var t1 = spawn(add, 2, 3)
var t2 = spawn(sleepy)

print(await t1)
print(awaitTimeout(t2, 0) == null)
print(await t2)

# EXPECTED
# 5
# true
# 7
# END_EXPECTED
