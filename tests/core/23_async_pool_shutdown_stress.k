# Stress VM shutdown with many queued async tasks that are not awaited.

from modules.threading import setPoolQueueLimit, poolStats;

async fn slow(n) {
    sleep(0.02)
    return n
}

setPoolQueueLimit(8)
var before = poolStats()

for (var i = 0; i < 120; i = i + 1) {
    slow(i)
}

print(before.queueMax >= 1)
print(true)

# EXPECTED
# true
# true
# END_EXPECTED