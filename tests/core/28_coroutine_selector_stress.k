# High-contention selector stress test: many producers, one recvSelectAny consumer.

from modules.coroutines import spawn, channel, recvSelectAny;

fn producer(ch, id, messages) {
    for (var i = 0; i < messages; i = i + 1) {
        ch.send(id)
    }
    return id
}

var producerCount = 6
var messagesPerProducer = 120
var expectedTotal = producerCount * messagesPerProducer

var chans = {}
var tasks = {}
var counts = {}

for (var i = 0; i < producerCount; i = i + 1) {
    var ch = channel(16)
    chans.append(ch)
    tasks.append(spawn(producer, ch, i, messagesPerProducer))
    counts.append(0)
}

var received = 0
while (received < expectedTotal) {
    var r = recvSelectAny(chans)
    if (r[0]) {
        var id = r[2]
        counts[id] = counts[id] + 1
        received = received + 1
    }
}

print(received)
for (var i = 0; i < producerCount; i = i + 1) {
    print(counts[i])
}

for (var i = 0; i < producerCount; i = i + 1) {
    print(await tasks[i])
}

# EXPECTED
# 720
# 120
# 120
# 120
# 120
# 120
# 120
# 0
# 1
# 2
# 3
# 4
# 5
# END_EXPECTED
