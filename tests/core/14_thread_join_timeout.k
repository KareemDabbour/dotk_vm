# Test Thread.join(timeoutMs) functionality

fn slowValue() {
    sleep(1)
    return 7
}

var t = Thread(slowValue)
var first = t.join(0)
print(type(first) == "null")
print(t.isAlive())
print(t.join(2000))
print(t.isAlive())

# EXPECTED
# true
# true
# 7
# false
# END_EXPECTED
