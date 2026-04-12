# Basic channel behavior in modules.coroutines

from modules.coroutines import spawn, Channel;

fn producer(ch) {
    ch.send(10)
    ch.send(20)
    ch.close()
    return 2
}

var ch = Channel(2)
var p = spawn(producer, ch)

var first = ch.recvTimeout(100)
var second = ch.recvTimeout(100)
var third = ch.recvTimeout(10)

print(first)
print(second)
print(third == null)
print(await p)

# EXPECTED
# 10
# 20
# true
# 2
# END_EXPECTED
