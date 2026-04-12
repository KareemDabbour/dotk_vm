from modules.threading import poolStats, setPoolQueueLimit;

async fn work(n) {
    var x = n + 1
    for (var i = 0; i < 3000; i = i + 1) {
        x = (x * 1103515245 + 12345) % 2147483647
    }
    return x % 1000
}

setPoolQueueLimit(64)
var before = poolStats()
var t0 = clock()

var tasks = []
for (var i = 0; i < 500; i = i + 1) {
    tasks.append(work(i))
}

var sum = 0
for (var i = 0; i < 500; i = i + 1) {
    sum = sum + await tasks[i]
}

var t1 = clock()
var after = poolStats()

print("pool_before_submitted=" + str(before.submitted))
print("pool_before_completed=" + str(before.completed))
print("pool_after_submitted=" + str(after.submitted))
print("pool_after_completed=" + str(after.completed))
print("elapsed_cpu_seconds=" + str(t1 - t0))
print("checksum=" + str(sum))