fn evens(n) {
  for (var i = 0; i < n; i += 1) {
    yield i * 2
  }
}

var xs = evens(4)
print(xs._next_())
print(xs._next_())
print(xs._next_())
print(xs._next_())
print(xs._next_())

for x in evens(3) {
  print(x)
}

fn withNull() {
  yield
  yield 42
}

var ys = withNull()
print(ys._next_())
print(ys._next_())
print(ys._next_())

# EXPECTED
# 0
# 2
# 4
# 6
# null
# 0
# 2
# 4
# null
# 42
# null
# END_EXPECTED
