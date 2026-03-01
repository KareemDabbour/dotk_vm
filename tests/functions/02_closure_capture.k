fn makeAdder(x) {
  return fn(y) {
    return x + y
  }
}

var add3 = makeAdder(3)
var add10 = makeAdder(10)
print(add3(7))
print(add10(5))

# EXPECTED
# 10
# 15
# END_EXPECTED
