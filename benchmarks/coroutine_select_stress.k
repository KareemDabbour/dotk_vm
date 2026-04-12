from modules.coroutines import spawn, channel, recvSelectAny;

fn producer(ch, id, messages) {
    for (var i = 0; i < messages; i = i + 1) {
        ch.send(id)
    }
    return id
}

var producerCount = 16
var messagesPerProducer = 300
var expectedTotal = producerCount * messagesPerProducer

var chans = {}
var tasks = {}
var counts = {}

for (var i = 0; i < producerCount; i = i + 1) {
    var ch = channel(64)
    chans.append(ch)
    tasks.append(spawn(producer, ch, i, messagesPerProducer))
    counts.append(0)
}

var t0 = clock()
var received = 0
while (received < expectedTotal) {
    var r = recvSelectAny(chans)
    if (r[0]) {
        var id = r[2]
        counts[id] = counts[id] + 1
        received = received + 1
    }
}
var t1 = clock()

for (var i = 0; i < producerCount; i = i + 1) {
    await tasks[i]
}

var minCount = counts[0]
var maxCount = counts[0]
var checksum = 0
for (var i = 0; i < producerCount; i = i + 1) {
    var v = counts[i]
    if (v < minCount) {
        minCount = v
    }
    if (v > maxCount) {
        maxCount = v
    }
    checksum = checksum + v
}

print("selector_producers=" + str(producerCount))
print("selector_messages_per_producer=" + str(messagesPerProducer))
print("selector_received_total=" + str(received))
print("selector_elapsed_cpu_seconds=" + str(t1 - t0))
print("selector_distribution_min=" + str(minCount))
print("selector_distribution_max=" + str(maxCount))
print("selector_distribution_skew=" + str(maxCount - minCount))
print("selector_checksum=" + str(checksum))
