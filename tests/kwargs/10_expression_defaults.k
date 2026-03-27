# Default values using expressions (not just literals)

# Empty list default
fn withList(x, items = []) {
  print(len(items))
}
withList(1)
var myList = {10, 20}
withList(1, myList)

# Empty list is fresh each call (no shared-state bug)
fn appendDefault(val, items = []) {
  items.append(val)
  return items
}
var r1 = appendDefault(1)
var r2 = appendDefault(2)
print(len(r1))
print(len(r2))

# Expression with arithmetic
fn calc(x, offset = 2 + 3) {
  print(x + offset)
}
calc(10)
calc(10, 1)

# Global variable reference as default
var DEFAULT_GREETING = "Hi"
fn greetV(name, greeting = DEFAULT_GREETING) {
  print(greeting + " " + name)
}
greetV("Alice")

# Function call as default
fn getDefault() {
  return 42
}
fn withFnDefault(x = getDefault()) {
  print(x)
}
withFnDefault()
withFnDefault(7)

# Constructor call as default
class Config {
  init(val = 0) {
    this.val = val
  }
}
fn withObjDefault(cfg = Config(99)) {
  print(cfg.val)
}
withObjDefault()
withObjDefault(Config(5))

# Earlier param referenced in later default
fn relative(a, b = a + 1) {
  print(a)
  print(b)
}
relative(10)
relative(10, 20)

# Method with expression default
class Adder {
  init(base = 0) {
    this.base = base
  }
  add(x, y = this.base) {
    return x + y
  }
}
var adder = Adder(100)
print(adder.add(5))
print(adder.add(5, 10))

# EXPECTED
# 0
# 2
# 1
# 1
# 15
# 11
# Hi Alice
# 42
# 7
# 99
# 5
# 10
# 11
# 10
# 20
# 105
# 15
# END_EXPECTED
