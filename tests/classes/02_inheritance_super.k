class A {
  init(x) {
    this.x = x
  }

  get() {
    return this.x
  }
}

class B<A> {
  init(x, y) {
    super.init(x)
    this.y = y
  }

  sum() {
    return this.x + this.y
  }
}

var b = B(2, 5)
print(b.get())
print(b.sum())

# EXPECTED
# 2
# 7
# END_EXPECTED
