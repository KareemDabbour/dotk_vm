fn makeCounter(start) {
  var x = start
  return fn() {
    x = x + 1
    return x
  }
}

var c = makeCounter(40)
print(c())
print(gc() >= 0)
print(c())
print(c())

# EXPECTED
# 41
# true
# 42
# 43
# END_EXPECTED
