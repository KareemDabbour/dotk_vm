# Basic default value
fn f(a, b = 10) {
  print(a)
  print(b)
}
f(1)
f(1, 2)

# All defaults
fn g(a = 1, b = 2, c = 3) {
  print(a + b + c)
}
g()
g(10)
g(10, 20)
g(10, 20, 30)

# Kwargs override defaults
f(@b=99, @a=1)

# Mixed positional + kwargs + defaults
g(10, @c=30)

# String default
fn greet(name, greeting = "Hello") {
  print(greeting + " " + name)
}
greet("Alice")
greet("Alice", "Hey")

# Bool and null defaults
fn h(a, b = true, c = null) {
  print(a)
  print(b)
  print(c)
}
h(1)

# Negative number default
fn neg(x = -5) {
  print(x)
}
neg()
neg(42)

# Default + variadic
fn dv(a, b = 10, *rest) {
  print(a)
  print(b)
  print(len(rest))
}
dv(1)
dv(1, 2, 3, 4)

# Class init with defaults
class Point {
  init(x, y = 0) {
    this.x = x
    this.y = y
  }
}
var p1 = Point(5)
print(p1.x)
print(p1.y)
var p2 = Point(3, 4)
print(p2.x)
print(p2.y)

# Arrow function with default
var add = fn(x, y = 1) => x + y
print(add(10))
print(add(10, 5))

# EXPECTED
# 1
# 10
# 1
# 2
# 6
# 15
# 33
# 60
# 1
# 99
# 42
# Hello Alice
# Hey Alice
# 1
# true
# null
# -5
# 42
# 1
# 10
# 0
# 1
# 2
# 2
# 5
# 0
# 3
# 4
# 11
# 15
# END_EXPECTED
