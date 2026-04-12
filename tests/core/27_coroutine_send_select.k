# sendSelect and recvSelectAny behavior

from modules.coroutines import Channel, recvSelectAny, sendSelect, spawn, sleepFor;

fn delayedSend(ch, delay, value) {
    sleepFor(delay)
    ch.send(value)
    return value
}

var rch1 = Channel(1)
var rch2 = Channel(1)
var rt = spawn(delayedSend, rch2, 0.01, 42)

var rlist = {}
rlist.append(rch1)
rlist.append(rch2)
var rr = recvSelectAny(rlist)
print(rr[0])
print(rr[1])
print(rr[2])
print(rr[3])
print(rr[4])
print(await rt)

var schA = Channel(1)
var schB = Channel(1)
schA.send(111)
var slist = {}
slist.append(schA)
slist.append(schB)
var sr = sendSelect(slist, 9, 20)
print(sr[0])
print(sr[1])
print(sr[2])
print(sr[3])
print(schB.recv())

var full = Channel(1)
full.send(1)
var onlyFull = {}
onlyFull.append(full)
var tr = sendSelect(onlyFull, 2, 5)
print(tr[0])
print(tr[1])
print(tr[2])
print(tr[3])

var closed = Channel(1)
closed.close()
var onlyClosed = {}
onlyClosed.append(closed)
var cr = sendSelect(onlyClosed, 3, 5)
print(cr[0])
print(cr[1])
print(cr[2])
print(cr[3])

# EXPECTED
# true
# 1
# 42
# false
# false
# 42
# true
# 1
# false
# false
# 9
# false
# -1
# false
# true
# false
# -1
# true
# false
# END_EXPECTED
