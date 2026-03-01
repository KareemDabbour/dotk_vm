class C {
  init(x, y, *rest) {
    this.x = x;
    this.y = y;
    this.rest = rest;
  }
}

var c = C(12, 31, 23, @y=8, @x=2)
print(c.x)
print(c.y)
print(len(c.rest))

# EXPECTED
# 2
# 8
# 3
# END_EXPECTED
