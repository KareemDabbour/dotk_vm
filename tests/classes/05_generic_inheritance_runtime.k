class Base[T] {
  init(v: T) {
    this.v = v
  }

  get() {
    return this.v
  }
}

class Child[T]<Base[T]> {
  init(v: T) {
    super.init(v)
  }
}

var n: Base[number] = Child(7)
var s: Base[number] = Base("")
var s: Base[str] = Child("ok")

print(n.get())
print(s.get())

# EXPECTED
# 7
# ok
# END_EXPECTED
