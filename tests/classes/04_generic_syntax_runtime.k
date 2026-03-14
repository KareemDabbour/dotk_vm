class Box[T] {
  init(v) {
    this.v = v
  }

  get() {
    return this.v
  }
}

class Base {
  who() {
    return "base"
  }
}

class Child<Base> {
  who() {
    return "child"
  }
}

var n: Box[number] = Box(42)
var s: Box[string] = Box("ok")
var c = Child()

print(n.get())
print(s.get())
print(c.who())

# EXPECTED
# 42
# ok
# child
# END_EXPECTED
