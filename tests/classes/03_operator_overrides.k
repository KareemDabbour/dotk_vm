class Box {
  init(v) {
    this.v = v
  }

  _eq_(other) {
    return this.v == other.v
  }

  _lt_(other) {
    return this.v < other.v
  }

  _gt_(other) {
    return this.v > other.v
  }
}

var a = Box(10)
var b = Box(10)
var c = Box(20)
print(a == b)
print(a < c)
print(c > b)

# EXPECTED
# true
# true
# true
# END_EXPECTED
