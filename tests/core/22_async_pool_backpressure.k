# Test async pool backpressure with bounded queue

from modules.threading import poolStats, setPoolQueueLimit;

async fn inc(n) {
    return n + 1
}

setPoolQueueLimit(4)
var start = poolStats()

var tasks = []
for (var i = 0; i < 40; i = i + 1) {
    tasks.append(inc(i))
}

var mid = poolStats()
print(mid.submitted >= start.submitted + 40)

var sum = 0
for (var i = 0; i < 40; i = i + 1) {
    sum = sum + await tasks[i]
}

var done = poolStats()
print(sum == 820)
print(done.completed >= start.completed + 40)
print(done.rejected == start.rejected)

# EXPECTED
# true
# true
# true
# true
# END_EXPECTED
