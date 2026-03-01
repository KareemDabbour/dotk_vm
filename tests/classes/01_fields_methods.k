class Counter {
  init(start) {
    this.v = start
  }

  inc() {
    this.v = this.v + 1
    return this.v
  }

  value() {
    return this.v
  }
}

var c = Counter(10)
print(c.value())
print(c.inc())
print(c.inc())

# EXPECTED
# 10
# 11
# 12
# END_EXPECTED
