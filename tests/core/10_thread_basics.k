# Test basic Thread class functionality

# Test 1: Simple thread that returns a value
fn hello() {
    return 42
}
var t = Thread(hello)
var r = t.join()
print(r)

# Test 2: Thread with arguments
fn add(a, b) {
    return a + b
}
var t2 = Thread(add, 10, 20)
print(t2.join())

# Test 3: Thread with string work
fn greet(name) {
    return "Hello, " + name + "!"
}
var t3 = Thread(greet, "World")
print(t3.join())

# Test 4: Thread doing computation
fn compute(x) {
    var sum = 0
    for (var i = 0; i < x; i = i + 1) {
        sum = sum + i
    }
    return sum
}
var t4 = Thread(compute, 100)
print(t4.join())

# Test 5: isAlive after join
fn quick() { return 1 }
var t5 = Thread(quick)
t5.join()
print(t5.isAlive())

# Test 6: Multiple threads sequentially
var t6 = Thread(add, 3, 7)
var t7 = Thread(add, 100, 200)
print(t6.join())
print(t7.join())

# EXPECTED
# 42
# 30
# Hello, World!
# 4950
# false
# 10
# 300
# END_EXPECTED
