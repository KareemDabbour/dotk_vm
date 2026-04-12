# Test Thread.done() and Thread.tryResult()

fn slowAdd(a, b) {
    sleep(1)
    return a + b
}

var t = Thread(slowAdd, 2, 3)
print(t.done())
print(type(t.tryResult()) == "null")
print(t.join(2000))
print(t.done())
print(t.tryResult())

# EXPECTED
# false
# true
# 5
# true
# 5
# END_EXPECTED
