# Test: generator send() for bidirectional coroutines

# --- yield as expression with send ---
fn echo_gen() {
  var received = yield "first"
  yield received
  yield "done"
}

var g = echo_gen()
print(g._next_() == "first")
print(g.send("hello") == "hello")
print(g.send("world") == "done")

# --- accumulator coroutine ---
fn accumulator() {
  var total = 0
  while (true) {
    var v = yield total
    total = total + v
  }
}

var acc = accumulator()
print(acc._next_() == 0)
print(acc.send(10) == 10)
print(acc.send(20) == 30)
print(acc.send(5) == 35)

# --- send to finished generator returns nil ---
fn simple() {
  yield 42
}

var s = simple()
print(s._next_() == 42)
print(s._next_() == null)
print(s.send("ignored") == null)

# --- next after send works ---
fn alternating() {
  var x = yield 1
  yield x + 10
  var y = yield 100
  yield y + 200
}

var a = alternating()
print(a._next_() == 1)
print(a.send(5) == 15)
print(a._next_() == 100)
print(a.send(7) == 207)

# EXPECTED
# true
# true
# true
# true
# true
# true
# true
# true
# true
# true
# true
# true
# true
# true
# END_EXPECTED
