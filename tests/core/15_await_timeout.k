# Test awaitTimeout(thread, timeoutMs)

from modules.threading import awaitTimeout;

async fn slowAsync() {
    sleep(1)
    return 9
}

var t = slowAsync()
var first = awaitTimeout(t, 0)
print(type(first) == "null")
print(awaitTimeout(t, 2000))
print(await t)

# EXPECTED
# true
# 9
# 9
# END_EXPECTED
