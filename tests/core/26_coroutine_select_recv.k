# Select-style channel receive helper behavior

from modules.coroutines import spawn, sleepFor, channel, recvSelect;

fn producer(ch, delay, value) {
    sleepFor(delay)
    ch.send(value)
    return value
}

var ch1 = channel(1)
var ch2 = channel(1)

var t1 = spawn(producer, ch1, 0.03, "A")
var t2 = spawn(producer, ch2, 0.01, "B")

var chans = {}
chans.append(ch1)
chans.append(ch2)

var r1 = recvSelect(chans, 100)
print(r1[0])
print(r1[1])
print(r1[2])
print(r1[3])
print(r1[4])

var r2 = recvSelect(chans, 100)
print(r2[0])
print(r2[1])
print(r2[2])
print(r2[3])
print(r2[4])

var ch3 = channel(1)
var single = {}
single.append(ch3)
var r3 = recvSelect(single, 10)
print(r3[0])
print(r3[1])
print(r3[3])
print(r3[4])

print(await t1)
print(await t2)

# EXPECTED
# true
# 1
# B
# false
# false
# true
# 0
# A
# false
# false
# false
# -1
# false
# true
# A
# B
# END_EXPECTED
