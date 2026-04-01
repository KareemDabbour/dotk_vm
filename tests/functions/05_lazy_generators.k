fn seq(n) {
  var i = 0
  while (i < n) {
    yield i
    i += 1
  }
}

var g = seq(3)
print(g._next_())
print(g._next_())
print(g._next_())
print(g._next_())

for x in seq(4) {
  print(x)
}

# EXPECTED
# 0
# 1
# 2
# null
# 0
# 1
# 2
# 3
# END_EXPECTED
