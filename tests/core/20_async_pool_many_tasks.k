# Phase 5 regression: async closures are queued through worker pool

async fn square(n) {
    return n * n
}

var tasks = []
for (var i = 0; i < 50; i = i + 1) {
    tasks.append(square(i))
}

var total = 0
for (var i = 0; i < 50; i = i + 1) {
    total = total + await tasks[i]
}

print(total)

# EXPECTED
# 40425
# END_EXPECTED
