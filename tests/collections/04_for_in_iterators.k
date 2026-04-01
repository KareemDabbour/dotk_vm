var xs = {1, 2, 3}
for x in xs {
    print(x)
}

for ch in "ok" {
    print(ch)
}

class CounterIter {
    init(stop) {
        this.i = 0
        this.stop = stop
    }

    _next_() {
        if (this.i >= this.stop) return null
        var out = this.i
        this.i += 1
        return out
    }
}

class Counter {
    init(stop) {
        this.stop = stop
    }

    _iter_() {
        return CounterIter(this.stop)
    }
}

for n in Counter(4) {
    print(n)
}

# EXPECTED
# 1
# 2
# 3
# o
# k
# 0
# 1
# 2
# 3
# END_EXPECTED
