# Test async/await functionality

# Test 1: Basic async function returns a future, await gets the value
async fn getVal() {
    return 42
}
var future = getVal()
print(await future)

# Test 2: Async function with arguments
async fn addAsync(a, b) {
    return a + b
}
print(await addAsync(10, 20))

# Test 3: Await on a non-future value (identity)
print(await 99)

# Test 4: Async function doing computation
async fn computeAsync(n) {
    var sum = 0
    for (var i = 0; i < n; i = i + 1) {
        sum = sum + i
    }
    return sum
}
print(await computeAsync(100))

# Test 5: Multiple concurrent async calls
async fn double(x) {
    return x * 2
}
var f1 = double(5)
var f2 = double(10)
var f3 = double(15)
print(await f1)
print(await f2)
print(await f3)

# Test 6: Async with string work
async fn greetAsync(name) {
    return "Hello, " + name + "!"
}
print(await greetAsync("Async"))

# Test 7: Nested await (async calling async)
async fn inner(x) {
    return x + 1
}
async fn outer(x) {
    var v = await inner(x)
    return v * 2
}
print(await outer(5))

# EXPECTED
# 42
# 30
# 99
# 4950
# 10
# 20
# 30
# Hello, Async!
# 12
# END_EXPECTED
